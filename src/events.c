#include "qb.h"

#include <errno.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool qb_queue_empty(struct qb_manager *cm)
{
    bool empty;
    pthread_mutex_lock(&cm->events.mutex);
    empty = cm->events.count == 0;
    pthread_mutex_unlock(&cm->events.mutex);
    return empty;
}

bool qb_queue_full(struct qb_manager *cm)
{
    bool full;
    pthread_mutex_lock(&cm->events.mutex);
    full = cm->events.count == QB_EVENT_CAPACITY;
    pthread_mutex_unlock(&cm->events.mutex);
    return full;
}

unsigned qb_queue_length(struct qb_manager *cm)
{
    unsigned count;
    pthread_mutex_lock(&cm->events.mutex);
    count = cm->events.count;
    pthread_mutex_unlock(&cm->events.mutex);
    QBLOG(0xfa, "quece length :%u\n", count);
    return count;
}

bool qb_queue_enqueue(struct qb_manager *cm, const char *event)
{
    struct qb_event_queue *q = &cm->events;
    bool ok = false;

    pthread_mutex_lock(&q->mutex);
    if (q->count < QB_EVENT_CAPACITY) {
        strncpy(q->items[q->rear], event, QB_EVENT_SIZE - 1);
        q->items[q->rear][QB_EVENT_SIZE - 1] = '\0';
        QBLOG(0x104, "enqueue event: %s\n", q->items[q->rear]);
        q->rear = (q->rear + 1) & (QB_EVENT_CAPACITY - 1);
        q->count++;
        ok = true;
    }
    pthread_mutex_unlock(&q->mutex);
    return ok;
}

bool qb_queue_dequeue(struct qb_manager *cm, char out[QB_EVENT_SIZE])
{
    struct qb_event_queue *q = &cm->events;
    bool ok = false;

    pthread_mutex_lock(&q->mutex);
    if (q->count) {
        strcpy(out, q->items[q->front]);
        q->front = (q->front + 1) & (QB_EVENT_CAPACITY - 1);
        q->count--;
        QBLOG(0x119, "dequeue event: %s\n", out);
        ok = true;
    }
    pthread_mutex_unlock(&q->mutex);
    return ok;
}

int qb_interruptible_sleep_ms(struct qb_manager *cm, unsigned milliseconds)
{
    while (milliseconds) {
        unsigned interval_ms = milliseconds > 100 ? 100 : milliseconds;

        usleep(interval_ms * 1000);
        milliseconds -= interval_ms;
        if (!qb_queue_empty(cm)) {
            QBLOG(0x12e, "%s", "stop sleep ,deal with quecue\n");
            return 1;
        }
    }
    return 0;
}

int qb_interruptible_sleep(struct qb_manager *cm, unsigned seconds)
{
    for (unsigned i = 0; i < seconds * 10; i++) {
        usleep(100000);
        if (!qb_queue_empty(cm)) {
            QBLOG(0x12e, "%s", "stop sleep ,deal with quecue\n");
            return 1;
        }
    }
    return 0;
}

static bool qb_known_port_event(const char *s, char port)
{
    char prefix[16];
    const char *value;

    snprintf(prefix, sizeof(prefix), "aw35615-%c=", port);
    if (strncmp(s, prefix, strlen(prefix)))
        return false;
    value = s + strlen(prefix);
    return !strcmp(value, "cc1_in") || !strcmp(value, "cc2_in") ||
           !strcmp(value, "cc_none") || !strcmp(value, "sink") ||
           !strcmp(value, "source");
}

static void qb_copy_event(char destination[QB_EVENT_SIZE], const char *source)
{
    size_t length = strnlen(source, QB_EVENT_SIZE - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void qb_dead_battery_restore(struct qb_manager *cm)
{
    int present;

    if (qb_read_int(QB_BATTERY_PATH, "present", &present) < 0 ||
        present != 1)
        return;

    cm->dead_battery_restore = true;
    for (int remaining = 10; remaining > 0 && present == 1; remaining--) {
        qb_write_str(QB_SGM41542_PATH, "ichrg_curr", "150000");
        qb_write_str(QB_SGM41542_PATH, "charge_en", "1");
        usleep(500000);
        if (qb_read_int(QB_BATTERY_PATH, "present", &present) < 0)
            break;
    }
    cm->dead_battery_restore = false;
}

static int qb_open_uevent_socket(void)
{
    struct sockaddr_nl address = {
        .nl_family = AF_NETLINK,
        .nl_pid = (uint32_t)getpid(),
        .nl_groups = 0xffffffffu,
    };
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);

    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int qb_receive_uevents(struct qb_manager *cm)
{
    char previous_a[QB_EVENT_SIZE] = "";
    char previous_b[QB_EVENT_SIZE] = "";
    char buffer[4096];
    int fd;

    QBLOG(0x3d, "%s", "Starting listener events\n");
    sleep(1);
    qb_get_port_info(&cm->pda, true);
    qb_get_port_info(&cm->pdb, true);
    if (cm->pda.cc_pin == QB_CC_NONE)
        qb_copy_event(previous_a, "aw35615-a=cc_none");
    else if (cm->pda.cc_pin == QB_CC1)
        qb_copy_event(previous_a, "aw35615-a=cc1_in");
    if (cm->pdb.cc_pin == QB_CC_NONE)
        qb_copy_event(previous_b, "aw35615-b=cc_none");
    else if (cm->pdb.cc_pin == QB_CC1)
        qb_copy_event(previous_b, "aw35615-b=cc1_in");

    fd = qb_open_uevent_socket();
    if (fd < 0) {
        QBLOG(0x68, "bind failed: %s\n", strerror(errno));
        return -1;
    }

    while (cm->running) {
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0)
            continue;
        buffer[n] = '\0';

        for (char *s = buffer; s < buffer + n; s += strlen(s) + 1) {
            if (!*s)
                continue;
            if (qb_known_port_event(s, 'a')) {
                if (strncmp(s, previous_a, 17)) {
                    qb_queue_enqueue(cm, s);
                    qb_copy_event(previous_a, s);
                }
                continue;
            }
            if (qb_known_port_event(s, 'b')) {
                if (strncmp(s, previous_b, 17)) {
                    qb_queue_enqueue(cm, s);
                    qb_copy_event(previous_b, s);
                }
                continue;
            }
            if (strstr(s, "battery=offline") || strstr(s, "battery=online")) {
                qb_queue_enqueue(cm, s);
            } else if (strstr(s, "battery=dead")) {
                qb_dead_battery_restore(cm);
                qb_queue_enqueue(cm, s);
            }
        }
    }
    close(fd);
    return 0;
}

static void qb_apply_port_event(struct qb_manager *cm, struct qb_pd_port *port,
                                const char *event, int port_index)
{
    const char *value = strchr(event, '=');
    if (!value)
        return;
    value++;

    if (!strcmp(value, "sink") || !strcmp(value, "source")) {
        bool source = !strcmp(value, "source");

        cm->work_mode = QB_MODE_RESELECT;
        cm->change_power_role = true;
        cm->ovp_status[port_index] = !source;
        cm->mos_status[port_index] = source;
    }
    if (!strcmp(value, "cc_none")) {
        port->attached = false;
        cm->hiz_status = 0;
        port->pump_handoff_complete = false;
        port->pump_handoff_samples = 0;
    } else {
        port->attached = true;
    }

    pthread_mutex_lock(&cm->reset_mutex);
    qb_reset_charge_state(cm);
    qb_get_port_info(port, false);
    qb_select_mode(cm);
    pthread_mutex_unlock(&cm->reset_mutex);
}

void *qb_event_monitor(void *arg)
{
    struct qb_manager *cm = arg;
    char event[QB_EVENT_SIZE];

    pthread_detach(pthread_self());
    while (cm->running) {
        while (qb_queue_empty(cm) && cm->running)
            usleep(500000);
        if (!cm->running)
            break;
        usleep(500000);
        if (!qb_queue_dequeue(cm, event))
            continue;

        if (qb_known_port_event(event, 'a'))
            qb_apply_port_event(cm, &cm->pda, event, 0);
        else if (qb_known_port_event(event, 'b'))
            qb_apply_port_event(cm, &cm->pdb, event, 1);
        else if (strstr(event, "battery=offline")) {
            cm->battery_offline_event = true;
            cm->battery.presence = QB_BATTERY_ABSENT;
            cm->battery.raw_present = 0;
            qb_reset_charge_state(cm);
            cm->work_mode = QB_MODE_RESELECT;
            qb_disable_pump_cfg(cm, &cm->pda);
            qb_disable_pump_cfg(cm, &cm->pdb);
            qb_disable_buck_cfg(cm, &cm->pda);
            qb_disable_buck_cfg(cm, &cm->pdb);
            qb_select_mode(cm);
            cm->battery_offline_event = false;
        } else if (strstr(event, "battery=online") || strstr(event, "battery=dead")) {
            cm->battery.presence = QB_BATTERY_UNKNOWN;
            cm->battery.raw_present = -1;
            qb_get_battery_online(cm);
            qb_reset_charge_state(cm);
            cm->work_mode = QB_MODE_RESELECT;
            qb_set_battery_cycle(cm);
            qb_select_mode(cm);
        }
    }
    return NULL;
}
