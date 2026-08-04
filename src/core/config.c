/* config.c - ddup-server configuration parsing; see config.h. */
#include "core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_init(ddup_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 6379;
    strcpy(cfg->bind, "0.0.0.0");
    cfg->maxmemory = 0;
    cfg->maxmemory_policy = DB_POLICY_ALLKEYS_LRU;
    strcpy(cfg->dir, ".");
    cfg->appendonly = 0;
    strcpy(cfg->appendfilename, "appendonly.aof");
    strcpy(cfg->dbfilename, "dump.ddr");
    cfg->save_sec = 0;
    cfg->replicaof_host[0] = '\0';
    cfg->replicaof_port = 0;
    cfg->repl_backlog_size = 1024 * 1024;
    cfg->tls_port = 0;
    cfg->tls_cert_file[0] = '\0';
    cfg->tls_key_file[0] = '\0';
    cfg->io[0] = '\0';
}

static int key_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int copy_str(char *dst, size_t cap, const char *src)
{
    size_t len = strlen(src);
    if (len == 0 || len >= cap)
        return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

static int parse_port(const char *v, uint16_t *out)
{
    char *end;
    long p = strtol(v, &end, 10);
    if (end == v || *end != '\0' || p <= 0 || p > 65535)
        return -1;
    *out = (uint16_t)p;
    return 0;
}

static int parse_u64(const char *v, uint64_t *out)
{
    char *end;
    unsigned long long x = strtoull(v, &end, 10);
    if (end == v || *end != '\0')
        return -1;
    *out = (uint64_t)x;
    return 0;
}

static int parse_bool(const char *v, int *out)
{
    if (key_eq(v, "yes")) {
        *out = 1;
        return 0;
    }
    if (key_eq(v, "no")) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_int_nonneg(const char *v, int *out)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v || *end != '\0' || x < 0 || x > 1000000)
        return -1;
    *out = (int)x;
    return 0;
}

int config_apply(ddup_config *cfg, const char *key, const char *value)
{
    if (key_eq(key, "port"))
        return parse_port(value, &cfg->port);
    if (key_eq(key, "bind"))
        return copy_str(cfg->bind, sizeof(cfg->bind), value);
    if (key_eq(key, "maxmemory"))
        return parse_u64(value, &cfg->maxmemory);
    if (key_eq(key, "maxmemory-policy")) {
        if (key_eq(value, "allkeys-lru")) {
            cfg->maxmemory_policy = DB_POLICY_ALLKEYS_LRU;
            return 0;
        }
        if (key_eq(value, "noeviction")) {
            cfg->maxmemory_policy = DB_POLICY_NOEVICTION;
            return 0;
        }
        return -1;
    }
    if (key_eq(key, "dir"))
        return copy_str(cfg->dir, sizeof(cfg->dir), value);
    if (key_eq(key, "appendonly"))
        return parse_bool(value, &cfg->appendonly);
    if (key_eq(key, "appendfilename"))
        return copy_str(cfg->appendfilename, sizeof(cfg->appendfilename),
                        value);
    if (key_eq(key, "dbfilename"))
        return copy_str(cfg->dbfilename, sizeof(cfg->dbfilename), value);
    if (key_eq(key, "save"))
        return parse_int_nonneg(value, &cfg->save_sec);
    if (key_eq(key, "replicaof")) {
        /* value is "<host> <port>" */
        char host[64];
        const char *sp = value;
        const char *space = NULL;
        size_t hl = 0;
        while (sp[hl] && sp[hl] != ' ' && sp[hl] != '\t')
            hl++;
        space = sp + hl;
        if (hl == 0 || hl >= sizeof(host) || *space == '\0')
            return -1;
        memcpy(host, sp, hl);
        host[hl] = '\0';
        while (*space == ' ' || *space == '\t')
            space++;
        if (parse_port(space, &cfg->replicaof_port) != 0)
            return -1;
        memcpy(cfg->replicaof_host, host, hl + 1);
        return 0;
    }
    if (key_eq(key, "repl-backlog-size"))
        return parse_u64(value, &cfg->repl_backlog_size);
    if (key_eq(key, "tls-port"))
        return parse_port(value, &cfg->tls_port);
    if (key_eq(key, "tls-cert-file"))
        return copy_str(cfg->tls_cert_file, sizeof(cfg->tls_cert_file),
                        value);
    if (key_eq(key, "tls-key-file"))
        return copy_str(cfg->tls_key_file, sizeof(cfg->tls_key_file),
                        value);
    if (key_eq(key, "io")) {
        if (!key_eq(value, "select") && !key_eq(value, "iocp"))
            return -1;
        return copy_str(cfg->io, sizeof(cfg->io), value);
    }
    return -1;
}

static int file_readable(const char *path)
{
    FILE *f;
    if (path[0] == '\0')
        return 0;
    f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

int config_validate(const ddup_config *cfg, char *err, size_t errcap)
{
    if (cfg->tls_port == 0)
        return 0;
    if (!file_readable(cfg->tls_cert_file)) {
        snprintf(err, errcap,
                 "tls-port is set but tls-cert-file '%s' is not readable",
                 cfg->tls_cert_file);
        return -1;
    }
    if (!file_readable(cfg->tls_key_file)) {
        snprintf(err, errcap,
                 "tls-port is set but tls-key-file '%s' is not readable",
                 cfg->tls_key_file);
        return -1;
    }
    return 0;
}

/* Trim leading/trailing ASCII whitespace in place; returns the start. */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while (end > s &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n'))
        *--end = '\0';
    return s;
}

int config_load_file(ddup_config *cfg, const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[1024];
    int lineno = 0;
    if (f == NULL) {
        fprintf(stderr, "ddup: cannot open config file '%s'\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p;
        char *key;
        char *value;
        lineno++;
        p = trim(line);
        if (*p == '\0' || *p == '#')
            continue;
        key = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p == '\0') {
            fprintf(stderr, "ddup: config line %d: missing value\n", lineno);
            fclose(f);
            return -1;
        }
        *p++ = '\0';
        value = trim(p);
        if (*value == '\0') {
            fprintf(stderr, "ddup: config line %d: missing value\n", lineno);
            fclose(f);
            return -1;
        }
        if (config_apply(cfg, key, value) != 0) {
            fprintf(stderr, "ddup: config line %d: bad directive '%s %s'\n",
                    lineno, key, value);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}
