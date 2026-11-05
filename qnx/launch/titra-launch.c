/*
 * titra-launch: starts every cot's processes and hands them to the High Availability Manager.
 *
 *   titra-launch [-c COTS] [-h HOST] [-P BASE_PORT] [-B BACKUP] [-T] [-S]
 *
 * For each cot N it starts, in order:
 *   spo2d    -n N -d tcp:HOST:BASE+2N-2      the oximeter link of cot N
 *   valved   -n N -d tcp:HOST:BASE+2N-1 -B BACKUP
 *   titractl -n N -B BACKUP -H
 * and waits for each pathname before starting the next. The drivers and the station are attached to HAM with their
 * command lines and a death condition whose action restarts them. The controllers attach themselves, because they
 * also send HAM a heartbeat (see titractl.c). With -T each command runs
 * under its own security type (`on -T spo2d2_t ...`, one set of types per cot), so the policy decides what each one
 * may open, down to which cot's drivers a controller may reach. With -S the nurse
 * station starts last.
 */
#include <errno.h>
#include <ha/ham.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_ARGS 24

extern char **environ;

static int g_types;

/* Splits a command line on spaces into argv (the line is modified). */
static int split(char *line, char **argv) {
    int n = 0;
    for (char *tok = strtok(line, " "); tok && n < MAX_ARGS - 1; tok = strtok(NULL, " ")) argv[n++] = tok;
    argv[n] = NULL;
    return n;
}

static pid_t start(const char *type, const char *cmd, char *line, size_t cap) {
    if (g_types) {
        snprintf(line, cap, "/proc/boot/on -T %s %s", type, cmd);
    } else {
        snprintf(line, cap, "%s", cmd);
    }
    char copy[256];
    snprintf(copy, sizeof copy, "%s", line);
    char *argv[MAX_ARGS];
    split(copy, argv);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "titra-launch: cannot start %s: %s\n", line, strerror(rc));
        return -1;
    }
    return pid;
}

static int wait_path(const char *path, double timeout_s) {
    struct timespec ts = {0, 50000000};
    for (double waited = 0.0; waited < timeout_s; waited += 0.05) {
        if (access(path, F_OK) == 0) return 0;
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* Attaches a running process to HAM with a death condition that restarts it from the same command line. A NULL name
 * leaves the process to supervise itself (the controllers attach themselves, for their heartbeat). */
static void supervise(const char *name, pid_t pid, const char *line) {
    if (name == NULL) return;
    ham_entity_t *e = ham_attach(name, 0, pid, line, 0);
    if (e == NULL) {
        fprintf(stderr, "titra-launch: HAM cannot attach %s: %s\n", name, strerror(errno));
        return;
    }
    ham_condition_t *c = ham_condition(e, CONDDEATH, "death", HREARMAFTERRESTART);
    if (c == NULL || ham_action_restart(c, "restart", line, HREARMAFTERRESTART) == NULL) {
        fprintf(stderr, "titra-launch: HAM restart action for %s not set: %s\n", name, strerror(errno));
    }
    ham_entity_handle_free(e);
}

static int launch(const char *name, const char *type, const char *cmd, const char *path) {
    char line[256];
    pid_t pid = start(type, cmd, line, sizeof line);
    if (pid < 0) return -1;
    if (path && wait_path(path, 10.0) != 0) {
        fprintf(stderr, "titra-launch: %s did not appear\n", path);
        return -1;
    }
    supervise(name, pid, line);
    fprintf(stderr, "titra-launch: %s pid %d: %s\n", name ? name : "self-supervised", (int)pid, line);
    return 0;
}

int main(int argc, char **argv) {
    int cots = 3, base = 5551, opt, station = 0;
    const char *host = "10.0.2.2";
    const char *backup = "0.30";
    while ((opt = getopt(argc, argv, "c:h:P:B:TS")) != -1) {
        switch (opt) {
            case 'c': cots = atoi(optarg); break;
            case 'h': host = optarg; break;
            case 'P': base = atoi(optarg); break;
            case 'B': backup = optarg; break;
            case 'T': g_types = 1; break;
            case 'S': station = 1; break;
            default:
                fprintf(stderr, "usage: titra-launch [-c COTS] [-h HOST] [-P BASE_PORT] [-B BACKUP] [-T] [-S]\n");
                return 2;
        }
    }
    if (ham_connect(0) == -1) {
        fprintf(stderr, "titra-launch: HAM is not running: %s\n", strerror(errno));
        return 1;
    }
    int failures = 0;
    for (int n = 1; n <= cots; n++) {
        char name[32], cmd[160], path[32], type[32];
        snprintf(name, sizeof name, "spo2d-%d", n);
        snprintf(cmd, sizeof cmd, "/system/bin/spo2d -n %d -d tcp:%s:%d", n, host, base + 2 * n - 2);
        snprintf(path, sizeof path, "/dev/spo2/%d", n);
        snprintf(type, sizeof type, "spo2d%d_t", n);
        failures += launch(name, type, cmd, path) != 0;

        snprintf(name, sizeof name, "valved-%d", n);
        snprintf(cmd, sizeof cmd, "/system/bin/valved -n %d -d tcp:%s:%d -B %s", n, host, base + 2 * n - 1, backup);
        snprintf(path, sizeof path, "/dev/valve/%d", n);
        snprintf(type, sizeof type, "valved%d_t", n);
        failures += launch(name, type, cmd, path) != 0;

        snprintf(type, sizeof type, "titractl%d_t", n);
        snprintf(cmd, sizeof cmd, "/system/bin/titractl -n %d -B %s -H%s%s", n, backup, g_types ? " -T " : "",
                 g_types ? type : "");
        snprintf(path, sizeof path, "/dev/titra/cot%d", n);
        failures += launch(NULL, type, cmd, path) != 0;
    }
    if (station) {
        char cmd[96];
        snprintf(cmd, sizeof cmd, "/system/bin/station -c %d", cots);
        failures += launch("station", "station_t", cmd, NULL) != 0;
    }
    ham_disconnect(0);
    return failures ? 1 : 0;
}
