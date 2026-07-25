#include "qb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


static int qb_watchdog_enabled(void)
{
    FILE *fp;
    char line[256];
    int enabled = 1;

    fp = fopen("/etc/ChargeIC_wdt", "r");
    if (!fp)
        return enabled;
    while (fgets(line, sizeof(line), fp)) {
        char key[100];
        int value;

        if (sscanf(line, "%99s = %d", key, &value) == 2 &&
            strcmp(key, "watchdog_enable") == 0) {
            enabled = value;
            break;
        }
    }
    fclose(fp);
    return enabled;
}

static int qb_watchdog_fd = -1;

static void qb_watchdog_signal_handler(int signo)
{
    (void)signo;
    if (qb_watchdog_fd > 0)
        close(qb_watchdog_fd);
    exit(EXIT_SUCCESS);
}

static int qb_open_watchdog_socket(void)
{
    int flags;

    qb_watchdog_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (qb_watchdog_fd < 0)
        return -1;
    flags = fcntl(qb_watchdog_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(qb_watchdog_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(qb_watchdog_fd);
        return -1;
    }
    return qb_watchdog_fd;
}

static bool qb_connect_watchdog(int fd)
{
    struct sockaddr_un address;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, QB_WDT_SOCKET, sizeof(address.sun_path) - 1);
    for (int tries = 0; tries < 10; tries++) {
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
            return true;
        sleep(5);
    }
    return false;
}

static void qb_run_system(const char *command)
{
    int result = system(command);
    (void)result;
}

static void qb_reboot_now(void)
{
    qb_run_system("echo 0 > /sys/devices/platform/hypervisor/"
                  "hypervisor:qcom,gh-watchdog/user_pet_enabled");
    if (system("reboot") < 0)
        QBLOG(0xacb, "%s", "reboot command failed\n");
}

void *qb_watchdog_monitor(void *arg)
{
    struct qb_manager *cm = arg;
    int failures;
    int fd;

    pthread_detach(pthread_self());
    signal(SIGINT, qb_watchdog_signal_handler);
    fd = qb_open_watchdog_socket();
    if (fd < 0)
        exit(EXIT_FAILURE);

    if (!qb_watchdog_enabled()) {
        qb_run_system("/etc/init.d/ql_wdt_service.init stop");
        qb_write_str(QB_SGM41542_PATH, "watchdog", "999");
        qb_write_str(QB_SGM41600_PATH, "watchdog", "999");
        qb_run_system("echo 0 > /sys/devices/platform/hypervisor/"
                      "hypervisor:qcom,gh-watchdog/user_pet_enabled");
        return NULL;
    }

    for (;;) {
        qb_write_str(QB_SGM41542_PATH, "watchdog", "998");
        qb_write_str(QB_SGM41600_PATH, "watchdog", "1");
        if (!qb_connect_watchdog(fd))
            qb_run_system("reboot");
        failures = 0;

        while (cm->running) {
            int buck_fault;
            int pump_fault;
            ssize_t sent = send(fd, "HEARTBEAT", 9, 0);

            if (sent < 0 && errno != EAGAIN) {
                qb_reboot_now();
                break;
            }
            if (qb_read_int(QB_SGM41542_PATH, "watchdog", &buck_fault) < 0 ||
                qb_read_int(QB_SGM41600_PATH, "watchdog", &pump_fault) < 0)
                exit(EXIT_SUCCESS);

            if (buck_fault == 0x50 || pump_fault == 0x20)
                qb_reboot_now();

            if (qb_write_str(QB_SGM41542_PATH, "watchdog", "1") < 0) {
                failures++;
                if (failures >= 10)
                    qb_reboot_now();
            } else {
                failures = 0;
            }
            sleep(5);
        }
        close(fd);
    }
}
