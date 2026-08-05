/* test_reshard.c - end-to-end slot resharding: two cluster nodes on
 * background threads, reshard_slot() moves every key in batches and
 * finalizes ownership on both ends (covers the ddup-reshard tool core). */
#include <stdio.h>
#include <string.h>

#include "core/hashslot.h"
#include "pal/pal_socket.h"
#include "pal/pal_thread.h"
#include "pal/pal_time.h"
#include "reshard_client.h"
#include "server/server.h"
#include "test.h"

/* memmem for RESP payloads (reply strings are not NUL-terminated) */
static int contains(const char *h, size_t hlen, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if (nl > hlen)
        return 0;
    for (i = 0; i + nl <= hlen; i++)
        if (memcmp(h + i, needle, nl) == 0)
            return 1;
    return 0;
}

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

typedef struct runner {
    server *s;
    volatile int stop;
    pal_thread th;
} runner;

static void *run_srv(void *arg)
{
    runner *r = (runner *)arg;
    while (!r->stop)
        (void)server_run_once(r->s, 5);
    return NULL;
}

static void runner_start(runner *r, server *s)
{
    r->s = s;
    r->stop = 0;
    DD_CHECK_EQ_INT(0, pal_thread_create(&r->th, run_srv, r));
}

static void runner_stop(runner *r)
{
    r->stop = 1;
    (void)pal_thread_join(&r->th, NULL);
}

/* expect a +OK simple-string reply */
static void expect_ok(resp_value *v)
{
    DD_CHECK(v->type == RESP_SIMPLE_STRING);
    DD_CHECK(v->len == 2 && memcmp(v->str, "OK", 2) == 0);
}

/* find nkeys distinct keys hashing to the same slot */
static int keys_in_slot(int nkeys, char out[][32], uint32_t *slot_out)
{
    int i;
    uint32_t slot = 0;
    int n = 0;
    for (i = 0; n < nkeys && i < 200000; i++) {
        char k[32];
        uint32_t s;
        snprintf(k, sizeof(k), "rskey%d", i);
        s = hash_slot(k, strlen(k));
        if (n == 0)
            slot = s;
        if (s == slot) {
            snprintf(out[n], 32, "%s", k);
            n++;
        }
    }
    *slot_out = slot;
    return n;
}

static void test_reshard_full_flow(void)
{
    server *a, *b;
    runner ra, rb;
    rs_conn ca, cb;
    arena ar;
    resp_value v;
    char keys[5][32];
    char portb[16];
    uint32_t slot;
    char slots[16];
    long long migrated = 0;
    int i, rc;

    DD_CHECK_EQ_INT(5, keys_in_slot(5, keys, &slot));
    snprintf(slots, sizeof(slots), "%u", (unsigned)slot);

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    runner_start(&ra, a);
    runner_start(&rb, b);
    snprintf(portb, sizeof(portb), "%u", (unsigned)server_port(b));

    arena_init(&ar, 4096);
    DD_CHECK_EQ_INT(0, rs_connect(&ca, "127.0.0.1", server_port(a)));
    DD_CHECK_EQ_INT(0, rs_connect(&cb, "127.0.0.1", server_port(b)));

    /* meet + take the slot; wait for gossip to reach b */
    {
        const char *args[] = {"CLUSTER", "MEET", "127.0.0.1", portb};
        DD_CHECK_EQ_INT(0, rs_exec(&ca, &ar, &v, 4, args, NULL));
        expect_ok(&v);
    }
    {
        int learned = 0;
        for (i = 0; i < 100 && !learned; i++) {
            const char *args[] = {"CLUSTER", "NODES"};
            DD_CHECK_EQ_INT(0, rs_exec(&cb, &ar, &v, 2, args, NULL));
            if (v.type == RESP_BULK_STRING && v.str != NULL &&
                contains(v.str, v.len, IDA))
                learned = 1;
            else
                pal_sleep_ms(50);
        }
        DD_CHECK_EQ_INT(1, learned);
    }
    {
        const char *args[] = {"CLUSTER", "ADDSLOTS", slots};
        DD_CHECK_EQ_INT(0, rs_exec(&ca, &ar, &v, 3, args, NULL));
        expect_ok(&v);
    }
    for (i = 0; i < 5; i++) {
        char val[8];
        const char *args[4];
        snprintf(val, sizeof(val), "v%d", i);
        args[0] = "SET";
        args[1] = keys[i];
        args[2] = val;
        DD_CHECK_EQ_INT(0, rs_exec(&ca, &ar, &v, 3, args, NULL));
        expect_ok(&v);
    }

    /* reshard the whole slot in batches of 2 */
    rc = reshard_slot("127.0.0.1", server_port(a), "127.0.0.1",
                      server_port(b), (int)slot, 2, 5000, &migrated, NULL);
    DD_CHECK_EQ_INT(0, rc);
    DD_CHECK_EQ_INT(5, (long long)migrated);

    /* b owns and serves the keys now */
    {
        const char *args[] = {"GET", keys[0]};
        DD_CHECK_EQ_INT(0, rs_exec(&cb, &ar, &v, 2, args, NULL));
        DD_CHECK(v.type == RESP_BULK_STRING);
        DD_CHECK(v.len == 2 && memcmp(v.str, "v0", 2) == 0);
    }
    /* a redirects with -MOVED */
    {
        const char *args[] = {"GET", keys[0]};
        DD_CHECK_EQ_INT(0, rs_exec(&ca, &ar, &v, 2, args, NULL));
        DD_CHECK(v.type == RESP_ERROR);
        DD_CHECK(contains(v.str, v.len, "MOVED"));
    }
    /* the slot is empty on a */
    {
        const char *args[] = {"CLUSTER", "GETKEYSINSLOT", slots, "10"};
        DD_CHECK_EQ_INT(0, rs_exec(&ca, &ar, &v, 4, args, NULL));
        DD_CHECK(v.type == RESP_ARRAY);
        DD_CHECK_EQ_INT(0, (long long)v.count);
    }

    rs_close(&ca);
    rs_close(&cb);
    arena_destroy(&ar);
    runner_stop(&ra);
    runner_stop(&rb);
    server_destroy(a);
    server_destroy(b);
    pal_socket_cleanup();
}

int main(void)
{
    DD_RUN(test_reshard_full_flow);
    return DD_TEST_SUMMARY();
}
