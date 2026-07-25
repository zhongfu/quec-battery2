#include "qb.h"

#include <ctype.h>
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

static void qb_parse_config_line(struct qb_manager *cm, char *line)
{
    char key[64];
    char value[64];

    while (isspace((unsigned char)*line))
        line++;
    if (sscanf(line, "option %63s %63s", key, value) != 2)
        return;
    qb_unquote_config(value);
    if (strcmp(key, "max_current_ma") == 0)
        cm->max_current_ma = atoi(value);
    else if (strcmp(key, "min_shutdown_mv") == 0)
        cm->min_shutdown_mv = atoi(value);
    else if (strcmp(key, "max_pd_vbus_mv") == 0)
        cm->max_pd_vbus_mv = atoi(value);
    else if (strcmp(key, "pd_full_mv") == 0)
        cm->config_pd_full_mv = atoi(value);
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
    QBLOG(0xd, "bat_cfg_uci max_current_ma: %d\n", cm->max_current_ma);
    QBLOG(0xf, "bat_cfg_uci min_shutdown_mv: %d\n", cm->min_shutdown_mv);
    QBLOG(0x11, "bat_cfg_uci max_pd_vbus_mv: %d\n", cm->max_pd_vbus_mv);
    QBLOG(0x13, "bat_cfg_uci pd_full_mv: %d\n", cm->config_pd_full_mv);
    return 0;
}

int qb_load_config(struct qb_manager *cm)
{
    return qb_load_config_file(cm, QB_UCI_CONFIG);
}
