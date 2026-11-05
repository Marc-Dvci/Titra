/* Helpers shared by the QNX processes. */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <unistd.h>

#include "titra_qnx.h"

int titra_set_priority(int prio) {
    struct sched_param sp;
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

int titra_set_process_cpus(int first, int count) {
    const int ncpu = _syspage_ptr->num_cpu;
    const int rsize = RMSK_SIZE(ncpu);
    int *buf = calloc((size_t)(1 + rsize * 2), sizeof(int));
    if (buf == NULL) return -1;
    buf[0] = rsize;
    unsigned *mask = (unsigned *)&buf[1];
    unsigned *inherit = mask + rsize;
    for (int c = first; c < first + count && c < ncpu; c++) {
        RMSK_SET(c, mask);
        RMSK_SET(c, inherit);
    }
    const int rc = ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, buf);
    free(buf);
    return rc == -1 ? -1 : 0;
}

void titra_write_pid(const char *name, int cot) {
    char path[64];
    snprintf(path, sizeof path, "/dev/shmem/titra-%s-%d.pid", name, cot);
    FILE *f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

int titra_cot_json(const titra_cot_status_t *s, char *buf, int cap) {
    return snprintf(buf, (size_t)cap,
                    "{\"cot\":%d,\"t\":%.3f,\"fio2\":%.2f,\"mode\":\"%s\",\"alarm\":%d,\"alarms\":%u,\"reasons\":%u,"
                    "\"spo2\":%d,\"pr\":%d,\"sample\":%d,\"blender\":%.3f,\"valve_link\":%d,\"blender_link\":%d,\"fallback\":%.2f,"
                    "\"backup\":%.2f,\"ticks\":%u,\"late_ticks\":%u,\"max_interval\":%.3f,\"started\":%.3f}\n",
                    s->cot, s->t, s->fio2, tc_mode_name(s->mode), s->alarm, (unsigned)s->alarms,
                    (unsigned)s->reasons, s->spo2, s->pr, s->sample_status, s->blender_pm / 1000.0, s->valve_link, s->blender_link,
                    s->fallback_fio2, s->backup_fio2, (unsigned)s->ticks, (unsigned)s->late_ticks, s->max_interval_s,
                    s->started_s);
}
