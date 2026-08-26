/* test_glob.c - Redis-style glob matcher tests (written before the impl). */
#include "test.h"

#include <string.h>

#include "ds/glob.h"

static int m(const char *pat, const char *str)
{
    return ddup_glob_match(pat, strlen(pat), str, strlen(str));
}

static void test_literal_and_empty(void)
{
    DD_CHECK(m("", "") == 1);
    DD_CHECK(m("", "a") == 0);
    DD_CHECK(m("a", "") == 0);
    DD_CHECK(m("abc", "abc") == 1);
    DD_CHECK(m("abc", "abd") == 0);
    DD_CHECK(m("abc", "ab") == 0);
    DD_CHECK(m("abc", "abcd") == 0);
}

static void test_star(void)
{
    DD_CHECK(m("*", "") == 1);
    DD_CHECK(m("*", "anything") == 1);
    DD_CHECK(m("h*llo", "hello") == 1);
    DD_CHECK(m("h*llo", "hllo") == 1);
    DD_CHECK(m("*llo", "hello") == 1);
    DD_CHECK(m("hel*", "hello") == 1);
    DD_CHECK(m("h*o", "hello") == 1);
    DD_CHECK(m("h*x", "hello") == 0);
    DD_CHECK(m("a*b*c", "aXbYc") == 1);
    DD_CHECK(m("a*b*c", "aXbY") == 0);
    DD_CHECK(m("**", "ab") == 1); /* consecutive stars collapse */
    DD_CHECK(m("a**b", "acb") == 1);
    DD_CHECK(m("*a*a*", "aaa") == 1);
    DD_CHECK(m("*a*a*", "ababa") == 1);
    DD_CHECK(m("*a*a*", "bbb") == 0);
}

static void test_question(void)
{
    DD_CHECK(m("?", "x") == 1);
    DD_CHECK(m("?", "") == 0);
    DD_CHECK(m("?", "xy") == 0);
    DD_CHECK(m("h?llo", "hello") == 1);
    DD_CHECK(m("h?llo", "hllo") == 0);
    DD_CHECK(m("h?llo", "heello") == 0);
    DD_CHECK(m("???", "abc") == 1);
    DD_CHECK(m("???", "ab") == 0);
}

static void test_class(void)
{
    DD_CHECK(m("[abc]", "a") == 1);
    DD_CHECK(m("[abc]", "c") == 1);
    DD_CHECK(m("[abc]", "d") == 0);
    DD_CHECK(m("[abc]", "") == 0);
    DD_CHECK(m("[a-z]", "m") == 1);
    DD_CHECK(m("[a-z]", "A") == 0);
    DD_CHECK(m("[a-z0-9]", "5") == 1);
    DD_CHECK(m("[z-a]", "m") == 0); /* reversed range matches nothing */
    DD_CHECK(m("key:[12]", "key:1") == 1);
    DD_CHECK(m("key:[12]", "key:3") == 0);
}

static void test_negated_class(void)
{
    DD_CHECK(m("[^abc]", "d") == 1);
    DD_CHECK(m("[^abc]", "a") == 0);
    DD_CHECK(m("[^a-z]", "m") == 0);
    DD_CHECK(m("[^a-z]", "1") == 1);
    DD_CHECK(m("[a^]", "^") == 1); /* '^' negates only at class start */
    DD_CHECK(m("[a^]", "b") == 0);
}

static void test_escapes(void)
{
    DD_CHECK(m("a\\*", "a*") == 1);
    DD_CHECK(m("a\\*", "ab") == 0);
    DD_CHECK(m("\\?", "?") == 1);
    DD_CHECK(m("\\?", "x") == 0);
    DD_CHECK(m("\\\\", "\\") == 1);
    DD_CHECK(m("a\\", "a\\") == 1); /* trailing backslash is literal */
    DD_CHECK(m("[\\]]", "]") == 1); /* escaped ']' inside a class */
    DD_CHECK(m("[\\[]", "[") == 1);
}

static void test_unterminated_class_fails_closed(void)
{
    /* Redis stringmatchlen rejects a class without a closing bracket. */
    DD_CHECK(m("[", "[") == 0);
    DD_CHECK(m("[abc", "a") == 0);
    DD_CHECK(m("prefix[abc", "prefixa") == 0);
}

static void test_binary_safe(void)
{
    const char pat[] = {'a', '*', 'c'};
    const char str[] = {'a', '\0', 'c'};
    DD_CHECK(ddup_glob_match(pat, sizeof(pat), str, sizeof(str)) == 1);
    DD_CHECK(ddup_glob_match("?", 1, str, sizeof(str)) == 0);
}

int main(void)
{
    DD_RUN(test_literal_and_empty);
    DD_RUN(test_star);
    DD_RUN(test_question);
    DD_RUN(test_class);
    DD_RUN(test_negated_class);
    DD_RUN(test_escapes);
    DD_RUN(test_unterminated_class_fails_closed);
    DD_RUN(test_binary_safe);
    return DD_TEST_SUMMARY();
}
