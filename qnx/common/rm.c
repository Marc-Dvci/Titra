/* A small resource manager. See rm.h. */
#include "rm.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The default OCB, named before the headers so the handlers see its fields. */
struct _iofunc_ocb;
#define IOFUNC_OCB_T struct _iofunc_ocb
#define RESMGR_OCB_T struct _iofunc_ocb
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>

#include "titra_qnx.h"

#define MSG_MAX 2048

typedef struct {
    char path[64];
    titra_devctl_fn devctl_fn;
    titra_text_fn text_fn;
    int prio;
    dispatch_t *dpp;
    resmgr_connect_funcs_t connect_funcs;
    resmgr_io_funcs_t io_funcs;
    iofunc_attr_t attr;
} rm_t;

static rm_t *rm_of(RESMGR_OCB_T *ocb) { return (rm_t *)((char *)ocb->attr - offsetof(rm_t, attr)); }

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    char text[1024];
    rm_t *rm = rm_of(ocb);
    int len = rm->text_fn ? rm->text_fn(text, sizeof text) : 0;
    if (len < 0) len = 0;
    if (len > (int)sizeof text) len = (int)sizeof text;
    off_t off = ocb->offset;
    size_t n = 0;
    if (off < len) {
        n = (size_t)(len - off);
        if (n > msg->i.nbytes) n = msg->i.nbytes;
    }
    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);
    ocb->offset += (off_t)n;
    SETIOV(ctp->iov, text + off, n);
    /* The reply is copied before this function returns, so the stack buffer is safe. */
    MsgReplyv(ctp->rcvid, (long)n, ctp->iov, 1);
    return _RESMGR_NOREPLY;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;
    rm_t *rm = rm_of(ocb);
    if (rm->devctl_fn == NULL) return ENOSYS;
    void *data = _DEVCTL_DATA(msg->i);
    size_t in_bytes = msg->i.nbytes;
    size_t cap = MSG_MAX - sizeof(msg->i);
    size_t out_bytes = 0;
    status = rm->devctl_fn(msg->i.dcmd, data, in_bytes, cap, &out_bytes);
    if (status != EOK) return status;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = 0;
    msg->o.nbytes = (uint32_t)out_bytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + out_bytes);
}

static void *loop(void *arg) {
    rm_t *rm = arg;
    titra_set_priority(rm->prio);
    dispatch_context_t *ctp = dispatch_context_alloc(rm->dpp);
    if (ctp == NULL) return NULL;
    for (;;) {
        dispatch_context_t *c = dispatch_block(ctp);
        if (c == NULL) continue;
        ctp = c;
        dispatch_handler(ctp);
    }
    return NULL;
}

int titra_rm_start(const char *path, titra_devctl_fn devctl_fn, titra_text_fn text_fn, int prio) {
    rm_t *rm = calloc(1, sizeof *rm);
    if (rm == NULL) return -1;
    snprintf(rm->path, sizeof rm->path, "%s", path);
    rm->devctl_fn = devctl_fn;
    rm->text_fn = text_fn;
    rm->prio = prio;
    rm->dpp = dispatch_create();
    if (rm->dpp == NULL) return -1;
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = MSG_MAX;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &rm->connect_funcs, _RESMGR_IO_NFUNCS, &rm->io_funcs);
    rm->io_funcs.read = io_read;
    rm->io_funcs.devctl = io_devctl;
    iofunc_attr_init(&rm->attr, S_IFCHR | 0660, NULL, NULL);
    if (resmgr_attach(rm->dpp, &rattr, rm->path, _FTYPE_ANY, 0, &rm->connect_funcs, &rm->io_funcs, &rm->attr) == -1) {
        return -1;
    }
    pthread_t tid;
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
    return pthread_create(&tid, &pa, loop, rm) == 0 ? 0 : -1;
}
