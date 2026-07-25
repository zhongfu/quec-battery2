#include "qb.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

volatile int qb_power_limit_state;

void qb_log(const char *func, int line, const char *fmt, ...)
{
    char message[768];
    va_list ap;

    if (access(QB_LOG_FLAG, F_OK) != 0)
        return;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    syslog(LOG_INFO, "[quec_battery-re][%s][%d] : %s", func, line, message);
}

static int qb_path(char *out, size_t size, const char *dir, const char *attr)
{
    int n = snprintf(out, size, "%s%s", dir, attr);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

int qb_read_str(const char *dir, const char *attr, char *buf, size_t size)
{
    char path[256];
    ssize_t n;
    int fd;

    if (size < 2 || qb_path(path, sizeof(path), dir, attr) < 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n')
        buf[--n] = '\0';
    return (int)n;
}

int qb_read_int(const char *dir, const char *attr, int *value)
{
    char buf[32];
    char *end;
    long parsed;

    if (qb_read_str(dir, attr, buf, sizeof(buf)) < 0) {
        QBLOG(0x206, "%s%s not exist\n", dir, attr);
        return -1;
    }
    errno = 0;
    parsed = strtol(buf, &end, 10);
    while (*end == ' ' || *end == '\r' || *end == '\n')
        end++;
    if (errno || end == buf || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        QBLOG(0x21b, "Conversion failed, non-numeric data found: %s\n", end);
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

int qb_write_str(const char *dir, const char *attr, const char *value)
{
    char path[256];
    size_t len = strlen(value);
    ssize_t n;
    int fd;

    if (qb_path(path, sizeof(path), dir, attr) < 0)
        return -1;
    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = write(fd, value, len);
    close(fd);
    return n == (ssize_t)len ? (int)n : -1;
}

int qb_write_int(const char *dir, const char *attr, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return qb_write_str(dir, attr, buf);
}

int qb_update_register(const char *dir, unsigned reg, unsigned mask,
                       unsigned value)
{
    char dump[1024];
    char request[32];
    char *line;
    char *saveptr = NULL;
    unsigned found_reg;
    unsigned old_value;
    unsigned new_value;

    if (reg > 0xff || mask > 0xff || value > 0xff ||
        qb_read_str(dir, "registers", dump, sizeof(dump)) < 0)
        return -1;

    for (line = strtok_r(dump, "\n", &saveptr); line;
         line = strtok_r(NULL, "\n", &saveptr)) {
        if (sscanf(line, "Reg[%x] = 0x%x", &found_reg, &old_value) != 2 ||
            found_reg != reg)
            continue;
        if (old_value > 0xff)
            return -1;
        new_value = (old_value & ~mask) | (value & mask);
        if (new_value == old_value)
            return 0;
        snprintf(request, sizeof(request), "0x%02x 0x%02x", reg, new_value);
        return qb_write_str(dir, "registers", request) < 0 ? -1 : 0;
    }
    return -1;
}
