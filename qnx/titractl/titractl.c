/*
 * titractl: the controller for one cot.
 *
 *   titractl -n COT [-B BACKUP] [-m MIN] [-M MAX] [-p PRIORITY] [-H] [-T TYPE] [-C CPU]
 *
 * A 1 Hz timer pulse drives each tick: ask the oximeter driver for its latest sample (devctl on /dev/spo2/COT),
 * run the controller (core/src/control.c), and send the command to the blender driver (devctl on /dev/valve/COT).
 * Both requests are QNX messages, so the drivers serve them at this thread's priority. The cot's status is
 * published at /dev/titra/cotCOT, served by a low-priority thread.
 *
 * At start the controller asks the blender driver for the FiO2 it applies and begins there, so a restart causes no
 * step. With -H it attaches itself to the High Availability Manager and sends a heartbeat each tick. HAM restarts it
 * from its own command line when it dies, and kills it when it misses its heartbeats, which a stopped or hung
 * controller does. -T names the security type the restarted controller runs under (`on -T TYPE`).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

#include "rm.h"
#include "titra/control.h"
#include "titra_qnx.h"

#ifndef TITRA_NO_HAM
#include <ha/ham.h>
#endif

#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 1)
#define STATUS_PRIO 10

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static titra_cot_status_t g_status;

static int on_devctl(unsigned dcmd, void *data, size_t in_bytes, size_t cap, size_t *out_bytes) {
    (void)in_bytes;
    if (dcmd != (unsigned)DCMD_TITRA_COT_GET) return ENOSYS;
    if (cap < sizeof(titra_cot_status_t)) return EMSGSIZE;
    pthread_mutex_lock(&g_mtx);
    memcpy(data, &g_status, sizeof g_status);
    pthread_mutex_unlock(&g_mtx);
    *out_bytes = sizeof(titra_cot_status_t);
    return EOK;
}

static int on_text(char *buf, int cap) {
    pthread_mutex_lock(&g_mtx);
    titra_cot_status_t s = g_status;
    pthread_mutex_unlock(&g_mtx);
    return titra_cot_json(&s, buf, cap);
}

static int reopen(int *fd, const char *path) {
    if (*fd >= 0) return 0;
    *fd = open(path, O_RDWR);
    return *fd >= 0 ? 0 : -1;
}

static void drop(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

/* Test mode (-X COUNT): sends COUNT commands the blender driver must refuse, alternating a FiO2 above the cot's limit
 * and a step above the slew limit, and prints how many were refused. Used by the bench for fault F11. */
static int send_bad_commands(const char *valve_path, int count) {
    int fd = open(valve_path, O_RDWR);
    if (fd < 0) {
        printf("{\"attempts\":%d,\"refused\":0,\"error\":\"%s\"}\n", count, strerror(errno));
        return 1;
    }
    titra_valve_state_t vs;
    int current = devctl(fd, DCMD_TITRA_VALVE_GET, &vs, sizeof vs, NULL) == EOK ? vs.applied_pm : 300;
    int refused = 0, accepted = 0;
    for (int i = 0; i < count; i++) {
        titra_valve_msg_t m;
        memset(&m, 0, sizeof m);
        m.cmd.fio2_pm = (i % 2 == 0) ? 1500 : current + 200;
        m.cmd.mode = TC_MODE_CONTROL;
        if (devctl(fd, DCMD_TITRA_VALVE_SET, &m, sizeof m, NULL) == EOK) {
            if (m.state.result != 0) {
                refused++;
            } else {
                accepted++;
            }
        }
    }
    close(fd);
    printf("{\"attempts\":%d,\"refused\":%d,\"accepted\":%d}\n", count, refused, accepted);
    return accepted ? 1 : 0;
}

static void usage(void) {
    fprintf(stderr, "usage: titractl -n COT [-B BACKUP] [-m MIN] [-M MAX] [-p PRIORITY] [-H] [-T TYPE] [-C CPU] [-X COUNT]\n");
    exit(2);
}

int main(int argc, char **argv) {
    int cot = 0, prio = 30, use_ham = 0, opt, bad = 0;
    const char *type = NULL;
    tc_cot_settings_t settings = {0.21, 1.00, 0.30};
    while ((opt = getopt(argc, argv, "n:B:m:M:p:HT:X:C:")) != -1) {
        switch (opt) {
            case 'n': cot = atoi(optarg); break;
            case 'B': settings.backup_fio2 = atof(optarg); break;
            case 'm': settings.fio2_min = atof(optarg); break;
            case 'M': settings.fio2_max = atof(optarg); break;
            case 'p': prio = atoi(optarg); break;
            case 'H': use_ham = 1; break;
            case 'T': type = optarg; break;
            case 'X': bad = atoi(optarg); break;
            case 'C': titra_set_process_cpus(atoi(optarg), 1); break;
            default: usage();
        }
    }
    if (cot <= 0) usage();

    char spo2_path[32], valve_path[32], status_path[32];
    snprintf(spo2_path, sizeof spo2_path, "/dev/spo2/%d", cot);
    snprintf(valve_path, sizeof valve_path, "/dev/valve/%d", cot);
    snprintf(status_path, sizeof status_path, "/dev/titra/cot%d", cot);
    if (bad > 0) return send_bad_commands(valve_path, bad);
    titra_write_pid("titractl", cot);

    titra_set_priority(prio);
    int fd_spo2 = -1, fd_valve = -1;

    /* Start from the FiO2 the blender driver applies now. */
    double now = titra_now();
    double initial = settings.backup_fio2;
    if (reopen(&fd_valve, valve_path) == 0) {
        titra_valve_state_t vs;
        if (devctl(fd_valve, DCMD_TITRA_VALVE_GET, &vs, sizeof vs, NULL) == EOK) initial = vs.applied_pm / 1000.0;
    }
    tc_params_t params = tc_params_default();
    tc_controller_t ctl;
    tc_controller_init(&ctl, &params, &settings, initial, now);

    g_status.cot = cot;
    g_status.backup_fio2 = settings.backup_fio2;
    g_status.started_s = now;
    g_status.fio2 = ctl.cmd;
    g_status.spo2 = -1;
    g_status.sample_status = -1;
    if (titra_rm_start(status_path, on_devctl, on_text, STATUS_PRIO) != 0) {
        fprintf(stderr, "titractl: cannot attach %s: %s\n", status_path, strerror(errno));
        return 1;
    }

#ifndef TITRA_NO_HAM
    if (use_ham) {
        char ename[32], line[512];
        snprintf(ename, sizeof ename, "titractl-%d", cot);
        /* The restart command is this command line, under the same security type. */
        int len = type ? snprintf(line, sizeof line, "/proc/boot/on -T %s", type) : 0;
        line[len] = '\0';
        for (int i = 0; i < argc && len < (int)sizeof line; i++) {
            len += snprintf(line + len, sizeof line - (size_t)len, "%s%s", len ? " " : "", argv[i]);
        }
        /* A previous instance of this controller may have left its entity behind. */
        ham_detach_name(0, ename, 0);
        /* Heartbeat every second; three missed is "low", five missed is "high". */
        ham_entity_t *e = ham_attach_self(ename, 1000000000ULL, 3, 5, 0);
        /* Missed heartbeats: SIGKILL, which ends even a stopped process. Death: run the command line again. */
        ham_condition_t *hb = e ? ham_condition(e, CONDHBEATMISSEDHIGH, "missed", 0) : NULL;
        ham_condition_t *death = e ? ham_condition(e, CONDDEATH, "death", 0) : NULL;
        if (hb == NULL || death == NULL || ham_action_notify_signal(hb, "kill", 0, getpid(), SIGKILL, 0, 0, 0) == NULL ||
            ham_action_execute(death, "restart", line, 0) == NULL) {
            fprintf(stderr, "titractl: HAM supervision not set up: %s\n", strerror(errno));
        }
    }
#else
    (void)use_ham;
    (void)type;
#endif

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, prio, PULSE_TICK, 0);
    timer_t tid;
    if (chid < 0 || coid < 0 || timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0) {
        fprintf(stderr, "titractl: cannot create the tick timer: %s\n", strerror(errno));
        return 1;
    }
    struct itimerspec its = {{1, 0}, {1, 0}};
    timer_settime(tid, 0, &its, NULL);
    fprintf(stderr, "titractl: cot %d from FiO2 %.2f, backup %.2f, limits %.2f-%.2f, priority %d\n", cot, initial,
            settings.backup_fio2, settings.fio2_min, settings.fio2_max, prio);

    double last_tick = -1.0;
    for (;;) {
        struct _pulse pulse;
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1 || pulse.code != PULSE_TICK) continue;
        now = titra_now();

        titra_spo2_state_t ss;
        const tc_sample_t *sample = NULL;
        if (reopen(&fd_spo2, spo2_path) == 0) {
            if (devctl(fd_spo2, DCMD_TITRA_SPO2_GET, &ss, sizeof ss, NULL) == EOK) {
                if (ss.have_sample) sample = &ss.sample;
            } else {
                drop(&fd_spo2);
            }
        }
        tc_output_t out = tc_controller_step(&ctl, sample, now);

        titra_valve_msg_t vm;
        memset(&vm, 0, sizeof vm);
        vm.cmd.fio2_pm = (int)lround(out.fio2 * 1000.0);
        vm.cmd.mode = out.mode;
        vm.cmd.alarm = out.alarm;
        int valve_ok = 0;
        if (reopen(&fd_valve, valve_path) == 0) {
            if (devctl(fd_valve, DCMD_TITRA_VALVE_SET, &vm, sizeof vm, NULL) == EOK) {
                valve_ok = 1;
            } else {
                drop(&fd_valve);
            }
        }

        pthread_mutex_lock(&g_mtx);
        if (last_tick >= 0.0) {
            double interval = now - last_tick;
            if (interval > 1.5) g_status.late_ticks++;
            if (interval > g_status.max_interval_s) g_status.max_interval_s = interval;
        }
        g_status.t = now;
        g_status.fio2 = out.fio2;
        g_status.mode = out.mode;
        g_status.alarm = out.alarm;
        g_status.alarms = out.alarms;
        g_status.reasons = out.reasons;
        g_status.spo2 = sample ? sample->spo2 : -1;
        g_status.pr = sample ? sample->pr : -1;
        g_status.sample_status = sample ? sample->status : -1;
        g_status.valve_link = valve_ok;
        g_status.blender_pm = valve_ok ? vm.state.blender_pm : -1;
        g_status.blender_link = valve_ok ? vm.state.blender_link_up : 0;
        g_status.fallback_fio2 = out.fallback_fio2;
        g_status.ticks++;
        pthread_mutex_unlock(&g_mtx);
        last_tick = now;

#ifndef TITRA_NO_HAM
        if (use_ham) ham_heartbeat();
#endif
    }
    return 0;
}
