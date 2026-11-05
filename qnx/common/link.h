/*
 * A device link: a serial port ("/dev/ser2", raw 8N1 at the given baud rate) or a serial-to-Ethernet converter
 * port ("tcp:10.0.2.2:5551"). Both carry the ASCII lines of docs/PROTOCOL.md.
 */
#ifndef TITRA_QNX_LINK_H
#define TITRA_QNX_LINK_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char spec[128];
    int baud;
    int fd;
    int is_tcp;
} titra_link_t;

/* Records the spec; the link opens on the first titra_link_open call. */
void titra_link_init(titra_link_t *l, const char *spec, int baud);

/* Opens the link if it is closed. Returns 0 when open, -1 otherwise (errno set). */
int titra_link_open(titra_link_t *l);

/* Blocking read of up to n bytes. Returns the count, 0 or -1 when the link closed (it is then closed here). */
ssize_t titra_link_read(titra_link_t *l, void *buf, size_t n);

/* Writes all n bytes. Returns 0, or -1 when the link closed (it is then closed here). */
int titra_link_write(titra_link_t *l, const void *buf, size_t n);

void titra_link_close(titra_link_t *l);

#endif
