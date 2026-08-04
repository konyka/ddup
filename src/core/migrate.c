/* migrate.c - MIGRATE implementation; see migrate.h. */
#include "core/migrate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/snapshot.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "resp/resp_writer.h"

static migrate_pump_fn g_pump;
static void *g_pump_ctx;

void migrate_set_pump_hook(migrate_pump_fn fn, void *ctx)
{
    g_pump = fn;
    g_pump_ctx = ctx;
}

/* Give the target a chance to run: the test hook pumps its event loop;
 * without a hook (real remote target) avoid a busy spin. */
static void migrate_pump(void)
{
    if (g_pump != NULL)
        g_pump(g_pump_ctx);
    else
        pal_sleep_ms(1);
}

/* Blocking-with-deadline send of the whole buffer (non-blocking socket). */
static int send_all(pal_socket_t fd, const char *buf, size_t len,
                    uint64_t deadline)
{
    size_t off = 0;
    while (off < len) {
        ptrdiff_t w = pal_send(fd, buf + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && !pal_would_block(pal_socket_error()))
            return -1;
        if (pal_now_ms() >= deadline)
            return -1;
        migrate_pump();
    }
    return 0;
}

/* Read one \n-terminated reply line (without the terminator).
 * Returns the line length, or -1 on error/timeout/close/overrun. */
static ptrdiff_t read_line(pal_socket_t fd, char *buf, size_t cap,
                           uint64_t deadline)
{
    size_t n = 0;
    for (;;) {
        char c;
        ptrdiff_t r = pal_recv(fd, &c, 1);
        if (r == 1) {
            if (c == '\n') {
                if (n > 0 && buf[n - 1] == '\r')
                    n--;
                return (ptrdiff_t)n;
            }
            if (n + 1 >= cap)
                return -1;
            buf[n++] = c;
            continue;
        }
        if (r == 0)
            return -1; /* orderly close mid-reply */
        if (!pal_would_block(pal_socket_error()))
            return -1;
        if (pal_now_ms() >= deadline)
            return -1;
        migrate_pump();
    }
}

/* Remaining ttl in ms (0 = none), written as a decimal string. */
static size_t ttl_arg(db *d, const char *key, size_t klen, uint64_t now_ms,
                      char out[24])
{
    const char *ev;
    size_t evl;
    uint64_t abs_ms = 0, left = 0;
    if (rh_get(&d->expires, key, klen, &ev, &evl) && evl == 8) {
        memcpy(&abs_ms, ev, 8);
        if (abs_ms > now_ms)
            left = abs_ms - now_ms;
    }
    return (size_t)snprintf(out, 24, "%llu", (unsigned long long)left);
}

int migrate_run(db *d, const char *host, uint16_t port,
                const resp_value *keys, size_t nkeys, uint64_t timeout_ms,
                int copy, int replace, uint64_t now_ms)
{
    resp_buf pipebuf, payload;
    size_t *idx;
    size_t nkeys_live = 0, i, confirmed = 0;
    pal_socket_t fd;
    uint64_t deadline;
    int rc = MIGRATE_IOERR;

    idx = (size_t *)malloc(nkeys * sizeof(size_t));
    if (idx == NULL)
        return MIGRATE_IOERR;
    resp_buf_init(&pipebuf);
    resp_buf_init(&payload);

    /* pipeline one RESTORE per live key */
    for (i = 0; i < nkeys; i++) {
        const char *k = keys[i].str;
        size_t kl = keys[i].len;
        const char *v;
        size_t vl;
        char hdr[64], ttl[24];
        size_t tl;

        db_expire_if_needed(d, k, kl, now_ms);
        if (!rh_get(&d->table, k, kl, &v, &vl))
            continue; /* missing: skipped */
        payload.len = 0;
        if (snapshot_dump_key(d, k, kl, &payload) != 0)
            continue;
        tl = ttl_arg(d, k, kl, now_ms, ttl);
        resp_buf_reserve(&pipebuf, sizeof(hdr) + kl + tl + payload.len + 48);
        /* ASKING first: the target may be importing this slot (cluster
         * migration); in non-cluster mode it is a harmless +OK. */
        pipebuf.len += (size_t)snprintf(pipebuf.data + pipebuf.len,
                                        sizeof(hdr), "*1\r\n$6\r\nASKING\r\n");
        resp_buf_reserve(&pipebuf, sizeof(hdr));
        pipebuf.len += (size_t)snprintf(
            pipebuf.data + pipebuf.len, sizeof(hdr), "*%d\r\n$7\r\nRESTORE\r\n",
            replace ? 5 : 4);
        resp_write_bulk(&pipebuf, k, kl);
        resp_write_bulk(&pipebuf, ttl, tl);
        resp_write_bulk(&pipebuf, payload.data, payload.len);
        if (replace)
            resp_write_bulk(&pipebuf, "REPLACE", 7);
        idx[nkeys_live++] = i;
    }
    if (nkeys_live == 0) {
        rc = MIGRATE_OK; /* nothing to transfer */
        goto out;
    }

    fd = pal_tcp_connect(host, port);
    if (fd == PAL_SOCKET_INVALID)
        goto out;
    if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd);
        goto out;
    }
    deadline = pal_now_ms() + timeout_ms;
    if (send_all(fd, pipebuf.data, pipebuf.len, deadline) == 0) {
        char line[256];
        /* one ASKING + one RESTORE reply per key, both must be +OK */
        while (confirmed < nkeys_live) {
            if (read_line(fd, line, sizeof(line), deadline) < 0)
                break;
            if (line[0] != '+')
                break;
            if (read_line(fd, line, sizeof(line), deadline) < 0)
                break;
            if (line[0] != '+')
                break; /* target error (e.g. BUSYKEY): stop here */
            confirmed++;
        }
        if (!copy) {
            for (i = 0; i < confirmed; i++)
                db_del_kv(d, keys[idx[i]].str, keys[idx[i]].len);
        }
        if (confirmed == nkeys_live)
            rc = MIGRATE_OK;
    }
    pal_close(fd);

out:
    resp_buf_free(&payload);
    resp_buf_free(&pipebuf);
    free(idx);
    return rc;
}
