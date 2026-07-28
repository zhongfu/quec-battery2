#include "qb.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool qb_pump_allowed(struct qb_manager *cm, struct qb_pd_port *port, int vbat_mv)
{
    return qb_battery_present(cm) && cm->pump.telemetry_valid &&
           port->telemetry_valid && cm->battery.telemetry_valid &&
           cm->charge_current_ma > 0 && !cm->pump_error &&
           !port->pump_handoff_complete && qb_pps_enabled(cm) &&
           qb_pump_entry_voltage_ok(cm, vbat_mv) &&
           port->supports_pps &&
           port->pps_min_voltage_mv <= cm->max_pd_vbus_mv &&
           cm->temp_status >= QB_TEMP_NORMAL &&
           cm->temp_status <= QB_TEMP_WARM;
}

void qb_no_charge(struct qb_manager *cm)
{
    cm->pump.working = false;
    cm->buck.working = false;
    cm->pump.charge_status = 0;
    cm->buck.charge_status = 0;
    cm->pda.working = false;
    cm->pdb.working = false;
}

bool qb_pump_entry_voltage_ok(const struct qb_manager *cm, int battery_mv)
{
    return battery_mv <
           qb_pump_target_mv(cm) - QB_PUMP_LIMIT_ENTRY_MARGIN_MV;
}

int qb_pump_start_voltage_mv(const struct qb_manager *cm, int battery_mv)
{
    return qb_limit_pps_voltage(
        cm, battery_mv * QB_PUMP_START_RATIO_PERCENT / 100);
}

int qb_pump_target_mv(const struct qb_manager *cm)
{
    if (cm->full_voltage_mv > 0 &&
        cm->full_voltage_mv < QB_STOCK_PD_FULL_MV)
        return cm->full_voltage_mv;
    return QB_STOCK_PD_FULL_MV;
}
int qb_pump_control_target_mv(const struct qb_manager *cm)
{
    return qb_pump_target_mv(cm);
}
int qb_pump_regulation_target_mv(const struct qb_manager *cm)
{
    int target_mv =
        qb_pump_target_mv(cm) + QB_PUMP_REGULATION_HEADROOM_MV;

    return target_mv < QB_PUMP_MAX_REGULATION_MV ?
           target_mv : QB_PUMP_MAX_REGULATION_MV;
}

bool qb_pump_handoff_ready(struct qb_manager *cm, struct qb_pd_port *port)
{
    int vbat = cm->pump.vbat_adc_mv;
    int target_mv = qb_pump_target_mv(cm);
    int handoff_current_ma =
        cm->full_voltage_mv > QB_STOCK_PD_FULL_MV ?
        QB_PUMP_HIGH_TARGET_HANDOFF_CURRENT_MA :
        QB_PUMP_HANDOFF_CURRENT_MA;

    if (vbat >= target_mv + QB_PUMP_HANDOFF_OVERSHOOT_MV) {
        port->pump_handoff_samples = 0;
        return true;
    }
    if (vbat < target_mv - QB_PUMP_CV_LOWER_MARGIN_MV ||
        cm->pump.ibat_adc_ma > handoff_current_ma) {
        port->pump_handoff_samples = 0;
        return false;
    }
    if (++port->pump_handoff_samples < QB_PUMP_HANDOFF_SAMPLES)
        return false;
    port->pump_handoff_samples = 0;
    return true;
}

void qb_pump_pps_control(struct qb_manager *cm)
{
    int vbat = cm->pump.vbat_adc_mv;
    int target_mv = qb_pump_control_target_mv(cm);
    int next;
    int adjustment_mv;
    bool near_limit =
        vbat >= target_mv - QB_PUMP_LIMIT_FINE_WINDOW_MV &&
        vbat < target_mv + QB_PUMP_LIMIT_FINE_WINDOW_MV;
    int bounded_ceiling_mv =
        target_mv * QB_PUMP_LIMIT_RATIO_PERCENT / 100;

    if (vbat < target_mv - QB_PUMP_CV_LOWER_MARGIN_MV &&
        cm->pump.ibat_adc_ma < cm->charge_current_ma - 300) {
        next = cm->pps_voltage_mv +
               (near_limit ? QB_PPS_VOLTAGE_STEP_MV : 100);
        if (next >= vbat * 235 / 100)
            next = cm->pps_voltage_mv;
        if (bounded_ceiling_mv && next > bounded_ceiling_mv)
            next = bounded_ceiling_mv;
        next = qb_limit_pps_voltage(cm, next);
        cm->pps_voltage_mv = next;
    }

    if (vbat > target_mv ||
        cm->pump.ibat_adc_ma > cm->charge_current_ma || vbat > 4300) {
        adjustment_mv = 50;
        if (near_limit)
            adjustment_mv = QB_PPS_VOLTAGE_STEP_MV;
        else if (vbat > target_mv)
            adjustment_mv = 100;
        next = cm->pps_voltage_mv - adjustment_mv;
        if (next <= vbat * 202 / 100)
            next = cm->pps_voltage_mv;
        next = qb_limit_pps_voltage(cm, next);
        cm->pps_voltage_mv = next;
    }
    if (bounded_ceiling_mv && cm->pps_voltage_mv > bounded_ceiling_mv)
        cm->pps_voltage_mv =
            qb_limit_pps_voltage(cm, bounded_ceiling_mv);

    if (!cm->pump.ibus_adc_ma) {
        cm->pump_error_count++;
        QBLOG(0x56b, "sgm41600 charge error count :%d\n", cm->pump_error_count);
        if (cm->pump_error_count > 10) {
            cm->pump_error = true;
            QBLOG(0x571, "%s", "sgm41600 charge error\n");
            return;
        }
    } else {
        cm->pump_error_count = 0;
    }
    cm->pump_error = false;
}

void qb_fixed_charge_control(struct qb_manager *cm)
{
    int vbus = cm->buck.vbus_adc_mv;
    int current_ceiling_ma =
        cm->charge_current_ma > 0 ? cm->charge_current_ma : 0;

    if (!vbus)
        return;
    if (vbus >= 10501) {
        qb_set_sgm41542_int(cm, "ibus_iindpm", 1500000);
        qb_set_sgm41542_int(cm, "vbus_vindpm", 10500000);
    } else if (vbus >= 7001) {
        qb_set_sgm41542_int(cm, "ibus_iindpm", 2000000);
        qb_set_sgm41542_int(cm, "vbus_vindpm", 7500000);
    } else if (vbus >= 4601) {
        qb_set_sgm41542_int(cm, "ibus_iindpm", 2000000);
        qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
    }

    if (vbus < 4300) {
        if (cm->fixed_charge_current_ma > 200)
            cm->fixed_charge_current_ma -= 200;
        else
            cm->fixed_charge_current_ma = 0;
    } else if (vbus >= 4801 &&
               cm->fixed_charge_current_ma < current_ceiling_ma) {
        cm->fixed_charge_current_ma += 200;
    }
    if (cm->fixed_charge_current_ma < 0)
        cm->fixed_charge_current_ma = 0;
    if (cm->fixed_charge_current_ma > current_ceiling_ma)
        cm->fixed_charge_current_ma = current_ceiling_ma;
    qb_set_sgm41542_int(cm, "ichrg_curr", cm->fixed_charge_current_ma * 1000);

    if (!cm->buck.ibat_adc_ma && cm->hiz_status == 2 &&
        cm->temp_status < QB_TEMP_HOT &&
        cm->buck.vbat_adc_mv < cm->full_voltage_mv - 50) {
        if (++cm->buck_error_count >= 11) {
            qb_set_sgm41542(cm, "charge_en", "0");
            qb_set_sgm41542(cm, "charge_en", "1");
            cm->buck_error_count = 0;
        }
    } else {
        cm->buck_error_count = 0;
    }
}

int qb_select_qc_max_voltage(struct qb_manager *cm)
{
    if (cm->pdb.max_voltage_mv == 12000) {
        cm->qc_max_voltage_mv = 5000;
        return 0;
    }

    if (cm->pdb.power_role == QB_ROLE_SOURCE)
        qb_mos_off(cm, 1);
    qb_disable_buck(cm);
    qb_ovp_on(cm, 1);
    if (qb_battery_absent(cm) && cm->pda.power_role == QB_ROLE_SINK &&
        cm->pdb.power_role == QB_ROLE_SINK)
        qb_ovp_on(cm, 0);
    else
        qb_ovp_off(cm, 0);

    if (qb_interruptible_sleep(cm, 2))
        goto cleanup;
    qb_set_sgm41542(cm, "qc_volt", "1 12");
    if (qb_interruptible_sleep(cm, 3))
        goto cleanup;
    qb_read_int(QB_SGM41542_PATH, "vbus_adc", &cm->buck.vbus_adc_mv);
    if (cm->buck.vbus_adc_mv >= 11001 && cm->buck.vbus_adc_mv < 13000) {
        cm->qc_12v_supported = true;
        goto cleanup;
    }

    cm->qc_12v_supported = false;
    qb_set_sgm41542(cm, "qc_volt", "1 5");
    usleep(500000);
    qb_set_sgm41542(cm, "qc_volt", "1 9");
    if (qb_interruptible_sleep(cm, 3))
        goto cleanup;
    qb_read_int(QB_SGM41542_PATH, "vbus_adc", &cm->buck.vbus_adc_mv);
    cm->qc_9v_supported =
        cm->buck.vbus_adc_mv >= 8001 && cm->buck.vbus_adc_mv < 10000;

cleanup:
    cm->qc_max_voltage_mv = cm->qc_12v_supported ? 12000 :
                            cm->qc_9v_supported ? 9000 : 5000;
    qb_set_sgm41542(cm, "qc_volt", "1 5");
    if (cm->pdb.power_role == QB_ROLE_SOURCE)
        qb_mos_on(cm, 1);
    return qb_interruptible_sleep(cm, 1);
}

void qb_mode1_charge(struct qb_manager *cm)
{
    qb_get_port_info(&cm->pda, false);
    if (qb_pump_allowed(cm, &cm->pda, cm->pump.vbat_adc_mv)) {
        qb_disable_buck(cm);
        qb_enable_pump_cfg(cm, &cm->pda);
    } else {
        qb_disable_pump(cm);
        qb_enable_buck_cfg(cm, &cm->pda);
    }
}

void qb_mode2_charge(struct qb_manager *cm)
{
    qb_get_port_info(&cm->pdb, false);
    if (qb_select_qc_max_voltage(cm))
        return;
    if (qb_pump_allowed(cm, &cm->pdb, cm->pump.vbat_adc_mv)) {
        qb_disable_buck(cm);
        qb_enable_pump_cfg(cm, &cm->pdb);
    } else {
        qb_disable_pump(cm);
        qb_enable_buck_cfg(cm, &cm->pdb);
    }
}

void qb_mode3_charge(struct qb_manager *cm)
{
    struct qb_pd_port *selected;

    qb_get_port_info(&cm->pda, false);
    qb_get_port_info(&cm->pdb, false);
    if (cm->pdb.power_role == QB_ROLE_SINK && qb_select_qc_max_voltage(cm))
        return;

    if (cm->pda.supports_pps ||
        (cm->pda.max_voltage_mv > cm->pdb.max_voltage_mv &&
         cm->pda.max_voltage_mv > cm->qc_max_voltage_mv))
        selected = &cm->pda;
    else
        selected = &cm->pdb;

    if (qb_pump_allowed(cm, selected, cm->pump.vbat_adc_mv)) {
        qb_disable_buck(cm);
        qb_enable_pump_cfg(cm, selected);
    } else {
        qb_disable_pump(cm);
        qb_enable_buck_cfg(cm, selected);
    }
}

void qb_reset_charge_state(struct qb_manager *cm)
{
    bool buck_was_working = cm->buck.working;

    cm->charge_mode_switching = true;
    qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
    cm->pps_voltage_mv = 0;
    cm->buck_input_current_ua = 0;
    cm->buck_charge_current_ua = 0;
    qb_no_charge(cm);
    qb_disable_pump(cm);
    qb_disable_buck(cm);
    cm->pps_voltage_mv = 5000;
    cm->pda.pps_current_ma = 1000;
    cm->pdb.pps_current_ma = 1000;

    if (!(cm->battery_offline_event && buck_was_working)) {
        qb_request_pdo(&cm->pda, 5000, cm->pda.pps_current_ma);
        qb_request_pdo(&cm->pdb, 5000, cm->pdb.pps_current_ma);
        qb_set_sgm41542(cm, "qc_volt", "1 5");
    }
    cm->charge_mode_switching = false;
}

int qb_enter_mode0(struct qb_manager *cm)
{
    qb_ovp_off(cm, 0);
    qb_ovp_off(cm, 1);
    qb_disable_buck(cm);
    qb_disable_pump(cm);
    if (cm->pda.power_role == QB_ROLE_SOURCE) qb_mos_on(cm, 0); else qb_mos_off(cm, 0);
    if (cm->pdb.power_role == QB_ROLE_SOURCE) qb_mos_on(cm, 1); else qb_mos_off(cm, 1);
    qb_no_charge(cm);
    cm->work_mode = QB_MODE_NONE;
    return 0;
}

int qb_enter_mode1(struct qb_manager *cm)
{
    qb_disable_buck(cm);
    qb_disable_pump(cm);
    if (!cm->change_power_role) {
        if (cm->pda.power_role == QB_ROLE_SOURCE) {
            qb_ovp_off(cm, 0); qb_mos_on(cm, 0);
        } else if (cm->pda.power_role == QB_ROLE_SINK) {
            qb_mos_off(cm, 0); qb_ovp_on(cm, 0);
        }
        if (qb_battery_absent(cm)) qb_ovp_on(cm, 1); else qb_ovp_off(cm, 1);
        qb_mos_off(cm, 1);
    }
    cm->change_power_role = false;
    if (!qb_queue_empty(cm)) goto done;
    if (qb_battery_present(cm)) {
        if (cm->pda.power_role == QB_ROLE_SINK) qb_mode1_charge(cm); else qb_no_charge(cm);
        cm->work_mode = QB_MODE_PORT_A;
        return 1;
    }
    if (!qb_battery_absent(cm)) {
        qb_no_charge(cm);
        goto done;
    }
    qb_enable_buck(cm, &cm->pda);
done:
    cm->work_mode = QB_MODE_PORT_A;
    return 0;
}

int qb_enter_mode2(struct qb_manager *cm)
{
    qb_disable_buck(cm);
    qb_disable_pump(cm);
    if (!cm->change_power_role) {
        if (cm->pdb.power_role == QB_ROLE_SOURCE) {
            qb_ovp_off(cm, 1); qb_mos_on(cm, 1);
        } else if (cm->pdb.power_role == QB_ROLE_SINK) {
            qb_mos_off(cm, 1); qb_ovp_on(cm, 1);
        }
        if (qb_battery_absent(cm)) qb_ovp_on(cm, 0); else qb_ovp_off(cm, 0);
        qb_mos_off(cm, 0);
    }
    cm->change_power_role = false;
    if (!qb_queue_empty(cm)) goto done;
    if (qb_battery_present(cm)) {
        if (cm->pdb.power_role == QB_ROLE_SINK) qb_mode2_charge(cm); else qb_no_charge(cm);
        cm->work_mode = QB_MODE_PORT_B;
        return 1;
    }
    if (!qb_battery_absent(cm)) {
        qb_no_charge(cm);
        goto done;
    }
    qb_get_port_info(&cm->pdb, false);
    if (!qb_select_qc_max_voltage(cm))
        qb_enable_buck(cm, &cm->pdb);
done:
    cm->work_mode = QB_MODE_PORT_B;
    return 0;
}

int qb_enter_mode3(struct qb_manager *cm)
{
    int role_status;

    for (;;) {
        qb_disable_buck(cm);
        qb_disable_pump(cm);

        if (cm->change_power_role) {
            role_status = -1;
        } else if (cm->pda.power_role == QB_ROLE_SINK) {
            if (cm->pdb.power_role == QB_ROLE_SINK) {
                role_status = 0;
                qb_mos_off(cm, 0);
                qb_mos_off(cm, 1);
                qb_ovp_on(cm, 1);
                qb_ovp_on(cm, 0);
            } else if (cm->pdb.power_role == QB_ROLE_SOURCE) {
                role_status = 1;
                qb_mos_on(cm, 1);
                qb_mos_off(cm, 0);
                qb_ovp_on(cm, 0);
                qb_ovp_off(cm, 1);
            } else {
                role_status = -1;
            }
        } else if (cm->pda.power_role == QB_ROLE_SOURCE) {
            if (cm->pdb.power_role == QB_ROLE_SINK) {
                role_status = 2;
                qb_mos_on(cm, 0);
                qb_mos_off(cm, 1);
                qb_ovp_on(cm, 1);
                qb_ovp_off(cm, 0);
            } else if (cm->pdb.power_role == QB_ROLE_SOURCE) {
                role_status = 3;
                qb_ovp_off(cm, 0);
                qb_ovp_off(cm, 1);
                qb_mos_on(cm, 0);
                qb_mos_on(cm, 1);
            } else {
                role_status = -1;
            }
        } else {
            role_status = -1;
        }

        cm->change_power_role = false;
        if (!qb_queue_empty(cm)) {
            cm->work_mode = QB_MODE_BOTH;
            return 0;
        }

        if (qb_battery_present(cm)) {
            if (cm->pda.power_role == QB_ROLE_SINK ||
                cm->pdb.power_role == QB_ROLE_SINK)
                qb_mode3_charge(cm);
            else
                qb_no_charge(cm);
            cm->work_mode = QB_MODE_BOTH;
            return role_status;
        }

        if (!qb_battery_absent(cm)) {
            qb_no_charge(cm);
            cm->work_mode = QB_MODE_BOTH;
            return role_status;
        }
        if (role_status == 1) {
            qb_get_port_info(&cm->pda, false);
            qb_enable_buck(cm, &cm->pda);
        } else if (role_status == 2) {
            qb_get_port_info(&cm->pdb, false);
            qb_enable_buck(cm, &cm->pdb);
        } else if (role_status == 0) {
            struct qb_pd_port *selected;
            int interrupted;

            qb_get_port_info(&cm->pda, false);
            qb_get_port_info(&cm->pdb, false);
            if (cm->pda.power_role != QB_ROLE_SINK ||
                cm->pdb.power_role != QB_ROLE_SINK)
                continue;
            interrupted = qb_select_qc_max_voltage(cm);
            if (interrupted) {
                cm->work_mode = QB_MODE_BOTH;
                return interrupted;
            }
            selected = cm->pda.max_voltage_mv > cm->pdb.max_voltage_mv &&
                       cm->pda.max_voltage_mv > cm->qc_max_voltage_mv ?
                       &cm->pda : &cm->pdb;
            qb_enable_buck(cm, selected);
        }

        cm->work_mode = QB_MODE_BOTH;
        return role_status;
    }
}

void qb_select_mode(struct qb_manager *cm)
{
    cm->mode = cm->pda.attached && cm->pdb.attached ? QB_MODE_BOTH :
               cm->pda.attached ? QB_MODE_PORT_A :
               cm->pdb.attached ? QB_MODE_PORT_B : QB_MODE_NONE;
    if (cm->work_mode == cm->mode)
        return;

    qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
    qb_set_sgm41542_int(cm, "ichrg_curr", 300000);
    qb_get_battery_online(cm);
    cm->pump_error_count = 0;
    cm->pump_error = false;
    switch (cm->mode) {
    case QB_MODE_NONE: qb_enter_mode0(cm); break;
    case QB_MODE_PORT_A: qb_enter_mode1(cm); break;
    case QB_MODE_PORT_B: qb_enter_mode2(cm); break;
    case QB_MODE_BOTH: qb_enter_mode3(cm); break;
    default: break;
    }
}
