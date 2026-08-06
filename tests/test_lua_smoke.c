/* test_lua_smoke.c - vendored Lua 5.1.5 builds and runs inside ddup. */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "test.h"

static void test_state_arithmetic(void)
{
    lua_State *L = luaL_newstate();
    DD_CHECK(L != NULL);
    DD_CHECK_EQ_INT(0, luaL_loadstring(L, "return 1+1"));
    DD_CHECK_EQ_INT(0, lua_pcall(L, 0, 1, 0));
    DD_CHECK_EQ_INT(2, (long long)lua_tonumber(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

static void test_limited_libs(void)
{
    /* scripting exposes base+string+table+math only (documented) */
    lua_State *L = luaL_newstate();
    DD_CHECK(L != NULL);
    luaopen_base(L);
    luaopen_string(L);
    luaopen_table(L);
    luaopen_math(L);
    DD_CHECK_EQ_INT(0, luaL_loadstring(
                          L, "return string.upper('abc') .. math.floor(2.7)"));
    DD_CHECK_EQ_INT(0, lua_pcall(L, 0, 1, 0));
    DD_CHECK(lua_isstring(L, -1));
    DD_CHECK_STR("ABC2", lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

int main(void)
{
    DD_RUN(test_state_arithmetic);
    DD_RUN(test_limited_libs);
    return DD_TEST_SUMMARY();
}
