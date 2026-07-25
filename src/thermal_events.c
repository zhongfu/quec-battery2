#include "qb.h"

#include <dirent.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/thermal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int qb_thermal_socket = -1;
static int qb_thermal_group = -1;
static int qb_battery_cooling_id = -1;

static bool qb_nla_valid(const struct nlattr *attribute, size_t remaining)
{
    return remaining >= sizeof(*attribute) &&
           attribute->nla_len >= NLA_HDRLEN &&
           attribute->nla_len <= remaining;
}

static const void *qb_nla_data(const struct nlattr *attribute)
{
    return (const char *)attribute + NLA_HDRLEN;
}

static size_t qb_nla_payload_length(const struct nlattr *attribute)
{
    return attribute->nla_len - NLA_HDRLEN;
}

static uint32_t qb_nla_u32(const struct nlattr *attribute)
{
    uint32_t value;

    memcpy(&value, qb_nla_data(attribute), sizeof(value));
    return value;
}

static int qb_parse_multicast_groups(const struct nlattr *groups)
{
    static const char event_group[] = THERMAL_GENL_EVENT_GROUP_NAME;
    size_t remaining = qb_nla_payload_length(groups);
    const struct nlattr *group = qb_nla_data(groups);

    while (qb_nla_valid(group, remaining)) {
        size_t group_remaining = qb_nla_payload_length(group);
        const struct nlattr *attribute = qb_nla_data(group);
        const char *name = NULL;
        size_t name_length = 0;
        int id = -1;

        while (qb_nla_valid(attribute, group_remaining)) {
            unsigned type = attribute->nla_type & NLA_TYPE_MASK;

            if (type == CTRL_ATTR_MCAST_GRP_NAME) {
                name = qb_nla_data(attribute);
                name_length = qb_nla_payload_length(attribute);
            } else if (type == CTRL_ATTR_MCAST_GRP_ID &&
                       qb_nla_payload_length(attribute) >= sizeof(uint32_t)) {
                id = (int)qb_nla_u32(attribute);
            }

            size_t step = NLA_ALIGN(attribute->nla_len);
            if (step > group_remaining)
                break;
            group_remaining -= step;
            attribute = (const struct nlattr *)((const char *)attribute + step);
        }

        if (name && name_length >= sizeof(event_group) &&
            memcmp(name, event_group, sizeof(event_group)) == 0)
            return id;

        size_t step = NLA_ALIGN(group->nla_len);
        if (step > remaining)
            break;
        remaining -= step;
        group = (const struct nlattr *)((const char *)group + step);
    }
    return -1;
}

static int qb_extract_thermal_event_group(char *buffer, ssize_t length)
{
    int remaining = (int)length;
    struct nlmsghdr *header;

    for (header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
        struct genlmsghdr *generic;
        const struct nlattr *attribute;
        size_t attribute_remaining;

        if (header->nlmsg_type == NLMSG_ERROR)
            return -1;
        if (header->nlmsg_type != GENL_ID_CTRL ||
            header->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
            continue;

        generic = NLMSG_DATA(header);
        attribute = (const struct nlattr *)((const char *)generic + GENL_HDRLEN);
        attribute_remaining = header->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
        while (qb_nla_valid(attribute, attribute_remaining)) {
            if ((attribute->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_MCAST_GROUPS)
                return qb_parse_multicast_groups(attribute);

            size_t step = NLA_ALIGN(attribute->nla_len);
            if (step > attribute_remaining)
                break;
            attribute_remaining -= step;
            attribute = (const struct nlattr *)((const char *)attribute + step);
        }
    }
    return -1;
}

static int qb_resolve_thermal_event_group(int fd)
{
    struct {
        struct nlmsghdr header;
        struct genlmsghdr generic;
        char attribute[NLA_ALIGN(NLA_HDRLEN + sizeof(THERMAL_GENL_FAMILY_NAME))];
    } request;
    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
    struct nlattr *family_name;
    char response[16384];
    ssize_t length;

    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = sizeof(request);
    request.header.nlmsg_type = GENL_ID_CTRL;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.header.nlmsg_seq = 1;
    request.generic.cmd = CTRL_CMD_GETFAMILY;
    family_name = (struct nlattr *)request.attribute;
    family_name->nla_type = CTRL_ATTR_FAMILY_NAME;
    family_name->nla_len = NLA_HDRLEN + sizeof(THERMAL_GENL_FAMILY_NAME);
    memcpy((char *)family_name + NLA_HDRLEN, THERMAL_GENL_FAMILY_NAME,
           sizeof(THERMAL_GENL_FAMILY_NAME));

    if (sendto(fd, &request, request.header.nlmsg_len, 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) < 0)
        return -1;
    length = recv(fd, response, sizeof(response), 0);
    if (length <= 0)
        return -1;
    return qb_extract_thermal_event_group(response, length);
}

static int qb_find_battery_cooling_device(void)
{
    DIR *directory;
    struct dirent *entry;
    int id = -1;

    directory = opendir("/sys/class/thermal");
    if (!directory)
        return -1;
    while ((entry = readdir(directory)) != NULL) {
        char type_path[512];
        char type[128];
        FILE *fp;

        if (strncmp(entry->d_name, "cooling_device", 14) != 0)
            continue;
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type",
                 entry->d_name);
        fp = fopen(type_path, "r");
        if (!fp)
            continue;
        if (fgets(type, sizeof(type), fp) &&
            strncmp(type, "battery-charger-cur", 19) == 0 &&
            sscanf(entry->d_name, "cooling_device%d", &id) == 1) {
            fclose(fp);
            break;
        }
        fclose(fp);
    }
    closedir(directory);
    return id;
}

static void qb_process_thermal_events(char *buffer, ssize_t length)
{
    int remaining = (int)length;
    struct nlmsghdr *header;

    for (header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
        struct genlmsghdr *generic;
        const struct nlattr *attribute;
        size_t attribute_remaining;
        uint32_t cooling_id = 0;
        uint32_t state = 0;
        bool have_cooling_id = false;
        bool have_state = false;

        if (header->nlmsg_type == NLMSG_ERROR ||
            header->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
            continue;
        generic = NLMSG_DATA(header);
        if (generic->cmd != THERMAL_GENL_EVENT_CDEV_STATE_UPDATE)
            continue;

        attribute = (const struct nlattr *)((const char *)generic + GENL_HDRLEN);
        attribute_remaining = header->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
        while (qb_nla_valid(attribute, attribute_remaining)) {
            unsigned type = attribute->nla_type & NLA_TYPE_MASK;

            if (type == THERMAL_GENL_ATTR_CDEV_ID &&
                qb_nla_payload_length(attribute) >= sizeof(uint32_t)) {
                cooling_id = qb_nla_u32(attribute);
                have_cooling_id = true;
            } else if (type == THERMAL_GENL_ATTR_CDEV_CUR_STATE &&
                       qb_nla_payload_length(attribute) >= sizeof(uint32_t)) {
                state = qb_nla_u32(attribute);
                have_state = true;
            }

            size_t step = NLA_ALIGN(attribute->nla_len);
            if (step > attribute_remaining)
                break;
            attribute_remaining -= step;
            attribute = (const struct nlattr *)((const char *)attribute + step);
        }

        if (have_cooling_id && have_state && qb_battery_cooling_id != -1 &&
            cooling_id == (uint32_t)qb_battery_cooling_id)
            qb_power_limit_state = (int)state;
    }
}

static void *qb_thermal_event_monitor(void *arg)
{
    char buffer[8192];

    (void)arg;
    if (qb_thermal_group == -1)
        return NULL;
    for (;;) {
        ssize_t length = recv(qb_thermal_socket, buffer, sizeof(buffer), 0);

        if (length > 0)
            qb_process_thermal_events(buffer, length);
    }
}

int qb_thermal_netlink_init(struct qb_manager *cm)
{
    struct sockaddr_nl local = { .nl_family = AF_NETLINK };

    if (qb_thermal_socket >= 0)
        return 0;
    qb_thermal_socket = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                               NETLINK_GENERIC);
    if (qb_thermal_socket < 0)
        return -1;
    if (bind(qb_thermal_socket, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(qb_thermal_socket);
        qb_thermal_socket = -1;
        return -1;
    }

    qb_thermal_group = qb_resolve_thermal_event_group(qb_thermal_socket);
    if (qb_thermal_group != -1 &&
        setsockopt(qb_thermal_socket, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                   &qb_thermal_group, sizeof(qb_thermal_group)) < 0) {
        close(qb_thermal_socket);
        qb_thermal_socket = -1;
        return -1;
    }

    qb_battery_cooling_id = qb_find_battery_cooling_device();
    (void)pthread_create(&cm->thermal_thread, NULL, qb_thermal_event_monitor, NULL);
    return 0;
}
