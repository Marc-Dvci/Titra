/* A minimal test harness: each TEST registers itself, CHECK records a failure and returns from the test. */
#ifndef TITRA_CHECK_H
#define TITRA_CHECK_H

#include <math.h>
#include <stdio.h>

typedef void (*tc_test_fn)(int *failed);

void tc_register(const char *name, tc_test_fn fn);

#define TEST(name)                                                                 \
    static void name(int *failed);                                                 \
    __attribute__((constructor)) static void register_##name(void) {               \
        tc_register(#name, name);                                                   \
    }                                                                              \
    static void name(int *failed)

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            *failed = 1;                                                           \
            return;                                                                \
        }                                                                          \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                       \
    do {                                                                                         \
        long _a = (long)(a), _b = (long)(b);                                                     \
        if (_a != _b) {                                                                          \
            fprintf(stderr, "  %s:%d: %s == %ld, expected %ld\n", __FILE__, __LINE__, #a, _a, _b); \
            *failed = 1;                                                                         \
            return;                                                                              \
        }                                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                           \
    do {                                                                                                \
        double _a = (double)(a), _b = (double)(b);                                                      \
        if (fabs(_a - _b) > (tol)) {                                                                    \
            fprintf(stderr, "  %s:%d: %s == %.6f, expected %.6f\n", __FILE__, __LINE__, #a, _a, _b);     \
            *failed = 1;                                                                                \
            return;                                                                                     \
        }                                                                                               \
    } while (0)

#endif
