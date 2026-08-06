/* test_script.c - SHA1 (RFC 3174 vectors) and the Lua script cache. */
#include <stdio.h>
#include <string.h>

#include "core/script.h"
#include "core/sha1.h"
#include "test.h"

static void test_sha1_vectors(void)
{
    char out[41];
    sha1_hex("", 0, out);
    DD_CHECK_STR("da39a3ee5e6b4b0d3255bfef95601890afd80709", out);
    sha1_hex("abc", 3, out);
    DD_CHECK_STR("a9993e364706816aba3e25717850c26c9cd0d89d", out);
    sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
             56, out);
    DD_CHECK_STR("84983e441c3bd26ebaae4aa1f95129e5e54670f1", out);
    sha1_hex("The quick brown fox jumps over the lazy dog", 43, out);
    DD_CHECK_STR("2fd4e1c67a2d28fced849ee1bb76e7391b93eb12", out);
    {
        /* RFC 3174: one million 'a' */
        char buf[1000];
        char acc[41];
        int i;
        sha1_ctx c;
        memset(buf, 'a', sizeof(buf));
        sha1_init(&c);
        for (i = 0; i < 1000; i++)
            sha1_update(&c, buf, sizeof(buf));
        sha1_final_hex(&c, acc);
        DD_CHECK_STR("34aa973cd4c4daa4f61eeb2bdbad27316534016f", acc);
    }
}

static void test_cache_load_hit_miss(void)
{
    db d;
    char sha[41], err[128];
    db_init(&d);

    DD_CHECK_EQ_INT(0, script_load(&d, "return 1", 8, sha, err, sizeof(err)));
    sha1_hex("return 1", 8, err);
    DD_CHECK_STR(err, sha);
    DD_CHECK_EQ_INT(1, script_cached(&d, sha));

    /* reload is a hit (same sha, no recompile) */
    DD_CHECK_EQ_INT(0, script_load(&d, "return 1", 8, sha, err, sizeof(err)));
    DD_CHECK_EQ_INT(1, script_cached(&d, sha));

    DD_CHECK_EQ_INT(0, script_cached(&d, "ffffffffffffffffffffffffffffffffffffffff"));
    /* uppercase hex also matches */
    {
        char up[41];
        int i;
        for (i = 0; i < 40; i++)
            up[i] = (sha[i] >= 'a' && sha[i] <= 'f') ? (char)(sha[i] - 32)
                                                    : sha[i];
        up[40] = '\0';
        DD_CHECK_EQ_INT(1, script_cached(&d, up));
    }
    db_destroy(&d);
}

static void test_compile_error(void)
{
    db d;
    char sha[41], err[256];
    db_init(&d);
    DD_CHECK_EQ_INT(-1, script_load(&d, "not valid lua !!", 16, sha, err,
                                    sizeof(err)));
    DD_CHECK(strlen(err) > 0);
    DD_CHECK_EQ_INT(0, script_cached(
                          &d, "0000000000000000000000000000000000000000"));
    db_destroy(&d);
}

static void test_flush(void)
{
    db d;
    char sha[41], err[128];
    db_init(&d);
    DD_CHECK_EQ_INT(0, script_load(&d, "return 1", 8, sha, err, sizeof(err)));
    DD_CHECK_EQ_INT(1, script_cached(&d, sha));
    script_flush(&d);
    DD_CHECK_EQ_INT(0, script_cached(&d, sha));
    /* cache usable again after a flush */
    DD_CHECK_EQ_INT(0, script_load(&d, "return 2", 8, sha, err, sizeof(err)));
    DD_CHECK_EQ_INT(1, script_cached(&d, sha));
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_sha1_vectors);
    DD_RUN(test_cache_load_hit_miss);
    DD_RUN(test_compile_error);
    DD_RUN(test_flush);
    return DD_TEST_SUMMARY();
}
