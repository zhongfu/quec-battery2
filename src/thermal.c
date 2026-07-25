#include "qb.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QB_GL_OTG_CONFIG "/etc/config/gl_otg"
#define QB_POWER_LIMIT_FILE "/tmp/power_limit_state"
#define QB_CYCLE_UPDATE_SECONDS 43200

struct qb_charger_config {
    uint32_t reserved0;
    uint16_t cycle_count;
    uint16_t reserved1;
    uint64_t reserved2;
};

typedef int (*qb_get_charger_config_fn)(struct qb_charger_config *config);
typedef int (*qb_set_charger_config_fn)(struct qb_charger_config config);

_Static_assert(sizeof(struct qb_charger_config) == 16,
               "charger config ABI must remain 16 bytes");
static void *qb_ql_sdk;
static qb_get_charger_config_fn qb_ql_get_config;
static qb_set_charger_config_fn qb_ql_set_config;
static int qb_ql_sdk_initialized;
static unsigned qb_raw_cycle;

static const uint16_t qb_ocv_mv[101] = {
    3300, 3487, 3568, 3624, 3664, 3683, 3688, 3689, 3691, 3693,
    3698, 3703, 3709, 3714, 3721, 3727, 3734, 3741, 3746, 3751,
    3756, 3760, 3764, 3768, 3771, 3775, 3777, 3780, 3782, 3785,
    3788, 3790, 3793, 3796, 3799, 3802, 3805, 3808, 3811, 3815,
    3819, 3823, 3827, 3831, 3835, 3840, 3844, 3850, 3855, 3861,
    3867, 3874, 3882, 3889, 3898, 3907, 3917, 3927, 3936, 3944,
    3952, 3960, 3969, 3977, 3985, 3994, 4002, 4011, 4020, 4029,
    4038, 4048, 4058, 4068, 4078, 4089, 4099, 4109, 4120, 4131,
    4141, 4152, 4163, 4174, 4185, 4196, 4207, 4218, 4230, 4241,
    4252, 4264, 4275, 4287, 4299, 4311, 4323, 4336, 4348, 4363,
    4378,
};

static void qb_init_ql_sdk(void)
{
    void *symbol;

    if (qb_ql_sdk_initialized)
        return;
    qb_ql_sdk_initialized = 1;
    qb_ql_sdk = dlopen("libql_sdk.so", RTLD_LAZY | RTLD_LOCAL);
    if (!qb_ql_sdk)
        return;
    symbol = dlsym(qb_ql_sdk, "ql_dm_get_charger_config");
    memcpy(&qb_ql_get_config, &symbol, sizeof(qb_ql_get_config));
    symbol = dlsym(qb_ql_sdk, "ql_dm_set_charger_config");
    memcpy(&qb_ql_set_config, &symbol, sizeof(qb_ql_set_config));
}

static int qb_cycle_voltage_limit(int cycle)
{
    if (cycle < 101)
        return 4400;
    if (cycle < 201)
        return 4350;
    if (cycle < 501)
        return 4300;
    return 4250;
}

static int qb_cycle_v42_capacity(int cycle)
{
    if (cycle < 101)
        return 86;
    if (cycle < 201)
        return 88;
    if (cycle < 501)
        return 92;
    return 98;
}

static void qb_run_system(const char *command)
{
    int result = system(command);
    (void)result;
}

static bool qb_adapter_online(struct qb_manager *cm)
{
    char value[32] = {0};
    bool online = false;

    qb_read_str(cm->pda.path, "cc_pin", value, sizeof(value));
    if (strcmp(value, "None") != 0) {
        qb_read_str(cm->pda.path, "pwr_role", value, sizeof(value));
        online = strcmp(value, "Sink") == 0;
    }

    qb_read_str(cm->pdb.path, "cc_pin", value, sizeof(value));
    if (strcmp(value, "None") != 0) {
        qb_read_str(cm->pdb.path, "pwr_role", value, sizeof(value));
        if (strcmp(value, "Sink") == 0)
            return true;
    }
    return online;
}

static void qb_unquote(char *value)
{
    size_t length = strlen(value);

    if (length > 1 && (value[0] == '\'' || value[0] == '"')) {
        memmove(value, value + 1, length);
        length--;
        if (length && (value[length - 1] == '\'' || value[length - 1] == '"'))
            value[length - 1] = '\0';
    }
}

static int qb_otg_capacity_threshold(void)
{
    static int cached_threshold;
    static bool threshold_cached;
    FILE *fp;
    char line[256];
    bool typec1 = false;

    if (threshold_cached)
        return cached_threshold;
    fp = fopen(QB_GL_OTG_CONFIG, "r");
    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        char first[64];
        char second[64];
        char *p = line;
        int fields;

        while (*p == ' ' || *p == '\t')
            p++;
        fields = sscanf(p, "config %63s %63s", first, second);
        if (fields > 0) {
            typec1 = false;
            if (fields == 2) {
                qb_unquote(second);
                typec1 = strcmp(second, "typec1") == 0;
            }
            continue;
        }
        if (typec1 && sscanf(p, "option %63s %63s", first, second) == 2) {
            qb_unquote(first);
            qb_unquote(second);
            if (!strcmp(first, "threshold")) {
                cached_threshold = atoi(second);
                threshold_cached = true;
                fclose(fp);
                return cached_threshold;
            }
        }
    }
    fclose(fp);
    return -1;
}

static void qb_set_capacity_limit_state(int state)
{
    FILE *fp = fopen(QB_POWER_LIMIT_FILE, "w");

    if (!fp)
        return;
    fprintf(fp, "%d\n", state);
    fclose(fp);
}

static void qb_apply_thermal_limits(struct qb_manager *cm, int full_mv,
                                    int current_ma, enum qb_temp_status status)
{
    int cycle_limit;

    if (qb_power_limit_state == 1) {
        cm->full_voltage_mv = 4180;
        cm->charge_current_ma = qb_limit_charge_current(
            cm, cm->buck.vbat_adc_mv > 3000 ? 1060 : 275);
        cm->temp_status = QB_TEMP_HOT;
        return;
    }
    if (qb_power_limit_state == 2) {
        cm->temp_status = QB_TEMP_OVERHEAT;
        cm->charge_current_ma = 0;
        return;
    }
    if (full_mv == 0)
        return;

    cycle_limit = qb_cycle_voltage_limit(cm->battery.cycle_count);
    cm->full_voltage_mv = full_mv < cycle_limit ? full_mv : cycle_limit;
    cm->temp_status = status;
    cm->charge_current_ma = qb_limit_charge_current(
        cm, cm->buck.vbat_adc_mv < 3001 ? 275 : current_ma);
    cm->v42_capacity = qb_cycle_v42_capacity(cm->battery.cycle_count);
}

void qb_init_temp_status(struct qb_manager *cm)
{
    int temp = cm->battery.temp_decic;

    if (temp < 0)
        cm->temp_status = QB_TEMP_COLD;
    else if (temp < 150)
        qb_apply_thermal_limits(cm, 4400, 1060, QB_TEMP_COOL);
    else if (temp < 450)
        qb_apply_thermal_limits(cm, 4400, 5300, QB_TEMP_NORMAL);
    else if (temp < 600)
        qb_apply_thermal_limits(cm, 4180, 1060, QB_TEMP_HOT);
    else
        qb_apply_thermal_limits(cm, 4180, 0, QB_TEMP_OVERHEAT);
}

void qb_update_charge_limits(struct qb_manager *cm)
{
    int temp = cm->battery.temp_decic;

    if (temp <= 0)
        qb_apply_thermal_limits(cm, 4400, 0, QB_TEMP_COLD);
    else if (temp < 30)
        qb_apply_thermal_limits(cm, 0, 0, cm->temp_status);
    else if (temp < 150)
        qb_apply_thermal_limits(cm, 4400, 1325, QB_TEMP_COOL);
    else if (temp < 180)
        qb_apply_thermal_limits(cm, 0, 0, cm->temp_status);
    else if (temp < 350)
        qb_apply_thermal_limits(cm, 4400, 5300, QB_TEMP_NORMAL);
    else if (temp < 380)
        qb_apply_thermal_limits(cm, 0, 0, cm->temp_status);
    else if (temp < 420)
        qb_apply_thermal_limits(cm, 4400, 3710, QB_TEMP_NORMAL_HIGH);
    else if (temp < 450)
        qb_apply_thermal_limits(cm, 0, 0, cm->temp_status);
    else if (temp < 470)
        qb_apply_thermal_limits(cm, 4180, 2650, QB_TEMP_WARM);
    else if (temp < 500)
        qb_apply_thermal_limits(cm, 0, 0, cm->temp_status);
    else if (temp < 570)
        qb_apply_thermal_limits(cm, 4180, 1325, QB_TEMP_HOT);
    else if (temp < 600)
        qb_apply_thermal_limits(cm, 0, 0, cm->temp_status);
    else
        qb_apply_thermal_limits(cm, 4180, 0, QB_TEMP_OVERHEAT);
}

int qb_set_battery_cycle(struct qb_manager *cm)
{
    struct qb_charger_config config = {0};
    char value[16];
    int rc;

    qb_init_ql_sdk();
    if (!qb_ql_get_config)
        return -1;
    rc = qb_ql_get_config(&config);
    if (rc != 0)
        config.cycle_count = 0;
    qb_raw_cycle = config.cycle_count;

    qb_get_battery_online(cm);
    if (!qb_battery_present(cm))
        return -1;
    snprintf(value, sizeof(value), "%u", qb_raw_cycle);
    qb_write_str(QB_CW2217_PATH, "bat_cycle", value);
    return 0;
}

int qb_update_battery_cycle(struct qb_manager *cm)
{
    struct qb_charger_config config = {0};
    int cycle;
    int rc;

    qb_init_ql_sdk();
    if (!qb_ql_get_config)
        return -1;
    rc = qb_ql_get_config(&config);
    if (rc != 0)
        config.cycle_count = 0;
    qb_raw_cycle = config.cycle_count;

    if (qb_read_int(QB_CW2217_PATH, "bat_cycle", &cycle) < 0)
        return -1;
    config.cycle_count = (uint16_t)cycle;
    if ((unsigned)config.cycle_count > qb_raw_cycle) {
        if (!qb_battery_present(cm) || !qb_ql_set_config)
            return -1;
        qb_ql_set_config(config);
        qb_raw_cycle = config.cycle_count;
    }
    return 0;
}

static int qb_ocv_to_soc(int vbat_mv)
{
    uint16_t voltage = (uint16_t)vbat_mv;

    if (voltage < 3301)
        return 0;
    if (voltage >= 4378)
        return 100;
    for (int i = 0; i < 100; i++) {
        if (voltage <= qb_ocv_mv[i + 1])
            return i + (voltage - qb_ocv_mv[i]) /
                       (qb_ocv_mv[i + 1] - qb_ocv_mv[i]);
    }
    return -1;
}

static bool qb_check_gauge_accuracy(struct qb_manager *cm, time_t now,
                                    time_t *last_check)
{
    int gauge_mv;
    int difference;
    int estimated_capacity;
    char reason[128];

    if (!qb_battery_present(cm) || cm->battery.current_ma < -149 ||
        cm->battery.current_ma > 149 || now == (time_t)-1 ||
        (*last_check != 0 && now - *last_check < 3600))
        return false;
    *last_check = now;

    if (qb_read_int(QB_SGM41542_PATH, "vbat_adc", &cm->buck.vbat_adc_mv) < 0)
        return false;
    gauge_mv = cm->battery.voltage_mv / 1000;
    difference = cm->buck.vbat_adc_mv - gauge_mv;
    if (difference < 0)
        difference = -difference;
    if (difference >= 1201) {
        snprintf(reason, sizeof(reason), "reset ,sgm vat:%dmv, fgu volt:%dmv",
                 cm->buck.vbat_adc_mv, gauge_mv);
        qb_write_str(QB_CW2217_PATH, "cw2217_reset", reason);
        return true;
    }

    estimated_capacity = qb_ocv_to_soc(cm->buck.vbat_adc_mv);
    difference = estimated_capacity - cm->battery.capacity;
    if (difference < 0)
        difference = -difference;
    if (estimated_capacity >= 0 && difference > 20) {
        snprintf(reason, sizeof(reason), "reset ,sgm cap:%d, fgu cap:%d",
                 estimated_capacity, cm->battery.capacity);
        qb_write_str(QB_CW2217_PATH, "cw2217_reset", reason);
        return true;
    }
    return false;
}

static void qb_update_capacity_reserve(struct qb_manager *cm,
                                       int *configured_threshold,
                                       int *mos1_pin, int *mos2_pin)
{
    int threshold = qb_otg_capacity_threshold();

    qb_read_int(QB_SGM41542_PATH, "set_cap", configured_threshold);
    if (*configured_threshold != threshold)
        qb_set_sgm41542_int(cm, "set_cap", threshold);

    if (cm->battery.capacity < threshold) {
        qb_read_int(QB_SGM41542_PATH, "mos1_pin", mos1_pin);
        qb_read_int(QB_SGM41542_PATH, "mos2_pin", mos2_pin);
        if (cm->power_limit) {
            if (!cm->mos_status[0] && !cm->mos_status[1] &&
                *mos1_pin != 0 && *mos2_pin != 0)
                return;
            qb_set_capacity_limit_state(1);
            qb_mos_off(cm, 0);
            qb_mos_off(cm, 1);
            return;
        }

        qb_set_capacity_limit_state(1);
        cm->power_limit = true;
        qb_reset_charge_state(cm);
        qb_get_port_info(&cm->pdb, false);
        cm->work_mode = QB_MODE_RESELECT;
        qb_select_mode(cm);
    } else if (cm->power_limit) {
        qb_set_capacity_limit_state(0);
        cm->power_limit = false;
        qb_reset_charge_state(cm);
        qb_get_port_info(&cm->pdb, false);
        cm->work_mode = QB_MODE_RESELECT;
        qb_select_mode(cm);
        qb_run_system("/usr/bin/usb_otg_manage restart &");
    }
}

static void qb_update_hiz_state(struct qb_manager *cm)
{
    int status = cm->temp_status;

    if (status < QB_TEMP_HOT) {
        if (cm->hiz_status == 2)
            return;
        goto exit_hiz;
    }
    if (cm->hiz_status == 1) {
        if (status != QB_TEMP_HOT || cm->v42_capacity < cm->battery.capacity)
            return;
    } else if (cm->buck.charge_status != 1) {
        if (cm->hiz_status == 2 || status != QB_TEMP_HOT ||
            cm->v42_capacity < cm->battery.capacity)
            return;
    } else if (cm->v42_capacity < cm->battery.capacity) {
        if (cm->buck.vbat_adc_mv < 4201 || cm->battery.current_ma > 1059)
            return;
        qb_reset_charge_state(cm);
        cm->work_mode = QB_MODE_RESELECT;
        qb_set_sgm41542(cm, "hiz_mode", "0");
        qb_set_sgm41542(cm, "charge_en", "0");
        cm->hiz_status = 1;
        return;
    } else if (cm->hiz_status == 2 || status != QB_TEMP_HOT) {
        return;
    }

    if (cm->buck.vbat_adc_mv > 4199)
        return;
exit_hiz:
    qb_set_sgm41542(cm, "hiz_mode", "1");
    cm->hiz_status = 2;
    qb_select_mode(cm);
}

void *qb_gauge_monitor(void *arg)
{
    struct qb_manager *cm = arg;
    int danger_count = 0;
    int configured_threshold = 0;
    int mos1_pin = 0;
    int mos2_pin = 0;
    time_t last_accuracy_check = 0;
    time_t next_cycle_update = 0;
    bool cycle_timer_started = false;
    struct timespec cycle_clock;

    qb_get_battery_info(cm);
    pthread_detach(pthread_self());
    cm->v42_capacity = 86;
    cm->hiz_status = 2;
    qb_get_battery_online(cm);
    if (clock_gettime(CLOCK_MONOTONIC, &cycle_clock) == 0) {
        next_cycle_update = cycle_clock.tv_sec + QB_CYCLE_UPDATE_SECONDS;
        cycle_timer_started = true;
    }
    qb_init_temp_status(cm);

    while (cm->running) {
        bool adapter_online;
        bool danger = false;
        bool gauge_reset;
        int battery_info_status;
        int presence_status;
        time_t now;

        presence_status = qb_get_battery_online(cm);
        if (presence_status < 0) {
            cm->charge_current_ma = 0;
            cm->temp_status = QB_TEMP_OVERHEAT;
            qb_disable_pump(cm);
            qb_disable_buck(cm);
            QBLOG(0x9ed, "battery presence unavailable raw:%d\n",
                  cm->battery.raw_present);
            sleep(3);
            continue;
        }

        battery_info_status = qb_battery_present(cm) ?
                              qb_get_battery_info(cm) : 0;
        if (qb_battery_present(cm) && battery_info_status < 0) {
            cm->charge_current_ma = 0;
            cm->temp_status = QB_TEMP_OVERHEAT;
            QBLOG(0x9ed, "battery telemetry unavailable failures:%u\n",
                  cm->battery.telemetry_failures);
            sleep(3);
            continue;
        }
        if (qb_battery_absent(cm))
            cm->battery.temp_decic = -5;
        qb_update_charge_limits(cm);

        QBLOG(0x9ed, "capacity:%d temp:%d current:%d mA vbat:%d mV full:%d mV "
              "charge:%d mA present:%d temp_status:%d mode:%d work_mode:%d",
              cm->battery.capacity, cm->battery.temp_decic,
              cm->battery.current_ma, cm->buck.vbat_adc_mv,
              cm->full_voltage_mv, cm->charge_current_ma,
              cm->battery.raw_present, cm->temp_status, cm->mode, cm->work_mode);

        if (qb_battery_present(cm)) {
            qb_update_capacity_reserve(cm, &configured_threshold,
                                       &mos1_pin, &mos2_pin);
            if ((uint32_t)cm->battery.temp_decic - 101U > 398U) {
                qb_mos_off(cm, 0);
                qb_mos_off(cm, 1);
            }

            adapter_online = qb_adapter_online(cm);
            if (cm->battery.capacity < 1 && !adapter_online) {
                qb_mos_off(cm, 0);
                qb_mos_off(cm, 1);
            }
            if (qb_low_voltage_danger(cm, adapter_online,
                                      cm->buck.vbat_adc_mv))
                danger = true;
            else if (cm->battery.temp_decic <= -100 ||
                     cm->battery.temp_decic >= 580)
                danger = true;

            if (danger) {
                danger_count++;
                if (danger_count > 10)
                    qb_run_system("poweroff");
            } else {
                danger_count = 0;
            }
            qb_update_hiz_state(cm);
        } else if (qb_battery_absent(cm)) {
            danger_count = 0;
            if (cm->power_limit) {
                qb_set_capacity_limit_state(0);
                cm->power_limit = false;
                qb_reset_charge_state(cm);
                qb_get_port_info(&cm->pdb, false);
                cm->work_mode = QB_MODE_RESELECT;
                qb_select_mode(cm);
                qb_run_system("/usr/bin/usb_otg_manage restart &");
            }
        }

        if (clock_gettime(CLOCK_MONOTONIC, &cycle_clock) == 0) {
            if (!cycle_timer_started) {
                next_cycle_update =
                    cycle_clock.tv_sec + QB_CYCLE_UPDATE_SECONDS;
                cycle_timer_started = true;
            } else if (cycle_clock.tv_sec >= next_cycle_update) {
                qb_update_battery_cycle(cm);
                next_cycle_update =
                    cycle_clock.tv_sec + QB_CYCLE_UPDATE_SECONDS;
            }
        }
        now = time(NULL);
        gauge_reset = qb_check_gauge_accuracy(cm, now, &last_accuracy_check);
        sleep(gauge_reset ? 5 : 3);
    }
    return NULL;
}
