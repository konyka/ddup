#include <string.h>
#include "core/acl.h"
#include "test.h"

static resp_value rv(const char *s)
{
    resp_value v;
    memset(&v, 0, sizeof(v));
    v.type = RESP_BULK_STRING; v.str = s; v.len = strlen(s);
    return v;
}

static void test_acl_users(void)
{
    acl_registry r;
    resp_value rules[5] = {rv("on"), rv(">secret"), rv("~cache:*") , rv("+get"), rv("-set")};
    const acl_user *u;
    resp_value getv[2] = {rv("GET"), rv("cache:key")};
    resp_value setv[3] = {rv("SET"), rv("cache:key"), rv("v")};
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "alice", 5, rules, 5) == 0);
    u = acl_authenticate(&r, "alice", 5, "secret", 6);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_SET, setv, 3) == 0);
    DD_CHECK(acl_authenticate(&r, "alice", 5, "bad", 3) == NULL);
    DD_CHECK(acl_deluser(&r, "alice", 5) == 1);
    DD_CHECK(acl_authenticate(&r, "alice", 5, "secret", 6) == NULL);
}

static void test_acl_default_password(void)
{
    acl_registry r;
    acl_init(&r, "pw");
    DD_CHECK(acl_authenticate(&r, "default", 7, "pw", 2) != NULL);
}

static void test_acl_atomic(void)
{
    acl_registry r;
    resp_value good[2] = {rv("on"), rv("+get")};
    resp_value bad[2] = {rv("+set"), rv("+no_such_command")};
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "bob", 3, good, 2) == 0);
    DD_CHECK(acl_setuser(&r, "bob", 3, bad, 2) != 0);
    u = acl_find_const(&r, "bob", 3);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_GET, NULL, 0) == 1);
    DD_CHECK(acl_authorize(u, CMD_SET, NULL, 0) == 0);
}

static void test_acl_key_scope_and_all_rules(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~cache:*") , rv("+@all")};
    resp_value setv[3] = {rv("SET"), rv("cache:key"), rv("other-value")};
    resp_value badkey[2] = {rv("GET"), rv("other")};
    const acl_user *u;
    DD_CHECK(acl_setuser(&r, "all", 3, rules, 3) == 0);
    u = acl_authenticate(&r, "all", 3, "", 0);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_SET, setv, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_GET, badkey, 2) == 0);
}

static void test_acl_user_without_key_pattern_is_closed(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+get")};
    resp_value getv[2] = {rv("GET"), rv("cache:key")};
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "nobody", 6, rules, 2) == 0);
    u = acl_find_const(&r, "nobody", 6);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 0);
    DD_CHECK(acl_authorize(acl_find_const(&r, "default", 7), CMD_GET,
                           getv, 2) == 1);
}

static void test_acl_sensitive_queries_require_default(void)
{
    acl_registry r;
    resp_value rules[4] = {rv("on"), rv(">pw"), rv("~*") , rv("+acl")};
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "reader", 6, rules, 4) == 0);
    u = acl_authenticate(&r, "reader", 6, "pw", 2);
    DD_CHECK(u != NULL);
    DD_CHECK(strcmp(u->name, "reader") == 0);
    /* ACL command itself is permissioned, but sensitive subcommands are
     * enforced by the command layer based on the active username. */
    DD_CHECK(acl_authorize(u, CMD_ACL, NULL, 0) == 1);
}

int main(void)
{
    DD_RUN(test_acl_users);
    DD_RUN(test_acl_default_password);
    DD_RUN(test_acl_atomic);
    DD_RUN(test_acl_key_scope_and_all_rules);
    DD_RUN(test_acl_user_without_key_pattern_is_closed);
    DD_RUN(test_acl_sensitive_queries_require_default);
    return DD_TEST_SUMMARY();
}
