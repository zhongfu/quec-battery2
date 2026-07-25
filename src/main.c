#include "qb.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static struct qb_manager *qb_global_manager;

static void qb_signal_handler(int signo)
{
    (void)signo;
    if (qb_global_manager)
        qb_global_manager->running = false;
}


static void qb_manager_init(struct qb_manager *cm)
{
    memset(cm, 0, sizeof(*cm));
    cm->max_current_ma = 5300;
    cm->min_shutdown_mv = 3400;
    cm->max_pd_vbus_mv = 9800;
    cm->config_pd_full_mv = 4050;
    cm->pd_full_mv = 4200;
    cm->full_voltage_mv = 4400;
    cm->charge_current_ma = 5300;
    cm->fixed_charge_current_ma = 300;
    cm->mode = QB_MODE_NONE;
    cm->work_mode = QB_MODE_RESELECT;
    cm->running = true;
    cm->pda.name = "pda";
    cm->pda.path = QB_PDA_PATH;
    cm->pda.manager = cm;
    cm->pdb.name = "pdb";
    cm->pdb.path = QB_PDB_PATH;
    cm->pdb.manager = cm;
    pthread_mutex_init(&cm->reset_mutex, NULL);
    pthread_mutex_init(&cm->events.mutex, NULL);
    qb_load_config(cm);
}

int main(void)
{
    struct qb_manager *cm;
    pthread_t dispatcher_thread;
    pthread_t gauge_thread;
    pthread_t watchdog_thread;
    pthread_t pump_thread;
    pthread_t buck_thread;

    openlog("quec_battery", LOG_PID, LOG_DAEMON);
    cm = calloc(1, sizeof(*cm));
    if (!cm)
        return EXIT_FAILURE;
    qb_manager_init(cm);
    qb_global_manager = cm;

    qb_set_battery_cycle(cm);
    sleep(5);
    signal(SIGINT, qb_signal_handler);
    signal(SIGTERM, qb_signal_handler);

    qb_get_battery_online(cm);
    qb_get_port_info(&cm->pda, true);
    qb_get_port_info(&cm->pdb, true);
    qb_get_sgm41542_info(cm);
    qb_get_sgm41600_info(cm);
    qb_get_battery_info(cm);

    if (pthread_create(&watchdog_thread, NULL, qb_watchdog_monitor, cm) != 0 ||
        pthread_create(&gauge_thread, NULL, qb_gauge_monitor, cm) != 0 ||
        pthread_create(&pump_thread, NULL, qb_pump_monitor, cm) != 0 ||
        pthread_create(&buck_thread, NULL, qb_buck_monitor, cm) != 0) {
        cm->running = false;
        return EXIT_FAILURE;
    }

    qb_select_mode(cm);
    if (qb_thermal_netlink_init(cm) < 0) {
        cm->running = false;
        QBLOG(0, "%s", "thermal cooling-device initialization failed");
        closelog();
        return EXIT_SUCCESS;
    }
    if (pthread_create(&dispatcher_thread, NULL, qb_event_monitor, cm) != 0) {
        cm->running = false;
        return EXIT_FAILURE;
    }
    sleep(1);
    qb_receive_uevents(cm);
    while (cm->running)
        sleep(10);

    cm->running = false;
    closelog();
    return EXIT_SUCCESS;
}
