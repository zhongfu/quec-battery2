#include "qb.h"

#include <unistd.h>

static bool qb_pump_ready_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->pump.working && port->working && !cm->pump_error &&
           port->supports_pps && cm->battery.present == 1 &&
           port->power_role == QB_ROLE_SINK &&
           cm->temp_status >= QB_TEMP_NORMAL && cm->temp_status <= QB_TEMP_WARM;
}
 
static bool qb_pump_running_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->pump.charge_status == 1 && port->working && !cm->pump_error &&
           cm->battery.present == 1 && port->power_role == QB_ROLE_SINK;
}

static void qb_pump_run_port(struct qb_manager *cm, struct qb_pd_port *port, int index)
{
    int initial_current;

    cm->pps_voltage_mv = cm->pump.vbat_adc_mv * 220 / 100;
    initial_current = (cm->charge_current_ma / 200) * 100;
    qb_request_pdo(port, cm->pps_voltage_mv, initial_current);
    qb_ovp_off(cm, index ^ 1);
    qb_ovp_on(cm, index);
    sleep(1);
    qb_set_sgm41600(cm, "charge_en", "2");

    if (!qb_pump_running_for(cm, port)) {
        qb_ovp_off(cm, index);
        qb_disable_pump(cm);
    }

    while (qb_pump_running_for(cm, port)) {
        if (qb_interruptible_sleep(cm, 2))
            return;
        if (cm->charge_mode_switching || cm->battery.present == 0)
            return;
        qb_get_sgm41600_info(cm);
        qb_pump_pps_control(cm);

        if ((cm->pump.vbat_adc_mv >= cm->pd_full_mv && cm->pump.ibat_adc_ma <= 2000) ||
            cm->pump_error || cm->pump.vbat_adc_mv < 3401 || !port->supports_pps ||
            cm->temp_status < QB_TEMP_NORMAL || cm->temp_status > QB_TEMP_WARM) {
            qb_disable_pump(cm);
            if (port == &cm->pda) {
                qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
                qb_disable_pump_cfg(cm, port);
                qb_enable_buck_cfg(cm, port);
            } else {
                qb_disable_pump_cfg(cm, port);
                qb_enable_buck_cfg(cm, port);
                qb_request_pdo(port, 5000, port->fixed_5v_current_ma);
            }
            return;
        }
        qb_request_pdo(port, cm->pps_voltage_mv, 2650);
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
    return cm->buck.working && port->working && cm->battery.present == 1 &&
           port->power_role == QB_ROLE_SINK &&
           cm->temp_status >= QB_TEMP_COOL && cm->temp_status <= QB_TEMP_HOT;
}

static bool qb_buck_running_for(struct qb_manager *cm, struct qb_pd_port *port)
{
    return cm->buck.charge_status == 1 && port->working &&
           cm->battery.present == 1 &&
           port->power_role == QB_ROLE_SINK;
}

static void qb_buck_run_port(struct qb_manager *cm, struct qb_pd_port *port, int index)
{
    int previous_temp_status = -1;

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
        if (cm->charge_mode_switching || cm->battery.present == 0)
            break;
        qb_get_sgm41542_info(cm);

        if (cm->temp_status < QB_TEMP_COOL || cm->temp_status > QB_TEMP_HOT) {
            qb_set_sgm41542_int(cm, "vbus_vindpm", 3900000);
            qb_disable_buck(cm);
            qb_request_pdo(port, 5000, port->pps_current_ma);
            if (port == &cm->pdb)
                qb_set_sgm41542(cm, "qc_volt", "1 5");
            return;
        }

        if (cm->buck.vbat_adc_mv > 3400 &&
            cm->buck.vbat_adc_mv < cm->pd_full_mv - 100 &&
            port->supports_pps &&
            cm->temp_status >= QB_TEMP_NORMAL && cm->temp_status <= QB_TEMP_WARM &&
            !cm->pump_error) {
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
        if (previous_temp_status != (int)cm->temp_status) {
            qb_set_sgm41542_int(cm, "vreg", cm->full_voltage_mv * 1000);
            previous_temp_status = cm->temp_status;
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
