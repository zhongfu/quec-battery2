#include "qb.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define QB_WATCHDOG_CONNECT_ATTEMPTS 10
#define QB_WATCHDOG_CONNECT_TIMEOUT_MS 5000
#define QB_WATCHDOG_FAILURE_LIMIT 10

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

int qb_connect_unix_socket(const char *path, int timeout_ms)
{
    struct sockaddr_un address;
    struct pollfd poll_fd;
    socklen_t address_length;
    socklen_t error_length;
    int socket_error = 0;
    int fd;

    if (!path || timeout_ms < 0 || strlen(path) >= sizeof(address.sun_path))
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 strlen(address.sun_path) + 1);
    if (connect(fd, (struct sockaddr *)&address, address_length) == 0)
        return fd;
    if (errno != EINPROGRESS)
        goto failure;

    poll_fd.fd = fd;
    poll_fd.events = POLLOUT;
    poll_fd.revents = 0;
    if (poll(&poll_fd, 1, timeout_ms) <= 0 ||
        !(poll_fd.revents & POLLOUT))
        goto failure;

    error_length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   &socket_error, &error_length) < 0 || socket_error != 0)
        goto failure;
    return fd;

failure:
    close(fd);
    return -1;
}

static bool qb_watchdog_sleep(struct qb_manager *cm, unsigned seconds)
{
    for (unsigned elapsed = 0; elapsed < seconds * 10; elapsed++) {
        if (!cm->running)
            return false;
        usleep(100000);
    }
    return cm->running;
}

static int qb_connect_watchdog(struct qb_manager *cm)
{
    for (int attempt = 0;
         attempt < QB_WATCHDOG_CONNECT_ATTEMPTS && cm->running;
         attempt++) {
        int fd = qb_connect_unix_socket(QB_WDT_SOCKET,
                                        QB_WATCHDOG_CONNECT_TIMEOUT_MS);
        if (fd >= 0)
            return fd;
        if (attempt + 1 < QB_WATCHDOG_CONNECT_ATTEMPTS &&
            !qb_watchdog_sleep(cm, 5))
            break;
    }
    return -1;
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
    if (system("reboot") != 0)
        QBLOG(0xacb, "%s", "reboot command failed\n");
}

void *qb_watchdog_monitor(void *arg)
{
    struct qb_manager *cm = arg;

    pthread_detach(pthread_self());
    if (!qb_watchdog_enabled()) {
        qb_run_system("/etc/init.d/ql_wdt_service.init stop");
        qb_write_str(QB_SGM41542_PATH, "watchdog", "999");
        qb_write_str(QB_SGM41600_PATH, "watchdog", "999");
        qb_run_system("echo 0 > /sys/devices/platform/hypervisor/"
                      "hypervisor:qcom,gh-watchdog/user_pet_enabled");
        return NULL;
    }

    while (cm->running) {
        int feed_failures = 0;
        int read_failures = 0;
        int fd;

        qb_write_str(QB_SGM41542_PATH, "watchdog", "998");
        qb_write_str(QB_SGM41600_PATH, "watchdog", "1");
        fd = qb_connect_watchdog(cm);
        if (fd < 0) {
            if (cm->running)
                qb_reboot_now();
            return NULL;
        }

        while (cm->running) {
            int buck_fault;
            int pump_fault;
            ssize_t sent = send(fd, "HEARTBEAT", 9, MSG_NOSIGNAL);

            if (sent != 9)
                break;

            if (qb_read_int(QB_SGM41542_PATH, "watchdog", &buck_fault) < 0 ||
                qb_read_int(QB_SGM41600_PATH, "watchdog", &pump_fault) < 0) {
                if (++read_failures >= QB_WATCHDOG_FAILURE_LIMIT) {
                    close(fd);
                    qb_reboot_now();
                    return NULL;
                }
            } else {
                read_failures = 0;
                if (buck_fault == 0x50 || pump_fault == 0x20) {
                    close(fd);
                    qb_reboot_now();
                    return NULL;
                }
            }

            if (qb_write_str(QB_SGM41542_PATH, "watchdog", "1") < 0) {
                if (++feed_failures >= QB_WATCHDOG_FAILURE_LIMIT) {
                    close(fd);
                    qb_reboot_now();
                    return NULL;
                }
            } else {
                feed_failures = 0;
            }
            if (!qb_watchdog_sleep(cm, 5))
                break;
        }
        close(fd);
    }
    return NULL;
}
