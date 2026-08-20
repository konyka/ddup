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

typedef struct stream_pending {
    uint64_t ms;
    uint64_t seq;
    uint64_t idle;           /* absolute wall ms of delivery */
    uint64_t delivery_count;
} stream_pending;

typedef struct stream_consumer {
    char *name;
    size_t name_len;
    stream_pending *pel;
    size_t pel_len;
    size_t pel_cap;
    uint64_t seen_time;   /* last successful read / claim */
    uint64_t active_time; /* creation wall ms */
} stream_consumer;

typedef struct stream_group {
    struct obj_stream *stream; /* back-pointer for group_mem accounting */
    char *name;
    size_t name_len;
    uint64_t last_ms;     /* last delivered id (exclusive cursor) */
    uint64_t last_seq;
    uint64_t entries_read;
    stream_consumer *consumers;
    size_t nconsumers;
    size_t consumers_cap;
} stream_group;

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
    uint64_t group_mem;      /* group/consumer/PEL subtree bytes */
    stream_group *groups;
    size_t ngroups;
    size_t groups_cap;
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

/* ------------------------------------------------------------------ */
/* consumer groups                                                    */
/* ------------------------------------------------------------------ */

stream_group *obj_stream_group_get(obj_stream *s, const char *name,
                                   size_t name_len);
/* Existing group returned with *created = 0. */
stream_group *obj_stream_group_create(obj_stream *s, const char *name,
                                      size_t name_len, uint64_t last_ms,
                                      uint64_t last_seq, int *created);
int obj_stream_group_destroy(obj_stream *s, const char *name,
                             size_t name_len);

stream_consumer *obj_stream_consumer_get(stream_group *g, const char *name,
                                         size_t name_len);
stream_consumer *obj_stream_consumer_create(stream_group *g,
                                            const char *name,
                                            size_t name_len);
int obj_stream_consumer_destroy(stream_group *g, const char *name,
                                size_t name_len);

/* Add a pending entry to a consumer PEL. Returns NULL on duplicate or
 * unrepresentable length. */
stream_pending *obj_stream_consumer_pel_add(stream_group *g,
                                            stream_consumer *c,
                                            uint64_t ms, uint64_t seq,
                                            uint64_t idle,
                                            uint64_t delivery_count);
stream_pending *obj_stream_consumer_pel_find(stream_consumer *c,
                                             uint64_t ms, uint64_t seq);
int obj_stream_consumer_pel_remove(stream_consumer *c, uint64_t ms,
                                   uint64_t seq);
/* Remove the first pending entry with this id from any consumer. */
int obj_stream_group_pel_remove(stream_group *g, uint64_t ms, uint64_t seq);
uint64_t obj_stream_group_pending_count(const stream_group *g);
#endif /* DDUP_STREAM_H */
