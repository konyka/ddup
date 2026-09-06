/* resp_writer.c - RESP serialization; see resp_writer.h. */
#include "resp/resp_writer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/buf_pool.h"

void resp_buf_init(resp_buf *b)
{
    if (b == NULL)
        return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->pool = NULL;
    b->pool_size = 0;
}

void resp_buf_free(resp_buf *b)
{
    if (b == NULL)
        return;
    if (b->pool != NULL && b->data != NULL) {
        buf_pool_put(b->pool, b->data, b->pool_size);
    } else {
        free(b->data);
    }
    b->data = NULL;
    b->len = b->cap = 0;
    b->pool = NULL;
    b->pool_size = 0;
}

int resp_buf_reserve(resp_buf *b, size_t n)
{
    size_t cap, needed;
    if (b == NULL)
        return -1;
    if (n > SIZE_MAX - b->len)
        return -1;
    needed = b->len + n;
    if (needed <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    if (b->pool != NULL) {
        size_t actual;
        char *p = (char *)buf_pool_get(b->pool, cap, &actual);
        if (p == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        if (b->data != NULL) {
            memcpy(p, b->data, b->len);
            buf_pool_put(b->pool, b->data, b->pool_size);
        }
        b->data = p;
        b->cap = actual;
        b->pool_size = actual;
    } else {
        char *p = (char *)realloc(b->data, cap);
        if (p == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        b->data = p;
        b->cap = cap;
    }
    return 0;
}

static int buf_append(resp_buf *b, const char *s, size_t n)
{
    if (b == NULL || (s == NULL && n != 0))
        return -1;
    if (resp_buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 0;
}

/* Fast unsigned-to-decimal; returns number of bytes written (max 20). */
static size_t u64_to_str(char *out, unsigned long long v)
{
    static const char digits100[] =
        "00010203040506070809"
        "10111213141516171819"
        "20212223242526272829"
        "30313233343536373839"
        "40414243444546474849"
        "50515253545556575859"
        "60616263646566676869"
        "70717273747576777879"
        "80818283848586878889"
        "90919293949596979899";
    char tmp[20];
    size_t n = 0;
    while (v >= 100) {
        unsigned long long rem = v % 100;
        v /= 100;
        tmp[n++] = digits100[rem * 2 + 1];
        tmp[n++] = digits100[rem * 2];
    }
    if (v < 10)
        tmp[n++] = (char)('0' + v);
    else {
        tmp[n++] = digits100[v * 2 + 1];
        tmp[n++] = digits100[v * 2];
    }
    for (size_t i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

#ifdef DDUP_TESTING
size_t resp_test_u64_to_str(char *out, unsigned long long v)
{
    return u64_to_str(out, v);
}
#endif

static size_t ll_to_str(char *out, long long v)
{
    unsigned long long u;
    size_t n = 0;
    if (v < 0) {
        out[n++] = '-';
        u = 0ULL - (unsigned long long)v; /* handles LLONG_MIN */
    } else {
        u = (unsigned long long)v;
    }
    return n + u64_to_str(out + n, u);
}

static void write_typed_line(resp_buf *b, char type, const char *s, size_t len)
{
    if (b == NULL || (s == NULL && len != 0))
        return;
    if (len > SIZE_MAX - 3 || resp_buf_reserve(b, len + 3) != 0)
        return;
    b->data[b->len++] = type;
    if (len > 0)
        memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len++] = '\r';
    b->data[b->len++] = '\n';
}

static void write_header(resp_buf *b, char type, size_t n)
{
    char num[20];
    size_t len = u64_to_str(num, (unsigned long long)n);
    if (resp_buf_reserve(b, 1 + len + 2) != 0)
        return;
    b->data[b->len++] = type;
    memcpy(b->data + b->len, num, len);
    b->len += len;
    b->data[b->len++] = '\r';
    b->data[b->len++] = '\n';
}

void resp_write_simple_string(resp_buf *b, const char *s, size_t len)
{
    if (b == NULL || (s == NULL && len != 0))
        return;
    write_typed_line(b, '+', s, len);
}

void resp_write_error(resp_buf *b, const char *s, size_t len)
{
    if (b == NULL || (s == NULL && len != 0))
        return;
    write_typed_line(b, '-', s, len);
}

void resp_write_integer(resp_buf *b, long long v)
{
    if (b == NULL)
        return;
    char num[24];
    size_t len = ll_to_str(num, v);
    write_typed_line(b, ':', num, len);
}

void resp_write_bulk(resp_buf *b, const char *s, size_t len)
{
    if (b == NULL || (s == NULL && len != 0))
        return;
    if (!s) {
        buf_append(b, "$-1\r\n", 5);
        return;
    }
    char num[20];
    size_t nl = u64_to_str(num, (unsigned long long)len);
    size_t overhead = 1 + nl + 2 + 2;
    if (len > SIZE_MAX - overhead ||
        resp_buf_reserve(b, overhead + len) != 0)
        return;
    b->data[b->len++] = '$';
    memcpy(b->data + b->len, num, nl);
    b->len += nl;
    b->data[b->len++] = '\r';
    b->data[b->len++] = '\n';
    memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len++] = '\r';
    b->data[b->len++] = '\n';
}

void resp_write_array_header(resp_buf *b, size_t n)
{
    if (b == NULL)
        return;
    write_header(b, '*', n);
}

void resp_write_null(resp_buf *b)
{
    if (b == NULL)
        return;
    buf_append(b, "_\r\n", 3);
}

void resp_write_boolean(resp_buf *b, int v)
{
    if (b == NULL)
        return;
    buf_append(b, v ? "#t\r\n" : "#f\r\n", 4);
}

void resp_write_double(resp_buf *b, double v)
{
    if (b == NULL)
        return;
    if (isinf(v)) {
        buf_append(b, v > 0 ? ",inf\r\n" : ",-inf\r\n", v > 0 ? 6 : 7);
        return;
    }
    if (isnan(v)) {
        buf_append(b, ",nan\r\n", 6);
        return;
    }
    char num[32];
    int len = snprintf(num, sizeof(num), "%.17g", v);
    write_typed_line(b, ',', num, (size_t)len);
}

void resp_write_big_number(resp_buf *b, const char *digits, size_t len)
{
    if (b == NULL || (digits == NULL && len != 0))
        return;
    write_typed_line(b, '(', digits, len);
}

void resp_write_map_header(resp_buf *b, size_t pairs)
{
    if (b == NULL)
        return;
    write_header(b, '%', pairs);
}

void resp_write_set_header(resp_buf *b, size_t n)
{
    if (b == NULL)
        return;
    write_header(b, '~', n);
}

void resp_write_push_header(resp_buf *b, size_t n)
{
    if (b == NULL)
        return;
    write_header(b, '>', n);
}

void resp_write_value(resp_buf *b, const resp_value *v)
{
    if (b == NULL || v == NULL)
        return;
    if ((v->type == RESP_SIMPLE_STRING || v->type == RESP_ERROR ||
         v->type == RESP_BULK_STRING || v->type == RESP_BIG_NUMBER ||
         v->type == RESP_BLOB_ERROR || v->type == RESP_VERBATIM_STRING) &&
        v->str == NULL && v->len != 0)
        return;
    switch (v->type) {
    case RESP_SIMPLE_STRING:
        resp_write_simple_string(b, v->str, v->len);
        break;
    case RESP_ERROR:
        resp_write_error(b, v->str, v->len);
        break;
    case RESP_INTEGER:
        resp_write_integer(b, v->integer);
        break;
    case RESP_BULK_STRING:
        resp_write_bulk(b, v->str, v->len);
        break;
    case RESP_NULL:
        resp_write_null(b);
        break;
    case RESP_BOOLEAN:
        resp_write_boolean(b, (int)v->integer);
        break;
    case RESP_DOUBLE:
        resp_write_double(b, v->dbl);
        break;
    case RESP_BIG_NUMBER:
        resp_write_big_number(b, v->str, v->len);
        break;
    case RESP_BLOB_ERROR:
        write_typed_line(b, '!', v->str, v->len);
        break;
    case RESP_VERBATIM_STRING:
        write_header(b, '=', v->len);
        buf_append(b, v->str, v->len);
        buf_append(b, "\r\n", 2);
        break;
    case RESP_ARRAY:
    case RESP_MAP:
    case RESP_SET:
    case RESP_PUSH:
        if (v->type == RESP_ARRAY && v->is_null) {
            buf_append(b, "*-1\r\n", 5); /* RESP2 null array */
            break;
        }
        if (v->type == RESP_MAP)
            resp_write_map_header(b, v->count / 2);
        else if (v->type == RESP_SET)
            resp_write_set_header(b, v->count);
        else if (v->type == RESP_PUSH)
            resp_write_push_header(b, v->count);
        else
            resp_write_array_header(b, v->count);
        for (size_t i = 0; i < v->count; i++)
            resp_write_value(b, &v->items[i]);
        break;
    }
}
