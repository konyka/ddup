/* resp_writer.h - RESP2/RESP3 serialization into a growable byte buffer. */
#ifndef DDUP_RESP_WRITER_H
#define DDUP_RESP_WRITER_H

#include <stddef.h>

#include "resp/resp.h"

typedef struct resp_buf {
    char *data;
    size_t len;
    size_t cap;
} resp_buf;

void resp_buf_init(resp_buf *b);
void resp_buf_free(resp_buf *b);
/* Ensure room for n more bytes (internal, but useful for zero-copy appends). */
void resp_buf_reserve(resp_buf *b, size_t n);

void resp_write_simple_string(resp_buf *b, const char *s, size_t len);
void resp_write_error(resp_buf *b, const char *s, size_t len);
void resp_write_integer(resp_buf *b, long long v);
/* s == NULL writes the RESP2 null bulk string ($-1). */
void resp_write_bulk(resp_buf *b, const char *s, size_t len);
void resp_write_array_header(resp_buf *b, size_t n);

/* RESP3 */
void resp_write_null(resp_buf *b);              /* _\r\n */
void resp_write_boolean(resp_buf *b, int v);    /* #t / #f */
void resp_write_double(resp_buf *b, double v);  /* ,<17g> / inf / -inf / nan */
void resp_write_big_number(resp_buf *b, const char *digits, size_t len);
void resp_write_map_header(resp_buf *b, size_t pairs);
void resp_write_set_header(resp_buf *b, size_t n);
void resp_write_push_header(resp_buf *b, size_t n);

/* Serialize a parsed value back to the wire format. */
void resp_write_value(resp_buf *b, const resp_value *v);

#endif /* DDUP_RESP_WRITER_H */
