#include "qb.h"

#include <unistd.h>

static bool qb_pump_ready_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->pump.working && cm->pump.telemetry_valid &&
           cm->battery.telemetry_valid && port->working &&
           port->telemetry_valid && !cm->pump_error &&
           qb_pps_enabled(cm) && cm->charge_current_ma > 0 &&
           port->supports_pps && qb_battery_present(cm) &&
           port->power_role == QB_ROLE_SINK &&
           cm->temp_status >= QB_TEMP_NORMAL &&
           cm->temp_status <= QB_TEMP_WARM;
}
 
static bool qb_pump_running_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->pump.charge_status == 1 && cm->pump.charge_en == 2 &&
           cm->pump.telemetry_valid && cm->battery.telemetry_valid &&
           port->working && port->telemetry_valid && !cm->pump_error &&
           qb_battery_present(cm) && port->power_role == QB_ROLE_SINK;
}

static void qb_pump_select_buck(struct qb_manager *cm,
                                struct qb_pd_port *port, int index)
{
    qb_ovp_off(cm, index);
    qb_disable_pump(cm);
    qb_disable_pump_cfg(cm, port);
    (void)qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
    qb_enable_buck_cfg(cm, port);
}

static void qb_pump_fallback_to_buck(struct qb_manager *cm,
                                     struct qb_pd_port *port, int index)
{
    cm->pump_error = true;
    qb_pump_select_buck(cm, port, index);
}

static void qb_pump_handoff_to_buck(struct qb_manager *cm,
                                    struct qb_pd_port *port, int index)
{
    int handoff_vbat_mv = cm->pump.vbat_adc_mv;
    int handoff_ibat_ma = cm->pump.ibat_adc_ma;

    if (qb_program_buck_voltage_limit(cm) < 0) {
        qb_pump_fallback_to_buck(cm, port, index);
        return;
    }

    qb_ovp_off(cm, index);
    qb_disable_pump(cm);
    if (!cm->pump.telemetry_valid || cm->pump.charge_en != 0) {
        cm->pump_error = true;
        qb_disable_pump_cfg(cm, port);
        qb_disable_buck(cm);
        qb_disable_buck_cfg(cm, port);
        (void)qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
        QBLOG(0x5a3, "%s",
              "pump handoff aborted: pump disable not confirmed");
        return;
    }

    if (!qb_enable_buck_handoff(cm, port)) {
        qb_pump_fallback_to_buck(cm, port, index);
        return;
    }

    port->pump_handoff_complete = true;
    port->pump_handoff_samples = 0;
    qb_enable_buck_cfg(cm, port);
    QBLOG(0x5a3, "pump handoff: vbat:%d mV ibat:%d mA",
          handoff_vbat_mv, handoff_ibat_ma);
}

static void qb_pump_run_port(struct qb_manager *cm, struct qb_pd_port *port, int index)
{
    int initial_current;
    int measured_vbus_mv;
    if (qb_program_pump_voltage_limit(cm) < 0) {
        qb_pump_fallback_to_buck(cm, port, index);
        return;
    }

    cm->pps_voltage_mv =
        qb_pump_start_voltage_mv(cm, cm->pump.vbat_adc_mv);
    initial_current = (cm->charge_current_ma / 200) * 100;
    if (!qb_request_pdo(port, cm->pps_voltage_mv, initial_current)) {
        qb_pump_fallback_to_buck(cm, port, index);
        return;
    }

    qb_ovp_off(cm, index ^ 1);
    qb_ovp_on(cm, index);
    sleep(1);
    if (port->power_role != QB_ROLE_SINK || !port->supports_pps ||
        qb_read_int(QB_SGM41600_PATH, "vbus_adc", &measured_vbus_mv) < 0 ||
        !qb_pps_voltage_matches(cm->pps_voltage_mv, measured_vbus_mv) ||
        qb_set_sgm41600(cm, "charge_en", "2") < 0) {
        QBLOG(0x59f,
              "pump start rejected: charge_en:%d requested:%d mV measured:%d mV",
              cm->pump.charge_en, cm->pps_voltage_mv, measured_vbus_mv);
        qb_pump_fallback_to_buck(cm, port, index);
        return;
    }

    if (!qb_pump_running_for(cm, port)) {
        QBLOG(0x5a1,
              "pump did not enter divider mode: charge_en:%d requested:%d mV",
              cm->pump.charge_en, cm->pps_voltage_mv);
        qb_pump_fallback_to_buck(cm, port, index);
        return;
    }
    port->pump_handoff_samples = 0;

    while (qb_pump_running_for(cm, port)) {
        if (qb_interruptible_sleep(cm, 2))
            return;
        if (cm->charge_mode_switching || !qb_battery_present(cm))
            return;
        if (qb_get_sgm41600_info(cm) < 0) {
            qb_pump_fallback_to_buck(cm, port, index);
            return;
        }
        if (cm->pump.charge_en != 2) {
            QBLOG(0x5a7, "pump stopped unexpectedly: charge_en:%d",
                  cm->pump.charge_en);
            qb_pump_fallback_to_buck(cm, port, index);
            return;
        }
        if (qb_program_pump_voltage_limit(cm) < 0) {
            qb_pump_fallback_to_buck(cm, port, index);
            return;
        }
        qb_pump_pps_control(cm);

        if (qb_pump_handoff_ready(cm, port)) {
            qb_pump_handoff_to_buck(cm, port, index);
            return;
        }
        if (cm->pump_error || cm->pump.vbat_adc_mv < 3401 ||
            !port->supports_pps ||
            cm->temp_status < QB_TEMP_NORMAL ||
            cm->temp_status > QB_TEMP_WARM) {
            qb_pump_fallback_to_buck(cm, port, index);
            return;
        }
        if (!qb_request_pdo(port, cm->pps_voltage_mv,
                            qb_limit_charge_current(cm, 2650))) {
            qb_pump_fallback_to_buck(cm, port, index);
            return;
        }
    }

    qb_disable_pump(cm);
    port->pps_current_ma = 0;
    cm->pps_voltage_mv = 0;
    cm->work_mode = QB_MODE_RESELECT;
}

void *qb_pump_monitor(void *arg)
{
    struct qb_manager *cm = arg;

    pthread_detach(pthread_self());
    while (cm->running) {
        sleep(3);
        pthread_mutex_lock(&cm->reset_mutex);
        qb_select_mode(cm);
        pthread_mutex_unlock(&cm->reset_mutex);

        if (!cm->pump.working) {
            qb_set_sgm41600(cm, "charge_en", "0");
            continue;
        }
        if (qb_pump_ready_for(cm, &cm->pda))
            qb_pump_run_port(cm, &cm->pda, 0);
        else if (qb_pump_ready_for(cm, &cm->pdb))
            qb_pump_run_port(cm, &cm->pdb, 1);
        else
            qb_set_sgm41600(cm, "charge_en", "0");
    }
    return NULL;
}

static bool qb_buck_ready_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->buck.working && cm->buck.telemetry_valid &&
           cm->battery.telemetry_valid && port->working &&
           port->telemetry_valid && cm->charge_current_ma > 0 &&
           qb_battery_present(cm) && port->power_role == QB_ROLE_SINK &&
           cm->temp_status >= QB_TEMP_COOL &&
           cm->temp_status <= QB_TEMP_HOT;
}

static bool qb_buck_running_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->buck.charge_status == 1 && cm->buck.telemetry_valid &&
           cm->battery.telemetry_valid && port->working &&
           port->telemetry_valid && qb_battery_present(cm) &&
           port->power_role == QB_ROLE_SINK;
}

static void qb_buck_run_port(struct qb_manager *cm, struct qb_pd_port *port, int index)
{
    if (qb_program_buck_voltage_limit(cm) < 0) {
        qb_disable_buck(cm);
        return;
    }
    qb_enable_buck(cm, port);
    if (!qb_buck_running_for(cm, port)) {
        qb_ovp_off(cm, index);
        qb_disable_buck(cm);
    }

    for (;;) {
        if (!qb_buck_running_for(cm, port))
            return;
        if (qb_interruptible_sleep(cm, 2))
            return;
        if (cm->charge_mode_switching || !qb_battery_present(cm))
            break;
        if (qb_get_sgm41542_info(cm) < 0) {
            qb_disable_buck(cm);
            (void)qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
            return;
        }

        if (cm->temp_status < QB_TEMP_COOL || cm->temp_status > QB_TEMP_HOT) {
            qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
            qb_disable_buck(cm);
            qb_request_pdo(port, 5000, port->pps_current_ma);
            if (port == &cm->pdb)
                qb_set_sgm41542(cm, "qc_volt", "1 5");
            return;
        }

        if (qb_pump_allowed(cm, port, cm->buck.vbat_adc_mv)) {
            qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
            if (port == &cm->pda) {
                qb_disable_buck_cfg(cm, port);
                qb_disable_buck(cm);
            } else {
                qb_disable_buck(cm);
                qb_disable_buck_cfg(cm, port);
            }
            qb_enable_pump_cfg(cm, port);
            cm->work_mode = QB_MODE_RESELECT;
            return;
        }

        qb_fixed_charge_control(cm);
        if (qb_program_buck_voltage_limit(cm) < 0) {
            qb_disable_buck(cm);
            return;
        }
    }
    qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
}

void *qb_buck_monitor(void *arg)
{
    struct qb_manager *cm = arg;

    pthread_detach(pthread_self());
    while (cm->running) {
        sleep(3);
        pthread_mutex_lock(&cm->reset_mutex);
        qb_select_mode(cm);
        pthread_mutex_unlock(&cm->reset_mutex);
        cm->fixed_charge_current_ma = 300;

        if (!cm->buck.working) {
            qb_disable_buck(cm);
            continue;
        }
        if (qb_buck_ready_for(cm, &cm->pda))
            qb_buck_run_port(cm, &cm->pda, 0);
        else if (qb_buck_ready_for(cm, &cm->pdb))
            qb_buck_run_port(cm, &cm->pdb, 1);
        else
            qb_disable_buck(cm);
    }
    return NULL;
}
