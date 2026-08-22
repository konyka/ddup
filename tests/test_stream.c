/* test_stream.c - stream core command semantics with synthetic time. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "core/snapshot.h"
#include "ds/obj.h"
#include "test.h"

static void exec_cmd(db *d, uint64_t now, resp_buf *out, int argc, ...)
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
    command_execute_at(d, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

#define T0 1ULL

static void test_xadd_xlen(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "*", "f", "v");
    EXPECT(out, "$3\r\n1-0\r\n");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "*", "f2", "v2");
    EXPECT(out, "$3\r\n1-1\r\n");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "2-*", "f3", "v3");
    EXPECT(out, "$3\r\n2-0\r\n");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "2-*", "f4", "v4");
    EXPECT(out, "$3\r\n2-1\r\n");

    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "nokey");
    EXPECT(out, ":0\r\n");

    /* equal-or-smaller IDs are rejected transactionally */
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-0", "bad", "x");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":4\r\n");

    /* NOMKSTREAM does not create the key */
    exec_cmd(&d, T0, &out, 6, "XADD", "no", "NOMKSTREAM", "*", "f", "v");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "no");
    EXPECT(out, ":0\r\n");

    /* wrong kind */
    exec_cmd(&d, T0, &out, 3, "SET", "str", "x");
    exec_cmd(&d, T0, &out, 5, "XADD", "str", "*", "f", "v");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    DD_CHECK(out.len > 9 && memcmp(out.data, "-WRONGTYPE", 10) == 0);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_xrange_xrevrange(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-1", "a", "b");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "2-0", "c", "d");

    exec_cmd(&d, T0, &out, 4, "XRANGE", "s", "-", "+");
    EXPECT(out,
           "*3\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n"
           "*2\r\n$3\r\n2-0\r\n*2\r\n$1\r\nc\r\n$1\r\nd\r\n");

    exec_cmd(&d, T0, &out, 4, "XRANGE", "s", "1-1", "+");
    EXPECT(out,
           "*2\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n"
           "*2\r\n$3\r\n2-0\r\n*2\r\n$1\r\nc\r\n$1\r\nd\r\n");

    exec_cmd(&d, T0, &out, 6, "XRANGE", "s", "-", "+", "COUNT", "2");
    EXPECT(out,
           "*2\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    exec_cmd(&d, T0, &out, 4, "XREVRANGE", "s", "+", "-");
    EXPECT(out,
           "*3\r\n"
           "*2\r\n$3\r\n2-0\r\n*2\r\n$1\r\nc\r\n$1\r\nd\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n");

    exec_cmd(&d, T0, &out, 4, "XRANGE", "nokey", "-", "+");
    EXPECT(out, "*0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_xdel_xtrim_xsetid(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "2-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "2-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "3-0", "f", "v");

    exec_cmd(&d, T0, &out, 5, "XDEL", "s", "1-0", "3-0", "9-9");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, T0, &out, 5, "XTRIM", "s", "MAXLEN", "=", "2");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":2\r\n");

    exec_cmd(&d, T0, &out, 5, "XTRIM", "s", "MINID", "=", "2-1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, T0, &out, 3, "XSETID", "s", "10-0");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "9-0", "f", "v");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "10-*", "f", "v");
    EXPECT(out, "$4\r\n10-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_stream_snapshot_roundtrip(void)
{
    db d, d2;
    resp_buf out;
    const char *path = "/tmp/ddup-test-stream.rdb";
    db_init(&d);
    db_init(&d2);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "5-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "6-*", "a", "b");
    DD_CHECK_EQ_INT(0, snapshot_save(&d, path));
    DD_CHECK_EQ_INT(0, snapshot_load(&d2, path, T0));

    exec_cmd(&d2, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d2, T0, &out, 4, "XRANGE", "s", "-", "+");
    EXPECT(out,
           "*2\r\n"
           "*2\r\n$3\r\n5-1\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
           "*2\r\n$3\r\n6-0\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    remove(path);
    resp_buf_free(&out);
    db_destroy(&d2);
    db_destroy(&d);
}

static void test_xgroup_xreadgroup_ack_pending(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "*", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "*", "a", "b");

    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "s", "g", "0-0");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "s", "g", "0-0");
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    exec_cmd(&d, T0, &out, 7, "XREADGROUP", "GROUP", "g", "c",
             "STREAMS", "s", ">");
    EXPECT(out,
           "*1\r\n"
           "*2\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    exec_cmd(&d, T0, &out, 3, "XPENDING", "s", "g");
    EXPECT(out,
           "*4\r\n:2\r\n$3\r\n1-0\r\n$3\r\n1-1\r\n"
           "*1\r\n*2\r\n$1\r\nc\r\n:2\r\n");

    exec_cmd(&d, T0, &out, 4, "XACK", "s", "g", "1-0");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "XPENDING", "s", "g");
    EXPECT(out,
           "*4\r\n:1\r\n$3\r\n1-1\r\n$3\r\n1-1\r\n"
           "*1\r\n*2\r\n$1\r\nc\r\n:1\r\n");

    exec_cmd(&d, T0, &out, 4, "XGROUP", "DESTROY", "s", "g");
    EXPECT(out, ":1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_xread(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-1", "a", "b");

    exec_cmd(&d, T0, &out, 6, "XREAD", "COUNT", "1", "STREAMS", "s", "1-0");
    EXPECT(out,
           "*1\r\n"
           "*1\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    exec_cmd(&d, T0, &out, 4, "XREAD", "STREAMS", "s", "1-1");
    EXPECT(out, "*1\r\n*0\r\n");

    exec_cmd(&d, T0, &out, 6, "XREAD", "BLOCK", "100", "STREAMS", "s", "0-0");
    EXPECT(out,
           "*1\r\n"
           "*2\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    exec_cmd(&d, T0, &out, 4, "XREAD", "STREAMS", "nokey", "0-0");
    EXPECT(out, "*1\r\n$-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_xclaim_xautoclaim_xinfo(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-1", "a", "b");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "s", "g", "0-0");
    exec_cmd(&d, T0, &out, 9, "XREADGROUP", "GROUP", "g", "c1",
             "COUNT", "1", "STREAMS", "s", ">");
    exec_cmd(&d, T0, &out, 9, "XREADGROUP", "GROUP", "g", "c1",
             "COUNT", "1", "STREAMS", "s", ">");

    exec_cmd(&d, T0, &out, 6, "XCLAIM", "s", "g", "c2", "0", "1-0");
    EXPECT(out,
           "*1\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n");

    exec_cmd(&d, T0, &out, 3, "XPENDING", "s", "g");
    EXPECT(out,
           "*4\r\n:2\r\n$3\r\n1-0\r\n$3\r\n1-1\r\n"
           "*2\r\n"
           "*2\r\n$2\r\nc1\r\n:1\r\n"
           "*2\r\n$2\r\nc2\r\n:1\r\n");

    exec_cmd(&d, T0, &out, 3, "XINFO", "STREAM", "s");
    EXPECT(out,
           "*16\r\n"
           "$6\r\nlength\r\n:2\r\n"
           "$17\r\nlast-generated-id\r\n$3\r\n1-1\r\n"
           "$13\r\nentries-added\r\n:2\r\n"
           "$20\r\nmax-deleted-entry-id\r\n$3\r\n0-0\r\n"
           "$7\r\nentries\r\n:2\r\n"
           "$6\r\ngroups\r\n:1\r\n"
           "$11\r\nfirst-entry\r\n"
           "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
           "$10\r\nlast-entry\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    exec_cmd(&d, T0, &out, 3, "XINFO", "GROUPS", "s");
    EXPECT(out,
           "*1\r\n"
           "*12\r\n"
           "$4\r\nname\r\n$1\r\ng\r\n"
           "$9\r\nconsumers\r\n:2\r\n"
           "$7\r\npending\r\n:2\r\n"
           "$17\r\nlast-delivered-id\r\n$3\r\n1-1\r\n"
           "$12\r\nentries-read\r\n:2\r\n"
           "$3\r\nlag\r\n:0\r\n");

    exec_cmd(&d, T0, &out, 4, "XINFO", "CONSUMERS", "s", "g");
    EXPECT(out,
           "*2\r\n"
           "*8\r\n$4\r\nname\r\n$2\r\nc1\r\n$7\r\npending\r\n:1\r\n"
           "$4\r\nidle\r\n:0\r\n$8\r\ninactive\r\n:1\r\n"
           "*8\r\n$4\r\nname\r\n$2\r\nc2\r\n$7\r\npending\r\n:1\r\n"
           "$4\r\nidle\r\n:0\r\n$8\r\ninactive\r\n:1\r\n");

    exec_cmd(&d, T0, &out, 8, "XAUTOCLAIM", "s", "g", "c3", "0", "0-0",
             "COUNT", "1");
    EXPECT(out,
           "*2\r\n"
           "$3\r\n1-0\r\n"
           "*1\r\n"
           "*2\r\n$3\r\n1-1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_stream_group_snapshot_roundtrip(void)
{
    db d, d2;
    resp_buf out;
    const char *path = "/tmp/ddup-test-stream-group.rdb";
    db_init(&d);
    db_init(&d2);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "XADD", "s", "5-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "6-0", "a", "b");
    exec_cmd(&d, T0, &out, 3, "SET", "z", "v");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "s", "g", "0-0");
    exec_cmd(&d, T0, &out, 9, "XREADGROUP", "GROUP", "g", "c",
             "COUNT", "1", "STREAMS", "s", ">");

    DD_CHECK_EQ_INT(0, snapshot_save(&d, path));
    DD_CHECK_EQ_INT(0, snapshot_load(&d2, path, T0));

    exec_cmd(&d2, T0, &out, 3, "XPENDING", "s", "g");
    EXPECT(out,
           "*4\r\n:1\r\n$3\r\n5-1\r\n$3\r\n5-1\r\n"
           "*1\r\n*2\r\n$1\r\nc\r\n:1\r\n");

    exec_cmd(&d2, T0, &out, 2, "GET", "z");
    EXPECT(out, "$1\r\nv\r\n");

    exec_cmd(&d2, T0, &out, 7, "XREADGROUP", "GROUP", "g", "c",
             "STREAMS", "s", ">");
    EXPECT(out,
           "*1\r\n"
           "*1\r\n"
           "*2\r\n$3\r\n6-0\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    remove(path);
    resp_buf_free(&out);
    db_destroy(&d2);
    db_destroy(&d);
}


static void test_xdelex_xackdel_xnack_xcfgset_xidmprecord(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* XDELEX deletes by ID and reports per-ID status. */
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "1-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "s", "2-0", "f", "v");
    exec_cmd(&d, T0, &out, 6, "XDELEX", "s", "IDS", "2", "1-0", "9-9");
    EXPECT(out, "*2\r\n:1\r\n:-1\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 6, "XDELEX", "missing", "IDS", "2", "1-0", "2-0");
    EXPECT(out, "*2\r\n:-1\r\n:-1\r\n");
    exec_cmd(&d, T0, &out, 6, "XDELEX", "s", "IDS", "2", "1-1", "2-0");
    EXPECT(out, "*2\r\n:1\r\n:1\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "s");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "s");
    EXPECT(out, ":1\r\n");

    /* XACKDEL acknowledges the group PEL and deletes the stream entry. */
    exec_cmd(&d, T0, &out, 5, "XADD", "a", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "a", "1-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "a", "g", "0-0");
    exec_cmd(&d, T0, &out, 7, "XREADGROUP", "GROUP", "g", "c",
             "STREAMS", "a", ">");
    exec_cmd(&d, T0, &out, 7, "XACKDEL", "a", "g", "IDS", "2", "1-0", "9-9");
    EXPECT(out, "*2\r\n:1\r\n:-1\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 6, "XACKDEL", "a", "g", "IDS", "1", "1-0");
    EXPECT(out, "*1\r\n:-1\r\n");

    /* ACKED keeps the entry while another group still references it. */
    exec_cmd(&d, T0, &out, 5, "XADD", "b", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "b", "g1", "0-0");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "b", "g2", "0-0");
    exec_cmd(&d, T0, &out, 7, "XREADGROUP", "GROUP", "g1", "c1",
             "STREAMS", "b", ">");
    exec_cmd(&d, T0, &out, 7, "XREADGROUP", "GROUP", "g2", "c2",
             "STREAMS", "b", ">");
    exec_cmd(&d, T0, &out, 7, "XACKDEL", "b", "g1", "ACKED", "IDS", "1", "1-0");
    EXPECT(out, "*1\r\n:2\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 7, "XACKDEL", "b", "g2", "ACKED", "IDS", "1", "1-0");
    EXPECT(out, "*1\r\n:1\r\n");
    exec_cmd(&d, T0, &out, 2, "XLEN", "b");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "b");
    EXPECT(out, ":1\r\n");

    /* XNACK updates pending delivery state and supports FORCE. */
    exec_cmd(&d, T0, &out, 5, "XADD", "n", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "n", "1-1", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XADD", "n", "2-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XGROUP", "CREATE", "n", "g", "0-0");
    exec_cmd(&d, T0, &out, 9, "XREADGROUP", "GROUP", "g", "c",
             "COUNT", "2", "STREAMS", "n", ">");
    exec_cmd(&d, T0, &out, 7, "XNACK", "n", "g", "FAIL", "IDS", "1", "1-0");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 8, "XNACK", "n", "g", "SILENT", "IDS", "1",
             "2-0", "FORCE");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 8, "XNACK", "n", "g", "SILENT", "IDS", "1", "9-9",
             "FORCE");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 7, "XNACK", "n", "nogroup", "FAIL", "IDS", "1",
             "1-0");
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    /* XCFGSET validates and accepts IDMP configuration. */
    exec_cmd(&d, T0, &out, 5, "XADD", "cfg", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 4, "XCFGSET", "cfg", "IDMP-DURATION", "100");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "XCFGSET", "cfg", "IDMP-MAXSIZE", "200");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "XCFGSET", "missing", "IDMP-DURATION", "100");
    EXPECT(out, "-ERR no such key\r\n");

    /* XIDMPRECORD is a validated metadata no-op on an existing entry. */
    exec_cmd(&d, T0, &out, 5, "XADD", "idm", "1-0", "f", "v");
    exec_cmd(&d, T0, &out, 5, "XIDMPRECORD", "idm", "pid", "iid", "1-0");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 5, "XIDMPRECORD", "idm", "pid", "iid", "9-9");
    EXPECT(out, "-ERR No such message in stream\r\n");
    exec_cmd(&d, T0, &out, 5, "XIDMPRECORD", "missing", "pid", "iid", "1-0");
    EXPECT(out, "-ERR no such key\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_xadd_xlen);
    DD_RUN(test_xrange_xrevrange);
    DD_RUN(test_xdel_xtrim_xsetid);
    DD_RUN(test_stream_snapshot_roundtrip);
    DD_RUN(test_xgroup_xreadgroup_ack_pending);
    DD_RUN(test_xread);
    DD_RUN(test_xclaim_xautoclaim_xinfo);
    DD_RUN(test_stream_group_snapshot_roundtrip);
    DD_RUN(test_xdelex_xackdel_xnack_xcfgset_xidmprecord);
    return DD_TEST_SUMMARY();
}
