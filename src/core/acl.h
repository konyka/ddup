#ifndef DDUP_ACL_H
#define DDUP_ACL_H

#include <stddef.h>
#include <stdint.h>

#include "core/command.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

#define ACL_MAX_USERS 32
#define ACL_MAX_PATTERNS 16
#define ACL_MAX_NAME 64
#define ACL_MAX_PASSWORD 128
#define ACL_MAX_PATTERN 128

typedef struct acl_user {
    char name[ACL_MAX_NAME];
    char password[ACL_MAX_PASSWORD];
    char patterns[ACL_MAX_PATTERNS][ACL_MAX_PATTERN];
    uint8_t pattern_count;
    uint64_t allow[CMD_STATS_SLOTS / 64];
    uint64_t deny[CMD_STATS_SLOTS / 64];
    int enabled;
    int all_commands;
    uint64_t generation;
} acl_user;

typedef struct acl_registry {
    acl_user users[ACL_MAX_USERS];
    uint8_t count;
    uint64_t generation_next;
} acl_registry;

void acl_init(acl_registry *r, const char *requirepass);
int acl_setuser(acl_registry *r, const char *name, size_t nlen,
                const resp_value *rules, size_t nrules);
int acl_deluser(acl_registry *r, const char *name, size_t nlen);
acl_user *acl_find(acl_registry *r, const char *name, size_t nlen);
const acl_user *acl_find_const(const acl_registry *r, const char *name,
                               size_t nlen);
const acl_user *acl_authenticate(const acl_registry *r, const char *name,
                                 size_t nlen, const char *password,
                                 size_t plen);
int acl_authorize(const acl_user *u, uint16_t cmd_id, const resp_value *argv,
                  size_t argc);
void acl_write_user(const acl_user *u, resp_buf *out);
void acl_write_rule_line(const acl_user *u, resp_buf *out);
int acl_match_pattern(const char *pat, size_t plen, const char *key,
                      size_t klen);

#endif
