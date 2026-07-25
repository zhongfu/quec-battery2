#include "qb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int stock_cycle_voltage_limit(int cycle)
{
    return cycle < 101 ? 4400 : cycle < 201 ? 4350 : cycle < 501 ? 4300 : 4250;
}

static int stock_v42_capacity(int cycle)
{
    return cycle < 101 ? 86 : cycle < 201 ? 88 : cycle < 501 ? 92 : 98;
}

/* FUN_0010d140 plus the temperature bands in FUN_0010d500. */
static void stock_update_limits(struct qb_manager *cm)
{
    int temp = cm->battery.temp_decic;
    int full_mv = 0;
    int current_ma = 0;
    int status = cm->temp_status;

    if (qb_power_limit_state == 1) {
        cm->full_voltage_mv = 4180;
        cm->charge_current_ma = cm->buck.vbat_adc_mv > 3000 ? 1060 : 275;
        cm->temp_status = QB_TEMP_HOT;
        return;
    }
    if (qb_power_limit_state == 2) {
        cm->charge_current_ma = 0;
        cm->temp_status = QB_TEMP_OVERHEAT;
        return;
    }

    if (temp <= 0) {
        full_mv = 4400; current_ma = 0; status = QB_TEMP_COLD;
    } else if (temp < 30) {
        return;
    } else if (temp < 150) {
        full_mv = 4400; current_ma = 1325; status = QB_TEMP_COOL;
    } else if (temp < 180) {
        return;
    } else if (temp < 350) {
        full_mv = 4400; current_ma = 5300; status = QB_TEMP_NORMAL;
    } else if (temp < 380) {
        return;
    } else if (temp < 420) {
        full_mv = 4400; current_ma = 3710; status = QB_TEMP_NORMAL_HIGH;
    } else if (temp < 450) {
        return;
    } else if (temp < 470) {
        full_mv = 4180; current_ma = 2650; status = QB_TEMP_WARM;
    } else if (temp < 500) {
        return;
    } else if (temp < 570) {
        full_mv = 4180; current_ma = 1325; status = QB_TEMP_HOT;
    } else if (temp < 600) {
        return;
    } else {
        full_mv = 4180; current_ma = 0; status = QB_TEMP_OVERHEAT;
    }

    if (full_mv > stock_cycle_voltage_limit(cm->battery.cycle_count))
        full_mv = stock_cycle_voltage_limit(cm->battery.cycle_count);
    cm->full_voltage_mv = full_mv;
    cm->charge_current_ma = cm->buck.vbat_adc_mv < 3001 ? 275 : current_ma;
    cm->temp_status = status;
    cm->v42_capacity = stock_v42_capacity(cm->battery.cycle_count);
}

/* FUN_001076fc. */
static void stock_pps_control(struct qb_manager *cm)
{
    int vbat_mv = cm->pump.vbat_adc_mv;
    int next_mv;

    if (vbat_mv <= cm->pd_full_mv &&
        cm->pump.ibat_adc_ma < cm->charge_current_ma - 300) {
        next_mv = cm->pps_voltage_mv + 100;
        if (next_mv >= vbat_mv * 235 / 100)
            next_mv = cm->pps_voltage_mv;
        if (next_mv < 6600)
            next_mv = 6600;
        else if (next_mv > 9800)
            next_mv = 9800;
        cm->pps_voltage_mv = next_mv;
    }
    if (cm->pump.ibat_adc_ma > cm->charge_current_ma || vbat_mv > 4300) {
        next_mv = cm->pps_voltage_mv - 50;
        if (next_mv <= vbat_mv * 202 / 100)
            next_mv = cm->pps_voltage_mv;
        if (next_mv < 6600)
            next_mv = 6600;
        else if (next_mv > 9800)
            next_mv = 9800;
        cm->pps_voltage_mv = next_mv;
    }

    if (cm->pump.ibus_adc_ma == 0) {
        cm->pump_error_count++;
        cm->pump_error = cm->pump_error_count > 10;
    } else {
        cm->pump_error_count = 0;
        cm->pump_error = false;
    }
}

static void test_pdo_parser(void)
{
    struct qb_pdo pdo;
    const char *fixed = "Fixed : 12V, 2A <-";
    const char *pps = "Pps : 3.3V ~ 11V, 3.25A";
    const char *malformed = "Pps";
    const char *zero_current = "Pps : 3.3V ~ 11V, 0A";
    const char *reversed = "Pps : 11V ~ 3.3V, 3A";
    const char *trailing = "Fixed : 5V, 3A garbage";
    const char *invalid = "Unknown : 5V, 3A";

    assert(qb_parse_pdo_line(fixed, &pdo, 2) == 1);
    assert(!pdo.pps && pdo.number == 3 && pdo.min_voltage_mv == 12000);
    assert(pdo.max_voltage_mv == 12000 && pdo.current_ma == 2000);
    assert(pdo.selected);

    assert(qb_parse_pdo_line(pps, &pdo, 3) == 1);
    assert(pdo.pps && pdo.number == 4 && pdo.min_voltage_mv == 3300);
    assert(pdo.max_voltage_mv == 11000 && pdo.current_ma == 3250);
    assert(!pdo.selected);

    assert(qb_parse_pdo_line(malformed, &pdo, 0) == 0);
    assert(!pdo.pps && pdo.min_voltage_mv == 0 &&
           pdo.max_voltage_mv == 0 && pdo.current_ma == 0);
    assert(qb_parse_pdo_line(zero_current, &pdo, 0) == 0);
    assert(qb_parse_pdo_line(reversed, &pdo, 0) == 0);
    assert(qb_parse_pdo_line(trailing, &pdo, 0) == 0);
    assert(qb_parse_pdo_line(invalid, &pdo, 0) == 0);
}

static void write_test_file(const char *directory, const char *name,
                            const char *contents)
{
    char path[160];
    FILE *fp;

    snprintf(path, sizeof(path), "%s%s", directory, name);
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs(contents, fp);
    fclose(fp);
}

static void test_pd_inventory(void)
{
    struct qb_manager cm;
    char directory[128];
    char path[160];

    memset(&cm, 0, sizeof(cm));
    snprintf(directory, sizeof(directory), "/tmp/qb-pd-test-%ld/",
             (long)getpid());
    snprintf(path, sizeof(path), "%.*s", (int)strlen(directory) - 1, directory);
    assert(mkdir(path, 0700) == 0);
    write_test_file(directory, "pdo_set",
                    "Fixed : 5V, 3A <-\n"
                    "Fixed : 9V, 3A\n"
                    "Fixed : 12V, 2A\n"
                    "Pps : 3.3V ~ 11V, 3A\n");
    write_test_file(directory, "cc_pin", "CC1\n");
    write_test_file(directory, "data_role", "UFP\n");
    write_test_file(directory, "pwr_role", "Sink\n");
    cm.pda.name = "test-pd";
    cm.pda.path = directory;
    cm.pda.manager = &cm;

    assert(qb_get_port_info(&cm.pda, true) == 0);
    assert(cm.pda.pdo_count == 4 && cm.pda.fixed_5v);
    assert(cm.pda.fixed_9v && cm.pda.fixed_12v && cm.pda.supports_pps);
    assert(cm.pda.fixed_5v_current_ma == 3000);
    assert(cm.pda.fixed_9v_current_ma == 3000);
    assert(cm.pda.fixed_12v_current_ma == 2000);
    assert(cm.pda.pps_min_voltage_mv == 3300);
    assert(cm.pda.pps_max_voltage_mv == 11000);
    assert(cm.pda.pps_current_ma == 3000 && cm.pda.max_voltage_mv == 12000);
    assert(cm.pda.attached && cm.pda.cc_pin == QB_CC1);
    assert(!cm.pda.data_role_dfp && cm.pda.power_role == QB_ROLE_SINK);

    write_test_file(directory, "cc_pin", "CC2\n");
    write_test_file(directory, "data_role", "DFP\n");
    write_test_file(directory, "pwr_role", "Source\n");
    assert(qb_get_port_info(&cm.pda, true) == 0);
    assert(cm.pda.attached && cm.pda.cc_pin == QB_CC2);
    assert(cm.pda.data_role_dfp && cm.pda.power_role == QB_ROLE_SOURCE);

    static const char *files[] = {"pdo_set", "cc_pin", "data_role", "pwr_role"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", directory, files[i]);
        assert(unlink(path) == 0);
    }
    snprintf(path, sizeof(path), "%.*s", (int)strlen(directory) - 1, directory);
    assert(rmdir(path) == 0);
}

static void test_event_queue(void)
{
    struct qb_manager cm;
    char event[QB_EVENT_SIZE];

    memset(&cm, 0, sizeof(cm));
    assert(pthread_mutex_init(&cm.events.mutex, NULL) == 0);
    for (unsigned i = 0; i < QB_EVENT_CAPACITY; i++) {
        char value[QB_EVENT_SIZE];
        snprintf(value, sizeof(value), "event-%u", i);
        assert(qb_queue_enqueue(&cm, value));
    }
    assert(qb_queue_full(&cm));
    assert(!qb_queue_enqueue(&cm, "overflow"));
    for (unsigned i = 0; i < QB_EVENT_CAPACITY; i++) {
        char expected[QB_EVENT_SIZE];
        snprintf(expected, sizeof(expected), "event-%u", i);
        assert(qb_queue_dequeue(&cm, event));
        assert(strcmp(event, expected) == 0);
    }
    assert(qb_queue_empty(&cm));
    assert(!qb_queue_dequeue(&cm, event));
    pthread_mutex_destroy(&cm.events.mutex);
}

static void test_config_loader(void)
{
    struct qb_manager cm;
    char path[96];
    FILE *fp;

    memset(&cm, 0, sizeof(cm));
    cm.max_current_ma = 5300;
    cm.min_shutdown_mv = 3400;
    cm.max_pd_vbus_mv = 9800;
    cm.pd_full_mv = QB_STOCK_PD_FULL_MV;
    snprintf(path, sizeof(path), "/tmp/qb-config-test-%ld", (long)getpid());
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("config ignored 'other'\n"
          "\toption max_current_ma '1'\n"
          "config battery 'settings'\n"
          "\toption max_current_ma '5100'\n"
          "\toption min_shutdown_mv \"3350\"\n"
          "\toption max_pd_vbus_mv '9600'\n"
          "\toption pd_full_mv '4025'\n", fp);
    fclose(fp);
    assert(qb_load_config_file(&cm, path) == 0);
    unlink(path);
    assert(cm.max_current_ma == 5100);
    assert(cm.min_shutdown_mv == QB_STOCK_MIN_SHUTDOWN_MV);
    assert(cm.max_pd_vbus_mv == 9600);
    assert(cm.pd_full_mv == 4025);
}

static void test_config_bounds_and_helpers(void)
{
    struct qb_manager cm;
    char path[96];
    FILE *fp;

    memset(&cm, 0, sizeof(cm));
    cm.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
    cm.min_shutdown_mv = QB_STOCK_MIN_SHUTDOWN_MV;
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.pd_full_mv = QB_STOCK_PD_FULL_MV;
    snprintf(path, sizeof(path), "/tmp/qb-config-bounds-%ld", (long)getpid());
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("config battery 'settings'\n"
          "\toption max_current_ma '6000'\n"
          "\toption min_shutdown_mv '5000'\n"
          "\toption max_pd_vbus_mv '12000'\n"
          "\toption pd_full_mv '1000'\n", fp);
    fclose(fp);
    assert(qb_load_config_file(&cm, path) == 0);
    assert(cm.max_current_ma == QB_STOCK_MAX_CURRENT_MA);
    assert(cm.min_shutdown_mv == QB_SAFE_MAX_SHUTDOWN_MV);
    assert(cm.max_pd_vbus_mv == QB_STOCK_MAX_PPS_VOLTAGE_MV);
    assert(cm.pd_full_mv == QB_MIN_PD_FULL_MV);

    cm.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
    cm.min_shutdown_mv = QB_STOCK_MIN_SHUTDOWN_MV;
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.pd_full_mv = QB_STOCK_PD_FULL_MV;
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("config battery 'settings'\n"
          "\toption max_current_ma '0'\n"
          "\toption min_shutdown_mv 'invalid'\n"
          "\toption max_pd_vbus_mv '5000'\n"
          "\toption pd_full_mv '4050'\n", fp);
    fclose(fp);
    assert(qb_load_config_file(&cm, path) == 0);
    unlink(path);
    assert(cm.max_current_ma == 0);
    assert(cm.min_shutdown_mv == QB_STOCK_MIN_SHUTDOWN_MV);
    assert(cm.max_pd_vbus_mv == 5000);
    assert(cm.pd_full_mv == 4050);
    assert(qb_limit_charge_current(&cm, 5300) == 0);
    assert(!qb_pps_enabled(&cm));
    assert(qb_limit_pps_voltage(&cm, 8000) == 0);

    cm.max_current_ma = 1200;
    cm.max_pd_vbus_mv = 9000;
    assert(qb_limit_charge_current(&cm, 5300) == 1200);
    assert(qb_limit_charge_current(&cm, -1) == 0);
    assert(qb_pps_enabled(&cm));
    assert(qb_limit_pps_voltage(&cm, 6000) == QB_PPS_MIN_VOLTAGE_MV);
    assert(qb_limit_pps_voltage(&cm, 9500) == 9000);

    cm.battery.temp_decic = 200;
    cm.buck.vbat_adc_mv = 4000;
    qb_power_limit_state = 0;
    qb_update_charge_limits(&cm);
    assert(cm.charge_current_ma == 1200);

    assert(qb_low_voltage_danger(&cm, false, 3399));
    assert(!qb_low_voltage_danger(&cm, false, 3400));
    assert(!qb_low_voltage_danger(&cm, true, 3000));

    struct qb_manager policy;
    memset(&policy, 0, sizeof(policy));
    policy.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
    policy.max_pd_vbus_mv = 9000;
    policy.pd_full_mv = 4000;
    policy.charge_current_ma = QB_STOCK_MAX_CURRENT_MA;
    policy.pps_voltage_mv = 9000;
    policy.pump.vbat_adc_mv = 4050;
    policy.pump.ibat_adc_ma = 1000;
    policy.pump.ibus_adc_ma = 1;
    qb_pump_pps_control(&policy);
    assert(policy.pps_voltage_mv == 9000);

    policy.pd_full_mv = 4100;
    policy.pps_voltage_mv = 9000;
    qb_pump_pps_control(&policy);
    assert(policy.pps_voltage_mv == 9000);
}

static void test_initial_temperature_policy(void)
{
    static const struct {
        int temp;
        enum qb_temp_status status;
        int full_mv;
        int current_ma;
    } cases[] = {
        {-1, QB_TEMP_COLD, 4400, 5300},
        {0, QB_TEMP_COOL, 4400, 1060},
        {149, QB_TEMP_COOL, 4400, 1060},
        {150, QB_TEMP_NORMAL, 4400, 5300},
        {449, QB_TEMP_NORMAL, 4400, 5300},
        {450, QB_TEMP_HOT, 4400, 1060},
        {599, QB_TEMP_HOT, 4400, 1060},
        {600, QB_TEMP_OVERHEAT, 4400, 0},
    };

    qb_power_limit_state = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct qb_manager cm;
        memset(&cm, 0, sizeof(cm));
        cm.full_voltage_mv = 4400;
        cm.charge_current_ma = 5300;
        cm.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
        cm.v42_capacity = 86;
        cm.buck.vbat_adc_mv = 4000;
        cm.battery.temp_decic = cases[i].temp;
        qb_init_temp_status(&cm);
        assert(cm.temp_status == cases[i].status);
        assert(cm.full_voltage_mv == cases[i].full_mv);
        assert(cm.charge_current_ma == cases[i].current_ma);
        assert(cm.v42_capacity == 86);
    }
}

static void test_temperature_policy_matrix(void)
{
    static const int temperatures[] = {
        -10, 0, 1, 29, 30, 149, 150, 179, 180, 349, 350, 379, 380,
        419, 420, 449, 450, 469, 470, 499, 500, 569, 570, 599, 600,
    };
    static const int cycles[] = {0, 100, 101, 200, 201, 500, 501};
    static const int battery_mv[] = {2999, 3000, 3001, 4000};

    for (int cooling = 0; cooling <= 2; cooling++) {
        for (int previous = 0; previous <= 6; previous++) {
            for (size_t t = 0; t < sizeof(temperatures) / sizeof(temperatures[0]); t++) {
                for (size_t c = 0; c < sizeof(cycles) / sizeof(cycles[0]); c++) {
                    for (size_t v = 0; v < sizeof(battery_mv) / sizeof(battery_mv[0]); v++) {
                        struct qb_manager got;
                        struct qb_manager expected;

                        memset(&got, 0, sizeof(got));
                        got.temp_status = previous;
                        got.full_voltage_mv = 4321;
                        got.charge_current_ma = 777;
                        got.v42_capacity = 66;
                        got.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
                        got.battery.temp_decic = temperatures[t];
                        got.battery.cycle_count = cycles[c];
                        got.buck.vbat_adc_mv = battery_mv[v];
                        expected = got;
                        qb_power_limit_state = cooling;
                        qb_update_charge_limits(&got);
                        stock_update_limits(&expected);
                        assert(got.temp_status == expected.temp_status);
                        assert(got.full_voltage_mv == expected.full_voltage_mv);
                        assert(got.charge_current_ma == expected.charge_current_ma);
                        assert(got.v42_capacity == expected.v42_capacity);
                    }
                }
            }
        }
    }
    qb_power_limit_state = 0;
}

static void test_pps_policy_matrix(void)
{
    for (int vbat = 3300; vbat <= 4400; vbat += 50) {
        for (int ibat = 0; ibat <= 6000; ibat += 250) {
            for (int pps = 6500; pps <= 9900; pps += 100) {
                for (int ibus = 0; ibus <= 1; ibus++) {
                    struct qb_manager got;
                    struct qb_manager expected;

                    memset(&got, 0, sizeof(got));
                    got.pd_full_mv = 4200;
                    got.charge_current_ma = 5300;
                    got.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
                    got.pps_voltage_mv = pps;
                    got.pump.vbat_adc_mv = vbat;
                    got.pump.ibat_adc_ma = ibat;
                    got.pump.ibus_adc_ma = ibus;
                    got.pump_error_count = 9;
                    got.pump_error = true;
                    expected = got;
                    qb_pump_pps_control(&got);
                    stock_pps_control(&expected);
                    assert(got.pps_voltage_mv == expected.pps_voltage_mv);
                    assert(got.pump_error_count == expected.pump_error_count);
                    assert(got.pump_error == expected.pump_error);
                }
            }
        }
    }
}

static void test_fixed_charge_ramp(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.charge_current_ma = 1000;
    cm.fixed_charge_current_ma = 500;
    cm.buck.vbus_adc_mv = 4200;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 300);
    cm.buck.vbus_adc_mv = 4500;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 300);
    cm.buck.vbus_adc_mv = 4801;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 500);
    cm.fixed_charge_current_ma = 900;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 1000);
    cm.fixed_charge_current_ma = 100;
    cm.buck.vbus_adc_mv = 4200;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == -100);
}

static void test_charge_state_flags(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    qb_enable_buck_cfg(&cm, &cm.pda);
    assert(cm.buck.working && cm.buck.charge_status == 1 && cm.pda.working);
    assert(!cm.pump.working && !cm.pdb.working);
    qb_disable_buck_cfg(&cm, &cm.pda);
    assert(!cm.buck.working && !cm.pda.working);
    qb_enable_pump_cfg(&cm, &cm.pdb);
    assert(cm.pump.working && cm.pump.charge_status == 1 && cm.pdb.working);
    assert(!cm.buck.working && !cm.pda.working);
    qb_no_charge(&cm);
    assert(!cm.pump.working && !cm.buck.working);
    assert(!cm.pda.working && !cm.pdb.working);
}

int main(void)
{
    test_pdo_parser();
    test_pd_inventory();
    test_event_queue();
    test_config_loader();
    test_config_bounds_and_helpers();
    test_initial_temperature_policy();
    test_temperature_policy_matrix();
    test_pps_policy_matrix();
    test_fixed_charge_ramp();
    test_charge_state_flags();
    puts("behavior regression suite passed");
    return 0;
}
