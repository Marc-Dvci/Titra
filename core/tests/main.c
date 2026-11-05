/* Runs every registered test and prints one line per test. Exit status 1 if any failed. */
#include <stdio.h>
#include <string.h>

#include "check.h"

#define MAX_TESTS 512

static struct {
    const char *name;
    tc_test_fn fn;
} tests[MAX_TESTS];
static int ntests;

void tc_register(const char *name, tc_test_fn fn) {
    if (ntests < MAX_TESTS) {
        tests[ntests].name = name;
        tests[ntests].fn = fn;
        ntests++;
    }
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    int run = 0, failed_count = 0;
    for (int i = 0; i < ntests; i++) {
        if (only && strstr(tests[i].name, only) == NULL) continue;
        int failed = 0;
        tests[i].fn(&failed);
        run++;
        if (failed) {
            failed_count++;
            printf("FAIL %s\n", tests[i].name);
        } else {
            printf("ok   %s\n", tests[i].name);
        }
    }
    printf("%d tests, %d failed\n", run, failed_count);
    return failed_count ? 1 : 0;
}
