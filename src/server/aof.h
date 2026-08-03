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
#include "pal/pal_file.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

typedef struct aof {
    pal_file *f;
    resp_buf pending; /* serialized commands not yet flushed to the file */
} aof;

/* Open for appending (created if missing). NULL on error. */
aof *aof_open(const char *path);

/* Buffer one command (argv of string-typed values) as a RESP array. */
void aof_log_cmd(aof *a, const resp_value *argv, size_t argc);

/* Write pending bytes to the file. 0 on success. */
int aof_flush(aof *a);

/* Flush and close. */
void aof_close(aof *a);

/* Replay the file into d (stack session, no hooks; nothing is re-logged).
 * Returns 0 on success (including a tolerated truncated tail), -1 if the
 * file cannot be read. */
int aof_replay(db *d, const char *path);

#endif /* DDUP_AOF_H */
