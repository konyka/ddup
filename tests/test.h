/* test.h - minimal zero-dependency test harness for ddup (TDD).
 *
 * Usage:
 *   #include "test.h"
 *   static void test_something(void) {
 *       DD_CHECK(1 + 1 == 2);
 *       DD_CHECK_EQ_INT(2, 1 + 1);
 *       DD_CHECK_MEM("ab", 2, "ab", 2);
 *   }
 *   int main(void) {
 *       DD_RUN(test_something);
 *       return DD_TEST_SUMMARY();
 *   }
 */
#ifndef DDUP_TEST_H
#define DDUP_TEST_H

#include <stdio.h>
#include <string.h>

static int dd_test_checks = 0;
static int dd_test_failures = 0;

#define DD_CHECK(cond)                                                        \
    do {                                                                      \
        dd_test_checks++;                                                     \
        if (!(cond)) {                                                        \
            dd_test_failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                     \
    } while (0)

#define DD_CHECK_EQ_INT(expected, actual)                                     \
    do {                                                                      \
        long long _e = (long long)(expected);                                 \
        long long _a = (long long)(actual);                                   \
        dd_test_checks++;                                                     \
        if (_e != _a) {                                                       \
            dd_test_failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: expected %lld, got %lld\n",          \
                    __FILE__, __LINE__, _e, _a);                              \
        }                                                                     \
    } while (0)

#define DD_CHECK_MEM(expected, expected_len, actual, actual_len)              \
    do {                                                                      \
        size_t _el = (size_t)(expected_len);                                  \
        size_t _al = (size_t)(actual_len);                                    \
        dd_test_checks++;                                                     \
        if (_el != _al || memcmp((expected), (actual), _el) != 0) {           \
            dd_test_failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: memory mismatch (len %llu vs %llu)\n",\
                    __FILE__, __LINE__,                                       \
                    (unsigned long long)_el, (unsigned long long)_al);        \
        }                                                                     \
    } while (0)

#define DD_CHECK_STR(expected, actual)                                        \
    DD_CHECK_MEM((expected), strlen(expected), (actual), strlen(actual))

#define DD_RUN(fn)                                                            \
    do {                                                                      \
        int _before = dd_test_failures;                                       \
        fn();                                                                 \
        printf("%-40s %s\n", #fn, dd_test_failures == _before ? "ok" : "FAILED");\
    } while (0)

#define DD_TEST_SUMMARY()                                                     \
    (printf("---\n%d checks, %d failure(s)\n", dd_test_checks, dd_test_failures),\
     dd_test_failures == 0 ? 0 : 1)

#endif /* DDUP_TEST_H */
