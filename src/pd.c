#include "qb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int qb_parse_pdo_line(char *line, struct qb_pdo *pdo, int index)
{
    char *token;
    int min_v = 0, max_v = 0, current_a = 0;
    if (sscanf(line, "Fixed :%dV, %dA", &min_v, &current_a) == 2) {
        pdo->min_voltage_v = min_v;
        pdo->max_voltage_v = min_v;
        pdo->current_a = current_a;
        pdo->number = index + 1;
        pdo->selected = strstr(line, "<-") != NULL;
        pdo->pps = false;
        return 1;
    }

    if (strncmp(line, "Pps", 3) != 0) {
        QBLOG(0x166, "%s", "Line does not start with Pps:\n");
        return 0;
    }
    token = strtok(line + 3, " :");
    if (token)
        min_v = atoi(token);
    token = strtok(NULL, " V~");
    if (token)
        max_v = atoi(token);
    token = strtok(NULL, " ,A");
    if (token)
        current_a = atoi(token);

    pdo->min_voltage_v = min_v;
    pdo->max_voltage_v = max_v;
    pdo->current_a = current_a;
    pdo->selected = false;
    pdo->pps = true;
    QBLOG(0x163, "Pps voltage_min: %d V, voltage_max :%d V, current: %d A\n",
          pdo->min_voltage_v, pdo->max_voltage_v, pdo->current_a);
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
            port->pps_min_voltage_mv = p->min_voltage_v * 1000;
            port->pps_max_voltage_mv = p->max_voltage_v * 1000;
            port->pps_current_ma = p->current_a * 1000;
            QBLOG(0x196, "Voltage = %d~%dV, Current = %dA\n",
                  p->min_voltage_v, p->max_voltage_v, p->current_a);
            continue;
        }
        switch (p->min_voltage_v) {
        case 5:
            port->fixed_5v = true;
            port->fixed_5v_current_ma = p->current_a * 1000;
            port->fixed_5v_number = p->number;
            if (port->max_voltage_mv < 5000)
                port->max_voltage_mv = 5000;
            break;
        case 9:
            port->fixed_9v = true;
            port->fixed_9v_current_ma = p->current_a * 1000;
            port->fixed_9v_number = p->number;
            if (port->max_voltage_mv < 9000)
                port->max_voltage_mv = 9000;
            break;
        case 12:
            port->fixed_12v = true;
            port->fixed_12v_current_ma = p->current_a * 1000;
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

    qb_get_pdo_info(port);
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
    return 0;
}

void qb_request_pdo(struct qb_pd_port *port, int voltage_mv, int current_ma)
{
    char request[32];

    for (int i = 0; i < port->pdo_count; i++) {
        struct qb_pdo *p = &port->pdo[i];
        int selected_current = current_ma;

        if (p->current_a * 1000 < selected_current)
            selected_current = p->current_a * 1000;
        if (p->pps) {
            int requested_voltage_mv = voltage_mv;

            if (port->manager) {
                if (!qb_pps_enabled(port->manager))
                    continue;
                if (requested_voltage_mv > port->manager->max_pd_vbus_mv)
                    requested_voltage_mv = port->manager->max_pd_vbus_mv;
            }
            if (requested_voltage_mv < p->min_voltage_v * 1000 ||
                requested_voltage_mv > p->max_voltage_v * 1000)
                continue;
            snprintf(request, sizeof(request), "%d  %d",
                     requested_voltage_mv, selected_current);
            p->requested_voltage_mv = requested_voltage_mv;
        } else {
            if (voltage_mv != p->min_voltage_v * 1000)
                continue;
            if (!p->current_a && port->manager) {
                port->manager->buck_charge_current_ua = 0;
                qb_set_sgm41542_int(port->manager, "ichrg_curr",
                                    port->manager->buck_charge_current_ua);
                qb_disable_buck_cfg(port->manager, port);
            }
            snprintf(request, sizeof(request), "%d  %d", i + 1, selected_current);
        }
        if (p->pps)
            p->requested_current_ma = selected_current;
        qb_write_str(port->path, "pdo_set", request);
        if (p->pps) {
            QBLOG(0x3bd, "Voltage = %d mV, Current = %d mA\n",
                  p->requested_voltage_mv, p->requested_current_ma);
        } else {
            QBLOG(0x3bd, "Voltage = %d mV, Current = %d mA\n",
                  voltage_mv, selected_current);
        }
        qb_get_port_info(port, false);
        return;
    }

    /* The original refreshes capability/contract state even if no PDO matched. */
    qb_get_port_info(port, false);
}
