/* script.c - Lua script cache; see script.h. */
#include "core/script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/sha1.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

void *script_state(db *d)
{
    if (d->lua_state == NULL) {
        lua_State *L = luaL_newstate();
        if (L == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        /* documented sandbox: base/string/table/math only */
        luaopen_base(L);
        luaopen_string(L);
        luaopen_table(L);
        luaopen_math(L);
        d->lua_state = L;
    }
    return d->lua_state;
}

/* normalize 40-hex to lowercase in place-ish (dst may equal src) */
static void sha_lower(char dst[41], const char *src)
{
    int i;
    for (i = 0; i < 40; i++) {
        char ch = src[i];
        dst[i] = (ch >= 'A' && ch <= 'F') ? (char)(ch + 32) : ch;
    }
    dst[40] = '\0';
}

int script_load(db *d, const char *src, size_t len, char out_sha1[41],
                char *errbuf, size_t errcap)
{
    lua_State *L = (lua_State *)script_state(d);
    const char *v;
    size_t vl;
    int ref;
    char b[4];

    sha1_hex(src, len, out_sha1);
    if (rh_get(&d->scripts, out_sha1, 40, &v, &vl))
        return 0; /* cache hit: no recompile */

    if (luaL_loadbuffer(L, src, len, "script") != 0) {
        const char *msg = lua_tostring(L, -1);
        snprintf(errbuf, errcap, "%s", msg != NULL ? msg : "unknown error");
        lua_pop(L, 1);
        return -1;
    }
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
    memcpy(b, &ref, 4);
    rh_set(&d->scripts, out_sha1, 40, b, 4);
    return 0;
}

int script_cached(db *d, const char *sha1)
{
    char sha[41];
    const char *v;
    size_t vl;
    sha_lower(sha, sha1);
    return rh_get(&d->scripts, sha, 40, &v, &vl);
}

int script_ref(db *d, const char *sha1)
{
    char sha[41];
    const char *v;
    size_t vl;
    int ref;
    sha_lower(sha, sha1);
    if (!rh_get(&d->scripts, sha, 40, &v, &vl) || vl != 4)
        return -1; /* LUA_NOREF is -1 in 5.1, but we return our own code */
    memcpy(&ref, v, 4);
    return ref;
}

/* unref one cached chunk during flush/cleanup */
static void unref_cb(const char *key, size_t klen, const char *val,
                     size_t vlen, void *ctx)
{
    int ref;
    (void)key;
    (void)klen;
    if (vlen == 4) {
        memcpy(&ref, val, 4);
        luaL_unref((lua_State *)ctx, LUA_REGISTRYINDEX, ref);
    }
}

void script_flush(db *d)
{
    if (d->lua_state != NULL)
        rh_each(&d->scripts, unref_cb, d->lua_state);
    rh_destroy(&d->scripts);
    rh_init(&d->scripts);
}

void script_cleanup(db *d)
{
    script_flush(d);
    if (d->lua_state != NULL) {
        lua_close((lua_State *)d->lua_state);
        d->lua_state = NULL;
    }
}
