/* test_snapshot.c - RDB-style snapshot save/load roundtrip. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session.h"
#include "core/snapshot.h"
#include "ds/obj.h"
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

/* ------------------------------------------------------------------ */
/* multi-database snapshots (DDUP0002 + DDUP0001 compat)               */
/* ------------------------------------------------------------------ */

typedef struct snap_dbset {
    db dbs[3];
} snap_dbset;

static db *snap_get(void *ctx, int idx)
{
    return &((snap_dbset *)ctx)->dbs[idx];
}

static db *unreachable_get(void *ctx, int idx)
{
    (void)ctx;
    (void)idx;
    DD_CHECK(0);
    return NULL;
}

static db *single_get(void *ctx, int idx)
{
    DD_CHECK_EQ_INT(0, idx);
    return (db *)ctx;
}

/* db structs are huge (cluster tables): always heap-allocate sets of them */
static snap_dbset *snap_dbset_new(void)
{
    int i;
    snap_dbset *ds = (snap_dbset *)calloc(1, sizeof(*ds));
    DD_CHECK(ds != NULL);
    for (i = 0; i < 3; i++)
        db_init(&ds->dbs[i]);
    return ds;
}

static void snap_dbset_free(snap_dbset *ds)
{
    int i;
    for (i = 0; i < 3; i++)
        db_destroy(&ds->dbs[i]);
    free(ds);
}

static void test_multidb_roundtrip(void)
{
    snap_dbset *ds, *ds2;
    session *s;
    resp_buf out, snap;

    ds = snap_dbset_new();
    ds2 = snap_dbset_new();
    resp_buf_init(&out);
    resp_buf_init(&snap);

    s = session_create(&ds->dbs[0]);
    s->sel_ctx = ds;
    s->sel_fn = snap_get;
    s->sel_ndbs = 3;

    exec_sess(s, T0, &out, 3, "SET", "a", "db0val");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 3, "SET", "b", "db1val");
    exec_sess(s, T0, &out, 4, "HSET", "h", "f", "v");
    exec_sess(s, T0, &out, 2, "SELECT", "2");
    exec_sess(s, T0, &out, 4, "SADD", "st", "x", "y");
    session_free(s);

    DD_CHECK_EQ_INT(0, snapshot_serialize_multi(ds, snap_get, 3, &snap));
    DD_CHECK(snap.len > 8);
    DD_CHECK_MEM("DDUP0002", 8, snap.data, 8);

    DD_CHECK_EQ_INT(0, snapshot_load_mem_multi(ds2, snap_get, 3, snap.data,
                                               snap.len, T0));

    /* db0 has only 'a', db1 has 'b' and 'h', db2 has 'st' */
    {
        session *r = session_create(&ds2->dbs[0]);
        r->sel_ctx = ds2;
        r->sel_fn = snap_get;
        r->sel_ndbs = 3;
        exec_sess(r, T0, &out, 2, "GET", "a");
        EXPECT(out, "$6\r\ndb0val\r\n");
        exec_sess(r, T0, &out, 2, "GET", "b");
        EXPECT(out, "$-1\r\n");
        exec_sess(r, T0, &out, 2, "SELECT", "1");
        exec_sess(r, T0, &out, 2, "GET", "b");
        EXPECT(out, "$6\r\ndb1val\r\n");
        exec_sess(r, T0, &out, 3, "HGET", "h", "f");
        EXPECT(out, "$1\r\nv\r\n");
        exec_sess(r, T0, &out, 2, "SELECT", "2");
        exec_sess(r, T0, &out, 3, "SISMEMBER", "st", "x");
        EXPECT(out, ":1\r\n");
        exec_sess(r, T0, &out, 2, "GET", "a");
        EXPECT(out, "$-1\r\n");
        session_free(r);
    }

    snap_dbset_free(ds);
    snap_dbset_free(ds2);
    resp_buf_free(&out);
    resp_buf_free(&snap);
}

static void test_multidb_v1_compat(void)
{
    /* a DDUP0001 (single-db) buffer loads into db 0 via the multi API */
    db d;
    snap_dbset *ds2;
    session *s;
    resp_buf snap, out;

    db_init(&d);
    resp_buf_init(&snap);
    resp_buf_init(&out);
    s = session_create(&d);
    exec_sess(s, T0, &out, 3, "SET", "k", "v1");
    session_free(s);
    DD_CHECK_EQ_INT(0, snapshot_serialize(&d, &snap));
    DD_CHECK_MEM("DDUP0001", 8, snap.data, 8);
    db_destroy(&d);

    ds2 = snap_dbset_new();
    DD_CHECK_EQ_INT(0, snapshot_load_mem_multi(ds2, snap_get, 3, snap.data,
                                               snap.len, T0));
    {
        session *r = session_create(&ds2->dbs[0]);
        exec_sess(r, T0, &out, 2, "GET", "k");
        EXPECT(out, "$2\r\nv1\r\n");
        session_free(r);
    }

    snap_dbset_free(ds2);
    resp_buf_free(&snap);
    resp_buf_free(&out);
}

static void test_serialize_rejects_unrepresentable_sizes(void)
{
    db d;
    session *s;
    resp_buf out, snap;
    const char *blob;
    size_t bloblen;
    obj_list *list;

    db_init(&d);
    resp_buf_init(&out);
    resp_buf_init(&snap);
    s = session_create(&d);
    exec_sess(s, T0, &out, 3, "RPUSH", "list", "item");
    DD_CHECK(rh_get(&d.table, "list", 4, &blob, &bloblen) == 1);
    list = (obj_list *)obj_unpack_ptr(blob, bloblen);
    list->len = (uint64_t)UINT32_MAX + 1;

    resp_buf_reserve(&snap, 4);
    memcpy(snap.data, "keep", 4);
    snap.len = 4;
    DD_CHECK_EQ_INT(-1, snapshot_serialize(&d, &snap));
    DD_CHECK_MEM("keep", 4, snap.data, snap.len);
    DD_CHECK_EQ_INT(-1,
                    snapshot_serialize_multi(NULL, unreachable_get,
                                             (int)UINT16_MAX + 1, &snap));
    DD_CHECK_MEM("keep", 4, snap.data, snap.len);
#if SIZE_MAX > UINT32_MAX
    d.table.size = (size_t)UINT32_MAX + 1;
    DD_CHECK_EQ_INT(-1, snapshot_serialize_multi(&d, single_get, 1, &snap));
    DD_CHECK_MEM("keep", 4, snap.data, snap.len);
    d.table.size = 1;
#endif

    snap.len = SIZE_MAX;
    snap.cap = SIZE_MAX;
    DD_CHECK_EQ_INT(-1, snapshot_serialize(&d, &snap));
    DD_CHECK(snap.len == SIZE_MAX);
    snap.len = 4;
    snap.cap = 256;

    list->len = 1;
    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
    resp_buf_free(&snap);
}

static void test_save_failure_preserves_existing_snapshot(void)
{
    db d;
    session *s;
    resp_buf out;
    const char *blob;
    size_t bloblen;
    obj_list *list;
    FILE *f;
    char disk[8];

    f = fopen(TMP_SNAP, "wb");
    DD_CHECK(f != NULL);
    if (f != NULL) {
        DD_CHECK_EQ_INT(8, (long long)fwrite("existing", 1, 8, f));
        DD_CHECK_EQ_INT(0, fclose(f));
    }
    (void)remove(TMP_SNAP ".tmp");

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);
    exec_sess(s, T0, &out, 3, "RPUSH", "list", "item");
    DD_CHECK(rh_get(&d.table, "list", 4, &blob, &bloblen) == 1);
    list = (obj_list *)obj_unpack_ptr(blob, bloblen);
    list->len = (uint64_t)UINT32_MAX + 1;
    DD_CHECK_EQ_INT(-1, snapshot_save(&d, TMP_SNAP));
    list->len = 1;

    f = fopen(TMP_SNAP, "rb");
    DD_CHECK(f != NULL);
    if (f != NULL) {
        DD_CHECK_EQ_INT(8, (long long)fread(disk, 1, sizeof(disk), f));
        DD_CHECK_EQ_INT(0, fclose(f));
        DD_CHECK_MEM("existing", 8, disk, sizeof(disk));
    }
    DD_CHECK(remove(TMP_SNAP ".tmp") != 0);
    remove(TMP_SNAP);
    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
}

static void test_multidb_count_has_no_eof_sentinel(void)
{
    snap_dbset *ds = snap_dbset_new();
    const char malformed[] = {
        'D', 'D', 'U', 'P', '0', '0', '0', '2',
        1, 0,
        0, 0,
        (char)0xff, (char)0xff, (char)0xff, (char)0xff
    };

    DD_CHECK_EQ_INT(-1, snapshot_load_mem_multi(ds, snap_get, 3, malformed,
                                                sizeof(malformed), T0));
    snap_dbset_free(ds);
}

static void test_reader_extreme_offsets_are_rejected(void)
{
    DD_CHECK_EQ_INT(0, snapshot_test_reader_bounds());
}

static void test_failed_install_and_restore_are_transactional(void)
{
    db src, dst;
    session *s;
    resp_buf out, payload;
    const char byte = 'x';
    const char *v;
    size_t vl;
    uint64_t used, dirty;

    db_init(&src);
    db_init(&dst);
    resp_buf_init(&out);
    resp_buf_init(&payload);
    s = session_create(&src);
    exec_sess(s, T0, &out, 3, "SET", "source", "value");
    DD_CHECK_EQ_INT(0, snapshot_dump_key(&src, "source", 6, &payload));
    session_free(s);

    {
        const char old[] = {(char)DDUP_OBJ_STRING, 'o', 'l', 'd'};
        DD_CHECK_EQ_INT(0, db_install_blob(&dst, "k", 1, old, sizeof(old), T0));
        DD_CHECK_EQ_INT(0, db_install_expiry(&dst, "k", 1, T0 + 10000));
    }
    dst.watch_refs = 1;
    used = dst.used_memory;
    dirty = dst.dirty;

    DD_CHECK_EQ_INT(-1,
                    db_install_blob(&dst, "k", 1, &byte, SIZE_MAX, T0));
    DD_CHECK_EQ_INT(-1,
                    db_install_expiry(&dst, &byte, SIZE_MAX, T0 + 20000));
    DD_CHECK_EQ_INT(-1,
                    snapshot_restore_key(&dst, &byte, SIZE_MAX, payload.data,
                                         payload.len, T0 + 30000, 1, T0));
    DD_CHECK(dst.used_memory == used);
    DD_CHECK(dst.dirty == dirty);
    DD_CHECK_EQ_INT(1, rh_size(&dst.table));
    DD_CHECK_EQ_INT(1, rh_size(&dst.expires));
    DD_CHECK(rh_get(&dst.table, "k", 1, &v, &vl) == 1);
    DD_CHECK_MEM("old", 3, v + 1, vl - 1);

    dst.watch_refs = 0;
    db_destroy(&src);
    db_destroy(&dst);
    resp_buf_free(&out);
    resp_buf_free(&payload);
}

int main(void)
{
    DD_RUN(test_roundtrip_all_types);
    DD_RUN(test_expired_keys_skipped_at_load);
    DD_RUN(test_corrupt_and_atomic);
    DD_RUN(test_save_lastsave_command);
    DD_RUN(test_multidb_roundtrip);
    DD_RUN(test_multidb_v1_compat);
    DD_RUN(test_serialize_rejects_unrepresentable_sizes);
    DD_RUN(test_save_failure_preserves_existing_snapshot);
    DD_RUN(test_multidb_count_has_no_eof_sentinel);
    DD_RUN(test_reader_extreme_offsets_are_rejected);
    DD_RUN(test_failed_install_and_restore_are_transactional);
    return DD_TEST_SUMMARY();
}
