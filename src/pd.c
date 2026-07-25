#include "qb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QB_PD_MAX_CAPABILITY_MV 50000
#define QB_PD_MAX_CAPABILITY_MA 10000

static bool qb_parse_scaled(double value, int maximum, int *result)
{
    double scaled = value * 1000.0;

    if (!(value > 0.0) || scaled > maximum)
        return false;
    *result = (int)(scaled + 0.5);
    return *result > 0;
}

static bool qb_parse_suffix(const char *suffix, bool *selected)
{
    while (*suffix == ' ' || *suffix == '\t')
        suffix++;
    if (*suffix == '\0') {
        *selected = false;
        return true;
    }
    if (!strcmp(suffix, "<-")) {
        *selected = true;
        return true;
    }
    return false;
}

int qb_parse_pdo_line(const char *line, struct qb_pdo *pdo, int index)
{
    const char *values;
    double min_v;
    double max_v;
    double current_a;
    int consumed = 0;
    bool selected;
    bool pps;

    memset(pdo, 0, sizeof(*pdo));
    if (!strncmp(line, "Fixed", 5)) {
        values = line + 5;
        pps = false;
        while (*values == ' ' || *values == '\t')
            values++;
        if (*values == ':')
            values++;
        if (sscanf(values, " %lfV , %lfA %n",
                   &min_v, &current_a, &consumed) != 2)
            return 0;
        max_v = min_v;
    } else if (!strncmp(line, "Pps", 3)) {
        values = line + 3;
        pps = true;
        while (*values == ' ' || *values == '\t')
            values++;
        if (*values == ':')
            values++;
        if (sscanf(values, " %lfV ~ %lfV , %lfA %n",
                   &min_v, &max_v, &current_a, &consumed) != 3)
            return 0;
    } else {
        QBLOG(0x166, "%s", "Unknown PDO line\n");
        return 0;
    }

    if (!qb_parse_suffix(values + consumed, &selected) ||
        !qb_parse_scaled(min_v, QB_PD_MAX_CAPABILITY_MV,
                         &pdo->min_voltage_mv) ||
        !qb_parse_scaled(max_v, QB_PD_MAX_CAPABILITY_MV,
                         &pdo->max_voltage_mv) ||
        !qb_parse_scaled(current_a, QB_PD_MAX_CAPABILITY_MA,
                         &pdo->current_ma) ||
        pdo->min_voltage_mv > pdo->max_voltage_mv) {
        memset(pdo, 0, sizeof(*pdo));
        return 0;
    }

    pdo->number = index + 1;
    pdo->selected = selected;
    pdo->pps = pps;
    QBLOG(0x163, "PDO voltage:%d~%d mV current:%d mA pps:%d\n",
          pdo->min_voltage_mv, pdo->max_voltage_mv,
          pdo->current_ma, pdo->pps);
    return 1;
}

int qb_get_pdo_info(struct qb_pd_port *port)
{
    char path[128];
    char line[104];
    FILE *fp;
    int count = 0;

    memset(port->pdo, 0, sizeof(port->pdo));
    port->pdo_count = 0;
    port->fixed_5v = port->fixed_9v = port->fixed_12v = false;
    port->fixed_5v_current_ma = 0;
    port->fixed_5v_number = 0;
    port->fixed_9v_current_ma = 0;
    port->fixed_9v_number = 0;
    port->fixed_12v_current_ma = 0;
    port->fixed_12v_number = 0;
    port->supports_pps = false;
    port->pps_min_voltage_mv = 0;
    port->pps_max_voltage_mv = 0;
    port->pps_current_ma = 0;
    port->max_voltage_mv = 0;

    snprintf(path, sizeof(path), "%spdo_set", port->path);
    QBLOG(0x176, "%s--%s\n", port->name, path);
    fp = fopen(path, "r");
    if (!fp) {
        perror("Failed to open file");
        return 1;
    }

    while (count < QB_MAX_PDOS && fgets(line, 100, fp)) {
        line[strcspn(line, "\n")] = '\0';
        if (qb_parse_pdo_line(line, &port->pdo[count], count))
            count++;
    }
    port->pdo_count = count;

    for (int i = 0; i < count; i++) {
        struct qb_pdo *p = &port->pdo[i];
        if (p->pps) {
            port->supports_pps = true;
            port->pps_min_voltage_mv = p->min_voltage_mv;
            port->pps_max_voltage_mv = p->max_voltage_mv;
            port->pps_current_ma = p->current_ma;
            QBLOG(0x196, "Voltage = %d~%d mV, Current = %d mA\n",
                  p->min_voltage_mv, p->max_voltage_mv, p->current_ma);
            continue;
        }
        switch (p->min_voltage_mv) {
        case 5000:
            port->fixed_5v = true;
            port->fixed_5v_current_ma = p->current_ma;
            port->fixed_5v_number = p->number;
            if (port->max_voltage_mv < 5000)
                port->max_voltage_mv = 5000;
            break;
        case 9000:
            port->fixed_9v = true;
            port->fixed_9v_current_ma = p->current_ma;
            port->fixed_9v_number = p->number;
            if (port->max_voltage_mv < 9000)
                port->max_voltage_mv = 9000;
            break;
        case 12000:
            port->fixed_12v = true;
            port->fixed_12v_current_ma = p->current_ma;
            port->fixed_12v_number = p->number;
            if (port->max_voltage_mv < 12000)
                port->max_voltage_mv = 12000;
            break;
        }
    }
    if (!port->max_voltage_mv)
        port->max_voltage_mv = 5000;
    QBLOG(0x1c0, "%s--pd final_max_volt_mv = %d mv\n", port->name,
          port->max_voltage_mv);
    QBLOG(0x1c9, "%s--pdo_nums :%d\n", port->name, port->pdo_count);
    fclose(fp);
    return 0;
}

int qb_get_port_info(struct qb_pd_port *port, bool read_connection)
{
    char buf[32] = {0};

    int status = qb_get_pdo_info(port);
    if (read_connection) {
        qb_read_str(port->path, "cc_pin", buf, sizeof(buf));
        if (!strcmp(buf, "CC1"))
            port->cc_pin = QB_CC1;
        else if (!strcmp(buf, "CC2"))
            port->cc_pin = QB_CC2;
        else
            port->cc_pin = QB_CC_NONE;
        port->attached = port->cc_pin != QB_CC_NONE;
    }

    qb_read_str(port->path, "data_role", buf, sizeof(buf));
    port->data_role_dfp = !strcmp(buf, "DFP");
    if (port->manager && port == &port->manager->pda)
        port->manager->otg_mode = port->data_role_dfp;

    qb_read_str(port->path, "pwr_role", buf, sizeof(buf));
    port->power_role = !strcmp(buf, "Source") ? QB_ROLE_SOURCE : QB_ROLE_SINK;
    return status == 0 ? 0 : -1;
}

bool qb_request_pdo(struct qb_pd_port *port, int voltage_mv, int current_ma)
{
    char request[32];

    if (current_ma <= 0)
        return false;

    for (int i = 0; i < port->pdo_count; i++) {
        struct qb_pdo *p = &port->pdo[i];
        int selected_current = current_ma;
        int requested_voltage_mv = voltage_mv;

        if (p->current_ma < selected_current)
            selected_current = p->current_ma;
        if (p->pps) {
            if (port->manager) {
                if (!qb_pps_enabled(port->manager))
                    continue;
                if (requested_voltage_mv > port->manager->max_pd_vbus_mv)
                    requested_voltage_mv = port->manager->max_pd_vbus_mv;
            }
            if (requested_voltage_mv < p->min_voltage_mv ||
                requested_voltage_mv > p->max_voltage_mv)
                continue;
            snprintf(request, sizeof(request), "%d  %d",
                     requested_voltage_mv, selected_current);
            p->requested_voltage_mv = requested_voltage_mv;
            p->requested_current_ma = selected_current;
        } else {
            if (requested_voltage_mv != p->min_voltage_mv)
                continue;
            snprintf(request, sizeof(request), "%d  %d", i + 1,
                     selected_current);
        }

        if (qb_write_str(port->path, "pdo_set", request) < 0) {
            QBLOG(0x3bd, "%s request write failed\n", port->name);
            return false;
        }
        QBLOG(0x3bd, "Voltage = %d mV, Current = %d mA\n",
              requested_voltage_mv, selected_current);
        return qb_get_port_info(port, false) == 0;
    }

    QBLOG(0x3bd, "%s has no compatible PDO\n", port->name);
    return false;
}

bool qb_pps_voltage_matches(int requested_mv, int measured_mv)
{
    const int tolerance_mv = 700;

    return requested_mv > 0 &&
           measured_mv >= requested_mv - tolerance_mv &&
           measured_mv <= requested_mv + tolerance_mv;
}
