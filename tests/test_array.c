#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
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

int main(void)
{
    DD_RUN(test_array_core);
    return DD_TEST_SUMMARY();
}
