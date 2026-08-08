#ifndef SP1_TEST_UTIL_H
#define SP1_TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>

static int sp1_test_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            sp1_test_failures++;                                             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long _a = (long)(actual), _e = (long)(expected);                     \
        if (_a != _e) {                                                      \
            printf("  FAIL %s:%d  %s: got %ld, want %ld\n",                  \
                   __FILE__, __LINE__, #actual, _a, _e);                     \
            sp1_test_failures++;                                             \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do { printf("- %s\n", #fn); fn(); } while (0)

#define TEST_MAIN_END()                                                      \
    do {                                                                     \
        if (sp1_test_failures) {                                             \
            printf("%d failure(s)\n", sp1_test_failures);                    \
            return 1;                                                        \
        }                                                                    \
        printf("ok\n");                                                      \
        return 0;                                                            \
    } while (0)

#endif
