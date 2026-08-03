/* test_snapshot.c - RDB-style snapshot save/load roundtrip. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/session.h"
#include "core/snapshot.h"
#include "test.h"

#define TMP_SNAP "test_snapshot_tmp.ddr"

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[10];
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

static void test_roundtrip_all_types(void)
{
    db d, d2;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "str", "hello");
    exec_sess(s, T0, &out, 6, "HSET", "h", "f1", "v1", "f2", "v2");
    exec_sess(s, T0, &out, 4, "RPUSH", "l", "a", "b");
    exec_sess(s, T0, &out, 4, "SADD", "st", "x", "y");
    exec_sess(s, T0, &out, 6, "ZADD", "z", "1.5", "m1", "-2", "m2");
    exec_sess(s, T0, &out, 5, "SET", "ttl", "x", "EX", "1000");
    /* a large value (u32 length sanity) */
    {
        char big[100001];
        memset(big, 'q', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        exec_sess(s, T0, &out, 3, "SET", "big", big);
        EXPECT(out, "+OK\r\n");
    }

    DD_CHECK_EQ_INT(0, snapshot_save(&d, TMP_SNAP));
    DD_CHECK(remove(TMP_SNAP ".tmp") != 0); /* tmp file must not linger */
    session_free(s);
    db_destroy(&d);

    db_init(&d2);
    DD_CHECK_EQ_INT(0, snapshot_load(&d2, TMP_SNAP, T0));
    {
        session *r = session_create(&d2);
        exec_sess(r, T0, &out, 2, "GET", "str");
        EXPECT(out, "$5\r\nhello\r\n");
        exec_sess(r, T0, &out, 2, "HGETALL", "h");
        DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
        exec_sess(r, T0, &out, 3, "HGET", "h", "f2");
        EXPECT(out, "$2\r\nv2\r\n");
        exec_sess(r, T0, &out, 4, "LRANGE", "l", "0", "-1");
        EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
        exec_sess(r, T0, &out, 3, "SISMEMBER", "st", "y");
        EXPECT(out, ":1\r\n");
        exec_sess(r, T0, &out, 3, "ZSCORE", "z", "m1");
        EXPECT(out, "$3\r\n1.5\r\n");
        exec_sess(r, T0, &out, 3, "ZSCORE", "z", "m2");
        EXPECT(out, "$2\r\n-2\r\n");
        /* TTL preserved (absolute) and still live at T0 */
        exec_sess(r, T0, &out, 2, "PTTL", "ttl");
        EXPECT(out, ":1000000\r\n");
        exec_sess(r, T0, &out, 2, "STRLEN", "big");
        EXPECT(out, ":100000\r\n");
        session_free(r);
    }
    db_destroy(&d2);
    resp_buf_free(&out);
    remove(TMP_SNAP);
}

static void test_expired_keys_skipped_at_load(void)
{
    db d, d2;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 5, "SET", "gone", "x", "EX", "10");
    exec_sess(s, T0, &out, 5, "SET", "kept", "y", "EX", "1000");
    exec_sess(s, T0, &out, 3, "SET", "plain", "z");
    DD_CHECK_EQ_INT(0, snapshot_save(&d, TMP_SNAP));
    session_free(s);
    db_destroy(&d);

    /* load 100s later: 'gone' (10s ttl) is skipped, others survive */
    db_init(&d2);
    DD_CHECK_EQ_INT(0, snapshot_load(&d2, TMP_SNAP, T0 + 100000));
    {
        session *r = session_create(&d2);
        exec_sess(r, T0 + 100000, &out, 2, "EXISTS", "gone");
        EXPECT(out, ":0\r\n");
        exec_sess(r, T0 + 100000, &out, 2, "GET", "kept");
        EXPECT(out, "$1\r\ny\r\n");
        exec_sess(r, T0 + 100000, &out, 2, "GET", "plain");
        EXPECT(out, "$1\r\nz\r\n");
        session_free(r);
    }
    db_destroy(&d2);
    resp_buf_free(&out);
    remove(TMP_SNAP);
}

static void test_corrupt_and_atomic(void)
{
    db d, d2;
    session *s;
    resp_buf out;
    FILE *f;
    long size;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    DD_CHECK_EQ_INT(0, snapshot_save(&d, TMP_SNAP));
    session_free(s);
    db_destroy(&d);

    /* truncate the file: load fails hard, target db untouched */
    f = fopen(TMP_SNAP, "rb");
    DD_CHECK(f != NULL);
    DD_CHECK(fseek(f, 0, SEEK_END) == 0);
    size = ftell(f);
    fclose(f);
    DD_CHECK(size > 10);
    {
        char trunc_path[] = "test_snapshot_trunc.ddr";
        FILE *in = fopen(TMP_SNAP, "rb");
        FILE *outf = fopen(trunc_path, "wb");
        long i;
        session *w;
        DD_CHECK(in != NULL && outf != NULL);
        for (i = 0; i < size - 5; i++)
            fputc(fgetc(in), outf);
        fclose(in);
        fclose(outf);

        db_init(&d2);
        w = session_create(&d2);
        exec_sess(w, T0, &out, 3, "SET", "safe", "1");
        DD_CHECK_EQ_INT(-1, snapshot_load(&d2, trunc_path, T0));
        exec_sess(w, T0, &out, 2, "GET", "safe");
        EXPECT(out, "$1\r\n1\r\n"); /* untouched */
        exec_sess(w, T0, &out, 2, "EXISTS", "k");
        EXPECT(out, ":0\r\n");
        session_free(w);
        db_destroy(&d2);
        remove(trunc_path);
    }

    /* bad magic */
    {
        FILE *bad = fopen(TMP_SNAP, "wb");
        DD_CHECK(bad != NULL);
        fputs("GARBAGE00", bad);
        fclose(bad);
        db_init(&d2);
        DD_CHECK_EQ_INT(-1, snapshot_load(&d2, TMP_SNAP, T0));
        db_destroy(&d2);
    }

    resp_buf_free(&out);
    remove(TMP_SNAP);
}

static void test_save_lastsave_command(void)
{
    db d, d2;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    /* SAVE without a configured path errors */
    exec_sess(s, T0, &out, 1, "SAVE");
    EXPECT(out, "-ERR snapshot path not configured\r\n");

    d.snapshot_path = TMP_SNAP;
    exec_sess(s, T0, &out, 1, "SAVE");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 1, "LASTSAVE");
    EXPECT(out, ":1000\r\n"); /* T0 / 1000 */
    session_free(s);
    db_destroy(&d);

    db_init(&d2);
    DD_CHECK_EQ_INT(0, snapshot_load(&d2, TMP_SNAP, T0));
    {
        session *r = session_create(&d2);
        exec_sess(r, T0, &out, 2, "GET", "k");
        EXPECT(out, "$1\r\nv\r\n");
        session_free(r);
    }
    db_destroy(&d2);
    resp_buf_free(&out);
    remove(TMP_SNAP);
}

int main(void)
{
    DD_RUN(test_roundtrip_all_types);
    DD_RUN(test_expired_keys_skipped_at_load);
    DD_RUN(test_corrupt_and_atomic);
    DD_RUN(test_save_lastsave_command);
    return DD_TEST_SUMMARY();
}
