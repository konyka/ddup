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

int main(void)
{
    DD_RUN(test_acl_users);
    DD_RUN(test_acl_default_password);
    DD_RUN(test_acl_atomic);
    return DD_TEST_SUMMARY();
}
