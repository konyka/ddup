/* test_session.c - session context executes commands like the db wrapper. */
#include <stdarg.h>
#include <string.h>

#include "core/session.h"
#include "test.h"

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[8];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *str = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = str;
        argv[i].len = strlen(str);
    }
    va_end(ap);
    out->len = 0;
    session_execute_at(s, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

#define T0 1000000ULL

static void test_session_basic(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    exec_sess(s, T0, &out, 1, "PING");
    EXPECT(out, "+PONG\r\n");

    /* two sessions share one db */
    {
        session *s2 = session_create(&d);
        DD_CHECK(s2 != NULL);
        exec_sess(s2, T0, &out, 2, "GET", "k");
        EXPECT(out, "$1\r\nv\r\n");
        session_free(s2);
    }

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_session_basic);
    return DD_TEST_SUMMARY();
}
