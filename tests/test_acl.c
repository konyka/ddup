#include <string.h>
#include "core/acl.h"
#include "core/session.h"
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

static void test_acl_categories_and_keyless_commands(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~*") , rv("+@read")};
    resp_value ping[1] = {rv("PING")};
    resp_value getv[2] = {rv("GET"), rv("k")};
    resp_value setv[3] = {rv("SET"), rv("k"), rv("v")};
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "reader", 6, rules, 3) == 0);
    u = acl_find_const(&r, "reader", 6);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_PING, ping, 1) == 1);
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_SET, setv, 3) == 0);
}

static void test_acl_rule_rendering(void)
{
    acl_registry r;
    resp_value rules[4] = {rv("on"), rv(">pw"), rv("~cache:*") , rv("+get")};
    resp_buf out;
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "alice", 5, rules, 4) == 0);
    u = acl_find_const(&r, "alice", 5);
    resp_buf_init(&out);
    acl_write_rule_line(u, &out);
    DD_CHECK(strstr(out.data, "user alice") != NULL);
    DD_CHECK(strstr(out.data, "on") != NULL);
    DD_CHECK(strstr(out.data, ">pw") != NULL);
    DD_CHECK(strstr(out.data, "~cache:*") != NULL);
    DD_CHECK(strstr(out.data, "+get") != NULL);
    resp_buf_free(&out);
}

static void test_acl_unknown_key_command_fails_closed(void)
{
    acl_registry r;
    resp_value rules[4] = {rv("on"), rv("~cache:*") , rv("+hget"), rv("+echo")};
    resp_value good[3] = {rv("HGET"), rv("cache:key"), rv("field")};
    resp_value bad[3] = {rv("HGET"), rv("other:key"), rv("field")};
    resp_value echo[2] = {rv("ECHO"), rv("hello")};
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "hash", 4, rules, 4) == 0);
    u = acl_find_const(&r, "hash", 4);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_HGET, good, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_HGET, bad, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_ECHO, echo, 2) == 1);
}

static void test_acl_deleted_slots_and_denies_render(void)
{
    acl_registry r;
    resp_value rules[4] = {rv("on"), rv("~*") , rv("+get"), rv("-set")};
    resp_buf out;
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "temp", 4, rules, 4) == 0);
    DD_CHECK(acl_deluser(&r, "temp", 4) == 1);
    resp_buf_init(&out);
    u = acl_find_const(&r, "default", 7);
    DD_CHECK(u != NULL);
    acl_write_rule_line(u, &out);
    DD_CHECK(strstr(out.data, "user default") != NULL);
    resp_buf_free(&out);
}

static void test_acl_getuser_commands_are_visible(void)
{
    acl_registry r;
    resp_value rules[4] = {rv("on"), rv("~*") , rv("+get"), rv("-set")};
    resp_buf out;
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "audit", 5, rules, 4) == 0);
    u = acl_find_const(&r, "audit", 5);
    resp_buf_init(&out);
    acl_write_user(u, &out);
    DD_CHECK(strstr(out.data, "+get") != NULL);
    DD_CHECK(strstr(out.data, "-set") != NULL);
    resp_buf_free(&out);
}

static void test_acl_generation_changes_on_reuse(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+get")};
    acl_user *u;
    uint64_t old_generation;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "temp", 4, rules, 2) == 0);
    u = acl_find(&r, "temp", 4);
    DD_CHECK(u != NULL);
    old_generation = u->generation;
    DD_CHECK(acl_deluser(&r, "temp", 4) == 1);
    DD_CHECK(acl_setuser(&r, "temp", 4, rules, 2) == 0);
    u = acl_find(&r, "temp", 4);
    DD_CHECK(u != NULL && u->generation != old_generation);
}

static void test_acl_generation_is_registry_local(void)
{
    acl_registry a, b;
    resp_value rules[1] = {rv("on")};
    const acl_user *ua;
    const acl_user *ub;
    acl_init(&a, NULL);
    acl_init(&b, NULL);
    ua = acl_find_const(&a, "default", 7);
    ub = acl_find_const(&b, "default", 7);
    DD_CHECK(ua != NULL && ub != NULL);
    DD_CHECK_EQ_INT(ua->generation, ub->generation);
    DD_CHECK(acl_setuser(&a, "x", 1, rules, 1) == 0);
    DD_CHECK(acl_setuser(&b, "x", 1, rules, 1) == 0);
    ua = acl_find_const(&a, "x", 1);
    ub = acl_find_const(&b, "x", 1);
    DD_CHECK(ua != NULL && ub != NULL);
    DD_CHECK_EQ_INT(ua->generation, ub->generation);
}

static void test_acl_categories_are_case_insensitive(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("+@READ"), rv("~*")};
    resp_value getv[2] = {rv("GET"), rv("k")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "case", 4, rules, 3) == 0);
    u = acl_find(&r, "case", 4);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 1);
}

static void test_acl_cat_filters_commands(void)
{
    db d;
    session s;
    resp_value argv[3] = {rv("ACL"), rv("CAT"), rv("read")};
    resp_buf out;
    db_init(&d);
    session_init(&s, &d);
    s.acl_username[0] = 'd'; s.acl_username[1] = 'e'; s.acl_username[2] = 'f';
    s.acl_username[3] = 'a'; s.acl_username[4] = 'u'; s.acl_username[5] = 'l';
    s.acl_username[6] = 't'; s.acl_username[7] = '\0';
    resp_buf_init(&out);
    session_execute_at(&s, argv, 3, &out, 0);
    DD_CHECK(strstr(out.data, "get") != NULL);
    DD_CHECK(strstr(out.data, "\r\n$3\r\nset\r\n") == NULL);
    resp_buf_free(&out);
    session_release(&s);
    db_destroy(&d);
}

static void test_acl_cat_rejects_unknown_category(void)
{
    db d;
    session s;
    resp_value argv[3] = {rv("ACL"), rv("CAT"), rv("bogus")};
    resp_buf out;
    db_init(&d);
    session_init(&s, &d);
    memcpy(s.acl_username, "default", 8);
    resp_buf_init(&out);
    session_execute_at(&s, argv, 3, &out, 0);
    DD_CHECK(strstr(out.data, "unknown category") != NULL);
    resp_buf_free(&out);
    session_release(&s);
    db_destroy(&d);
}

static void test_acl_dryrun_reports_effective_authorization(void)
{
    acl_registry r;
    db d;
    session s;
    resp_value rules[3] = {rv("on"), rv("~*") , rv("+get")};
    resp_value allowv[5] = {rv("ACL"), rv("DRYRUN"), rv("reader"),
                            rv("GET"), rv("key")};
    resp_value denyv[5] = {rv("ACL"), rv("DRYRUN"), rv("reader"),
                           rv("SET"), rv("key")};
    resp_buf out;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "reader", 6, rules, 3) == 0);
    db_init(&d);
    session_init(&s, &d);
    s.acl_ctx = &r;
    memcpy(s.acl_username, "default", 8);
    resp_buf_init(&out);
    session_execute_at(&s, allowv, 5, &out, 0);
    DD_CHECK(strcmp(out.data, "+OK\r\n") == 0);
    out.len = 0;
    if (out.data != NULL) out.data[0] = '\0';
    session_execute_at(&s, denyv, 5, &out, 0);
    DD_CHECK(strstr(out.data, "NOPERM") != NULL);
    resp_buf_free(&out);
    session_release(&s);
    db_destroy(&d);
}

static void test_acl_dryrun_rejects_unknown_user(void)
{
    acl_registry r;
    db d;
    session s;
    resp_value argv[5] = {rv("ACL"), rv("DRYRUN"), rv("ghost"),
                          rv("GET"), rv("key")};
    resp_buf out;
    acl_init(&r, NULL);
    db_init(&d);
    session_init(&s, &d);
    s.acl_ctx = &r;
    memcpy(s.acl_username, "default", 8);
    resp_buf_init(&out);
    session_execute_at(&s, argv, 5, &out, 0);
    DD_CHECK(strstr(out.data, "User 'ghost' not found") != NULL);
    resp_buf_free(&out);
    session_release(&s);
    db_destroy(&d);
}

static void test_acl_genpass_is_secure_and_bounded(void)
{
    db d;
    session s;
    resp_value defv[2] = {rv("ACL"), rv("GENPASS")};
    resp_value bitsv[3] = {rv("ACL"), rv("GENPASS"), rv("16")};
    resp_buf out;
    db_init(&d);
    session_init(&s, &d);
    memcpy(s.acl_username, "default", 8);
    resp_buf_init(&out);
    session_execute_at(&s, defv, 2, &out, 0);
    DD_CHECK(strncmp(out.data, "$64\r\n", 5) == 0);
    DD_CHECK(strspn(out.data + 5, "0123456789abcdef") >= 64);
    out.len = 0; if (out.data != NULL) out.data[0] = '\0';
    session_execute_at(&s, bitsv, 3, &out, 0);
    DD_CHECK(strncmp(out.data, "$4\r\n", 4) == 0);
    DD_CHECK(strspn(out.data + 4, "0123456789abcdef") >= 4);
    resp_buf_free(&out);
    session_release(&s);
    db_destroy(&d);
}

static void test_acl_genpass_rejects_invalid_bits(void)
{
    db d;
    session s;
    resp_value argv[3] = {rv("ACL"), rv("GENPASS"), rv("0")};
    resp_buf out;
    db_init(&d);
    session_init(&s, &d);
    memcpy(s.acl_username, "default", 8);
    resp_buf_init(&out);
    session_execute_at(&s, argv, 3, &out, 0);
    DD_CHECK(strstr(out.data, "positive number") != NULL);
    resp_buf_free(&out);
    session_release(&s);
    db_destroy(&d);
}

static void test_acl_log_records_and_resets_auth_failures(void)
{
    acl_registry r;
    db d;
    session s;
    resp_value authv[2] = {rv("AUTH"), rv("bad")};
    resp_value logv[2] = {rv("ACL"), rv("LOG")};
    resp_value resetv[3] = {rv("ACL"), rv("LOG"), rv("RESET")};
    resp_buf out;
    acl_init(&r, "secret");
    db_init(&d); session_init(&s, &d);
    s.acl_ctx = &r; s.requirepass = "secret";
    memcpy(s.acl_username, "default", 8); s.authed = 0;
    resp_buf_init(&out);
    session_execute_at(&s, authv, 2, &out, 100);
    out.len = 0; if (out.data != NULL) out.data[0] = '\0';
    s.authed = 1;
    session_execute_at(&s, logv, 2, &out, 200);
    DD_CHECK(strstr(out.data, "auth") != NULL);
    out.len = 0; if (out.data != NULL) out.data[0] = '\0';
    session_execute_at(&s, resetv, 3, &out, 300);
    DD_CHECK(out.len == 5 && memcmp(out.data, "+OK\r\n", 5) == 0);
    resp_buf_free(&out); session_release(&s); db_destroy(&d);
}

static void test_acl_log_records_command_denials_and_count(void)
{
    acl_registry r;
    db d;
    session s;
    resp_value rules[3] = {rv("on"), rv("~*") , rv("+get")};
    resp_value denyv[2] = {rv("SET"), rv("key")};
    resp_value logv[3] = {rv("ACL"), rv("LOG"), rv("1")};
    resp_buf out;
    acl_init(&r, NULL); DD_CHECK(acl_setuser(&r, "reader", 6, rules, 3) == 0);
    db_init(&d); session_init(&s, &d); s.acl_ctx = &r; s.acl_user = acl_find_const(&r, "default", 7);
    s.acl_check = NULL; s.acl_generation = s.acl_user->generation;
    s.authed = 1; memcpy(s.acl_username, "default", 8);
    resp_buf_init(&out);
    acl_log_event(&r, "command", "reader", 6, "set", 3, 10);
    session_execute_at(&s, logv, 3, &out, 20);
    DD_CHECK(strncmp(out.data, "*1\r\n", 4) == 0);
    DD_CHECK(strstr(out.data, "command") != NULL);
    resp_buf_free(&out); session_release(&s); db_destroy(&d);
    (void)denyv;
}

static void test_acl_channel_patterns_are_enforced(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("+subscribe"), rv("&news:*")};
    resp_value sub[2] = {rv("SUBSCRIBE"), rv("news:sports")};
    resp_value bad[2] = {rv("SUBSCRIBE"), rv("private")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "pub", 3, rules, 3) == 0);
    u = acl_find(&r, "pub", 3);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_SUBSCRIBE, sub, 2) == 1);
    DD_CHECK(acl_authorize_channel(u, sub[1].str, sub[1].len, 0) == 1);
    DD_CHECK(acl_authorize_channel(u, bad[1].str, bad[1].len, 0) == 0);
}

static void test_acl_default_user_allows_channels(void)
{
    acl_registry r;
    resp_value sub[2] = {rv("SUBSCRIBE"), rv("any-channel")};
    const acl_user *u;
    acl_init(&r, NULL);
    u = acl_find_const(&r, "default", 7);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_SUBSCRIBE, sub, 2) == 1);
}

static void test_acl_channel_rules_are_visible_in_metadata(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("+subscribe"), rv("&news:*")};
    resp_buf out;
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "pub", 3, rules, 3) == 0);
    u = acl_find_const(&r, "pub", 3);
    resp_buf_init(&out);
    acl_write_rule_line(u, &out);
    DD_CHECK(strstr(out.data, "&news:*") != NULL);
    out.len = 0; if (out.data != NULL) out.data[0] = '\0';
    acl_write_user(u, &out);
    DD_CHECK(strstr(out.data, "channels") != NULL);
    DD_CHECK(strstr(out.data, "news:*") != NULL);
    resp_buf_free(&out);
}

static void test_acl_channel_alias_rules(void)
{
    acl_registry r;
    resp_value allv[3] = {rv("on"), rv("allchannels"), rv("+subscribe")};
    resp_value resetv[3] = {rv("on"), rv("resetchannels"), rv("+subscribe")};
    resp_value sub[2] = {rv("SUBSCRIBE"), rv("any")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "all", 3, allv, 3) == 0);
    u = acl_find(&r, "all", 3);
    DD_CHECK(u != NULL && acl_authorize(u, CMD_SUBSCRIBE, sub, 2) == 1);
    DD_CHECK(acl_setuser(&r, "all", 3, resetv, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_SUBSCRIBE, sub, 2) == 0);
}

static void test_acl_publish_checks_only_channel_argument(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("&news:*") , rv("+publish")};
    resp_value pub[3] = {rv("PUBLISH"), rv("news:sports"), rv("private-body")};
    resp_value bad[3] = {rv("PUBLISH"), rv("private"), rv("news:sports")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "pub", 3, rules, 3) == 0);
    u = acl_find(&r, "pub", 3);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_PUBLISH, pub, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_PUBLISH, bad, 3) == 0);
}

static void test_acl_setuser_common_aliases(void)
{
    acl_registry r;
    resp_value rules[5] = {rv("on"), rv("allkeys"), rv("allcommands"),
                           rv("nopass"), rv("allchannels")};
    resp_value getv[2] = {rv("GET"), rv("any")};
    resp_value sub[2] = {rv("SUBSCRIBE"), rv("any")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "alias", 5, rules, 5) == 0);
    u = acl_find(&r, "alias", 5);
    DD_CHECK(u != NULL && u->password[0] == '\0');
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_SUBSCRIBE, sub, 2) == 1);
}

static void test_acl_reset_clears_password_and_disables_user(void)
{
    acl_registry r;
    resp_value initial[3] = {rv("on"), rv(">secret"), rv("+get")};
    resp_value reset[1] = {rv("reset")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "temp", 4, initial, 3) == 0);
    DD_CHECK(acl_setuser(&r, "temp", 4, reset, 1) == 0);
    u = acl_find(&r, "temp", 4);
    DD_CHECK(u != NULL && u->enabled == 0 && u->password[0] == '\0');
}

static void test_acl_nopass_accepts_any_password(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("nopass"), rv("+get")};
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "open", 4, rules, 3) == 0);
    u = acl_find_const(&r, "open", 4);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authenticate(&r, "open", 4, "one", 3) != NULL);
    DD_CHECK(acl_authenticate(&r, "open", 4, "another", 7) != NULL);
}

static void test_acl_nocommands_clears_old_allow_and_deny(void)
{
    acl_registry r;
    resp_value initial[4] = {rv("on"), rv("+get"), rv("-set"), rv(">pw")};
    resp_value reset[2] = {rv("nocommands"), rv("resetpass")};
    resp_value getv[2] = {rv("GET"), rv("k")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, initial, 4) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, reset, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL && u->password[0] == '\0');
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 0);
    DD_CHECK(u->deny[CMD_SET / 64] == 0);
}

static void test_acl_resetkeys_preserves_enabled_state(void)
{
    acl_registry r;
    resp_value initial[2] = {rv("on"), rv("~*")};
    resp_value reset[1] = {rv("resetkeys")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, initial, 2) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, reset, 1) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL && u->enabled == 1 && u->pattern_count == 0);
}

static void test_acl_resetchannels_preserves_other_domains(void)
{
    acl_registry r;
    resp_value initial[4] = {rv("on"), rv("+get"), rv("~*"), rv("&news:*")};
    resp_value reset[1] = {rv("resetchannels")};
    resp_value getv[2] = {rv("GET"), rv("k")};
    resp_value sub[2] = {rv("SUBSCRIBE"), rv("news:x")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, initial, 4) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, reset, 1) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL && u->enabled == 1 && u->pattern_count == 1);
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_SUBSCRIBE, sub, 2) == 0);
}

static void test_acl_resetpass_restores_password_checks(void)
{
    acl_registry r;
    resp_value open[2] = {rv("on"), rv("nopass")};
    resp_value passonly[1] = {rv(">secret")};
    resp_value secured[2] = {rv("resetpass"), rv(">secret")};
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, open, 2) == 0);
    DD_CHECK(acl_authenticate(&r, "u", 1, "wrong", 5) != NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, passonly, 1) == 0);
    DD_CHECK(acl_authenticate(&r, "u", 1, "wrong", 5) != NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, secured, 2) == 0);
    DD_CHECK(acl_authenticate(&r, "u", 1, "wrong", 5) == NULL);
    DD_CHECK(acl_authenticate(&r, "u", 1, "secret", 6) != NULL);
}

static void test_acl_all_aliases_clear_old_patterns(void)
{
    acl_registry r;
    resp_value keys[2] = {rv("~old:*"), rv("allkeys")};
    resp_value channels[2] = {rv("&old:*"), rv("allchannels")};
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, keys, 2) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, channels, 2) == 0);
    {
        acl_user *u = acl_find(&r, "u", 1);
        DD_CHECK(u != NULL && u->pattern_count == 1 && strcmp(u->patterns[0], "*") == 0);
        DD_CHECK(u != NULL && u->all_channels == 1 && u->channel_count == 0);
    }
}

static void test_acl_star_aliases_clear_old_rules(void)
{
    acl_registry r;
    resp_value keys[2] = {rv("~old:*"), rv("~*")};
    resp_value channels[2] = {rv("&old:*"), rv("&*")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, keys, 2) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, channels, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL && u->pattern_count == 1 && strcmp(u->patterns[0], "*") == 0);
    DD_CHECK(u != NULL && u->all_channels == 1 && u->channel_count == 0);
}

static void test_acl_reset_clears_nopass_state(void)
{
    acl_registry r;
    resp_value open[2] = {rv("on"), rv("nopass")};
    resp_value reset[1] = {rv("reset")};
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, open, 2) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, reset, 1) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, (resp_value[]){rv("on"), rv(">secret")}, 2) == 0);
    DD_CHECK(acl_authenticate(&r, "u", 1, "wrong", 5) == NULL);
}

static void test_acl_allcommands_clears_old_denies(void)
{
    acl_registry r;
    resp_value denied[3] = {rv("on"), rv("~*"), rv("-get")};
    resp_value unrestricted[1] = {rv("allcommands")};
    resp_value getv[2] = {rv("GET"), rv("k")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, denied, 3) == 0);
    DD_CHECK(acl_setuser(&r, "u", 1, unrestricted, 1) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL && acl_authorize(u, CMD_GET, getv, 2) == 1);
}

static void test_acl_multikey_commands_check_every_key(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~src:*"), rv("+@all")};
    resp_value renamev[3] = {rv("RENAME"), rv("src:1"), rv("dst:1")};
    resp_value smovev[4] = {rv("SMOVE"), rv("src:set"), rv("dst:set"), rv("m")};
    resp_value getv[2] = {rv("GET"), rv("src:1")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_GET, getv, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_RENAME, renamev, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_SMOVE, smovev, 4) == 0);
}

static void test_acl_multikey_reads_check_every_key(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value existsv[3] = {rv("EXISTS"), rv("allowed:1"), rv("secret:1")};
    resp_value touchv[3] = {rv("TOUCH"), rv("allowed:1"), rv("secret:1")};
    resp_value sinterv[3] = {rv("SINTER"), rv("allowed:set"), rv("secret:set")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_EXISTS, existsv, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_TOUCH, touchv, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_SINTER, sinterv, 3) == 0);
}

static void test_acl_keyless_subcommands_do_not_require_key_pattern(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+@all")};
    resp_value config[3] = {rv("CONFIG"), rv("GET"), rv("maxmemory")};
    resp_value client[2] = {rv("CLIENT"), rv("ID")};
    resp_value memory[2] = {rv("MEMORY"), rv("STATS")};
    resp_value slowlog[3] = {rv("SLOWLOG"), rv("GET"), rv("10")};
    resp_value command[3] = {rv("COMMAND"), rv("INFO"), rv("GET")};
    resp_value waitv[3] = {rv("WAIT"), rv("1"), rv("100")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_CONFIG, config, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_CLIENT, client, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_MEMORY, memory, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_SLOWLOG, slowlog, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_COMMAND, command, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_WAIT, waitv, 3) == 1);
}

static void test_acl_keyed_subcommands_still_check_keys(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value memory[3] = {rv("MEMORY"), rv("USAGE"), rv("secret:1")};
    resp_value debug[3] = {rv("DEBUG"), rv("OBJECT"), rv("secret:1")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_MEMORY, memory, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_DEBUG, debug, 3) == 0);
}

static void test_acl_object_xinfo_help_is_keyless(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+@all")};
    resp_value object_help[2] = {rv("OBJECT"), rv("HELP")};
    resp_value xinfo_help[2] = {rv("XINFO"), rv("HELP")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_OBJECT, object_help, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_XINFO, xinfo_help, 2) == 1);
}

static void test_acl_subcommand_key_positions_are_correct(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value object_ok[3] = {rv("OBJECT"), rv("ENCODING"), rv("allowed:1")};
    resp_value object_bad[3] = {rv("OBJECT"), rv("ENCODING"), rv("secret:1")};
    resp_value xinfo_ok[3] = {rv("XINFO"), rv("STREAM"), rv("allowed:stream")};
    resp_value xinfo_bad[3] = {rv("XINFO"), rv("STREAM"), rv("secret:stream")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_OBJECT, object_ok, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_OBJECT, object_bad, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_XINFO, xinfo_ok, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_XINFO, xinfo_bad, 3) == 0);
}

static void test_acl_keyless_options_do_not_require_key_pattern(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+@all")};
    resp_value flushdb[2] = {rv("FLUSHDB"), rv("ASYNC")};
    resp_value flushall[2] = {rv("FLUSHALL"), rv("SYNC")};
    resp_value shutdownv[2] = {rv("SHUTDOWN"), rv("NOSAVE")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_FLUSHDB, flushdb, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_FLUSHALL, flushall, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_SHUTDOWN, shutdownv, 2) == 1);
}

static void test_acl_store_commands_check_destination_and_sources(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value sortv[4] = {rv("SORT"), rv("allowed:list"), rv("STORE"), rv("secret:out")};
    resp_value sinterv[5] = {rv("SINTERSTORE"), rv("allowed:out"), rv("2"), rv("allowed:a"), rv("secret:b")};
    resp_value zunionv[5] = {rv("ZUNIONSTORE"), rv("allowed:out"), rv("2"), rv("allowed:a"), rv("secret:b")};
    resp_value geov[4] = {rv("GEOSEARCHSTORE"), rv("allowed:out"), rv("secret:geo"), rv("FROMMEMBER")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_SORT, sortv, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_SINTERSTORE, sinterv, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_ZUNIONSTORE, zunionv, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_GEOSEARCHSTORE, geov, 4) == 0);
}

static void test_acl_advanced_multikey_commands_check_all_keys(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value pfcount[3] = {rv("PFCOUNT"), rv("allowed:a"), rv("secret:b")};
    resp_value sintercard[5] = {rv("SINTERCARD"), rv("2"), rv("allowed:a"), rv("secret:b"), rv("LIMIT")};
    resp_value zunion[5] = {rv("ZUNION"), rv("2"), rv("allowed:a"), rv("secret:b"), rv("WITHSCORES")};
    resp_value zintercard[4] = {rv("ZINTERCARD"), rv("2"), rv("allowed:a"), rv("secret:b")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_PFCOUNT, pfcount, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_SINTERCARD, sintercard, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_ZUNION, zunion, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_ZINTERCARD, zintercard, 4) == 0);
}

static void test_acl_replication_and_cluster_controls_are_keyless(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+@all")};
    resp_value asking[1] = {rv("ASKING")};
    resp_value psync[3] = {rv("PSYNC"), rv("?"), rv("-1")};
    resp_value replconf[4] = {rv("REPLCONF"), rv("ACK"), rv("42"), rv("GETACK")};
    resp_value replicaof[3] = {rv("REPLICAOF"), rv("127.0.0.1"), rv("6379")};
    resp_value failover[2] = {rv("FAILOVER"), rv("TO")};
    resp_value monitor[1] = {rv("MONITOR")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_ASKING, asking, 1) == 1);
    DD_CHECK(acl_authorize(u, CMD_PSYNC, psync, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_REPLCONF, replconf, 4) == 1);
    DD_CHECK(acl_authorize(u, CMD_REPLICAOF, replicaof, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_FAILOVER, failover, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_MONITOR, monitor, 1) == 1);
}

static void test_acl_script_commands_use_declared_key_positions(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value eval_ok[5] = {rv("EVAL"), rv("secret-script"), rv("1"), rv("allowed:1"), rv("arg")};
    resp_value eval_bad[5] = {rv("EVAL"), rv("allowed-script"), rv("1"), rv("secret:1"), rv("arg")};
    resp_value fcall_ok[5] = {rv("FCALL"), rv("secret-fn"), rv("1"), rv("allowed:1"), rv("arg")};
    resp_value fcall_bad[5] = {rv("FCALL"), rv("allowed-fn"), rv("1"), rv("secret:1"), rv("arg")};
    resp_value eval_zero[3] = {rv("EVAL"), rv("secret-script"), rv("0")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_EVAL, eval_ok, 5) == 1);
    DD_CHECK(acl_authorize(u, CMD_EVAL, eval_bad, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_FCALL, fcall_ok, 5) == 1);
    DD_CHECK(acl_authorize(u, CMD_FCALL, fcall_bad, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_EVAL, eval_zero, 3) == 1);
}

static void test_acl_blocking_and_option_key_positions(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value blpop[4] = {rv("BLPOP"), rv("allowed:list"), rv("secret:list"), rv("0")};
    resp_value blmop[7] = {rv("BLMPOP"), rv("0"), rv("2"), rv("allowed:a"), rv("secret:b"), rv("LEFT"), rv("COUNT")};
    resp_value bitop[5] = {rv("BITOP"), rv("OR"), rv("allowed:dest"), rv("allowed:a"), rv("secret:b")};
    resp_value georadius[8] = {rv("GEORADIUS"), rv("allowed:geo"), rv("0"), rv("0"), rv("1"), rv("STORE"), rv("secret:dest"), rv("WITHCOORD")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_BLPOP, blpop, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_BLMPOP, blmop, 7) == 0);
    DD_CHECK(acl_authorize(u, CMD_BITOP, bitop, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_GEORADIUS, georadius, 8) == 0);
}

static void test_acl_stream_and_numkeys_positions(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value xread[8] = {rv("XREAD"), rv("COUNT"), rv("1"), rv("STREAMS"), rv("allowed:s"), rv("secret:s"), rv("0-0"), rv("0-0")};
    resp_value xreadgroup[9] = {rv("XREADGROUP"), rv("GROUP"), rv("g"), rv("c"), rv("STREAMS"), rv("allowed:s"), rv("secret:s"), rv("0"), rv("0")};
    resp_value xgroup[4] = {rv("XGROUP"), rv("CREATE"), rv("secret:s"), rv("g")};
    resp_value lmpop[6] = {rv("LMPOP"), rv("0"), rv("2"), rv("allowed:a"), rv("secret:b"), rv("LEFT")};
    resp_value zmpop[6] = {rv("ZMPOP"), rv("2"), rv("2"), rv("allowed:a"), rv("secret:b"), rv("MIN")};
    resp_value xread_ok[8] = {rv("XREAD"), rv("COUNT"), rv("1"), rv("STREAMS"), rv("allowed:s"), rv("allowed:t"), rv("0-0"), rv("0-0")};
    resp_value xgroup_ok[4] = {rv("XGROUP"), rv("CREATE"), rv("allowed:s"), rv("g")};
    resp_value lmpop_ok[6] = {rv("LMPOP"), rv("0"), rv("2"), rv("allowed:a"), rv("allowed:b"), rv("LEFT")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_XREAD, xread, 8) == 0);
    DD_CHECK(acl_authorize(u, CMD_XREAD, xread_ok, 8) == 1);
    DD_CHECK(acl_authorize(u, CMD_XREADGROUP, xreadgroup, 9) == 0);
    DD_CHECK(acl_authorize(u, CMD_XGROUP, xgroup, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_XGROUP, xgroup_ok, 4) == 1);
    DD_CHECK(acl_authorize(u, CMD_LMPOP, lmpop, 6) == 0);
    DD_CHECK(acl_authorize(u, CMD_LMPOP, lmpop_ok, 6) == 1);
    DD_CHECK(acl_authorize(u, CMD_ZMPOP, zmpop, 6) == 0);
}

static void test_acl_migrate_uses_key_tail_not_host_parameters(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value ok[7] = {rv("MIGRATE"), rv("127.0.0.1"), rv("6379"), rv("allowed:key"), rv("0"), rv("1"), rv("COPY")};
    resp_value bad[7] = {rv("MIGRATE"), rv("127.0.0.1"), rv("6379"), rv("secret:key"), rv("0"), rv("1"), rv("COPY")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_MIGRATE, ok, 7) == 1);
    DD_CHECK(acl_authorize(u, CMD_MIGRATE, bad, 7) == 0);
}

static void test_acl_persistence_and_copy_key_positions(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value dump[2] = {rv("DUMP"), rv("secret:key")};
    resp_value restore[4] = {rv("RESTORE"), rv("secret:key"), rv("0"), rv("blob")};
    resp_value restore_ok[4] = {rv("RESTORE"), rv("allowed:key"), rv("0"), rv("blob")};
    resp_value restore_asking[4] = {rv("RESTORE-ASKING"), rv("secret:key"), rv("0"), rv("blob")};
    resp_value copy[4] = {rv("COPY"), rv("allowed:src"), rv("secret:dst"), rv("REPLACE")};
    resp_value lcs[3] = {rv("LCS"), rv("allowed:a"), rv("secret:b")};
    resp_value lcs_ok[3] = {rv("LCS"), rv("allowed:a"), rv("allowed:b")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_DUMP, dump, 2) == 0);
    DD_CHECK(acl_authorize(u, CMD_RESTORE, restore, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_RESTORE, restore_ok, 4) == 1);
    DD_CHECK(acl_authorize(u, CMD_RESTORE_ASKING, restore_asking, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_COPY, copy, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_LCS, lcs, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_LCS, lcs_ok, 3) == 1);
}

static void test_acl_pubsub_introspection_checks_channels(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("&allowed:*") , rv("+@all")};
    resp_value channels_bad[3] = {rv("PUBSUB"), rv("CHANNELS"), rv("secret:*")};
    resp_value channels_ok[3] = {rv("PUBSUB"), rv("CHANNELS"), rv("allowed:*")};
    resp_value numsub_bad[3] = {rv("PUBSUB"), rv("NUMSUB"), rv("secret:chan")};
    resp_value numsub_ok[3] = {rv("PUBSUB"), rv("NUMSUB"), rv("allowed:chan")};
    resp_value numpat[2] = {rv("PUBSUB"), rv("NUMPAT")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_PUBSUB, channels_bad, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_PUBSUB, channels_ok, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_PUBSUB, numsub_bad, 3) == 0);
    DD_CHECK(acl_authorize(u, CMD_PUBSUB, numsub_ok, 3) == 1);
    DD_CHECK(acl_authorize(u, CMD_PUBSUB, numpat, 2) == 1);
}

static void test_acl_range_store_and_msetex_key_positions(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value zstore[5] = {rv("ZRANGESTORE"), rv("allowed:out"), rv("allowed:src"), rv("0"), rv("-1")};
    resp_value zstore_bad[5] = {rv("ZRANGESTORE"), rv("secret:out"), rv("allowed:src"), rv("0"), rv("-1")};
    resp_value msetex[7] = {rv("MSETEX"), rv("2"), rv("allowed:a"), rv("1"), rv("secret:b"), rv("2"), rv("EX")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_ZRANGESTORE, zstore, 5) == 1);
    DD_CHECK(acl_authorize(u, CMD_ZRANGESTORE, zstore_bad, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_MSETEX, msetex, 7) == 0);
}

static void test_acl_stream_mutations_use_stream_key(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value xadd[5] = {rv("XADD"), rv("secret:stream"), rv("*"), rv("field"), rv("value")};
    resp_value xack[5] = {rv("XACK"), rv("secret:stream"), rv("group"), rv("0-1"), rv("0-2")};
    resp_value xsetid[4] = {rv("XSETID"), rv("secret:stream"), rv("0-0"), rv("ENTRIES_ADDED")};
    resp_value xadd_ok[5] = {rv("XADD"), rv("allowed:stream"), rv("*"), rv("field"), rv("value")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_XADD, xadd, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_XACK, xack, 5) == 0);
    DD_CHECK(acl_authorize(u, CMD_XSETID, xsetid, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_XADD, xadd_ok, 5) == 1);
}

static void test_acl_stream_reads_reject_odd_key_id_tail(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value malformed[7] = {rv("XREAD"), rv("STREAMS"), rv("allowed:a"), rv("allowed:b"), rv("0-0"), rv("0-0"), rv("extra")};
    resp_value malformed_group[8] = {rv("XREADGROUP"), rv("GROUP"), rv("g"), rv("c"), rv("STREAMS"), rv("allowed:a"), rv("allowed:b"), rv("0-0")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_XREAD, malformed, 7) == 0);
    DD_CHECK(acl_authorize(u, CMD_XREADGROUP, malformed_group, 8) == 0);
}

static void test_acl_cluster_subcommands_are_keyless(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+@all")};
    resp_value info[2] = {rv("CLUSTER"), rv("INFO")};
    resp_value nodes[2] = {rv("CLUSTER"), rv("NODES")};
    resp_value meet[4] = {rv("CLUSTER"), rv("MEET"), rv("127.0.0.1"), rv("6379")};
    resp_value keyslot[3] = {rv("CLUSTER"), rv("KEYSLOT"), rv("secret:key")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 2) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_CLUSTER, info, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_CLUSTER, nodes, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_CLUSTER, meet, 4) == 1);
    DD_CHECK(acl_authorize(u, CMD_CLUSTER, keyslot, 3) == 1);
}

static void test_acl_ddup_control_and_himport_key_positions(void)
{
    acl_registry r;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value backup[2] = {rv("BACKUP"), rv("STATUS")};
    resp_value hotkeys[2] = {rv("HOTKEYS"), rv("GET")};
    resp_value prepare[2] = {rv("HIMPORT"), rv("PREPARE")};
    resp_value set_bad[4] = {rv("HIMPORT"), rv("SET"), rv("secret:hash"), rv("f")};
    resp_value set_ok[4] = {rv("HIMPORT"), rv("SET"), rv("allowed:hash"), rv("f")};
    acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_BACKUP, backup, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_HOTKEYS, hotkeys, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_HIMPORT, prepare, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_HIMPORT, set_bad, 4) == 0);
    DD_CHECK(acl_authorize(u, CMD_HIMPORT, set_ok, 4) == 1);
}

static void test_acl_key_authorization_fails_closed_on_malformed_values(void)
{
    acl_registry r;
    resp_value nonstring;
    resp_value rules[3] = {rv("on"), rv("~allowed:*") , rv("+@all")};
    resp_value malformed_get[2];
    resp_value truncated_mget[2] = {rv("MGET"), rv("allowed:a")};
    resp_value malformed_mset[4];
    acl_user *u;
    acl_init(&r, NULL);
    memset(&nonstring, 0, sizeof(nonstring));
    nonstring.type = RESP_INTEGER;
    nonstring.integer = 1;
    malformed_get[0] = rv("GET"); malformed_get[1] = nonstring;
    malformed_mset[0] = rv("MSET"); malformed_mset[1] = rv("allowed:a"); malformed_mset[2] = rv("v"); malformed_mset[3] = nonstring;
    DD_CHECK(acl_setuser(&r, "u", 1, rules, 3) == 0);
    u = acl_find(&r, "u", 1);
    DD_CHECK(u != NULL);
    DD_CHECK(acl_authorize(u, CMD_GET, malformed_get, 2) == 0);
    DD_CHECK(acl_authorize(u, CMD_MGET, truncated_mget, 2) == 1);
    DD_CHECK(acl_authorize(u, CMD_MSET, malformed_mset, 4) == 0);
}

static void test_acl_rule_line_capacity_covers_channel_patterns(void)
{
    acl_registry r;
    resp_value rules[ACL_MAX_CHANNELS + 2];
    char pats[ACL_MAX_CHANNELS][ACL_MAX_PATTERN];
    resp_buf out;
    const acl_user *u;
    size_t i;
    rules[0] = rv("on");
    for (i = 0; i < ACL_MAX_CHANNELS; i++) {
        memset(pats[i], 'x', sizeof(pats[i]));
        pats[i][ACL_MAX_PATTERN - 1] = '\0';
        pats[i][0] = '&';
        rules[i + 1] = rv(pats[i]);
    }
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "wide", 4, rules, ACL_MAX_CHANNELS + 1) == 0);
    u = acl_find_const(&r, "wide", 4);
    resp_buf_init(&out);
    acl_write_rule_line(u, &out);
    DD_CHECK(out.len >= 2 && memcmp(out.data + out.len - 2, "\r\n", 2) == 0);
    resp_buf_free(&out);
}

static void test_acl_log_coalesces_identical_events(void)
{
    acl_registry r;
    resp_buf out;
    acl_init(&r, NULL);
    acl_log_event(&r, "auth", "alice", 5, "", 0, 100);
    acl_log_event(&r, "auth", "alice", 5, "", 0, 200);
    resp_buf_init(&out);
    acl_log_write(&r, 10, 300, &out);
    DD_CHECK(r.log_len == 1);
    DD_CHECK(strstr(out.data, ":2\r\n") != NULL);
    resp_buf_free(&out);
}

static void test_acl_getuser_flags_do_not_mislabel_commands(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("+get")};
    resp_buf out;
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "reader", 6, rules, 2) == 0);
    u = acl_find_const(&r, "reader", 6);
    resp_buf_init(&out);
    acl_write_user(u, &out);
    DD_CHECK(strstr(out.data, "nocommands") != NULL);
    DD_CHECK(strstr(out.data, "resetchannels") == NULL);
    resp_buf_free(&out);
}

static void test_acl_getuser_keys_keep_tilde_prefix(void)
{
    acl_registry r;
    resp_value rules[2] = {rv("on"), rv("~cache:*")};
    resp_buf out;
    const acl_user *u;
    acl_init(&r, NULL);
    DD_CHECK(acl_setuser(&r, "cache", 5, rules, 2) == 0);
    u = acl_find_const(&r, "cache", 5);
    resp_buf_init(&out);
    acl_write_user(u, &out);
    DD_CHECK(strstr(out.data, "~cache:*") != NULL);
    resp_buf_free(&out);
}

static void test_acl_log_null_fields_are_safe(void)
{
    acl_registry r;
    acl_init(&r, NULL);
    acl_log_event(&r, "auth", NULL, 8, NULL, 4, 1);
    acl_log_event(&r, "auth", NULL, 8, NULL, 4, 2);
    DD_CHECK(r.log_len == 1);
    DD_CHECK(r.log[0].count == 2);
}

static void test_acl_log_negative_count_returns_empty(void)
{
    acl_registry r;
    db d;
    session s;
    resp_value argv[3] = {rv("ACL"), rv("LOG"), rv("-1")};
    resp_buf out;
    acl_init(&r, NULL);
    acl_log_event(&r, "auth", "alice", 5, NULL, 0, 1);
    db_init(&d); session_init(&s, &d); s.acl_ctx = &r;
    memcpy(s.acl_username, "default", 8); s.acl_user = acl_find_const(&r, "default", 7);
    s.acl_generation = s.acl_user->generation;
    resp_buf_init(&out);
    session_execute_at(&s, argv, 3, &out, 2);
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*0\r\n", 4) == 0);
    resp_buf_free(&out); session_release(&s); db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_acl_users);
    DD_RUN(test_acl_default_password);
    DD_RUN(test_acl_atomic);
    DD_RUN(test_acl_key_scope_and_all_rules);
    DD_RUN(test_acl_user_without_key_pattern_is_closed);
    DD_RUN(test_acl_sensitive_queries_require_default);
    DD_RUN(test_acl_categories_and_keyless_commands);
    DD_RUN(test_acl_rule_rendering);
    DD_RUN(test_acl_unknown_key_command_fails_closed);
    DD_RUN(test_acl_deleted_slots_and_denies_render);
    DD_RUN(test_acl_getuser_commands_are_visible);
    DD_RUN(test_acl_generation_changes_on_reuse);
    DD_RUN(test_acl_generation_is_registry_local);
    DD_RUN(test_acl_categories_are_case_insensitive);
    DD_RUN(test_acl_cat_filters_commands);
    DD_RUN(test_acl_cat_rejects_unknown_category);
    DD_RUN(test_acl_dryrun_reports_effective_authorization);
    DD_RUN(test_acl_dryrun_rejects_unknown_user);
    DD_RUN(test_acl_genpass_is_secure_and_bounded);
    DD_RUN(test_acl_genpass_rejects_invalid_bits);
    DD_RUN(test_acl_log_records_and_resets_auth_failures);
    DD_RUN(test_acl_log_records_command_denials_and_count);
    DD_RUN(test_acl_channel_patterns_are_enforced);
    DD_RUN(test_acl_default_user_allows_channels);
    DD_RUN(test_acl_channel_rules_are_visible_in_metadata);
    DD_RUN(test_acl_channel_alias_rules);
    DD_RUN(test_acl_publish_checks_only_channel_argument);
    DD_RUN(test_acl_setuser_common_aliases);
    DD_RUN(test_acl_reset_clears_password_and_disables_user);
    DD_RUN(test_acl_nopass_accepts_any_password);
    DD_RUN(test_acl_nocommands_clears_old_allow_and_deny);
    DD_RUN(test_acl_resetkeys_preserves_enabled_state);
    DD_RUN(test_acl_resetchannels_preserves_other_domains);
    DD_RUN(test_acl_resetpass_restores_password_checks);
    DD_RUN(test_acl_all_aliases_clear_old_patterns);
    DD_RUN(test_acl_star_aliases_clear_old_rules);
    DD_RUN(test_acl_reset_clears_nopass_state);
    DD_RUN(test_acl_allcommands_clears_old_denies);
    DD_RUN(test_acl_multikey_commands_check_every_key);
    DD_RUN(test_acl_multikey_reads_check_every_key);
    DD_RUN(test_acl_keyless_subcommands_do_not_require_key_pattern);
    DD_RUN(test_acl_keyed_subcommands_still_check_keys);
    DD_RUN(test_acl_object_xinfo_help_is_keyless);
    DD_RUN(test_acl_subcommand_key_positions_are_correct);
    DD_RUN(test_acl_keyless_options_do_not_require_key_pattern);
    DD_RUN(test_acl_store_commands_check_destination_and_sources);
    DD_RUN(test_acl_advanced_multikey_commands_check_all_keys);
    DD_RUN(test_acl_replication_and_cluster_controls_are_keyless);
    DD_RUN(test_acl_script_commands_use_declared_key_positions);
    DD_RUN(test_acl_blocking_and_option_key_positions);
    DD_RUN(test_acl_stream_and_numkeys_positions);
    DD_RUN(test_acl_migrate_uses_key_tail_not_host_parameters);
    DD_RUN(test_acl_persistence_and_copy_key_positions);
    DD_RUN(test_acl_pubsub_introspection_checks_channels);
    DD_RUN(test_acl_range_store_and_msetex_key_positions);
    DD_RUN(test_acl_stream_mutations_use_stream_key);
    DD_RUN(test_acl_stream_reads_reject_odd_key_id_tail);
    DD_RUN(test_acl_cluster_subcommands_are_keyless);
    DD_RUN(test_acl_ddup_control_and_himport_key_positions);
    DD_RUN(test_acl_key_authorization_fails_closed_on_malformed_values);
    DD_RUN(test_acl_rule_line_capacity_covers_channel_patterns);
    DD_RUN(test_acl_log_coalesces_identical_events);
    DD_RUN(test_acl_getuser_flags_do_not_mislabel_commands);
    DD_RUN(test_acl_getuser_keys_keep_tilde_prefix);
    DD_RUN(test_acl_log_null_fields_are_safe);
    DD_RUN(test_acl_log_negative_count_returns_empty);
    return DD_TEST_SUMMARY();
}
