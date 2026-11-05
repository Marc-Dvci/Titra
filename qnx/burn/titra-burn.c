/*
 * titra-burn: keeps CPUs busy at a chosen priority, for the CPU-load fault (F13).
 *
 *   titra-burn [-n THREADS] [-p PRIORITY] [-t SECONDS]
 *
 * Each thread spins at SCHED_RR priority PRIORITY (default 10, below the controllers at 30) for SECONDS (default 300).
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int g_prio = 10;
static double g_seconds = 300.0;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void *spin(void *arg) {
    (void)arg;
    struct sched_param sp = {.sched_priority = g_prio};
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
    volatile unsigned long x = 0;
    double end = now() + g_seconds;
    while (now() < end) {
        for (int i = 0; i < 100000; i++) x += (unsigned long)i;
    }
    return NULL;
}

int main(int argc, char **argv) {
    int threads = 4, opt;
    while ((opt = getopt(argc, argv, "n:p:t:")) != -1) {
        switch (opt) {
            case 'n': threads = atoi(optarg); break;
            case 'p': g_prio = atoi(optarg); break;
            case 't': g_seconds = atof(optarg); break;
            default:
                fprintf(stderr, "usage: titra-burn [-n THREADS] [-p PRIORITY] [-t SECONDS]\n");
                return 2;
        }
    }
    pthread_t tids[64];
    if (threads < 1 || threads > 64) threads = 4;
    for (int i = 0; i < threads; i++) pthread_create(&tids[i], NULL, spin, NULL);
    for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);
    return 0;
}
