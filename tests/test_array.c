#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "ds/obj.h"
#include "test.h"

static void exec_cmd(db *d, resp_buf *out, int argc, ...)
{
    resp_value argv[16];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *s = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = s;
        argv[i].len = strlen(s);
    }
    va_end(ap);
    out->len = 0;
    command_execute_at(d, argv, (size_t)argc, out, 1000000);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

static void test_array_core(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 2, "ARLEN", "missing");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 2, "ARCOUNT", "missing");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 3, "ARGET", "missing", "0");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, &out, 5, "ARSET", "a", "2", "x", "y");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, &out, 2, "ARLEN", "a");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, &out, 2, "ARCOUNT", "a");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, &out, 3, "ARGET", "a", "2");
    EXPECT(out, "$1\r\nx\r\n");
    exec_cmd(&d, &out, 3, "ARGET", "a", "1");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, &out, 4, "ARSET", "a", "1", "z");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 2, "ARCOUNT", "a");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, &out, 3, "ARGET", "a", "1");
    EXPECT(out, "$1\r\nz\r\n");

    exec_cmd(&d, &out, 4, "ARGETRANGE", "a", "0", "3");
    EXPECT(out, "*4\r\n$-1\r\n$1\r\nz\r\n$1\r\nx\r\n$1\r\ny\r\n");
    exec_cmd(&d, &out, 5, "ARMGET", "a", "0", "2", "99");
    EXPECT(out, "*3\r\n$-1\r\n$1\r\nx\r\n$-1\r\n");
    exec_cmd(&d, &out, 3, "ARDEL", "a", "2");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 4, "ARDELRANGE", "a", "0", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 2, "ARCOUNT", "a");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, &out, 4, "ARMSET", "a", "0", "q");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 4, "ARMSET", "a", "2", "w");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 2, "ARNEXT", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 3, "ARSEEK", "a", "5");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 2, "ARNEXT", "a");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, &out, 4, "ARINSERT", "a", "i", "j");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, &out, 5, "ARRING", "a", "3", "r");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 6, "ARSCAN", "a", "0", "8", "LIMIT", "3");
    EXPECT(out, "*3\r\n*2\r\n:0\r\n$1\r\nq\r\n*2\r\n:1\r\n$1\r\nr\r\n*2\r\n:2\r\n$1\r\nw\r\n");
    exec_cmd(&d, &out, 2, "ARINFO", "a");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    exec_cmd(&d, &out, 3, "ARINFO", "a", "FULL");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    exec_cmd(&d, &out, 4, "ARLASTITEMS", "a", "2", "REV");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    exec_cmd(&d, &out, 6, "AROP", "a", "0", "20", "MATCH", "r");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 7, "ARGREP", "a", "0", "20", "EXACT", "r", "WITHVALUES");
    DD_CHECK(out.len > 0 && out.data[0] == '*');
    exec_cmd(&d, &out, 8, "ARGREP", "a", "0", "20", "RE", "r", "LIMIT", "1");
    EXPECT(out, "*1\r\n:1\r\n");

    exec_cmd(&d, &out, 4, "ARSET", "a", "-1", "bad");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_cmd(&d, &out, 4, "ARSET", "a", "999999999999999999999", "bad");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_cmd(&d, &out, 3, "SET", "str", "v");
    exec_cmd(&d, &out, 2, "ARLEN", "str");
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_array_api_rejects_null_object(void)
{
    const char *value = NULL;
    size_t length = 0;
    DD_CHECK_EQ_INT(0, obj_array_get(NULL, 0, &value, &length));
    DD_CHECK(value == NULL);
    DD_CHECK_EQ_INT(0, (long long)length);
    DD_CHECK_EQ_INT(-1, obj_array_history_push(NULL, 0));
}

static void test_object_limits_reject_null_outputs(void)
{
    obj_limits saved;
    obj_limits current;
    obj_limits_get(&saved);
    obj_limits_get(NULL);
    obj_limits_apply(NULL);
    obj_limits_get(&current);
    DD_CHECK_EQ_INT(saved.list_fill, current.list_fill);
    DD_CHECK_EQ_INT(saved.hash_entries, current.hash_entries);
    DD_CHECK_EQ_INT(saved.zset_value, current.zset_value);
}

static void test_array_batch_prevalidates_views(void)
{
    obj_array *a = obj_array_new();
    const char *values[] = {"ok", NULL};
    const size_t lengths[] = {2, 1};
    const char *stored = NULL;
    size_t stored_len = 0;

    DD_CHECK(a != NULL);
    if (a != NULL) {
        DD_CHECK_EQ_INT(-1, obj_array_set(a, 0, values, lengths, 2, NULL));
        DD_CHECK_EQ_INT(0, (long long)obj_array_count(a));
        DD_CHECK_EQ_INT(0, (long long)obj_array_len(a));
        DD_CHECK_EQ_INT(0, obj_array_get(a, 0, &stored, &stored_len));
        DD_CHECK_EQ_INT(-1, obj_array_ring(a, 4, values, lengths, 2, NULL));
        DD_CHECK_EQ_INT(0, (long long)obj_array_count(a));
        DD_CHECK_EQ_INT(0, obj_array_get(a, 0, &stored, &stored_len));
        obj_array_free(a);
    }
}

static void test_array_match_rejects_malformed_views(void)
{
    db d;
    resp_buf out;
    resp_value argv[6];

    db_init(&d);
    resp_buf_init(&out);
    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING; argv[0].str = "ARSET"; argv[0].len = 5;
    argv[1].type = RESP_BULK_STRING; argv[1].str = "a"; argv[1].len = 1;
    argv[2].type = RESP_BULK_STRING; argv[2].str = "0"; argv[2].len = 1;
    argv[3].type = RESP_BULK_STRING; argv[3].str = NULL; argv[3].len = 0;
    command_execute_at(&d, argv, 4, &out, 1000000);
    DD_CHECK(out.len > 0 && out.data[0] == ':');

    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING; argv[0].str = "AROP"; argv[0].len = 4;
    argv[1].type = RESP_BULK_STRING; argv[1].str = "a"; argv[1].len = 1;
    argv[2].type = RESP_BULK_STRING; argv[2].str = "0"; argv[2].len = 1;
    argv[3].type = RESP_BULK_STRING; argv[3].str = "0"; argv[3].len = 1;
    argv[4].type = RESP_BULK_STRING; argv[4].str = "MATCH"; argv[4].len = 5;
    argv[5].type = RESP_BULK_STRING; argv[5].str = NULL; argv[5].len = 1;
    out.len = 0;
    command_execute_at(&d, argv, 6, &out, 1000000);
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_array_history_failure_does_not_partially_commit(void)
{
    obj_array *a = obj_array_new();
    const char *value = "x";
    const size_t length = 1;
    const char *stored = NULL;
    size_t stored_len = 0;

    DD_CHECK(a != NULL);
    if (a != NULL) {
        /* Force the next history growth to fail its overflow guard. */
        a->history_len = SIZE_MAX;
        a->history_cap = SIZE_MAX;
        DD_CHECK_EQ_INT(-1, obj_array_insert(a, &value, &length, 1, NULL));
        DD_CHECK_EQ_INT(0, (long long)obj_array_count(a));
        DD_CHECK_EQ_INT(0, obj_array_get(a, 0, &stored, &stored_len));
        DD_CHECK_EQ_INT(0, (long long)obj_array_next(a));
        obj_array_free(a);
    }
}

int main(void)
{
    DD_RUN(test_array_core);
    DD_RUN(test_array_api_rejects_null_object);
    DD_RUN(test_object_limits_reject_null_outputs);
    DD_RUN(test_array_batch_prevalidates_views);
    DD_RUN(test_array_match_rejects_malformed_views);
    DD_RUN(test_array_history_failure_does_not_partially_commit);
    return DD_TEST_SUMMARY();
}
