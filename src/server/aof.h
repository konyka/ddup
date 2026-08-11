/* aof.h - append-only file persistence.
 *
 * Format: the raw RESP command stream. Every successful mutating command
 * is re-serialized as a RESP array of bulk strings and appended (buffered,
 * flushed once per server loop iteration — an appendfsync-everysec-ish
 * simplification; see docs/architecture.md).
 *
 * Replay tolerates a truncated tail (last command partially written).
 */
#ifndef DDUP_AOF_H
#define DDUP_AOF_H

#include <stddef.h>

#include "core/command.h"
#include "core/session.h"
#include "pal/pal_file.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

typedef struct aof {
    pal_file *f;
    resp_buf pending; /* serialized commands not yet flushed to the file */
    ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n);
    int failed; /* write result was ambiguous; do not retry buffered bytes */
} aof;

/* Open for appending (created if missing). NULL on error. */
aof *aof_open(const char *path);

/* Buffer one command (argv of string-typed values) as a RESP array. */
void aof_log_cmd(aof *a, const resp_value *argv, size_t argc);

/* Write pending bytes to the file. 0 on success, -1 on a latched failure. */
int aof_flush(aof *a);

/* Replace the writer for deterministic short-write/error tests. */
void aof_test_set_write_fn(
    aof *a, ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n));

/* Flush and close. */
void aof_close(aof *a);

/* Replay the file into d (stack session, no hooks; nothing is re-logged).
 * Replay is all-or-nothing: a read, malformed-frame, invalid-command, or
 * execution error leaves d unchanged. A truncated final command is tolerated.
 */
int aof_replay(db *d, const char *path);

/* Replay the file through a full session (multi-db AOFs: embedded SELECT
 * commands switch the target db via the session's selection hook). The
 * session and all selected databases are unchanged when replay fails. */
int aof_replay_session(session *s, const char *path);

#endif /* DDUP_AOF_H */
