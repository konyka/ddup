/* script.c - Lua script cache; see script.h. */
#include "core/script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/session.h"
#include "core/sha1.h"
#include "pal/pal_cstd.h"
#include "pal/pal_thread.h"
#include "pal/pal_time.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "resp/resp_parser.h"

static int lua_redis_call(lua_State *L);
static int lua_redis_pcall(lua_State *L);

#define SCRIPT_HOOK_INSTRUCTIONS 1000
#define SCRIPT_INSTRUCTION_BUDGET 1000000U
#define SCRIPT_TIME_BUDGET_MS 5000U

static void lua_script_limit_hook(lua_State *L, lua_Debug *ar)
{
    uint64_t deadline;
    (void)ar;

    lua_getfield(L, LUA_REGISTRYINDEX, "ddup_script_budget");
    if (lua_tonumber(L, -1) <= 0) {
        lua_pop(L, 1);
        luaL_error(L, "script exceeded instruction limit");
        return;
    }
    lua_pushnumber(L, lua_tonumber(L, -1) - SCRIPT_HOOK_INSTRUCTIONS);
    lua_setfield(L, LUA_REGISTRYINDEX, "ddup_script_budget");
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "ddup_script_deadline_ms");
    deadline = (uint64_t)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (pal_now_ms() >= deadline)
        luaL_error(L, "script exceeded execution time limit");
}

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
        /* the redis.* bridge (dispatch registered via script_set_command_fn) */
        lua_newtable(L);
        lua_pushcfunction(L, lua_redis_call);
        lua_setfield(L, -2, "call");
        lua_pushcfunction(L, lua_redis_pcall);
        lua_setfield(L, -2, "pcall");
        lua_setglobal(L, "redis");
        d->lua_state = L;
    }
    return d->lua_state;
}

/* Validate and normalize an exact 40-character SHA-1 string. */
static int sha_normalize(char dst[41], const char *src, size_t src_len)
{
    int i;
    if (src == NULL || src_len != 40)
        return 0;
    for (i = 0; i < 40; i++) {
        char ch = src[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F')))
            return 0;
        dst[i] = (ch >= 'A' && ch <= 'F') ? (char)(ch + 32) : ch;
    }
    dst[40] = '\0';
    return 1;
}

static int script_registry_ref_valid(lua_State *L, int ref)
{
    int valid;
    if (L == NULL || ref <= 0)
        return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    valid = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return valid;
}

static int script_cache_ref(db *d, const char *sha, int *ref_out)
{
    const char *v;
    size_t vl;
    int ref;

    if (!rh_get(&d->scripts, sha, 40, &v, &vl) || vl != sizeof(ref))
        return 0;
    memcpy(&ref, v, sizeof(ref));
    if (!script_registry_ref_valid((lua_State *)d->lua_state, ref))
        return 0;
    *ref_out = ref;
    return 1;
}

int script_load(db *d, const char *src, size_t len, char out_sha1[41],
                char *errbuf, size_t errcap)
{
    lua_State *L = (lua_State *)script_state(d);
    int ref;
    char b[sizeof(ref)];

    sha1_hex(src, len, out_sha1);
    if (script_cache_ref(d, out_sha1, &ref))
        return 0; /* cache hit: no recompile */

    if (luaL_loadbuffer(L, src, len, "=script") != 0) {
        const char *msg = lua_tostring(L, -1);
        snprintf(errbuf, errcap, "%s", msg != NULL ? msg : "unknown error");
        lua_pop(L, 1);
        return -1;
    }
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
    memcpy(b, &ref, sizeof(ref));
    if (rh_set(&d->scripts, out_sha1, 40, b, sizeof(ref)) < 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        snprintf(errbuf, errcap, "%s", "script cache insertion failed");
        return -1;
    }
    return 0;
}

int script_cached(db *d, const char *sha1, size_t sha1_len)
{
    char sha[41];
    int ref;
    if (!sha_normalize(sha, sha1, sha1_len))
        return 0;
    return script_cache_ref(d, sha, &ref);
}

int script_ref(db *d, const char *sha1, size_t sha1_len)
{
    char sha[41];
    int ref;
    if (!sha_normalize(sha, sha1, sha1_len) ||
        !script_cache_ref(d, sha, &ref))
        return -1;
    return ref;
}

/* unref one cached chunk during flush/cleanup */
static void unref_cb(const char *key, size_t klen, const char *val,
                      size_t vlen, void *ctx)
{
    db *d = ctx;
    int ref;
    (void)key;
    (void)klen;
    if (vlen == sizeof(ref)) {
        memcpy(&ref, val, sizeof(ref));
        if (!script_registry_ref_valid((lua_State *)d->lua_state, ref))
            return;
        luaL_unref((lua_State *)d->lua_state, LUA_REGISTRYINDEX, ref);
    }
}

void script_flush(db *d)
{
    if (d->lua_state != NULL)
        rh_each(&d->scripts, unref_cb, d);
    rh_destroy(&d->scripts);
    rh_init(&d->scripts);
}

void script_cleanup(db *d)
{
    script_flush(d);
    rh_destroy(&d->function_libs);
    rh_init(&d->function_libs);
    if (d->lua_state != NULL) {
        lua_close((lua_State *)d->lua_state);
        d->lua_state = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* execution (EVAL family)                                            */
/* ------------------------------------------------------------------ */

static script_command_fn g_cmd_fn = NULL;
static ddup_atomic_int g_cmd_state = 0; /* 0=unpublished, 1=publishing, 2=ready */

#ifdef DDUP_TESTING
static unsigned script_blocked_probe_count;

int script_test_command_ready(void)
{
    return ddup_atomic_load(&g_cmd_state, ddup_memory_order_acquire) == 2;
}

void script_test_reset_blocked_probes(void)
{
    script_blocked_probe_count = 0;
}

unsigned script_test_blocked_probes(void)
{
    return script_blocked_probe_count;
}
#endif

void script_set_command_fn(script_command_fn fn)
{
    int state;
    int expected;
    if (fn == NULL)
        return;
    state = ddup_atomic_load(&g_cmd_state, ddup_memory_order_acquire);
    if (state == 2)
        return;
    expected = 0;
    if (ddup_atomic_compare_exchange(&g_cmd_state, &expected, 1,
                                     ddup_memory_order_acquire)) {
        g_cmd_fn = fn;
        ddup_atomic_store(&g_cmd_state, 2, ddup_memory_order_release);
        return;
    }
    while (ddup_atomic_load(&g_cmd_state, ddup_memory_order_acquire) != 2)
        pal_thread_yield();
}

/* case-insensitive compare (command.c's ci_equal is file-local). */
static int script_ci_equal(const char *a, size_t alen, const char *b,
                           size_t blen)
{
    size_t i;
#ifdef DDUP_TESTING
    script_blocked_probe_count++;
#endif
    if (blen != alen)
        return 0;
    for (i = 0; i < alen; i++) {
        char x = a[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        /* Blacklist literals are lowercase, so only normalize the input. */
        if (x != b[i])
            return 0;
    }
    return 1;
}

/* Commands scripts may not invoke (documented). Keeping lengths beside the
 * literals avoids a strlen call for every redis.call() command. */
static int name_blocked(const char *n, size_t nl)
{
    switch (nl) {
    case 4:
        return script_ci_equal(n, nl, "eval", sizeof("eval") - 1);
    case 6:
        return script_ci_equal(n, nl, "script", sizeof("script") - 1);
    case 7:
        return script_ci_equal(n, nl, "evalsha", sizeof("evalsha") - 1) ||
               script_ci_equal(n, nl, "eval_ro", sizeof("eval_ro") - 1);
    case 8:
        return script_ci_equal(n, nl, "shutdown", sizeof("shutdown") - 1);
    case 9:
        return script_ci_equal(n, nl, "subscribe", sizeof("subscribe") - 1);
    case 10:
        return script_ci_equal(n, nl, "psubscribe", sizeof("psubscribe") - 1) ||
               script_ci_equal(n, nl, "evalsha_ro", sizeof("evalsha_ro") - 1);
    case 11:
        return script_ci_equal(n, nl, "unsubscribe", sizeof("unsubscribe") - 1);
    case 12:
        return script_ci_equal(n, nl, "punsubscribe", sizeof("punsubscribe") - 1);
    default:
        return 0;
    }
}

/* RESP reply of an effect command -> Lua value */
static void resp_to_lua(lua_State *L, const resp_value *v)
{
    size_t i;
    switch (v->type) {
    case RESP_SIMPLE_STRING:
        lua_pushlstring(L, v->str, v->len);
        break;
    case RESP_INTEGER:
    case RESP_BOOLEAN:
        lua_pushnumber(L, (lua_Number)v->integer);
        break;
    case RESP_DOUBLE:
        lua_pushnumber(L, v->dbl);
        break;
    case RESP_BULK_STRING:
    case RESP_VERBATIM_STRING:
    case RESP_BIG_NUMBER:
        if (v->str != NULL)
            lua_pushlstring(L, v->str, v->len);
        else
            lua_pushboolean(L, 0);
        break;
    case RESP_ARRAY:
    case RESP_MAP:
    case RESP_SET:
        if (v->is_null || v->items == NULL) {
            lua_pushboolean(L, 0);
            break;
        }
        lua_newtable(L);
        for (i = 0; i < v->count; i++) {
            resp_to_lua(L, &v->items[i]);
            lua_rawseti(L, -2, (lua_Number)i + 1);
        }
        break;
    default:
        lua_pushboolean(L, 0);
        break;
    }
}

static int lua_redis_generic(lua_State *L, int raise_error)
{
    int argc = lua_gettop(L);
    resp_value argv[64];
    resp_buf out;
    arena a;
    session *s;
    uint64_t now_ms;
    uint64_t dirty_before;
    int i;

    if (ddup_atomic_load(&g_cmd_state, ddup_memory_order_acquire) != 2 ||
        g_cmd_fn == NULL)
        return luaL_error(L, "scripting engine not initialized");
    if (argc < 1)
        return luaL_error(
            L, "Please specify at least one argument for redis.call()");
    if (argc > 63)
        return luaL_error(L, "too many arguments for redis.call()");
    for (i = 1; i <= argc; i++) {
        size_t sl = 0;
        const char *str = lua_tolstring(L, i, &sl);
        if (str == NULL)
            return luaL_error(L, "Lua redis() command arguments must be "
                                 "strings or integers");
        memset(&argv[i - 1], 0, sizeof(argv[i - 1]));
        argv[i - 1].type = RESP_BULK_STRING;
        argv[i - 1].str = str;
        argv[i - 1].len = sl;
    }
    if (name_blocked(argv[0].str, argv[0].len)) {
        lua_pushstring(L,
                       "This Redis command is not allowed from scripts");
        return lua_error(L);
    }

    lua_getfield(L, LUA_REGISTRYINDEX, "ddup_session");
    s = (session *)lua_touserdata(L, -1);
    lua_getfield(L, LUA_REGISTRYINDEX, "ddup_now");
    now_ms = (uint64_t)lua_tonumber(L, -1);
    if (s == NULL)
        return luaL_error(L, "scripting context lost");

    resp_buf_init(&out);
    dirty_before = s->d->dirty;
    s->in_script++;
    g_cmd_fn(s, argv, (size_t)argc, &out, now_ms);
    s->in_script--;
    /* effects replication: log the effect command itself (AOF/backlog/
     * downstream replicas) — never the script */
    if (s->d->dirty != dirty_before && s->aof_log != NULL)
        s->aof_log(s->aof_ctx, s->db_index, argv, (size_t)argc, NULL, 0);

    /* parse the single reply the dispatch produced */
    arena_init(&a, 4096);
    {
        resp_value v;
        ptrdiff_t used = resp_parse(out.data, out.len, &v, &a);
        if (used <= 0) {
            arena_destroy(&a);
            resp_buf_free(&out);
            return luaL_error(L, "script: bad reply from command dispatch");
        }
        if (v.type == RESP_ERROR || v.type == RESP_BLOB_ERROR) {
            if (raise_error) {
                lua_pushlstring(L, v.str, v.len);
                arena_destroy(&a);
                resp_buf_free(&out);
                return lua_error(L);
            }
            lua_newtable(L);
            lua_pushlstring(L, v.str, v.len);
            lua_setfield(L, -2, "err");
        } else {
            resp_to_lua(L, &v);
        }
    }
    arena_destroy(&a);
    resp_buf_free(&out);
    return 1;
}

static int lua_redis_call(lua_State *L)
{
    return lua_redis_generic(L, 1);
}

static int lua_redis_pcall(lua_State *L)
{
    return lua_redis_generic(L, 0);
}

/* Lua return value -> RESP reply (Redis conversion rules) */
static void lua_to_resp(lua_State *L, int idx, resp_buf *out)
{
    if (idx < 0)
        idx = lua_gettop(L) + 1 + idx; /* pin before pushes shift the top */
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER:
        resp_write_integer(out, (long long)lua_tonumber(L, idx));
        break;
    case LUA_TSTRING: {
        size_t l = 0;
        const char *s = lua_tolstring(L, idx, &l);
        resp_write_bulk(out, s, l);
        break;
    }
    case LUA_TBOOLEAN:
        if (lua_toboolean(L, idx))
            resp_write_integer(out, 1);
        else
            resp_write_bulk(out, NULL, 0);
        break;
    case LUA_TTABLE: {
        size_t l = 0;
        const char *s;
        lua_getfield(L, idx, "ok");
        s = lua_tolstring(L, -1, &l);
        if (s != NULL) {
            resp_write_simple_string(out, s, l);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 1);
        lua_getfield(L, idx, "err");
        s = lua_tolstring(L, -1, &l);
        if (s != NULL) {
            resp_write_error(out, s, l);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 1);
        /* array part, stopping at the first nil (Redis semantics) */
        {
            size_t n = 0, i;
            for (;;) {
                lua_rawgeti(L, idx, (lua_Number)n + 1);
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                lua_pop(L, 1);
                n++;
            }
            resp_write_array_header(out, n);
            for (i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, (lua_Number)i);
                lua_to_resp(L, -1, out);
                lua_pop(L, 1);
            }
        }
        break;
    }
    default:
        resp_write_bulk(out, NULL, 0); /* nil and everything else */
        break;
    }
}

void script_exec(session *s, const char *sha1, const resp_value *argv,
                 size_t nkeys, size_t nargs, resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    lua_State *L = (lua_State *)script_state(d);
    int ref = script_ref(d, sha1, 40);
    size_t i;
    int rc;

    if (ref < 0) {
        static const char E[] = "NOSCRIPT No matching script. Please use EVAL.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    /* context for the redis.* bridge */
    lua_pushlightuserdata(L, s);
    lua_setfield(L, LUA_REGISTRYINDEX, "ddup_session");
    lua_pushnumber(L, (lua_Number)now_ms);
    lua_setfield(L, LUA_REGISTRYINDEX, "ddup_now");
    /* KEYS / ARGV */
    lua_newtable(L);
    for (i = 0; i < nkeys; i++) {
        lua_pushlstring(L, argv[i].str, argv[i].len);
        lua_rawseti(L, -2, (lua_Number)i + 1);
    }
    lua_setglobal(L, "KEYS");
    lua_newtable(L);
    for (i = 0; i < nargs; i++) {
        lua_pushlstring(L, argv[nkeys + i].str, argv[nkeys + i].len);
        lua_rawseti(L, -2, (lua_Number)i + 1);
    }
    lua_setglobal(L, "ARGV");

    lua_pushnumber(L, (lua_Number)SCRIPT_INSTRUCTION_BUDGET);
    lua_setfield(L, LUA_REGISTRYINDEX, "ddup_script_budget");
    lua_pushnumber(L, (lua_Number)(pal_now_ms() + SCRIPT_TIME_BUDGET_MS));
    lua_setfield(L, LUA_REGISTRYINDEX, "ddup_script_deadline_ms");
    lua_sethook(L, lua_script_limit_hook, LUA_MASKCOUNT,
                SCRIPT_HOOK_INSTRUCTIONS);
    rc = lua_pcall(L, 0, 1, 0);
    lua_sethook(L, NULL, 0, 0);
    if (rc != 0) {
        const char *msg = lua_tostring(L, -1);
        char sha_lc[41];
        char ebuf[512];
        int n;
        if (!sha_normalize(sha_lc, sha1, 40))
            strcpy(sha_lc, "unknown");
        n = snprintf(ebuf, sizeof(ebuf),
                     "ERR Error running script (call to f_%s): %s", sha_lc,
                     msg != NULL ? msg : "unknown error");
        resp_write_error(out, ebuf, (size_t)n);
        lua_settop(L, 0);
        return;
    }
    lua_to_resp(L, -1, out);
    lua_settop(L, 0);
}
