#include "qb.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void qb_unquote_config(char *value)
{
    size_t length = strlen(value);

    if (length > 1 && (value[0] == '\'' || value[0] == '"')) {
        memmove(value, value + 1, length);
        length--;
        if (length && (value[length - 1] == '\'' || value[length - 1] == '"'))
            value[length - 1] = '\0';
    }
}

static bool qb_parse_config_int(const char *value, int *result)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno || end == value || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX)
        return false;
    *result = (int)parsed;
    return true;
}

static int qb_clamp(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

int qb_limit_charge_current(const struct qb_manager *cm, int requested_ma)
{
    if (requested_ma <= 0 || cm->max_current_ma <= 0)
        return 0;
    return requested_ma < cm->max_current_ma ? requested_ma : cm->max_current_ma;
}

bool qb_pps_enabled(const struct qb_manager *cm)
{
    return cm->max_pd_vbus_mv >= QB_PPS_MIN_VOLTAGE_MV;
}

int qb_limit_pps_voltage(const struct qb_manager *cm, int requested_mv)
{
    if (!qb_pps_enabled(cm))
        return 0;
    return qb_clamp(requested_mv, QB_PPS_MIN_VOLTAGE_MV,
                    cm->max_pd_vbus_mv);
}

int qb_charge_voltage_limit(const struct qb_manager *cm, int policy_mv)
{
    if (cm->charge_limit_mv == 0 || policy_mv <= cm->charge_limit_mv)
        return policy_mv;
    return cm->charge_limit_mv;
}

bool qb_low_voltage_danger(const struct qb_manager *cm, bool adapter_online,
                           int battery_mv)
{
    return !adapter_online && battery_mv < cm->min_shutdown_mv;
}

static void qb_validate_config(struct qb_manager *cm)
{
    int raw_max_current_ma = cm->max_current_ma;
    int raw_min_shutdown_mv = cm->min_shutdown_mv;
    int raw_max_pd_vbus_mv = cm->max_pd_vbus_mv;
    int raw_pd_full_mv = cm->pd_full_mv;
    int raw_charge_limit_mv = cm->charge_limit_mv;

    cm->max_current_ma =
        qb_clamp(cm->max_current_ma, 0, QB_STOCK_MAX_CURRENT_MA);
    cm->min_shutdown_mv =
        qb_clamp(cm->min_shutdown_mv, QB_STOCK_MIN_SHUTDOWN_MV,
                 QB_SAFE_MAX_SHUTDOWN_MV);
    cm->max_pd_vbus_mv =
        qb_clamp(cm->max_pd_vbus_mv, 0, QB_STOCK_MAX_PPS_VOLTAGE_MV);
    cm->pd_full_mv =
        qb_clamp(cm->pd_full_mv, QB_MIN_PD_FULL_MV, QB_STOCK_PD_FULL_MV);
    if (cm->charge_limit_mv < 0)
        cm->charge_limit_mv = 0;
    else if (cm->charge_limit_mv > 0)
        cm->charge_limit_mv = qb_clamp(cm->charge_limit_mv,
                                      QB_MIN_CHARGE_LIMIT_MV,
                                      QB_MAX_CHARGE_LIMIT_MV);
    if (cm->full_voltage_mv > 0)
        cm->full_voltage_mv =
            qb_charge_voltage_limit(cm, cm->full_voltage_mv);

    QBLOG(0xd, "max_current_ma raw:%d effective:%d\n",
          raw_max_current_ma, cm->max_current_ma);
    QBLOG(0xf, "min_shutdown_mv raw:%d effective:%d\n",
          raw_min_shutdown_mv, cm->min_shutdown_mv);
    QBLOG(0x11, "max_pd_vbus_mv raw:%d effective:%d\n",
          raw_max_pd_vbus_mv, cm->max_pd_vbus_mv);
    QBLOG(0x13, "pd_full_mv raw:%d effective:%d\n",
          raw_pd_full_mv, cm->pd_full_mv);
    QBLOG(0x15, "charge_limit_mv raw:%d effective:%d\n",
          raw_charge_limit_mv, cm->charge_limit_mv);
}

static void qb_parse_config_line(struct qb_manager *cm, char *line)
{
    char key[64];
    char value[64];
    int parsed;

    while (isspace((unsigned char)*line))
        line++;
    if (sscanf(line, "option %63s %63s", key, value) != 2)
        return;
    qb_unquote_config(value);
    if (!qb_parse_config_int(value, &parsed))
        return;

    if (strcmp(key, "max_current_ma") == 0)
        cm->max_current_ma = parsed;
    else if (strcmp(key, "min_shutdown_mv") == 0)
        cm->min_shutdown_mv = parsed;
    else if (strcmp(key, "max_pd_vbus_mv") == 0)
        cm->max_pd_vbus_mv = parsed;
    else if (strcmp(key, "pd_full_mv") == 0)
        cm->pd_full_mv = parsed;
    else if (strcmp(key, "charge_limit_mv") == 0)
        cm->charge_limit_mv = parsed;
}

int qb_load_config_file(struct qb_manager *cm, const char *path)
{
    FILE *fp;
    char line[256];
    bool settings = false;

    fp = fopen(path, "r");
    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        char section_type[64];
        char section_name[64];
        char *p = line;
        int fields;

        while (isspace((unsigned char)*p))
            p++;
        fields = sscanf(p, "config %63s %63s", section_type, section_name);
        if (fields > 0) {
            qb_unquote_config(section_type);
            if (fields == 2)
                qb_unquote_config(section_name);
            settings = !strcmp(fields == 2 ? section_name : section_type, "settings");
            continue;
        }
        if (settings)
            qb_parse_config_line(cm, p);
    }
    fclose(fp);
    qb_validate_config(cm);
    return 0;
}

int qb_load_config(struct qb_manager *cm)
{
    return qb_load_config_file(cm, QB_UCI_CONFIG);
}
