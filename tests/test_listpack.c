/* test_listpack.c - unit tests for src/ds/listpack (Redis-compatible
 * compact encoding). Written before the implementation (TDD). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/listpack.h"
#include "test.h"

/* lp_get_str: string entries come back as a pointer, int entries are
 * decimal-materialized into buf. */
static void expect_str(unsigned char *p, const char *s)
{
    unsigned char buf[64];
    uint32_t len = 0;
    const unsigned char *v = lp_get_str(p, buf, &len);
    DD_CHECK(v != NULL);
    if (v != NULL)
        DD_CHECK_MEM(s, strlen(s), v, len);
}

/* entry must be int-encoded and decode to v */
static void expect_int(unsigned char *p, int64_t v)
{
    uint32_t len = 999;
    int64_t iv = 0;
    const unsigned char *s = lp_get(p, &len, &iv);
    DD_CHECK(s == NULL && iv == v);
}

/* entry must be string-encoded with these exact bytes */
static void expect_rawstr(unsigned char *p, const char *s)
{
    uint32_t len = 0;
    int64_t iv = 0;
    const unsigned char *v = lp_get(p, &len, &iv);
    DD_CHECK(v != NULL);
    if (v != NULL)
        DD_CHECK_MEM(s, strlen(s), v, len);
}

static void test_new_empty(void)
{
    unsigned char *lp = lp_new();
    DD_CHECK(lp != NULL);
    DD_CHECK_EQ_INT(7, (long long)lp_bytes(lp)); /* 6-byte header + EOF */
    DD_CHECK_EQ_INT(0, (long long)lp_length(lp));
    DD_CHECK(lp_first(lp) == NULL);
    DD_CHECK(lp_last(lp) == NULL);
    DD_CHECK(lp_seek(lp, 0) == NULL);
    lp_free(lp);
}

static void test_empty_null_payload_is_safe(void)
{
    unsigned char *lp = lp_new();
    uint32_t len = 123;
    int64_t value = 0;

    lp = lp_append(lp, NULL, 0);
    DD_CHECK_EQ_INT(1, (long long)lp_length(lp));
    DD_CHECK(lp_get(lp_first(lp), &len, &value) != NULL);
    DD_CHECK_EQ_INT(0, (long long)len);
    lp_free(lp);
}

static void test_append_strings(void)
{
    unsigned char *lp = lp_new();
    lp = lp_append(lp, (const unsigned char *)"hello", 5);
    lp = lp_append(lp, (const unsigned char *)"world", 5);
    DD_CHECK_EQ_INT(2, (long long)lp_length(lp));
    {
        unsigned char *p = lp_first(lp);
        DD_CHECK(p != NULL);
        expect_str(p, "hello");
        p = lp_next(lp, p);
        DD_CHECK(p != NULL);
        expect_str(p, "world");
        DD_CHECK(lp_next(lp, p) == NULL);
        /* backward walk */
        p = lp_last(lp);
        expect_str(p, "world");
        p = lp_prev(lp, p);
        expect_str(p, "hello");
        DD_CHECK(lp_prev(lp, p) == NULL);
        /* seek: positive, negative, out of range */
        expect_str(lp_seek(lp, 0), "hello");
        expect_str(lp_seek(lp, 1), "world");
        expect_str(lp_seek(lp, -1), "world");
        expect_str(lp_seek(lp, -2), "hello");
        DD_CHECK(lp_seek(lp, 2) == NULL);
        DD_CHECK(lp_seek(lp, -3) == NULL);
    }
    lp_free(lp);
}

static void test_prepend(void)
{
    unsigned char *lp = lp_new();
    lp = lp_prepend(lp, (const unsigned char *)"c", 1);
    lp = lp_prepend(lp, (const unsigned char *)"a", 1);
    lp = lp_insert(lp, (const unsigned char *)"b", 1, lp_seek(lp, 1),
                   LP_BEFORE, NULL);
    DD_CHECK_EQ_INT(3, (long long)lp_length(lp));
    expect_str(lp_seek(lp, 0), "a");
    expect_str(lp_seek(lp, 1), "b");
    expect_str(lp_seek(lp, 2), "c");
    lp_free(lp);
}

static void test_int_encodings(void)
{
    struct {
        const char *s;
        int64_t v;
        size_t entry_bytes; /* encoding + payload + backlen */
    } cases[] = {
        { "0", 0, 2 },
        { "127", 127, 2 },                  /* 7-bit uint */
        { "128", 128, 3 },                  /* 13-bit int */
        { "4095", 4095, 3 },
        { "-4096", -4096, 3 },
        { "4096", 4096, 4 },                /* 16-bit int */
        { "-4097", -4097, 4 },
        { "32767", 32767, 4 },
        { "-32768", -32768, 4 },
        { "32768", 32768, 5 },              /* 24-bit int */
        { "8388607", 8388607, 5 },
        { "-8388608", -8388608, 5 },
        { "8388608", 8388608, 6 },          /* 32-bit int */
        { "2147483647", 2147483647, 6 },
        { "-2147483648", -2147483648LL, 6 },
        { "2147483648", 2147483648LL, 10 }, /* 64-bit int */
        { "-2147483649", -2147483649LL, 10 },
        { "9223372036854775807", 9223372036854775807LL, 10 },
    };
    unsigned char *lp = lp_new();
    size_t expected = 7; /* header + EOF */
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lp = lp_append(lp, (const unsigned char *)cases[i].s,
                       (uint32_t)strlen(cases[i].s));
        expected += cases[i].entry_bytes;
        DD_CHECK_EQ_INT((long long)expected, (long long)lp_bytes(lp));
        expect_int(lp_seek(lp, (long)i), cases[i].v);
        expect_str(lp_seek(lp, (long)i), cases[i].s);
    }
    DD_CHECK_EQ_INT((long long)(sizeof(cases) / sizeof(cases[0])),
                    (long long)lp_length(lp));
    lp_free(lp);
}

static void test_string_encodings(void)
{
    static const size_t sizes[] = { 63, 64, 4095, 4096, 70000 };
    /* encoding bytes + payload + backlen bytes */
    static const size_t entry_bytes[] = { 1 + 63 + 1, 2 + 64 + 1,
                                          2 + 4095 + 2, 5 + 4096 + 2,
                                          5 + 70000 + 3 };
    char *buf = (char *)malloc(70000);
    unsigned char *lp = lp_new();
    size_t expected = 7;
    size_t i, j;
    DD_CHECK(buf != NULL);
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (j = 0; j < sizes[i]; j++)
            buf[j] = (char)('a' + (j % 26));
        lp = lp_append(lp, (const unsigned char *)buf, (uint32_t)sizes[i]);
        expected += entry_bytes[i];
        DD_CHECK_EQ_INT((long long)expected, (long long)lp_bytes(lp));
        {
            uint32_t len = 0;
            int64_t iv;
            const unsigned char *v = lp_get(lp_seek(lp, (long)i), &len, &iv);
            DD_CHECK(v != NULL);
            DD_CHECK_EQ_INT((long long)sizes[i], (long long)len);
            DD_CHECK(memcmp(v, buf, sizes[i]) == 0);
        }
    }
    free(buf);
    lp_free(lp);
}

static void test_strict_int_parse_fallback(void)
{
    /* not canonical integers: must stay string-encoded */
    static const char *const strs[] = { "01",  "-0", "+1", " 1", "1 ",
                                        "1x",  "",   "--1",
                                        "9223372036854775808",  /* overflow */
                                        "-9223372036854775809", /* underflow */
                                        "18446744073709551616",  /* uint64 wrap to zero */
                                        "-18446744073709551617", /* uint64 wrap to one */
                                        "99999999999999999999",  /* uint64 wrap */
                                        "-99999999999999999999", /* uint64 wrap */
                                        "00000000000000000001" };
    unsigned char *lp = lp_new();
    size_t i;
    for (i = 0; i < sizeof(strs) / sizeof(strs[0]); i++)
        lp = lp_append(lp, (const unsigned char *)strs[i],
                       (uint32_t)strlen(strs[i]));
    for (i = 0; i < sizeof(strs) / sizeof(strs[0]); i++)
        expect_rawstr(lp_seek(lp, (long)i), strs[i]);
    lp_free(lp);
}

static void test_insert_delete_middle(void)
{
    unsigned char *lp = lp_new();
    unsigned char *newp = NULL;
    int i;
    for (i = 0; i < 10; i++) {
        char tmp[16];
        int n = snprintf(tmp, sizeof(tmp), "e%d", i * 2);
        lp = lp_append(lp, (const unsigned char *)tmp, (uint32_t)n);
    }
    /* insert "x" before e4 (index 2) and "y" after e4 */
    lp = lp_insert(lp, (const unsigned char *)"x", 1, lp_seek(lp, 2),
                   LP_BEFORE, &newp);
    DD_CHECK(newp != NULL);
    expect_str(newp, "x");
    lp = lp_insert(lp, (const unsigned char *)"y", 1, lp_seek(lp, 3),
                   LP_AFTER, NULL);
    DD_CHECK_EQ_INT(12, (long long)lp_length(lp));
    expect_str(lp_seek(lp, 2), "x");
    expect_str(lp_seek(lp, 3), "e4");
    expect_str(lp_seek(lp, 4), "y");

    /* delete middle, then head, then tail */
    lp = lp_delete(lp, lp_seek(lp, 3), &newp); /* removes e4 */
    DD_CHECK_EQ_INT(11, (long long)lp_length(lp));
    DD_CHECK(newp != NULL);
    expect_str(newp, "y"); /* now at the deleted slot */
    expect_str(lp_seek(lp, 3), "y");
    lp = lp_delete(lp, lp_first(lp), NULL);
    expect_str(lp_first(lp), "e2");
    lp = lp_delete(lp, lp_last(lp), &newp);
    DD_CHECK(newp == NULL); /* deleted the tail: nothing follows */
    expect_str(lp_last(lp), "e16");
    lp_free(lp);
}

static void test_delete_all(void)
{
    unsigned char *lp = lp_new();
    int i;
    for (i = 0; i < 5; i++)
        lp = lp_append(lp, (const unsigned char *)"v", 1);
    while (lp_first(lp) != NULL)
        lp = lp_delete(lp, lp_first(lp), NULL);
    DD_CHECK_EQ_INT(0, (long long)lp_length(lp));
    DD_CHECK_EQ_INT(7, (long long)lp_bytes(lp));
    /* still usable */
    lp = lp_append(lp, (const unsigned char *)"z", 1);
    DD_CHECK_EQ_INT(1, (long long)lp_length(lp));
    expect_str(lp_first(lp), "z");
    lp_free(lp);
}

static void test_replace(void)
{
    unsigned char *lp = lp_new();
    lp = lp_append(lp, (const unsigned char *)"a", 1);
    lp = lp_append(lp, (const unsigned char *)"123", 3); /* int-encoded */
    lp = lp_append(lp, (const unsigned char *)"b", 1);
    lp = lp_replace(lp, lp_seek(lp, 1), (const unsigned char *)"replaced", 8);
    DD_CHECK_EQ_INT(3, (long long)lp_length(lp));
    expect_str(lp_seek(lp, 0), "a");
    expect_rawstr(lp_seek(lp, 1), "replaced");
    expect_str(lp_seek(lp, 2), "b");
    /* replace with an int-looking string: becomes int-encoded */
    lp = lp_replace(lp, lp_seek(lp, 1), (const unsigned char *)"42", 2);
    expect_int(lp_seek(lp, 1), 42);
    lp_free(lp);
}

static void test_find(void)
{
    unsigned char *lp = lp_new();
    lp = lp_append(lp, (const unsigned char *)"foo", 3);
    lp = lp_append(lp, (const unsigned char *)"123", 3);  /* int-encoded */
    lp = lp_append(lp, (const unsigned char *)"bar", 3);
    lp = lp_append(lp, (const unsigned char *)"123", 3);  /* duplicate */
    /* string hit */
    expect_str(lp_find(lp, NULL, (const unsigned char *)"foo", 3), "foo");
    /* int hit matches its decimal form */
    expect_int(lp_find(lp, NULL, (const unsigned char *)"123", 3), 123);
    /* first hit wins; continuing from after it finds the duplicate */
    {
        unsigned char *first = lp_find(lp, NULL, (const unsigned char *)"123", 3);
        unsigned char *second = lp_find(lp, first, (const unsigned char *)"123", 3);
        DD_CHECK(first != NULL && second != NULL && second != first);
        DD_CHECK(lp_find(lp, second, (const unsigned char *)"123", 3) == NULL);
    }
    /* miss */
    DD_CHECK(lp_find(lp, NULL, (const unsigned char *)"nope", 4) == NULL);
    /* "0123" is not 123 */
    DD_CHECK(lp_find(lp, NULL, (const unsigned char *)"0123", 4) == NULL);
    lp_free(lp);
}

static void test_many_entries_count_overflow(void)
{
    /* > 65534 entries: the 2-byte count saturates at 0xFFFF and
     * lp_length() falls back to scanning. */
    unsigned char *lp = lp_new();
    long i;
    const long n = 65540;
    for (i = 0; i < n; i++)
        lp = lp_append(lp, (const unsigned char *)"1", 1);
    DD_CHECK_EQ_INT(n, (long long)lp_length(lp));
    expect_int(lp_seek(lp, 0), 1);
    expect_int(lp_seek(lp, n - 1), 1);
    expect_int(lp_last(lp), 1);
    /* delete back below the unknown-count threshold: count becomes exact
     * again is NOT guaranteed; only length must stay correct. */
    lp = lp_delete(lp, lp_first(lp), NULL);
    lp = lp_delete(lp, lp_first(lp), NULL);
    DD_CHECK_EQ_INT(n - 2, (long long)lp_length(lp));
    lp_free(lp);
}

static void test_reverse_iteration(void)
{
    unsigned char *lp = lp_new();
    int i;
    char tmp[16];
    for (i = 0; i < 200; i++) {
        int n = snprintf(tmp, sizeof(tmp), "%d", i);
        lp = lp_append(lp, (const unsigned char *)tmp, (uint32_t)n);
    }
    /* walk back from the tail checking values */
    {
        unsigned char *p = lp_last(lp);
        i = 199;
        while (p != NULL) {
            unsigned char b[64];
            uint32_t l = 0;
            const unsigned char *v = lp_get_str(p, b, &l);
            int n = snprintf(tmp, sizeof(tmp), "%d", i);
            DD_CHECK_MEM(tmp, (size_t)n, v, l);
            i--;
            p = lp_prev(lp, p);
        }
        DD_CHECK_EQ_INT(-1, i);
    }
    lp_free(lp);
}

int main(void)
{
    DD_RUN(test_new_empty);
    DD_RUN(test_empty_null_payload_is_safe);
    DD_RUN(test_append_strings);
    DD_RUN(test_prepend);
    DD_RUN(test_int_encodings);
    DD_RUN(test_string_encodings);
    DD_RUN(test_strict_int_parse_fallback);
    DD_RUN(test_insert_delete_middle);
    DD_RUN(test_delete_all);
    DD_RUN(test_replace);
    DD_RUN(test_find);
    DD_RUN(test_many_entries_count_overflow);
    DD_RUN(test_reverse_iteration);
    return DD_TEST_SUMMARY();
}
