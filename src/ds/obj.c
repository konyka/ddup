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
    case DDUP_OBJ_LIST:
        return obj_list_mem((obj_list *)obj_unpack_ptr(val, vlen));
    case DDUP_OBJ_SET:
        return obj_set_mem((obj_set *)obj_unpack_ptr(val, vlen));
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
    case DDUP_OBJ_LIST:
        obj_list_free((obj_list *)obj_unpack_ptr(val, vlen));
        break;
    case DDUP_OBJ_SET:
        obj_set_free((obj_set *)obj_unpack_ptr(val, vlen));
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

/* ------------------------------------------------------------------ */
/* list object                                                        */
/* ------------------------------------------------------------------ */

/* Same per-allocation estimate as the db layer (16 bytes malloc overhead). */
static uint64_t node_bytes(size_t elen)
{
    return (uint64_t)sizeof(list_node) + 16 + elen;
}

static list_node *node_new(const char *data, size_t len)
{
    list_node *n = (list_node *)malloc(sizeof(list_node) + len);
    if (n == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    n->prev = NULL;
    n->next = NULL;
    n->len = (uint32_t)len;
    memcpy(n->data, data, len);
    return n;
}

obj_list *obj_list_new(void)
{
    obj_list *l = (obj_list *)malloc(sizeof(*l));
    if (l == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    l->head = NULL;
    l->tail = NULL;
    l->len = 0;
    l->mem = (uint64_t)sizeof(*l);
    return l;
}

void obj_list_free(obj_list *l)
{
    list_node *n;
    if (l == NULL)
        return;
    n = l->head;
    while (n != NULL) {
        list_node *next = n->next;
        free(n);
        n = next;
    }
    free(l);
}

uint64_t obj_list_mem(const obj_list *l)
{
    return l->mem;
}

void obj_list_push(obj_list *l, int left, const char *data, size_t len)
{
    list_node *n = node_new(data, len);
    if (left) {
        n->next = l->head;
        if (l->head != NULL)
            l->head->prev = n;
        else
            l->tail = n;
        l->head = n;
    } else {
        n->prev = l->tail;
        if (l->tail != NULL)
            l->tail->next = n;
        else
            l->head = n;
        l->tail = n;
    }
    l->len++;
    l->mem += node_bytes(len);
}

int obj_list_pop(obj_list *l, int left, char **data, size_t *len)
{
    list_node *n = left ? l->head : l->tail;
    if (n == NULL)
        return 0;
    *data = (char *)malloc(n->len);
    if (*data == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    memcpy(*data, n->data, n->len);
    *len = n->len;
    if (n->prev != NULL)
        n->prev->next = n->next;
    else
        l->head = n->next;
    if (n->next != NULL)
        n->next->prev = n->prev;
    else
        l->tail = n->prev;
    l->len--;
    l->mem -= node_bytes(n->len);
    free(n);
    return 1;
}

list_node *obj_list_at(obj_list *l, size_t idx)
{
    size_t i;
    list_node *n;
    if (idx >= l->len)
        return NULL;
    if (idx < l->len / 2) {
        n = l->head;
        for (i = 0; i < idx; i++)
            n = n->next;
    } else {
        n = l->tail;
        for (i = l->len - 1; i > idx; i--)
            n = n->prev;
    }
    return n;
}

int obj_list_set_at(obj_list *l, size_t idx, const char *data, size_t len)
{
    list_node *old = obj_list_at(l, idx);
    list_node *n;
    if (old == NULL)
        return 0;
    n = node_new(data, len);
    n->prev = old->prev;
    n->next = old->next;
    if (old->prev != NULL)
        old->prev->next = n;
    else
        l->head = n;
    if (old->next != NULL)
        old->next->prev = n;
    else
        l->tail = n;
    l->mem -= node_bytes(old->len);
    l->mem += node_bytes(len);
    free(old);
    return 1;
}

/* ------------------------------------------------------------------ */
/* set object                                                         */
/* ------------------------------------------------------------------ */

obj_set *obj_set_new(void)
{
    obj_set *s = (obj_set *)malloc(sizeof(*s));
    if (s == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    rh_init(&s->members);
    s->mem = (uint64_t)sizeof(*s);
    return s;
}

void obj_set_free(obj_set *s)
{
    if (s == NULL)
        return;
    rh_destroy(&s->members);
    free(s);
}

uint64_t obj_set_mem(const obj_set *s)
{
    return s->mem;
}

int obj_set_add(obj_set *s, const char *m, size_t mlen)
{
    const char *old;
    size_t oldl;
    if (rh_get(&s->members, m, mlen, &old, &oldl))
        return 0;
    rh_set(&s->members, m, mlen, "", 0);
    s->mem += field_bytes(mlen, 0);
    return 1;
}

int obj_set_has(obj_set *s, const char *m, size_t mlen)
{
    const char *old;
    size_t oldl;
    return rh_get(&s->members, m, mlen, &old, &oldl);
}

int obj_set_rem(obj_set *s, const char *m, size_t mlen)
{
    if (!obj_set_has(s, m, mlen))
        return 0;
    rh_del(&s->members, m, mlen);
    s->mem -= field_bytes(mlen, 0);
    return 1;
}
