/* obj.c - typed value objects; see obj.h. */
#include "ds/obj.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int obj_tag_of(const char *val, size_t vlen)
{
    if (vlen == 0)
        return DDUP_OBJ_STRING;
    return (unsigned char)val[0];
}

void obj_str(const char *val, size_t vlen, const char **s, size_t *len)
{
    *s = val + 1;
    *len = vlen - 1;
}

void obj_pack_ptr(char buf[9], int tag, const void *ptr)
{
    buf[0] = (char)tag;
    memcpy(buf + 1, &ptr, 8);
}

void *obj_unpack_ptr(const char *val, size_t vlen)
{
    void *ptr = NULL;
    if (vlen >= 9)
        memcpy(&ptr, val + 1, 8);
    return ptr;
}

uint64_t obj_extra_mem(const char *val, size_t vlen)
{
    switch (obj_tag_of(val, vlen)) {
    case DDUP_OBJ_HASH:
        return obj_hash_mem((obj_hash *)obj_unpack_ptr(val, vlen));
    default:
        return 0;
    }
}

void obj_free_value(const char *val, size_t vlen)
{
    switch (obj_tag_of(val, vlen)) {
    case DDUP_OBJ_HASH:
        obj_hash_free((obj_hash *)obj_unpack_ptr(val, vlen));
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* hash object                                                        */
/* ------------------------------------------------------------------ */

/* Same per-entry estimate as the db layer uses for the main table. */
static uint64_t field_bytes(size_t flen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + flen + vlen;
}

obj_hash *obj_hash_new(void)
{
    obj_hash *h = (obj_hash *)malloc(sizeof(*h));
    if (h == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    rh_init(&h->fields);
    h->mem = (uint64_t)sizeof(*h);
    return h;
}

void obj_hash_free(obj_hash *h)
{
    if (h == NULL)
        return;
    rh_destroy(&h->fields);
    free(h);
}

uint64_t obj_hash_mem(const obj_hash *h)
{
    return h->mem;
}

int obj_hash_set(obj_hash *h, const char *f, size_t flen, const char *v,
                 size_t vlen)
{
    const char *old;
    size_t oldl;
    int is_new = !rh_get(&h->fields, f, flen, &old, &oldl);
    if (!is_new)
        h->mem -= field_bytes(flen, oldl);
    rh_set(&h->fields, f, flen, v, vlen);
    h->mem += field_bytes(flen, vlen);
    return is_new;
}

int obj_hash_get(obj_hash *h, const char *f, size_t flen, const char **v,
                 size_t *vlen)
{
    return rh_get(&h->fields, f, flen, v, vlen);
}

int obj_hash_del(obj_hash *h, const char *f, size_t flen)
{
    const char *old;
    size_t oldl;
    if (!rh_get(&h->fields, f, flen, &old, &oldl))
        return 0;
    h->mem -= field_bytes(flen, oldl);
    rh_del(&h->fields, f, flen);
    return 1;
}
