/* reshard_args.c - ddup-reshard command-line value parsing. */
#include "reshard_args.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int ascii_digits(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (*p == '\0')
        return 0;
    for (; *p != '\0'; p++)
        if (*p < (unsigned char)'0' || *p > (unsigned char)'9')
            return 0;
    return 1;
}

int reshard_parse_addr(const char *s, char *host, size_t hostcap,
                       uint16_t *port)
{
    const char *host_start = s;
    const char *port_start;
    size_t host_len;
    char *end;
    long p;

    if (s[0] == '[') {
        const char *close = strchr(s + 1, ']');

        if (close == NULL || close == s + 1 || close[1] != ':')
            return -1;
        host_start = s + 1;
        host_len = (size_t)(close - host_start);
        port_start = close + 2;
    } else {
        const char *colon = strrchr(s, ':');

        if (colon == NULL || colon == s)
            return -1;
        host_len = (size_t)(colon - s);
        port_start = colon + 1;
    }
    if (host_len >= hostcap)
        return -1;
    if (!ascii_digits(port_start))
        return -1;
    errno = 0;
    p = strtol(port_start, &end, 10);
    if (end == port_start || *end != '\0' || errno == ERANGE || p < 1 ||
        p > 65535)
        return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    *port = (uint16_t)p;
    return 0;
}

int reshard_parse_int(const char *s, int min_value, int max_value, int *out)
{
    char *end;
    long value;

    if (!ascii_digits(s))
        return -1;
    errno = 0;
    value = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || value < min_value ||
        value > max_value)
        return -1;
    *out = (int)value;
    return 0;
}
