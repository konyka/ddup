/* aof.h - append-only file persistence.
 *
 * Format: the raw RESP command stream. Every successful mutating command
 * is re-serialized as a RESP array of bulk strings and appended (buffered,
 * flushed once per server loop iteration). Durability follows the
 * appendfsync policy (Redis semantics): every flush (always), at most once
 * per wall-clock second via an injectable clock (everysec, the default;
 * a final sync always runs at close), or never (no).
 *
 * Replay tolerates a truncated tail (last command partially written).
 */
#ifndef DDUP_AOF_H
#define DDUP_AOF_H

#include <stddef.h>
#include <stdint.h>

#include "core/command.h"
#include "core/session.h"
#include "pal/pal_file.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

/* appendfsync policies (aof.fsync_mode). */
#define AOF_FSYNC_ALWAYS 0
#define AOF_FSYNC_EVERYSEC 1 /* default, like Redis */
#define AOF_FSYNC_NO 2

typedef struct aof {
    pal_file *f;
    char *path;
    resp_buf pending; /* serialized commands not yet flushed to the file */
    ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n);
    int failed; /* write result was ambiguous; do not retry buffered bytes */
    int fsync_mode;              /* AOF_FSYNC_* */
    uint64_t last_sync_ms;       /* wall-ms of the last successful sync */
    int (*sync_fn)(pal_file *f); /* pal_file_sync by default */
    uint64_t (*now_fn)(void);    /* pal_wall_ms by default */
} aof;

/* Open for appending (created if missing). NULL on error. */
aof *aof_open(const char *path);

/* Buffer one command (argv of string-typed values) as a RESP array. */
void aof_log_cmd(aof *a, const resp_value *argv, size_t argc);

/* Set the appendfsync policy (AOF_FSYNC_*). */
void aof_set_fsync_mode(aof *a, int mode);

/* Write pending bytes to the file and apply the fsync policy. 0 on success,
 * -1 on a latched failure (a sync failure latches exactly like a flush
 * failure: fail-closed). */
int aof_flush(aof *a);

/* Flush and return the durable append offset. */
uint64_t aof_durable_offset(aof *a);

/* Atomically copy bytes appended after offset into path. */
int aof_copy_delta(aof *a, uint64_t offset, const char *path);

/* Replace the writer for deterministic short-write/error tests. */
void aof_test_set_write_fn(
    aof *a, ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n));
/* Test seams: deterministic sync failures and an injectable wall clock. */
void aof_test_set_sync_fn(aof *a, int (*sync_fn)(pal_file *f));
void aof_test_set_now_fn(aof *a, uint64_t (*now_fn)(void));

/* Flush, run the policy's final sync (everysec/always), and close. */
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
