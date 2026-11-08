/*
 * mono_main.c - host for the same driver code as a conventional resource
 * manager: one process owns the device path and the state.
 *
 * This is the baseline the hot-swap design is compared with. Upgrading it
 * means stopping the old process and starting the new one, which is what a
 * QNX High Availability Manager restart action does too:
 *   - clients' open file descriptors die with the old process (reads fail,
 *     usually with EBADF) and they must reopen the device,
 *   - the path does not exist until the new process has initialised and
 *     attached it,
 *   - the driver state is lost: the new process starts fresh (for a sensor
 *     that includes recalibrating).
 *
 * Usage: <mono> [-n path] [-i init_ms]
 *   -n  device path (default: the driver's path)
 *   -i  simulated hardware initialisation time before the driver can serve
 *       (the same option as the swappable core, for a fair comparison)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "driver.h"

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    uint8_t buf[CORE_MAX_REPLY];
    ssize_t len = drv.read(buf, sizeof buf);
    if (len < 0) {
        return EIO;
    }
    size_t n = msg->i.nbytes < (size_t)len ? msg->i.nbytes : (size_t)len;
    MsgReply(ctp->rcvid, (long)n, buf, n);
    return _RESMGR_NOREPLY;
}

int main(int argc, char **argv)
{
    const char *path = drv.path;
    long init_ms = 0;
    int opt;
    while ((opt = getopt(argc, argv, "n:i:")) != -1) {
        switch (opt) {
        case 'n': path = optarg; break;
        case 'i': init_ms = atol(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-n path] [-i init_ms]\n", argv[0]);
            return 2;
        }
    }

    /* Hardware initialisation and a fresh driver start happen before the
     * path exists: during this time a restarted driver is simply
     * unavailable to its clients. */
    if (init_ms > 0) {
        usleep((useconds_t)init_ms * 1000);
    }
    static struct core_state empty;     /* size 0: no predecessor state */
    if (drv.import_state(&empty) == -1) {
        fprintf(stderr, "mono v%u: driver failed to start\n", drv.version);
        return 1;
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

    if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        fprintf(stderr, "mono v%u: cannot attach %s: %s\n",
                drv.version, path, strerror(errno));
        return 1;
    }
    printf("mono v%u (pid %d): serving %s\n", drv.version, getpid(), path);
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
