/* config.h - ddup-server configuration (redis-style flat config file +
 * inline --key value overrides).
 */
#ifndef DDUP_CONFIG_H
#define DDUP_CONFIG_H

#include <stdint.h>

#include "core/command.h" /* DB_POLICY_* */

typedef struct ddup_config {
    uint16_t port;            /* default 6379 */
    char bind[64];            /* default "0.0.0.0" */
    uint64_t maxmemory;       /* bytes, 0 = unlimited */
    int maxmemory_policy;     /* DB_POLICY_* */
    char dir[512];            /* working dir for persistence files */
    int appendonly;           /* 0/1 */
    char appendfilename[256];
    char dbfilename[256];
    int save_sec;             /* snapshot interval, 0 = off */
    char replicaof_host[64];  /* "" = master role */
    uint16_t replicaof_port;  /* 0 = unset */
    uint64_t repl_backlog_size; /* replication backlog ring bytes */
    uint16_t tls_port;        /* 0 = off */
    char tls_cert_file[512];
    char tls_key_file[512];
    char io[16]; /* "" = auto (iocp on Windows, select elsewhere) */
    int cluster_enabled;           /* default 0 */
    char cluster_config_file[256]; /* default "nodes.conf" */
} ddup_config;

void config_init(ddup_config *cfg);

/* Parse a redis-style config file (key value lines, # comments,
 * case-insensitive keys). Returns 0 on success, -1 on error (message to
 * stderr). Unknown keys and invalid values are errors. */
int config_load_file(ddup_config *cfg, const char *path);

/* Apply one inline override (key without leading dashes, case-insensitive).
 * Returns 0 on success, -1 on unknown key or invalid value. */
int config_apply(ddup_config *cfg, const char *key, const char *value);

/* Cross-field validation (tls-port requires readable cert+key files).
 * Returns 0 ok, -1 and a message in err otherwise. */
int config_validate(const ddup_config *cfg, char *err, size_t errcap);

#endif /* DDUP_CONFIG_H */
