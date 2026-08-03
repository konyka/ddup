/* repl.h - master/replica replication state (Phase 7.1).
 *
 * Simplifications (documented in docs/architecture.md): full-resync only
 * (no PSYNC/partial resync), the backlog exists for future partial resync
 * and for master_repl_offset reporting, and replicas that fall too far
 * behind are dropped and must re-SYNC.
 */
#ifndef DDUP_REPL_H
#define DDUP_REPL_H

#include <stddef.h>
#include <stdint.h>

/* Ring buffer of the propagated command stream (master side). */
typedef struct repl_backlog {
    char *buf;
    size_t cap;
    size_t start;      /* oldest byte */
    size_t len;        /* valid bytes in the ring */
    uint64_t offset;   /* total bytes ever appended (master_repl_offset) */
} repl_backlog;

void repl_backlog_init(repl_backlog *b, size_t cap);
void repl_backlog_free(repl_backlog *b);
/* Append, dropping the oldest bytes on overflow. offset counts all bytes
 * ever appended, including dropped ones. */
void repl_backlog_append(repl_backlog *b, const char *data, size_t n);
/* Copy the ring contents (oldest first) into out; returns bytes copied. */
size_t repl_backlog_read(const repl_backlog *b, char *out, size_t max);

#endif /* DDUP_REPL_H */
