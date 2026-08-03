/* resp.h - RESP2/RESP3 value model (zero-copy: strings point into the
 * receive buffer; arrays are allocated from a caller-supplied arena). */
#ifndef DDUP_RESP_H
#define DDUP_RESP_H

#include <stddef.h>

typedef enum resp_type {
    /* RESP2 */
    RESP_SIMPLE_STRING,  /* +OK            -> str/len                     */
    RESP_ERROR,          /* -ERR ...       -> str/len                     */
    RESP_INTEGER,        /* :42            -> integer                     */
    RESP_BULK_STRING,    /* $3\r\nfoo      -> str/len; str==NULL: null    */
    RESP_ARRAY,          /* *2 ...         -> items/count; items==NULL: null */
    /* RESP3 */
    RESP_NULL,           /* _              -> (no payload)                */
    RESP_BOOLEAN,        /* #t / #f        -> integer 1/0                 */
    RESP_DOUBLE,         /* ,3.14          -> dbl                         */
    RESP_BIG_NUMBER,     /* (123...        -> str/len (digits)            */
    RESP_BLOB_ERROR,     /* !21\r\n...     -> str/len                     */
    RESP_VERBATIM_STRING,/* =15\r\ntxt:... -> str/len (incl. "txt:" pfx)  */
    RESP_MAP,            /* %2 ...         -> items/count, count = 2*pairs*/
    RESP_SET,            /* ~3 ...         -> items/count                 */
    RESP_PUSH            /* >3 ...         -> items/count                 */
} resp_type;

typedef struct resp_value {
    resp_type type;
    const char *str;         /* string payload, zero-copy into input buffer */
    size_t len;
    long long integer;       /* RESP_INTEGER / RESP_BOOLEAN */
    double dbl;              /* RESP_DOUBLE */
    struct resp_value *items;/* RESP_ARRAY/MAP/SET/PUSH children (arena) */
    size_t count;            /* child count; for RESP_MAP: 2 * pair count */
} resp_value;

#endif /* DDUP_RESP_H */
