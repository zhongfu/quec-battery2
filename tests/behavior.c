#include "qb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
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

/* Reference model for the unified bounded PPS controller. */
static void bounded_pps_control_reference(struct qb_manager *cm)
{
    int vbat_mv = cm->pump.vbat_adc_mv;
    int target_mv = cm->full_voltage_mv < QB_PUMP_MAX_TARGET_MV ?
                    cm->full_voltage_mv : QB_PUMP_MAX_TARGET_MV;
    int bounded_ceiling_mv =
        target_mv * QB_PUMP_LIMIT_RATIO_PERCENT / 100;
    if (cm->pump_pps_ceiling_mv > 0 &&
        cm->pump_pps_ceiling_mv < bounded_ceiling_mv)
        bounded_ceiling_mv = cm->pump_pps_ceiling_mv;
    bool near_limit =
        vbat_mv >= target_mv - QB_PUMP_LIMIT_FINE_WINDOW_MV &&
        vbat_mv < target_mv + QB_PUMP_LIMIT_FINE_WINDOW_MV;
    int next_mv;
    int adjustment_mv;

    if (vbat_mv < target_mv - QB_PUMP_CV_LOWER_MARGIN_MV &&
        cm->pump.ibat_adc_ma < cm->charge_current_ma - 300) {
        next_mv = cm->pps_voltage_mv +
                  (near_limit ? QB_PPS_VOLTAGE_STEP_MV : 100);
        if (next_mv >= vbat_mv * 235 / 100)
            next_mv = cm->pps_voltage_mv;
        if (next_mv > bounded_ceiling_mv)
            next_mv = bounded_ceiling_mv;
        if (next_mv < QB_PPS_MIN_VOLTAGE_MV)
            next_mv = QB_PPS_MIN_VOLTAGE_MV;
        else if (next_mv > cm->max_pd_vbus_mv)
            next_mv = cm->max_pd_vbus_mv;
        cm->pps_voltage_mv = next_mv;
    }
    if (vbat_mv > target_mv ||
        cm->pump.ibat_adc_ma > cm->charge_current_ma || vbat_mv > 4300) {
        adjustment_mv = near_limit ? QB_PPS_VOLTAGE_STEP_MV :
                        (vbat_mv > target_mv ? 100 : 50);
        next_mv = cm->pps_voltage_mv - adjustment_mv;
        if (next_mv <= vbat_mv * 202 / 100)
            next_mv = cm->pps_voltage_mv;
        if (next_mv < QB_PPS_MIN_VOLTAGE_MV)
            next_mv = QB_PPS_MIN_VOLTAGE_MV;
        else if (next_mv > cm->max_pd_vbus_mv)
            next_mv = cm->max_pd_vbus_mv;
        cm->pps_voltage_mv = next_mv;
    }
    if (cm->pps_voltage_mv > bounded_ceiling_mv)
        cm->pps_voltage_mv = bounded_ceiling_mv;

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
static void test_buck_fixed_pdo_selection(void)
{
    struct qb_pd_port port;
    int voltage_mv = 0;
    int current_ma = 0;

    memset(&port, 0, sizeof(port));
    port.fixed_5v = true;
    port.fixed_5v_current_ma = 3000;
    port.fixed_9v = true;
    port.fixed_9v_current_ma = 3000;
    port.fixed_12v = true;
    port.fixed_12v_current_ma = 2000;

    assert(qb_select_buck_fixed_pdo(&port, &voltage_mv, &current_ma));
    assert(voltage_mv == 12000 && current_ma == 2000);

    port.fixed_12v_current_ma = 0;
    assert(qb_select_buck_fixed_pdo(&port, &voltage_mv, &current_ma));
    assert(voltage_mv == 9000 && current_ma == 3000);

    port.fixed_9v = false;
    assert(qb_select_buck_fixed_pdo(&port, &voltage_mv, &current_ma));
    assert(voltage_mv == 5000 && current_ma == 3000);

    port.fixed_5v = false;
    assert(!qb_select_buck_fixed_pdo(&port, &voltage_mv, &current_ma));
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
    assert(cm.pda.telemetry_valid && cm.pda.telemetry_failures == 0);

    write_test_file(directory, "cc_pin", "CC2\n");
    write_test_file(directory, "data_role", "DFP\n");
    write_test_file(directory, "pwr_role", "Source\n");
    assert(qb_get_port_info(&cm.pda, true) == 0);
    assert(cm.pda.attached && cm.pda.cc_pin == QB_CC2);
    assert(cm.pda.data_role_dfp && cm.pda.power_role == QB_ROLE_SOURCE);
    assert(cm.pda.telemetry_valid && cm.pda.telemetry_failures == 0);

    snprintf(path, sizeof(path), "%spwr_role", directory);
    assert(unlink(path) == 0);
    assert(qb_get_port_info(&cm.pda, false) == -1);
    assert(!cm.pda.telemetry_valid);
    assert(cm.pda.power_role == QB_ROLE_UNKNOWN);
    assert(cm.pda.telemetry_failures == 1);
    write_test_file(directory, "pwr_role", "Sink\n");
    assert(qb_get_port_info(&cm.pda, false) == 0);
    assert(cm.pda.telemetry_valid && cm.pda.telemetry_failures == 0);

    static const char *files[] = {"pdo_set", "cc_pin", "data_role", "pwr_role"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", directory, files[i]);
        assert(unlink(path) == 0);
    }
    snprintf(path, sizeof(path), "%.*s", (int)strlen(directory) - 1, directory);
    assert(rmdir(path) == 0);
}

static void test_pdo_request_safety(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.pda.name = "test-pd";
    cm.pda.path = "/tmp/qb-pdo-request-missing/";
    cm.pda.manager = &cm;
    cm.pda.pdo_count = 1;
    cm.pda.pdo[0].pps = true;
    cm.pda.pdo[0].min_voltage_mv = 3300;
    cm.pda.pdo[0].max_voltage_mv = 11000;
    cm.pda.pdo[0].current_ma = 3000;

    assert(!qb_request_pdo(&cm.pda, 9000, 0));
    assert(!qb_request_pdo(&cm.pda, 12000, 2000));
    assert(!qb_request_pdo(&cm.pda, 9000, 2000));
    assert(qb_pps_voltage_matches(9000, 8300));
    assert(qb_pps_voltage_matches(9000, 9700));
    assert(!qb_pps_voltage_matches(9000, 8299));
    assert(!qb_pps_voltage_matches(9000, 9701));
    assert(!qb_pps_voltage_matches(0, 0));
}

static void test_sysfs_integer_validation(void)
{
    char directory[128];
    char path[160];
    int value = 77;

    snprintf(directory, sizeof(directory), "/tmp/qb-read-test-%ld/",
             (long)getpid());
    snprintf(path, sizeof(path), "%.*s", (int)strlen(directory) - 1, directory);
    assert(mkdir(path, 0700) == 0);

    write_test_file(directory, "value", "123\n");
    assert(qb_read_int(directory, "value", &value) == 0 && value == 123);
    write_test_file(directory, "value", "");
    assert(qb_read_int(directory, "value", &value) == -1 && value == 123);
    write_test_file(directory, "value", "999999999999999999999999\n");
    assert(qb_read_int(directory, "value", &value) == -1 && value == 123);
    write_test_file(directory, "value", "123garbage\n");
    assert(qb_read_int(directory, "value", &value) == -1 && value == 123);

    snprintf(path, sizeof(path), "%svalue", directory);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%.*s", (int)strlen(directory) - 1, directory);
    assert(rmdir(path) == 0);
}

static void test_watchdog_socket_connect(void)
{
    struct sockaddr_un address;
    char path[sizeof(address.sun_path)];
    int listener;
    int client;
    int server;

    snprintf(path, sizeof(path), "/tmp/qb-wdt-test-%ld.sock", (long)getpid());
    unlink(path);
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);

    client = qb_connect_unix_socket(path, 1000);
    assert(client >= 0);
    server = accept(listener, NULL, NULL);
    assert(server >= 0);
    assert(send(client, "HEARTBEAT", 9, 0) == 9);

    close(server);
    close(client);
    close(listener);
    assert(unlink(path) == 0);
    assert(qb_connect_unix_socket(path, 10) == -1);
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
    cm.full_voltage_mv = 4400;
    snprintf(path, sizeof(path), "/tmp/qb-config-test-%ld", (long)getpid());
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("config ignored 'other'\n"
          "\toption max_current_ma '1'\n"
          "config battery 'settings'\n"
          "\toption max_current_ma '5100'\n"
          "\toption min_shutdown_mv \"3350\"\n"
          "\toption max_pd_vbus_mv '9600'\n"
          "\toption charge_limit_mv '4057'\n"
          "\toption charge_limit_percent '80'\n", fp);
    fclose(fp);
    assert(qb_load_config_file(&cm, path) == 0);
    unlink(path);
    assert(cm.max_current_ma == 5100);
    assert(cm.min_shutdown_mv == QB_STOCK_MIN_SHUTDOWN_MV);
    assert(cm.max_pd_vbus_mv == 9600);
    assert(cm.charge_limit_mv == 4050);
    assert(cm.full_voltage_mv == 4050);
    assert(cm.charge_limit_percent == 80);
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
    cm.full_voltage_mv = 4400;
    snprintf(path, sizeof(path), "/tmp/qb-config-bounds-%ld", (long)getpid());
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("config battery 'settings'\n"
          "\toption max_current_ma '6000'\n"
          "\toption min_shutdown_mv '5000'\n"
          "\toption max_pd_vbus_mv '12000'\n"
          "\toption charge_limit_mv '5000'\n"
          "\toption charge_limit_percent '120'\n", fp);
    fclose(fp);
    assert(qb_load_config_file(&cm, path) == 0);
    assert(cm.max_current_ma == QB_STOCK_MAX_CURRENT_MA);
    assert(cm.min_shutdown_mv == QB_SAFE_MAX_SHUTDOWN_MV);
    assert(cm.max_pd_vbus_mv == QB_STOCK_MAX_PPS_VOLTAGE_MV);
    assert(cm.charge_limit_mv == QB_MAX_CHARGE_LIMIT_MV);
    assert(cm.full_voltage_mv == QB_MAX_CHARGE_LIMIT_MV);
    assert(cm.charge_limit_percent == 100);

    cm.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
    cm.min_shutdown_mv = QB_STOCK_MIN_SHUTDOWN_MV;
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.charge_limit_mv = 0;
    cm.charge_limit_percent = 0;
    cm.full_voltage_mv = 4400;
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("config battery 'settings'\n"
          "\toption max_current_ma '0'\n"
          "\toption min_shutdown_mv 'invalid'\n"
          "\toption max_pd_vbus_mv '5000'\n"
          "\toption charge_limit_mv '-1'\n"
          "\toption charge_limit_percent '-1'\n", fp);
    fclose(fp);
    assert(qb_load_config_file(&cm, path) == 0);
    unlink(path);
    assert(cm.max_current_ma == 0);
    assert(cm.min_shutdown_mv == QB_STOCK_MIN_SHUTDOWN_MV);
    assert(cm.max_pd_vbus_mv == 5000);
    assert(cm.charge_limit_mv == 0);
    assert(cm.full_voltage_mv == 4400);
    assert(cm.charge_limit_percent == 0);
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
    policy.full_voltage_mv = 4400;
    policy.charge_current_ma = QB_STOCK_MAX_CURRENT_MA;
    policy.pps_voltage_mv = 9000;
    policy.pump.vbat_adc_mv = 4050;
    policy.pump.ibat_adc_ma = 1000;
    policy.pump.ibus_adc_ma = 1;
    qb_pump_pps_control(&policy);
    assert(policy.pps_voltage_mv == 9000);

}

static void test_capacity_charge_limit(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.max_current_ma = QB_STOCK_MAX_CURRENT_MA;
    cm.charge_limit_mv = 4000;
    cm.charge_limit_percent = 80;
    cm.battery.presence = QB_BATTERY_PRESENT;
    cm.battery.telemetry_valid = true;
    cm.battery.capacity = 80;
    cm.battery.temp_decic = 200;
    cm.buck.vbat_adc_mv = 3900;

    cm.charge_current_ma = 1000;
    assert(!qb_update_capacity_charge_limit(&cm));
    assert(!cm.capacity_charge_hold);
    assert(!qb_update_capacity_charge_limit(&cm));
    assert(!cm.capacity_charge_hold);
    assert(qb_update_capacity_charge_limit(&cm));
    assert(cm.capacity_charge_hold);
    assert(cm.charge_current_ma == 0);

    cm.battery.capacity = 78;
    cm.charge_current_ma = 1000;
    assert(!qb_update_capacity_charge_limit(&cm));
    assert(cm.capacity_charge_hold && cm.charge_current_ma == 0);
    cm.battery.capacity = 77;
    cm.charge_current_ma = 1000;
    assert(!qb_update_capacity_charge_limit(&cm));
    cm.charge_current_ma = 1000;
    assert(!qb_update_capacity_charge_limit(&cm));
    cm.charge_current_ma = 1000;
    assert(qb_update_capacity_charge_limit(&cm));
    assert(!cm.capacity_charge_hold);
    assert(cm.charge_current_ma == 1000);

    cm.battery.capacity = 80;
    assert(!qb_update_capacity_charge_limit(&cm));
    cm.battery.capacity = 79;
    assert(!qb_update_capacity_charge_limit(&cm));
    assert(cm.capacity_stop_samples == 0);

    cm.capacity_charge_hold = true;
    cm.battery.presence = QB_BATTERY_UNKNOWN;
    cm.charge_current_ma = 1000;
    assert(!qb_update_capacity_charge_limit(&cm));
    assert(cm.capacity_charge_hold && cm.charge_current_ma == 0);
    cm.battery.presence = QB_BATTERY_ABSENT;
    assert(qb_update_capacity_charge_limit(&cm));
    assert(!cm.capacity_charge_hold);

    cm.battery.presence = QB_BATTERY_PRESENT;
    cm.capacity_charge_hold = true;
    cm.charge_limit_percent = 0;
    assert(qb_update_capacity_charge_limit(&cm));
    assert(!cm.capacity_charge_hold);

    cm.charge_limit_percent = 80;
    cm.capacity_charge_hold = true;
    cm.battery.capacity = 80;
    qb_power_limit_state = 0;
    qb_update_charge_limits(&cm);
    assert(cm.full_voltage_mv == 4000);
    assert(cm.charge_current_ma == QB_STOCK_MAX_CURRENT_MA);
    assert(!qb_update_capacity_charge_limit(&cm));
    assert(cm.charge_current_ma == 0);
}

static void test_charge_voltage_limit(void)
{
    struct qb_manager cm;
    unsigned bat_ovp;
    unsigned regulation;
    unsigned first_register;
    unsigned second_register;
    unsigned long long irq_count;
    char directory[96];
    char path[128];
    char value[64];
    FILE *fp;

    memset(&cm, 0, sizeof(cm));
    assert(qb_charge_voltage_limit(&cm, 4400) == 4400);
    cm.charge_limit_mv = 4000;
    assert(qb_charge_voltage_limit(&cm, 4400) == 4000);
    assert(qb_charge_voltage_limit(&cm, 3900) == 3900);

    assert(qb_sgm41600_voltage_registers(3800, &bat_ovp,
                                         &regulation) == 3800);
    assert(bat_ovp == 0x80 && regulation == 0x47);
    assert(qb_sgm41600_voltage_registers(4000, &bat_ovp,
                                         &regulation) == 4000);
    assert(bat_ovp == 0x82 && regulation == 0x44);
    assert(qb_sgm41600_voltage_registers(4180, &bat_ovp,
                                         &regulation) == 4175);
    assert(bat_ovp == 0x89 && regulation == 0x44);
    assert(qb_sgm41600_voltage_registers(4200, &bat_ovp,
                                         &regulation) == 4200);
    assert(bat_ovp == 0x8a && regulation == 0x44);
    assert(qb_sgm41600_voltage_registers(4250, &bat_ovp,
                                         &regulation) == 4250);
    assert(bat_ovp == 0x8c && regulation == 0x44);
    assert(qb_sgm41600_voltage_registers(4300, &bat_ovp,
                                         &regulation) == 4300);
    assert(bat_ovp == 0x8e && regulation == 0x44);
    assert(qb_sgm41600_voltage_registers(4350, &bat_ovp,
                                         &regulation) == 4350);
    assert(bat_ovp == 0x90 && regulation == 0x44);
    assert(qb_sgm41600_voltage_registers(4351, &bat_ovp,
                                         &regulation) == -1);
    assert(qb_sgm41600_voltage_registers(3799, &bat_ovp,
                                         &regulation) == -1);

    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.full_voltage_mv = 4000;
    cm.charge_current_ma = QB_STOCK_MAX_CURRENT_MA;
    cm.pps_voltage_mv = 9000;
    cm.pump.vbat_adc_mv = 4000;
    cm.pump.ibat_adc_ma = 1000;
    cm.pump.ibus_adc_ma = 1;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8800);
    cm.pump.vbat_adc_mv = 3950;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8800);

    snprintf(directory, sizeof(directory), "/tmp/qb-register-test-%ld/",
             (long)getpid());
    assert(mkdir(directory, 0700) == 0);
    snprintf(path, sizeof(path), "%sregisters", directory);
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("Reg[04] = 0x88\nReg[05] = 0xaf\n", fp);
    fclose(fp);
    assert(qb_read_register_pair(directory, 0x04, &first_register,
                                 0x05, &second_register) == 0);
    assert(first_register == 0x88 && second_register == 0xaf);
    assert(qb_update_register(directory, 0x05, 0x80, 0) == 0);
    assert(qb_read_str(directory, "registers", value, sizeof(value)) > 0);
    assert(strcmp(value, "0x05 0x2f") == 0);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%sinterrupts", directory);
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("151: 7 3 0 0 msmgpio 19 Edge 2-006f\n", fp);
    fclose(fp);
    assert(qb_read_irq_count(path, QB_SGM41600_IRQ_LABEL,
                             &irq_count) == 0);
    assert(irq_count == 10);
    assert(qb_read_irq_count(path, "missing-device", &irq_count) == -1);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
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
        {450, QB_TEMP_HOT, 4180, 1060},
        {599, QB_TEMP_HOT, 4180, 1060},
        {600, QB_TEMP_OVERHEAT, 4180, 0},
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

        cm.battery.cycle_count = 600;
        cm.battery.temp_decic = 200;
        qb_init_temp_status(&cm);
        assert(cm.full_voltage_mv == 4250);
        cm.battery.temp_decic = 500;
        qb_init_temp_status(&cm);
        assert(cm.full_voltage_mv == 4180);
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
                    got.full_voltage_mv = 4400;
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
                    bounded_pps_control_reference(&expected);
                    assert(got.pps_voltage_mv == expected.pps_voltage_mv);
                    assert(got.pump_error_count == expected.pump_error_count);
                    assert(got.pump_error == expected.pump_error);
                }
            }
        }
    }
}

static void test_bounded_pps_ceiling(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.charge_limit_mv = 4100;
    cm.full_voltage_mv = 4100;
    cm.charge_current_ma = 5300;
    assert(qb_pump_control_target_mv(&cm) == 4100);
    assert(qb_pump_regulation_target_mv(&cm) == 4150);
    cm.pump.vbat_adc_mv = 4022;
    cm.pump.ibat_adc_ma = 0;
    cm.pump.ibus_adc_ma = 1;
    assert(qb_pump_start_voltage_mv(&cm, 4022) == 8848);
    assert(qb_pump_entry_voltage_ok(&cm, 3999));
    assert(!qb_pump_entry_voltage_ok(&cm, 4000));
    assert(!qb_pump_entry_voltage_ok(&cm, 4060));

    cm.pps_voltage_mv = 9369;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 9020);

    cm.pps_voltage_mv = 8920;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8940);

    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8960);
    cm.pump.vbat_adc_mv = 4075;
    cm.pps_voltage_mv = 8500;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8500);
    cm.pump.vbat_adc_mv = 4100;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8500);
    cm.pump.vbat_adc_mv = 4074;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8520);
    cm.pump.vbat_adc_mv = 4101;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8500);

    cm.pump.vbat_adc_mv = 3999;
    cm.pps_voltage_mv = 8800;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8900);

    cm.pump.vbat_adc_mv = 4100;
    cm.pps_voltage_mv = 8900;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8900);

    cm.pump.vbat_adc_mv = 4200;
    cm.pps_voltage_mv = 9000;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 8900);

    cm.pump.vbat_adc_mv = 4101;
    cm.pump.ibat_adc_ma = QB_PUMP_HANDOFF_CURRENT_MA;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 1);
    cm.pump.ibat_adc_ma = QB_PUMP_HANDOFF_CURRENT_MA + 1;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);

    memset(&cm, 0, sizeof(cm));
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.full_voltage_mv = 4400;
    cm.charge_current_ma = 5300;
    cm.pump.vbat_adc_mv = 4000;
    cm.pump.ibat_adc_ma = 0;
    cm.pump.ibus_adc_ma = 1;
    assert(qb_pump_target_mv(&cm) == 4300);
    assert(qb_pump_regulation_target_mv(&cm) == 4350);
    assert(qb_pump_entry_voltage_ok(&cm, 4199));
    assert(!qb_pump_entry_voltage_ok(&cm, 4200));
    cm.pps_voltage_mv = 9500;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 9460);
    assert(qb_pump_regulation_retreat(&cm));
    assert(cm.pps_voltage_mv == 9440);
    assert(cm.pump_pps_ceiling_mv == 9440);
    assert(cm.pump_regulation_steps == 1);
    assert(cm.pump_regulation_retreating);
    cm.pump.vbat_adc_mv = 4000;
    qb_pump_pps_control(&cm);
    assert(cm.pps_voltage_mv == 9440);
    assert(qb_pump_regulation_retreat(&cm));
    assert(qb_pump_regulation_retreat(&cm));
    assert(qb_pump_regulation_retreat(&cm));
    assert(qb_pump_regulation_retreat(&cm));
    assert(cm.pps_voltage_mv == 9360);
    assert(!qb_pump_regulation_retreat(&cm));
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
    assert(cm.fixed_charge_current_ma == 0);
    cm.fixed_charge_current_ma = -100;
    cm.buck.vbus_adc_mv = 4500;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 0);
    cm.charge_current_ma = 0;
    cm.fixed_charge_current_ma = 300;
    cm.buck.vbus_adc_mv = 4801;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 0);

    memset(&cm, 0, sizeof(cm));
    cm.charge_limit_mv = 4050;
    cm.charge_current_ma = 3710;
    cm.pdb.pump_handoff_complete = true;
    cm.fixed_charge_current_ma = 900;
    cm.buck.vbus_adc_mv = 9000;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 1100);
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 1300);
    cm.charge_current_ma = 800;
    qb_fixed_charge_control(&cm);
    assert(cm.fixed_charge_current_ma == 800);
}

static void test_battery_presence_policy(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.battery.presence = qb_battery_presence_from_raw(-1);
    assert(!qb_battery_present(&cm));
    assert(!qb_battery_absent(&cm));

    cm.battery.presence = qb_battery_presence_from_raw(0);
    assert(!qb_battery_present(&cm));
    assert(qb_battery_absent(&cm));

    cm.battery.presence = qb_battery_presence_from_raw(1);
    assert(qb_battery_present(&cm));
    assert(!qb_battery_absent(&cm));

    cm.battery.presence = qb_battery_presence_from_raw(2);
    assert(!qb_battery_present(&cm));
    assert(!qb_battery_absent(&cm));
}

static void test_pump_eligibility(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.battery.presence = QB_BATTERY_PRESENT;
    cm.battery.telemetry_valid = true;
    cm.pump.telemetry_valid = true;
    cm.pdb.telemetry_valid = true;
    cm.pdb.supports_pps = true;
    cm.pdb.pps_min_voltage_mv = QB_PPS_MIN_VOLTAGE_MV;
    cm.charge_current_ma = QB_STOCK_MAX_CURRENT_MA;
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.full_voltage_mv = 4400;
    cm.temp_status = QB_TEMP_NORMAL;

    assert(qb_pump_allowed(&cm, &cm.pdb, 3700));
    assert(qb_pump_entry_voltage_ok(&cm, 4199));
    assert(!qb_pump_entry_voltage_ok(&cm, 4200));
    cm.max_pd_vbus_mv = 0;
    assert(!qb_pump_allowed(&cm, &cm.pdb, 3700));
    cm.max_pd_vbus_mv = QB_STOCK_MAX_PPS_VOLTAGE_MV;
    cm.pump_error = true;
    assert(!qb_pump_allowed(&cm, &cm.pdb, 3700));
    cm.pump_error = false;
    cm.pdb.pump_handoff_complete = true;
    assert(!qb_pump_allowed(&cm, &cm.pdb, 3700));
}

static void test_pump_handoff_policy(void)
{
    struct qb_manager cm;

    memset(&cm, 0, sizeof(cm));
    cm.charge_limit_mv = 4050;
    cm.full_voltage_mv = 4050;
    cm.pump.ibat_adc_ma = 5300;
    cm.pump.vbat_adc_mv = 4024;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);
    cm.pump.vbat_adc_mv = 4025;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);

    cm.pump.ibat_adc_ma = 1501;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);
    cm.pump.ibat_adc_ma = 1500;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 1);
    assert(qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);

    cm.pump.ibat_adc_ma = 5300;
    cm.pump.vbat_adc_mv = 4050;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    cm.pump.vbat_adc_mv = 4100;
    assert(qb_pump_handoff_ready(&cm, &cm.pdb));

    cm.charge_limit_mv = 0;
    cm.full_voltage_mv = 4400;
    cm.pump.vbat_adc_mv = 4275;
    cm.pump.ibat_adc_ma = 1501;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);
    cm.pump.ibat_adc_ma = 1500;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 1);
    assert(qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(cm.pdb.pump_handoff_samples == 0);

    cm.charge_limit_mv = 4200;
    cm.full_voltage_mv = 4200;
    cm.pump.vbat_adc_mv = 4175;
    cm.pump.ibat_adc_ma = 1501;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    cm.pump.ibat_adc_ma = 1500;
    assert(!qb_pump_handoff_ready(&cm, &cm.pdb));
    assert(qb_pump_handoff_ready(&cm, &cm.pdb));
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
    test_buck_fixed_pdo_selection();
    test_pd_inventory();
    test_pdo_request_safety();
    test_event_queue();
    test_config_loader();
    test_config_bounds_and_helpers();
    test_sysfs_integer_validation();
    test_watchdog_socket_connect();
    test_battery_presence_policy();
    test_capacity_charge_limit();
    test_charge_voltage_limit();
    test_initial_temperature_policy();
    test_temperature_policy_matrix();
    test_pps_policy_matrix();
    test_bounded_pps_ceiling();
    test_fixed_charge_ramp();
    test_pump_eligibility();
    test_pump_handoff_policy();
    test_charge_state_flags();
    puts("behavior regression suite passed");
    return 0;
}
