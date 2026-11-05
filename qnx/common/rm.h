/*
 * A small resource manager shared by Titra's drivers and controller: one pathname, devctl() commands handled by a
 * callback, and read() returning a text line built by another callback (so `cat /dev/spo2/1` shows the state).
 */
#ifndef TITRA_QNX_RM_H
#define TITRA_QNX_RM_H

#include <stddef.h>

/* Handles one devctl. `data` holds in_bytes of input and has room for cap bytes of output. Sets *out_bytes and
 * returns EOK, or returns an errno value. */
typedef int (*titra_devctl_fn)(unsigned dcmd, void *data, size_t in_bytes, size_t cap, size_t *out_bytes);

/* Writes the text a read() returns. Returns its length. */
typedef int (*titra_text_fn)(char *buf, int cap);

/* Attaches `path` and runs the dispatch loop in a new thread at SCHED_FIFO priority `prio`. Returns 0 or -1. The
 * receiving thread's priority floats to each client's priority while it serves that client. */
int titra_rm_start(const char *path, titra_devctl_fn devctl_fn, titra_text_fn text_fn, int prio);

#endif
