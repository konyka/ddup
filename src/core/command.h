/* command.h - RESP command dispatch over the in-memory store. */
#ifndef DDUP_COMMAND_H
#define DDUP_COMMAND_H

#include <stddef.h>

#include "core/rhtable.h"
#include "resp/resp.h"
#include "resp/resp_writer.h"

/* One logical database. Shared-nothing: each IO thread owns its own. */
typedef struct db {
    rh_table table;
} db;

void db_init(db *d);
void db_destroy(db *d);

/* Execute one command. argv items must be string-typed values (bulk/simple);
 * the RESP reply is appended to out. */
void command_execute(db *d, const resp_value *argv, size_t argc, resp_buf *out);

#endif /* DDUP_COMMAND_H */
