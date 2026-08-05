/* test_cmdid.c - tests for the command ID table and resolver. */
#include "test.h"

#include "core/command.h"
#include <string.h>

static void test_resolve_known_command(void)
{
    DD_CHECK(cmd_resolve("GET", 3) != CMD_ID_UNKNOWN);
    DD_CHECK(cmd_resolve("get", 3) == cmd_resolve("GET", 3));
    DD_CHECK(cmd_resolve("Get", 3) == cmd_resolve("GET", 3));
    DD_CHECK(cmd_resolve("SET", 3) != CMD_ID_UNKNOWN);
    DD_CHECK(cmd_resolve("CLUSTER", 7) != CMD_ID_UNKNOWN);
}

static void test_resolve_unknown_command(void)
{
    DD_CHECK(cmd_resolve("NOTACMD", 7) == CMD_ID_UNKNOWN);
    DD_CHECK(cmd_resolve("", 0) == CMD_ID_UNKNOWN);
}

static void test_write_flag(void)
{
    DD_CHECK(cmd_is_write(cmd_resolve("SET", 3)));
    DD_CHECK(cmd_is_write(cmd_resolve("DEL", 3)));
    DD_CHECK(cmd_is_write(cmd_resolve("HSET", 4)));
    DD_CHECK(!cmd_is_write(cmd_resolve("GET", 3)));
    DD_CHECK(!cmd_is_write(cmd_resolve("PING", 4)));
}

static void test_arity_helpers(void)
{
    uint16_t get_id = cmd_resolve("GET", 3);
    DD_CHECK_EQ_INT(2, cmd_min_argc(get_id));
    DD_CHECK_EQ_INT(2, cmd_max_argc(get_id));

    uint16_t mset_id = cmd_resolve("MSET", 4);
    DD_CHECK_EQ_INT(3, cmd_min_argc(mset_id));
    DD_CHECK_EQ_INT(-1, cmd_max_argc(mset_id));
    DD_CHECK_EQ_INT(1, cmd_parity(mset_id)); /* odd parity */
}

int main(void)
{
    DD_RUN(test_resolve_known_command);
    DD_RUN(test_resolve_unknown_command);
    DD_RUN(test_write_flag);
    DD_RUN(test_arity_helpers);
    return DD_TEST_SUMMARY();
}
