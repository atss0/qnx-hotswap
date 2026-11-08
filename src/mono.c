/*
 * mono.c - the same counter driver written the conventional way: one
 * resource manager process holds both the path /dev/counter and the state.
 *
 * This is the baseline the hot-swap design is compared with. Upgrading it
 * means stopping the old process and starting the new one, which is what a
 * QNX High Availability Manager restart action does too:
 *   - clients' open file descriptors die with the old process (reads fail,
 *     usually with EBADF) and they must reopen the device,
 *   - /dev/counter does not exist until the new process has attached it,
 *   - the counter state is lost: the new process starts from 0.
 *
 * Built as mono_v1 and mono_v2 (MONO_VERSION) so the client can see which
 * version answered.
 *
 * Usage: mono_vN [-i init_ms]
 *   -i  simulated hardware initialisation time before the driver can serve
 *       (probe, reset, calibration). core_vN takes the same option, so the
 *       two designs are compared with equal start-up work.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "hotswap_proto.h"

#ifndef MONO_VERSION
#define MONO_VERSION 1
#endif

static uint64_t counter_value;  /* lives and dies with this process */

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    struct counter_rec rec = {
        .value = ++counter_value,
        .core_version = MONO_VERSION,
    };
    size_t n = msg->i.nbytes < sizeof rec ? msg->i.nbytes : sizeof rec;
    MsgReply(ctp->rcvid, (long)n, &rec, n);
    return _RESMGR_NOREPLY;
}

int main(int argc, char **argv)
{
    long init_ms = 0;
    int opt;
    while ((opt = getopt(argc, argv, "i:")) != -1) {
        if (opt == 'i') {
            init_ms = atol(optarg);
        } else {
            fprintf(stderr, "usage: %s [-i init_ms]\n", argv[0]);
            return 2;
        }
    }

    /* Hardware initialisation happens before the path exists: during this
     * time a restarted driver is simply unavailable to its clients. */
    if (init_ms > 0) {
        usleep((useconds_t)init_ms * 1000);
    }

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        perror("dispatch_create");
        return 1;
    }

    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t      io_funcs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;

    static iofunc_attr_t attr;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    if (resmgr_attach(dpp, &rattr, HOTSWAP_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        fprintf(stderr, "mono v%d: cannot attach %s: %s\n",
                MONO_VERSION, HOTSWAP_PATH, strerror(errno));
        return 1;
    }
    printf("mono v%d (pid %d): serving %s\n", MONO_VERSION, getpid(), HOTSWAP_PATH);
    fflush(stdout);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            perror("dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
