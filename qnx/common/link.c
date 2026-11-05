/* Device links over a serial port or TCP. See link.h. */
#include "link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

void titra_link_init(titra_link_t *l, const char *spec, int baud) {
    memset(l, 0, sizeof(*l));
    snprintf(l->spec, sizeof l->spec, "%s", spec);
    l->baud = baud;
    l->fd = -1;
    l->is_tcp = strncmp(spec, "tcp:", 4) == 0;
}

static speed_t speed_for(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        default: return B115200;
    }
}

static int open_serial(titra_link_t *l) {
    int fd = open(l->spec, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, speed_for(l->baud));
        cfsetospeed(&t, speed_for(l->baud));
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    return fd;
}

static int open_tcp(titra_link_t *l) {
    char host[96];
    const char *rest = l->spec + 4;
    const char *colon = strrchr(rest, ':');
    if (colon == NULL || (size_t)(colon - rest) >= sizeof host) {
        errno = EINVAL;
        return -1;
    }
    memcpy(host, rest, (size_t)(colon - rest));
    host[colon - rest] = '\0';
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int titra_link_open(titra_link_t *l) {
    if (l->fd >= 0) return 0;
    l->fd = l->is_tcp ? open_tcp(l) : open_serial(l);
    return l->fd >= 0 ? 0 : -1;
}

void titra_link_close(titra_link_t *l) {
    if (l->fd >= 0) close(l->fd);
    l->fd = -1;
}

ssize_t titra_link_read(titra_link_t *l, void *buf, size_t n) {
    if (l->fd < 0) return -1;
    ssize_t r = read(l->fd, buf, n);
    if (r < 0 && errno == EINTR) return 0;
    if (r <= 0) titra_link_close(l);
    return r;
}

int titra_link_write(titra_link_t *l, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        if (l->fd < 0) return -1;
        ssize_t w = write(l->fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            titra_link_close(l);
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
