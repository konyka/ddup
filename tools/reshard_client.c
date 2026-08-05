/* reshard_client.c - see reshard_client.h. */
#include "reshard_client.h"

#include <stdlib.h>
#include <string.h>

#include "pal/pal_time.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

/* inactivity deadline for one round-trip (server-side MIGRATE timeouts are
 * bounded separately) */
#define RS_IO_TIMEOUT_MS 30000
#define RS_MAX_REPLY (64u * 1024u * 1024u)

int rs_connect(rs_conn *c, const char *host, uint16_t port)
{
    c->fd = pal_tcp_connect(host, port);
    if (c->fd == PAL_SOCKET_INVALID)
        return -1;
    c->buf = (char *)malloc(16384);
    if (c->buf == NULL) {
        pal_close(c->fd);
        c->fd = PAL_SOCKET_INVALID;
        return -1;
    }
    c->len = 0;
    c->cap = 16384;
    return 0;
}

void rs_close(rs_conn *c)
{
    if (c->fd != PAL_SOCKET_INVALID)
        pal_close(c->fd);
    free(c->buf);
    c->fd = PAL_SOCKET_INVALID;
    c->buf = NULL;
    c->len = 0;
    c->cap = 0;
}

/* recv into c->buf until one full value parses, with an inactivity
 * deadline. Returns bytes consumed (>0), 0 on timeout, -1 on error. */
static ptrdiff_t rs_read_reply(rs_conn *c, arena *ar, resp_value *v)
{
    uint64_t deadline = pal_now_ms() + RS_IO_TIMEOUT_MS;
    for (;;) {
        ptrdiff_t used;
        if (c->len > 0) {
            used = resp_parse(c->buf, c->len, v, ar);
            if (used != 0)
                return used; /* >0 parsed, -1 protocol error */
        }
        if (pal_now_ms() >= deadline)
            return 0;
        if (c->cap - c->len < 4096) {
            size_t ncap = c->cap * 2;
            char *nb;
            if (ncap > RS_MAX_REPLY)
                return -1;
            nb = (char *)realloc(c->buf, ncap);
            if (nb == NULL)
                return -1;
            c->buf = nb;
            c->cap = ncap;
        }
        {
            ptrdiff_t n = pal_recv(c->fd, c->buf + c->len, c->cap - c->len);
            if (n > 0) {
                c->len += (size_t)n;
            } else if (n == 0) {
                return -1; /* orderly close mid-reply */
            } else if (!pal_would_block(pal_socket_error())) {
                return -1;
            } else {
                pal_sleep_ms(1);
            }
        }
    }
}

int rs_exec(rs_conn *c, arena *ar, resp_value *v, int argc,
            const char *const *args, const size_t *lens)
{
    resp_buf req;
    ptrdiff_t used;
    size_t sent = 0;
    int i;
    uint64_t deadline;

    /* build the full request first: args may be zero-copy views into this
     * connection's own buffer (previous reply), which the read below
     * overwrites */
    resp_buf_init(&req);
    resp_write_array_header(&req, (size_t)argc);
    for (i = 0; i < argc; i++) {
        size_t n = lens != NULL ? lens[i] : strlen(args[i]);
        resp_write_bulk(&req, args[i], n);
    }

    c->len = 0; /* drop the previous reply (already consumed by caller) */
    arena_reset(ar);

    deadline = pal_now_ms() + RS_IO_TIMEOUT_MS;
    while (sent < req.len) {
        ptrdiff_t n = pal_send(c->fd, req.data + sent, req.len - sent);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && pal_would_block(pal_socket_error())) {
            if (pal_now_ms() >= deadline) {
                resp_buf_free(&req);
                return -1;
            }
            pal_sleep_ms(1);
        } else {
            resp_buf_free(&req);
            return -1;
        }
    }
    resp_buf_free(&req);

    used = rs_read_reply(c, ar, v);
    if (used <= 0)
        return -1;
    return 0;
}

/* expect +OK; logs the server error text on mismatch. Returns 0/+OK. */
static int rs_expect_ok(rs_conn *c, arena *ar, resp_value *v, int argc,
                        const char *const *args, FILE *log)
{
    if (rs_exec(c, ar, v, argc, args, NULL) != 0)
        return -1;
    if (v->type == RESP_SIMPLE_STRING && v->len == 2 &&
        memcmp(v->str, "OK", 2) == 0)
        return 0;
    if (log != NULL && v->type == RESP_ERROR)
        fprintf(log, "reshard: server error: %.*s\n", (int)v->len, v->str);
    return -1;
}

/* read a 40-char node id (CLUSTER MYID) */
static int rs_myid(rs_conn *c, arena *ar, resp_value *v, char id[41])
{
    const char *args[] = {"CLUSTER", "MYID"};
    if (rs_exec(c, ar, v, 2, args, NULL) != 0)
        return -1;
    if (v->type != RESP_BULK_STRING || v->len != 40 || v->str == NULL)
        return -1;
    memcpy(id, v->str, 40);
    id[40] = '\0';
    return 0;
}

int reshard_slot(const char *from_host, uint16_t from_port,
                 const char *to_host, uint16_t to_port, int slot, int count,
                 int timeout_ms, long long *migrated, FILE *log)
{
    rs_conn from, to;
    arena ar;
    resp_value v;
    char from_id[41], to_id[41];
    char slots[16], counts[16], tos[16], ports[16];
    long long total = 0;
    int rc = -1;

    if (slot < 0 || slot >= 16384 || count < 1)
        return -1;
    snprintf(slots, sizeof(slots), "%d", slot);
    snprintf(counts, sizeof(counts), "%d", count);
    snprintf(tos, sizeof(tos), "%d", timeout_ms);
    snprintf(ports, sizeof(ports), "%u", (unsigned)to_port);

    arena_init(&ar, 4096);
    if (rs_connect(&from, from_host, from_port) != 0) {
        if (log != NULL)
            fprintf(log, "reshard: cannot connect to %s:%u\n", from_host,
                    (unsigned)from_port);
        arena_destroy(&ar);
        return -1;
    }
    if (rs_connect(&to, to_host, to_port) != 0) {
        if (log != NULL)
            fprintf(log, "reshard: cannot connect to %s:%u\n", to_host,
                    (unsigned)to_port);
        rs_close(&from);
        arena_destroy(&ar);
        return -1;
    }

    if (rs_myid(&from, &ar, &v, from_id) != 0 ||
        rs_myid(&to, &ar, &v, to_id) != 0) {
        if (log != NULL)
            fprintf(log, "reshard: CLUSTER MYID failed (cluster mode?)\n");
        goto out;
    }

    /* mark the slot migrating on the source, importing on the target */
    {
        const char *args[] = {"CLUSTER", "SETSLOT", slots,
                              "MIGRATING", "TO", to_id};
        if (rs_expect_ok(&from, &ar, &v, 6, args, log) != 0)
            goto out;
    }
    {
        const char *args[] = {"CLUSTER", "SETSLOT", slots,
                              "IMPORTING", "FROM", from_id};
        if (rs_expect_ok(&to, &ar, &v, 6, args, log) != 0)
            goto out;
    }

    /* move keys in batches */
    for (;;) {
        const char *gargs[] = {"CLUSTER", "GETKEYSINSLOT", slots, counts};
        const char **margs;
        size_t *mlens;
        size_t nkeys, ki;
        int ok;

        if (rs_exec(&from, &ar, &v, 4, gargs, NULL) != 0)
            goto out;
        if (v.type != RESP_ARRAY)
            goto out;
        nkeys = v.count;
        if (nkeys == 0)
            break;

        /* key payloads are zero-copy views into the conn buffer: gather
         * them before the MIGRATE call rebuilds that buffer */
        margs = (const char **)malloc((nkeys + 8) * sizeof(*margs));
        mlens = (size_t *)malloc((nkeys + 8) * sizeof(*mlens));
        if (margs == NULL || mlens == NULL) {
            free(margs);
            free(mlens);
            goto out;
        }
        for (ki = 0; ki < nkeys; ki++) {
            margs[8 + ki] = v.items[ki].str;
            mlens[8 + ki] = v.items[ki].len;
        }
        margs[0] = "MIGRATE";
        mlens[0] = 7;
        margs[1] = to_host;
        mlens[1] = strlen(to_host);
        margs[2] = ports;
        mlens[2] = strlen(ports);
        margs[3] = "";
        mlens[3] = 0;
        margs[4] = "0";
        mlens[4] = 1;
        margs[5] = tos;
        mlens[5] = strlen(tos);
        margs[6] = "REPLACE";
        mlens[6] = 7;
        margs[7] = "KEYS";
        mlens[7] = 4;

        ok = rs_exec(&from, &ar, &v, (int)(8 + nkeys),
                     (const char *const *)margs, mlens);
        free(margs);
        free(mlens);
        if (ok != 0)
            goto out;
        if (v.type != RESP_SIMPLE_STRING) {
            if (log != NULL && v.type == RESP_ERROR)
                fprintf(log, "reshard: MIGRATE failed: %.*s\n",
                        (int)v.len, v.str);
            goto out;
        }
        total += (long long)nkeys;
        if (log != NULL)
            fprintf(log, "reshard: slot %d: %lld keys migrated so far\n",
                    slot, total);
    }

    /* finalize ownership on both ends */
    {
        const char *args[] = {"CLUSTER", "SETSLOT", slots, "NODE", to_id};
        if (rs_expect_ok(&from, &ar, &v, 5, args, log) != 0)
            goto out;
        if (rs_expect_ok(&to, &ar, &v, 5, args, log) != 0)
            goto out;
    }
    if (migrated != NULL)
        *migrated = total;
    rc = 0;

out:
    rs_close(&from);
    rs_close(&to);
    arena_destroy(&ar);
    return rc;
}
