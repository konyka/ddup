/* test_pubsub.c - pub/sub via sessions with a test-owned registry. */
#include <stdarg.h>
#include <string.h>

#include "core/session.h"
#include "server/server.h"
#include "test.h"

/* ------------------------------------------------------------------ */
/* minimal test registry implementing the session pub/sub hooks       */
/* ------------------------------------------------------------------ */
#define MAX_CHANS 8
#define MAX_SESS 4

typedef struct test_chan {
    char name[32];
    size_t nlen;
    session *subs[MAX_SESS];
    int nsubs;
} test_chan;

typedef struct test_registry {
    test_chan chans[MAX_CHANS];
    int nchans;
} test_registry;

static test_chan *chan_find(test_registry *r, const char *ch, size_t len,
                            int create)
{
    int i;
    for (i = 0; i < r->nchans; i++)
        if (r->chans[i].nlen == len && memcmp(r->chans[i].name, ch, len) == 0)
            return &r->chans[i];
    if (!create)
        return NULL;
    DD_CHECK(r->nchans < MAX_CHANS);
    {
        test_chan *c = &r->chans[r->nchans++];
        DD_CHECK(len < sizeof(c->name));
        memcpy(c->name, ch, len);
        c->nlen = len;
        c->nsubs = 0;
        return c;
    }
}

static ptrdiff_t t_subscribe(void *ctx, session *s, const char *ch, size_t len)
{
    test_registry *r = (test_registry *)ctx;
    test_chan *c = chan_find(r, ch, len, 1);
    int i;
    for (i = 0; i < c->nsubs; i++)
        if (c->subs[i] == s)
            return s->nsub; /* already subscribed */
    DD_CHECK(c->nsubs < MAX_SESS);
    c->subs[c->nsubs++] = s;
    return ++s->nsub;
}

static ptrdiff_t t_reject_subscribe(void *ctx, session *s, const char *ch,
                                    size_t len)
{
    (void)ctx;
    (void)s;
    (void)ch;
    (void)len;
    return -1;
}

static size_t t_unsubscribe(void *ctx, session *s, const char *ch, size_t len)
{
    test_registry *r = (test_registry *)ctx;
    test_chan *c = chan_find(r, ch, len, 0);
    int i;
    if (c == NULL)
        return s->nsub;
    for (i = 0; i < c->nsubs; i++) {
        if (c->subs[i] == s) {
            c->subs[i] = c->subs[c->nsubs - 1];
            c->nsubs--;
            if (s->nsub > 0)
                s->nsub--;
            return s->nsub;
        }
    }
    return s->nsub;
}

static void t_each_channel(void *ctx, session *s,
                           void (*cb)(const char *ch, size_t len, void *arg),
                           void *arg)
{
    test_registry *r = (test_registry *)ctx;
    int i, j;
    for (i = 0; i < r->nchans; i++)
        for (j = 0; j < r->chans[i].nsubs; j++)
            if (r->chans[i].subs[j] == s) {
                cb(r->chans[i].name, r->chans[i].nlen, arg);
                break;
            }
}

static long t_publish(void *ctx, const char *ch, size_t chlen, const char *msg,
                      size_t mlen)
{
    test_registry *r = (test_registry *)ctx;
    test_chan *c = chan_find(r, ch, chlen, 0);
    long n = 0;
    int i;
    if (c == NULL)
        return 0;
    for (i = 0; i < c->nsubs; i++) {
        session *s = c->subs[i];
        s->deliver(s->owner, ch, chlen, msg, mlen);
        n++;
    }
    return n;
}

static void t_deliver(void *owner, const char *ch, size_t chlen,
                      const char *msg, size_t mlen)
{
    resp_buf *cap = (resp_buf *)owner;
    resp_write_array_header(cap, 3);
    resp_write_bulk(cap, "message", 7);
    resp_write_bulk(cap, ch, chlen);
    resp_write_bulk(cap, msg, mlen);
}

static session *hooked_session(db *d, test_registry *r, resp_buf *cap)
{
    session *s = session_create(d);
    s->ps_ctx = r;
    s->subscribe = t_subscribe;
    s->unsubscribe = t_unsubscribe;
    s->each_channel = t_each_channel;
    s->publish = t_publish;
    s->owner = cap;
    s->deliver = t_deliver;
    return s;
}

/* ------------------------------------------------------------------ */

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

static void test_subscribe_publish(void)
{
    db d;
    test_registry reg;
    resp_buf out, cap_a;
    session *a, *b;
    db_init(&d);
    memset(&reg, 0, sizeof(reg));
    resp_buf_init(&out);
    resp_buf_init(&cap_a);
    a = hooked_session(&d, &reg, &cap_a);
    b = session_create(&d);
    b->ps_ctx = &reg;
    b->publish = t_publish;

    /* subscribe reply shape, count increments */
    exec_sess(a, T0, &out, 2, "SUBSCRIBE", "ch");
    EXPECT(out, "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    exec_sess(a, T0, &out, 2, "SUBSCRIBE", "ch2");
    EXPECT(out, "*3\r\n$9\r\nsubscribe\r\n$3\r\nch2\r\n:2\r\n");
    /* re-subscribe is idempotent for delivery but reported as-is */
    exec_sess(a, T0, &out, 2, "SUBSCRIBE", "ch");
    EXPECT(out, "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:2\r\n");

    /* publish from another session delivers to the subscriber only */
    exec_sess(b, T0, &out, 3, "PUBLISH", "ch", "hello");
    EXPECT(out, ":1\r\n");
    EXPECT(cap_a, "*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n");
    exec_sess(b, T0, &out, 3, "PUBLISH", "nosuch", "x");
    EXPECT(out, ":0\r\n");

    /* unsubscribe one channel; the other remains */
    exec_sess(a, T0, &out, 2, "UNSUBSCRIBE", "ch");
    EXPECT(out, "*3\r\n$11\r\nunsubscribe\r\n$2\r\nch\r\n:1\r\n");
    exec_sess(b, T0, &out, 3, "PUBLISH", "ch", "hello");
    EXPECT(out, ":0\r\n");
    exec_sess(b, T0, &out, 3, "PUBLISH", "ch2", "yo");
    EXPECT(out, ":1\r\n");

    /* unsubscribe with no args: one push per channel */
    cap_a.len = 0;
    exec_sess(a, T0, &out, 1, "UNSUBSCRIBE");
    EXPECT(out, "*3\r\n$11\r\nunsubscribe\r\n$3\r\nch2\r\n:0\r\n");

    /* no-arg unsubscribe with no subscriptions: single nil push */
    exec_sess(a, T0, &out, 1, "UNSUBSCRIBE");
    EXPECT(out, "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n");

    session_free(b);
    session_free(a);
    resp_buf_free(&cap_a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_subscribed_mode_restriction(void)
{
    db d;
    test_registry reg;
    resp_buf out, cap_a;
    session *a;
    db_init(&d);
    memset(&reg, 0, sizeof(reg));
    resp_buf_init(&out);
    resp_buf_init(&cap_a);
    a = hooked_session(&d, &reg, &cap_a);

    exec_sess(a, T0, &out, 2, "SUBSCRIBE", "ch");
    EXPECT(out, "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");

    /* only a small command set is allowed while subscribed */
    exec_sess(a, T0, &out, 2, "GET", "k");
    EXPECT(out,
           "-ERR Can't execute 'get': only (P)SUBSCRIBE / (P)UNSUBSCRIBE / "
           "PING / QUIT / SHUTDOWN are allowed in this context\r\n");
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out,
           "-ERR Can't execute 'multi': only (P)SUBSCRIBE / (P)UNSUBSCRIBE / "
           "PING / QUIT / SHUTDOWN are allowed in this context\r\n");
    exec_sess(a, T0, &out, 1, "PING");
    EXPECT(out, "+PONG\r\n");
    exec_sess(a, T0, &out, 3, "PUBLISH", "ch", "x"); /* not allowed */
    EXPECT(out,
           "-ERR Can't execute 'publish': only (P)SUBSCRIBE / "
           "(P)UNSUBSCRIBE / PING / QUIT / SHUTDOWN are allowed in this "
           "context\r\n");

    /* after unsubscribing, everything works again */
    exec_sess(a, T0, &out, 1, "UNSUBSCRIBE");
    EXPECT(out, "*3\r\n$11\r\nunsubscribe\r\n$2\r\nch\r\n:0\r\n");
    exec_sess(a, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");

    session_free(a);
    resp_buf_free(&cap_a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_server_registry_rejection_is_transactional(void)
{
    db d;
    session *s;
    resp_buf out;

    DD_CHECK_EQ_INT(0, server_test_pubsub_rejected_subscribe());
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);
    s->subscribe = t_reject_subscribe;
    exec_sess(s, T0, &out, 2, "SUBSCRIBE", "rejected");
    EXPECT(out, "-ERR key or value length is not representable\r\n");
    DD_CHECK_EQ_INT(0, s->nsub);
    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_psubscribe_no_hooks(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    /* registry-less session: the pattern counter moves, confirm frames
     * report the total across nsub/nssub/npsub */
    exec_sess(s, T0, &out, 2, "SUBSCRIBE", "ch");
    EXPECT(out, "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    exec_sess(s, T0, &out, 2, "PSUBSCRIBE", "news.*");
    EXPECT(out, "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:2\r\n");
    exec_sess(s, T0, &out, 2, "PSUBSCRIBE", "news.*"); /* duplicate */
    EXPECT(out, "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:3\r\n");

    /* npsub enters subscribed mode too */
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out,
           "-ERR Can't execute 'get': only (P)SUBSCRIBE / (P)UNSUBSCRIBE / "
           "PING / QUIT / SHUTDOWN are allowed in this context\r\n");

    /* PUBSUB introspection without server hooks: empty/zero (from a
     * non-subscribed session; PUBSUB itself is not whitelisted) */
    {
        session *p = session_create(&d);
        exec_sess(p, T0, &out, 2, "PUBSUB", "CHANNELS");
        EXPECT(out, "*0\r\n");
        exec_sess(p, T0, &out, 3, "PUBSUB", "NUMSUB", "ch");
        EXPECT(out, "*2\r\n$2\r\nch\r\n:0\r\n");
        exec_sess(p, T0, &out, 2, "PUBSUB", "NUMPAT");
        EXPECT(out, ":0\r\n");
        session_free(p);
    }

    /* punsubscribe one pattern, then the no-arg form */
    exec_sess(s, T0, &out, 2, "PUNSUBSCRIBE", "news.*");
    EXPECT(out, "*3\r\n$12\r\npunsubscribe\r\n$6\r\nnews.*\r\n:2\r\n");
    exec_sess(s, T0, &out, 2, "PUNSUBSCRIBE", "news.*");
    EXPECT(out, "*3\r\n$12\r\npunsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    exec_sess(s, T0, &out, 1, "PUNSUBSCRIBE");
    EXPECT(out, "*3\r\n$12\r\npunsubscribe\r\n$-1\r\n:1\r\n");

    /* fully unsubscribed: normal commands work again (registry-less
     * no-arg UNSUBSCRIBE replies a single nil push) */
    exec_sess(s, T0, &out, 1, "UNSUBSCRIBE");
    EXPECT(out, "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_subscribe_publish);
    DD_RUN(test_subscribed_mode_restriction);
    DD_RUN(test_server_registry_rejection_is_transactional);
    DD_RUN(test_psubscribe_no_hooks);
    return DD_TEST_SUMMARY();
}
