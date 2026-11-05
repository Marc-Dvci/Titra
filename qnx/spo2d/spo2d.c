/*
 * spo2d: the oximeter driver for one cot.
 *
 *   spo2d -n COT -d LINK [-b BAUD] [-p PRIORITY]
 *
 * A reader thread takes bytes from the oximeter link (a serial port or a serial-to-Ethernet converter port), splits
 * them into frames, parses each $OX frame and runs the sensor-level signal quality rules (core/src/quality.c). The
 * latest assessed sample is published at /dev/spo2/COT:
 *   devctl(DCMD_TITRA_SPO2_GET)   the sample and the link counters, for the controller
 *   read()                        the same as one JSON line, for people (`cat /dev/spo2/1`)
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "link.h"
#include "rm.h"
#include "titra/frame.h"
#include "titra/quality.h"
#include "titra_qnx.h"

static titra_link_t g_link;
static tc_line_t g_line;
static tc_quality_t g_quality;
static titra_spo2_state_t g_state;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_prio = 30;

static void *reader(void *arg) {
    (void)arg;
    titra_set_priority(g_prio);
    char buf[256];
    for (;;) {
        if (titra_link_open(&g_link) != 0) {
            pthread_mutex_lock(&g_mtx);
            g_state.link_up = 0;
            pthread_mutex_unlock(&g_mtx);
            struct timespec ts = {0, 500000000};
            nanosleep(&ts, NULL);
            continue;
        }
        pthread_mutex_lock(&g_mtx);
        g_state.link_up = 1;
        pthread_mutex_unlock(&g_mtx);
        ssize_t n = titra_link_read(&g_link, buf, sizeof buf);
        double rx = titra_now();
        if (n <= 0) continue;
        for (ssize_t i = 0; i < n; i++) {
            const char *line;
            size_t len;
            if (!tc_line_feed(&g_line, buf[i], &line, &len)) continue;
            tc_ox_frame_t f;
            tc_frame_result_t r = tc_ox_parse(line, len, &f);
            pthread_mutex_lock(&g_mtx);
            if (r == TC_FRAME_OK) {
                g_state.sample = tc_quality_on_frame(&g_quality, &f, rx);
                g_state.have_sample = 1;
                g_state.frames_ok++;
            } else {
                g_state.frames_rejected++;
            }
            pthread_mutex_unlock(&g_mtx);
        }
        pthread_mutex_lock(&g_mtx);
        g_state.bytes_dropped = g_line.dropped_bytes;
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

static int on_devctl(unsigned dcmd, void *data, size_t in_bytes, size_t cap, size_t *out_bytes) {
    (void)in_bytes;
    if (dcmd != (unsigned)DCMD_TITRA_SPO2_GET) return ENOSYS;
    if (cap < sizeof(titra_spo2_state_t)) return EMSGSIZE;
    pthread_mutex_lock(&g_mtx);
    memcpy(data, &g_state, sizeof g_state);
    pthread_mutex_unlock(&g_mtx);
    *out_bytes = sizeof(titra_spo2_state_t);
    return EOK;
}

static int on_text(char *buf, int cap) {
    pthread_mutex_lock(&g_mtx);
    titra_spo2_state_t s = g_state;
    pthread_mutex_unlock(&g_mtx);
    return snprintf(buf, (size_t)cap,
                    "{\"link_up\":%d,\"have_sample\":%d,\"status\":%d,\"reasons\":%u,\"spo2\":%d,\"pr\":%d,"
                    "\"pi\":%d,\"clock\":%u,\"rx\":%.3f,\"age\":%.3f,\"frames_ok\":%u,\"frames_rejected\":%u,"
                    "\"bytes_dropped\":%u}\n",
                    s.link_up, s.have_sample, s.sample.status, (unsigned)s.sample.reasons, s.sample.spo2, s.sample.pr,
                    s.sample.pi_centi, (unsigned)s.sample.clock, s.sample.rx_s,
                    s.have_sample ? titra_now() - s.sample.rx_s : -1.0, (unsigned)s.frames_ok,
                    (unsigned)s.frames_rejected, (unsigned)s.bytes_dropped);
}

static void usage(void) {
    fprintf(stderr, "usage: spo2d -n COT -d LINK [-b BAUD] [-p PRIORITY]\n");
    exit(2);
}

int main(int argc, char **argv) {
    int cot = 0, baud = 115200, opt;
    const char *spec = NULL;
    while ((opt = getopt(argc, argv, "n:d:b:p:C:")) != -1) {
        switch (opt) {
            case 'n': cot = atoi(optarg); break;
            case 'd': spec = optarg; break;
            case 'b': baud = atoi(optarg); break;
            case 'p': g_prio = atoi(optarg); break;
            case 'C': titra_set_process_cpus(atoi(optarg), 1); break;
            default: usage();
        }
    }
    if (cot <= 0 || spec == NULL) usage();

    titra_link_init(&g_link, spec, baud);
    tc_line_init(&g_line);
    tc_quality_cfg_t qc = tc_quality_cfg_default();
    tc_quality_init(&g_quality, &qc);

    char path[64];
    snprintf(path, sizeof path, "/dev/spo2/%d", cot);
    if (titra_rm_start(path, on_devctl, on_text, g_prio) != 0) {
        fprintf(stderr, "spo2d: cannot attach %s: %s\n", path, strerror(errno));
        return 1;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, reader, NULL) != 0) {
        fprintf(stderr, "spo2d: cannot start the reader: %s\n", strerror(errno));
        return 1;
    }
    titra_write_pid("spo2d", cot);
    fprintf(stderr, "spo2d: cot %d on %s at %s\n", cot, spec, path);
    pthread_join(tid, NULL);
    return 0;
}
