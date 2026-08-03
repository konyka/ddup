/* obj.c - typed value objects; see obj.h. */
#include "ds/obj.h"

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
    /* hash/list objects arrive in the next sub-steps; strings own nothing */
    (void)val;
    (void)vlen;
    return 0;
}

void obj_free_value(const char *val, size_t vlen)
{
    (void)val;
    (void)vlen;
}
