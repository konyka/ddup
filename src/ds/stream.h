/* stream.h - compact ordered stream object for the X* command family.
 *
 * Entries are kept in a single insertion-ordered dynamic array. Stream IDs
 * are appended monotonically (the only legal XADD shape), so appends are
 * amortized O(1) and range queries use binary search to find the first/last
 * entry in the requested ID window; entry field/value bytes live in one
 * contiguous allocation per entry. This is a deliberately simpler layout
 * than Redis's radix-tree + listpack stream, trading radix-tree fanout for
 * cache-friendly scans over the common append/range path.
 */
#ifndef DDUP_STREAM_H
#define DDUP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define OBJ_STREAM_ADD_OK 0
#define OBJ_STREAM_ADD_ERR (-1)
#define OBJ_STREAM_ADD_SMALL (-2)

typedef struct stream_entry {
    uint64_t ms;
    uint64_t seq;
    uint32_t nfields;
    uint32_t *lens;  /* 2*nfields item lengths (field,value,...) */
    char *data;      /* field/value bytes concatenated in the same order */
    size_t data_len;
} stream_entry;

typedef struct obj_stream {
    stream_entry *entries;
    size_t len;
    size_t cap;
    uint64_t last_ms;        /* last generated/assigned ID */
    uint64_t last_seq;
    uint64_t entries_added;
    uint64_t max_deleted_ms; /* highest deleted ID (0-0 = none) */
    uint64_t max_deleted_seq;
    uint64_t mem;            /* sizeof(obj_stream) + entries + payloads */
} obj_stream;

obj_stream *obj_stream_new(void);
void obj_stream_free(obj_stream *s);
uint64_t obj_stream_mem(const obj_stream *s);
size_t obj_stream_len(const obj_stream *s);
const stream_entry *obj_stream_at(const obj_stream *s, size_t idx);

/* First entry with id >= ms:seq. Returns len when none. */
size_t obj_stream_lower_bound(const obj_stream *s, uint64_t ms, uint64_t seq);

/* Append one entry; the id must be strictly greater than the stream's
 * last id. Returns OBJ_STREAM_ADD_* above. */
int obj_stream_append(obj_stream *s, uint64_t ms, uint64_t seq,
                      const char *const *fields, const size_t *flens,
                      const char *const *values, const size_t *vlens,
                      size_t nfields);

/* Delete one exact entry. Returns 1 when removed, else 0. */
int obj_stream_delete(obj_stream *s, uint64_t ms, uint64_t seq);

/* Trim the oldest entries, honoring a per-call LIMIT. Return removed count. */
size_t obj_stream_trim_maxlen(obj_stream *s, uint64_t maxlen, uint64_t limit);
size_t obj_stream_trim_minid(obj_stream *s, uint64_t ms, uint64_t seq,
                             uint64_t limit);
#endif /* DDUP_STREAM_H */
