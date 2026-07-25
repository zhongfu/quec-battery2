#include "qb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int qb_get_sgm41542_info(struct qb_manager *cm)
{
    struct qb_sgm41542 next = cm->buck;
    struct stat st;
    char pair[64];
    int debug_value;

    if (qb_read_int(QB_SGM41542_PATH, "charge_en", &next.charge_en) < 0 ||
        qb_read_int(QB_SGM41542_PATH, "ichrg_curr", &next.ichrg_curr_ua) < 0 ||
        qb_read_int(QB_SGM41542_PATH, "vreg", &next.vreg_uv) < 0 ||
        qb_read_int(QB_SGM41542_PATH, "vbus_adc", &next.vbus_adc_mv) < 0 ||
        qb_read_int(QB_SGM41542_PATH, "ibus_adc", &next.ibus_adc_ma) < 0 ||
        qb_read_int(QB_SGM41542_PATH, "vbat_adc", &next.vbat_adc_mv) < 0 ||
        qb_read_int(QB_SGM41542_PATH, "ibat_adc", &next.ibat_adc_ma) < 0 ||
        qb_read_str(QB_SGM41542_PATH, "vbus_ovp_vindpm",
                    pair, sizeof(pair)) < 0 ||
        sscanf(pair, "vbus_ovp_uv:%d; vindpm_uv: %d",
               &next.vbus_ovp_uv, &next.vindpm_uv) != 2)
        goto invalid;

    if (!stat("/var/dbg_vbat", &st)) {
        if (qb_read_int("/var/", "dbg_vbat", &debug_value) < 0)
            goto invalid;
        next.vbat_adc_mv = debug_value;
    }
    if (!stat("/var/dbg_ibat", &st)) {
        if (qb_read_int("/var/", "dbg_ibat", &debug_value) < 0)
            goto invalid;
        next.ibat_adc_ma = debug_value;
    }

    next.telemetry_valid = true;
    next.telemetry_failures = 0;
    cm->buck = next;
    return 0;

invalid:
    cm->buck.telemetry_valid = false;
    cm->buck.telemetry_failures++;
    return -1;
}

int qb_get_sgm41600_info(struct qb_manager *cm)
{
    struct qb_sgm41600 next = cm->pump;
    struct stat st;
    int debug_value;

    if (qb_read_int(QB_SGM41600_PATH, "charge_en", &next.charge_en) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "vbus_ocp_ua", &next.vbus_ocp_ua) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "vbus_ovp_uv", &next.vbus_ovp_uv) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "bat_ovp_uv", &next.bat_ovp_uv) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "bat_ocp_ua", &next.bat_ocp_ua) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "vbus_adc", &next.vbus_adc_mv) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "ibus_adc", &next.ibus_adc_ma) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "vbat_adc", &next.vbat_adc_mv) < 0 ||
        qb_read_int(QB_SGM41600_PATH, "ibat_adc", &next.ibat_adc_ma) < 0)
        goto invalid;

    if (!stat("/var/dbg_vbat", &st)) {
        if (qb_read_int("/var/", "dbg_vbat", &debug_value) < 0)
            goto invalid;
        next.vbat_adc_mv = debug_value;
    }
    if (!stat("/var/dbg_ibat", &st)) {
        if (qb_read_int("/var/", "dbg_ibat", &debug_value) < 0)
            goto invalid;
        next.ibat_adc_ma = debug_value;
    }

    next.telemetry_valid = true;
    next.telemetry_failures = 0;
    cm->pump = next;
    return 0;

invalid:
    cm->pump.telemetry_valid = false;
    cm->pump.telemetry_failures++;
    return -1;
}

int qb_get_battery_info(struct qb_manager *cm)
{
    struct qb_battery next = cm->battery;
    struct stat st;
    int debug_value;

    if (qb_read_int(QB_BATTERY_PATH, "capacity", &next.capacity) < 0 ||
        qb_read_int(QB_BATTERY_PATH, "cycle_count", &next.cycle_count) < 0 ||
        qb_read_int(QB_BATTERY_PATH, "voltage_now", &next.voltage_mv) < 0 ||
        qb_read_int(QB_BATTERY_PATH, "current_now", &next.current_ma) < 0 ||
        qb_read_int(QB_BATTERY_PATH, "temp", &next.temp_decic) < 0 ||
        qb_read_str(QB_BATTERY_PATH, "health",
                    next.health, sizeof(next.health)) < 0)
        goto invalid;

    if (!stat("/var/dbg_vbat", &st)) {
        if (qb_read_int("/var/", "dbg_vbat", &debug_value) < 0)
            goto invalid;
        next.voltage_mv = debug_value;
    }
    if (!stat("/var/dbg_temp", &st)) {
        if (qb_read_int("/var/", "dbg_temp", &debug_value) < 0)
            goto invalid;
        next.temp_decic = debug_value;
    }

    next.telemetry_valid = true;
    next.telemetry_failures = 0;
    cm->battery = next;
    return 0;

invalid:
    cm->battery.telemetry_valid = false;
    cm->battery.telemetry_failures++;
    return -1;
}

void qb_get_battery_online(struct qb_manager *cm)
{
    int present = cm->battery.present;

    qb_read_int(QB_BATTERY_PATH, "present", &present);
    cm->battery.present = present;
    if (cm->battery.present == 0) {
        cm->buck_input_current_ua = 3000000;
        qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
        cm->battery.temp_decic = 240;
        cm->temp_status = QB_TEMP_NORMAL;
        qb_set_pwm(true);
    } else {
        qb_set_pwm(false);
    }
}

int qb_set_sgm41542(struct qb_manager *cm, const char *attr, const char *value)
{
    int rc = qb_write_str(QB_SGM41542_PATH, attr, value);
    qb_get_sgm41542_info(cm);
    return rc;
}

int qb_set_sgm41542_int(struct qb_manager *cm, const char *attr, int value)
{
    int rc = qb_write_int(QB_SGM41542_PATH, attr, value);
    qb_get_sgm41542_info(cm);
    return rc;
}

int qb_set_sgm41600(struct qb_manager *cm, const char *attr, const char *value)
{
    int rc = qb_write_str(QB_SGM41600_PATH, attr, value);
    qb_get_sgm41600_info(cm);
    return rc;
}

void qb_ovp_on(struct qb_manager *cm, int port)
{
    qb_write_str(QB_SGM41542_PATH, port ? "ovp2_pin" : "ovp1_pin", "0");
    cm->ovp_status[port != 0] = true;
}

void qb_ovp_off(struct qb_manager *cm, int port)
{
    qb_write_str(QB_SGM41542_PATH, port ? "ovp2_pin" : "ovp1_pin", "1");
    cm->ovp_status[port != 0] = false;
}

static bool qb_adapter_online(struct qb_manager *cm)
{
    char value[32] = {0};
    bool online = false;

    qb_read_str(cm->pda.path, "cc_pin", value, sizeof(value));
    if (strcmp(value, "None")) {
        qb_read_str(cm->pda.path, "pwr_role", value, sizeof(value));
        online = !strcmp(value, "Sink");
    }

    qb_read_str(cm->pdb.path, "cc_pin", value, sizeof(value));
    if (strcmp(value, "None")) {
        qb_read_str(cm->pdb.path, "pwr_role", value, sizeof(value));
        if (!strcmp(value, "Sink"))
            online = true;
    }
    return online;
}

void qb_mos_on(struct qb_manager *cm, int port)
{
    bool adapter_online;
    bool temperature_bad;
    bool capacity_bad;
    bool power_limited;

    qb_get_battery_online(cm);
    adapter_online = qb_adapter_online(cm);
    temperature_bad = cm->battery.present == 1 &&
                      (cm->battery.temp_decic <= -100 ||
                       cm->battery.temp_decic >= 600);
    capacity_bad = cm->battery.present == 1 && cm->battery.capacity < 1 &&
                   !adapter_online;
    power_limited = cm->battery.present == 1 && cm->power_limit;

    if (temperature_bad) {
        qb_write_str(QB_SGM41542_PATH, port ? "mos2_pin" : "mos1_pin", "1");
        return;
    }
    if (capacity_bad) {
        qb_write_str(QB_SGM41542_PATH, "otg_pin", "0");
        qb_write_str(QB_SGM41542_PATH, "mos1_pin", "1");
        return;
    }
    if (power_limited) {
        if (!port)
            qb_write_str(QB_SGM41542_PATH, "otg_pin", "0");
        qb_write_str(QB_SGM41542_PATH, port ? "mos2_pin" : "mos1_pin", "1");
        return;
    }

    qb_write_str(QB_SGM41542_PATH, port ? "mos2_pin" : "mos1_pin", "0");
    qb_write_str(QB_SGM41542_PATH, "otg_pin", "1");
    cm->mos_status[port != 0] = true;
}

void qb_mos_off(struct qb_manager *cm, int port)
{
    qb_write_str(QB_SGM41542_PATH, port ? "mos2_pin" : "mos1_pin", "1");
    cm->mos_status[port != 0] = false;
    if (!cm->mos_status[0] && !cm->mos_status[1])
        qb_write_str(QB_SGM41542_PATH, "otg_pin", "0");
}

void qb_disable_buck(struct qb_manager *cm)
{
    if (!cm->dead_battery_restore)
        qb_set_sgm41542(cm, "charge_en", "0");
}

void qb_disable_pump(struct qb_manager *cm)
{
    cm->pump.working = false;
    cm->pump.charge_status = 0;
    qb_set_sgm41600(cm, "charge_en", "0");
}

void qb_enable_buck_cfg(struct qb_manager *cm, struct qb_pd_port *port)
{
    qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
    cm->pump.working = false;
    cm->pump.charge_status = 0;
    cm->buck.working = true;
    cm->buck.charge_status = 1;
    cm->pda.working = port == &cm->pda;
    cm->pdb.working = port == &cm->pdb;
}

void qb_disable_buck_cfg(struct qb_manager *cm, struct qb_pd_port *port)
{
    (void)port;
    cm->buck.working = false;
    cm->buck.charge_status = 0;
    cm->pda.working = false;
    cm->pdb.working = false;
}

void qb_enable_pump_cfg(struct qb_manager *cm, struct qb_pd_port *port)
{
    cm->pump.working = true;
    cm->pump.charge_status = 1;
    cm->buck.working = false;
    cm->buck.charge_status = 0;
    cm->pda.working = port == &cm->pda;
    cm->pdb.working = port == &cm->pdb;
}

void qb_disable_pump_cfg(struct qb_manager *cm, struct qb_pd_port *port)
{
    cm->pump.working = false;
    cm->pump.charge_status = 0;
    port->working = false;
}

void qb_enable_buck(struct qb_manager *cm, struct qb_pd_port *port)
{
    int voltage_mv = 5000;

    cm->buck_charge_current_ua = 300000;
    if (cm->battery.present == 0 && cm->pda.power_role == QB_ROLE_SINK &&
        cm->pdb.power_role == QB_ROLE_SINK)
        qb_ovp_on(cm, port == &cm->pda ? 1 : 0);
    else
        qb_ovp_off(cm, port == &cm->pda ? 1 : 0);
    qb_ovp_on(cm, port == &cm->pda ? 0 : 1);

    if (port == &cm->pda) {
        if (port->fixed_12v) {
            voltage_mv = 12000;
            qb_request_pdo(port, 12000, port->fixed_12v_current_ma);
            cm->buck_input_current_ua = 1500000;
            cm->buck_charge_current_ua = cm->charge_current_ma;
            qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
        } else if (port->fixed_9v) {
            voltage_mv = 9000;
            qb_request_pdo(port, 9000, port->fixed_9v_current_ma);
            cm->buck_input_current_ua = 2000000;
            qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
        } else {
            qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
            cm->buck_input_current_ua = 2000000;
            qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
        }
    } else {
        qb_read_int(QB_SGM41542_PATH, "vbus_adc", &cm->buck.vbus_adc_mv);
        if (port->fixed_12v) {
            voltage_mv = 12000;
            qb_request_pdo(port, 12000, port->fixed_12v_current_ma);
            cm->buck_input_current_ua = 1500000;
            qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
        } else if (port->fixed_9v) {
            voltage_mv = 9000;
            qb_request_pdo(port, 9000, port->fixed_9v_current_ma);
            cm->buck_input_current_ua = 2000000;
            qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
        } else if (cm->qc_max_voltage_mv == 12000) {
            voltage_mv = 12000;
            if (cm->buck.vbus_adc_mv < 11000) {
                qb_set_sgm41542(cm, "qc_volt", "1 5");
                usleep(1000000);
                qb_set_sgm41542(cm, "qc_volt", "3 12");
                cm->buck_input_current_ua = 1500000;
                qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
            }
        } else if (cm->qc_max_voltage_mv == 9000) {
            voltage_mv = 9000;
            if (cm->buck.vbus_adc_mv < 8000) {
                qb_set_sgm41542(cm, "qc_volt", "1 5");
                usleep(1000000);
                qb_set_sgm41542(cm, "qc_volt", "3 9");
                cm->buck_input_current_ua = 2000000;
                qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
            }
        } else {
            qb_set_sgm41542(cm, "qc_volt", "3 5");
            cm->buck_input_current_ua = 2000000;
            qb_set_sgm41542_int(cm, "ibus_iindpm", cm->buck_input_current_ua);
            qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
        }
    }

    if (cm->battery.present == 1) {
        qb_set_sgm41542_int(cm, "ichrg_curr", 300000);
        qb_set_sgm41542(cm, "charge_en", "1");
    }
    QBLOG(0x4eb, "%s out volte_mv:%d\n", port->name, voltage_mv);
}

void qb_set_pwm(bool enabled)
{
    int current = -1;
    qb_read_int(QB_SGM41542_PATH, "pwm", &current);
    if (current != (int)enabled)
        qb_write_str(QB_SGM41542_PATH, "pwm", enabled ? "1" : "0");
}
