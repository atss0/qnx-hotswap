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
 * A better conventional upgrade, "make before break", is available too (-b
 * and -s): the new process starts and initialises while the old one still
 * serves, loads the calibration the previous process saved in a file, and
 * attaches the path in front of the old one. Then the old process is
 * stopped. The path never disappears and calibration is not repeated, but
 * clients' descriptors still die with the old process, and whatever was not
 * saved (filter history, sequence numbers) is still lost.
 *
 * Usage: <mono> [-n path] [-i init_ms] [-b] [-s state_file]
 *   -n  device path (default: the driver's path)
 *   -i  simulated hardware initialisation time before the driver can serve
 *       (the same option as the swappable core, for a fair comparison)
 *   -b  attach in front of an existing server on the same path
 *   -s  load the initial state from this file if it exists; otherwise start
 *       fresh and save the state to it
 */
#include <errno.h>
#include <fcntl.h>
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
    const char *state_file = NULL;
    long init_ms = 0;
    unsigned attach_flags = 0;
    int opt;
    while ((opt = getopt(argc, argv, "n:i:bs:")) != -1) {
        switch (opt) {
        case 'n': path = optarg; break;
        case 'i': init_ms = atol(optarg); break;
        case 'b': attach_flags |= _RESMGR_FLAG_BEFORE; break;
        case 's': state_file = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-n path] [-i init_ms] [-b] [-s state_file]\n", argv[0]);
            return 2;
        }
    }

    /* Hardware initialisation and a fresh driver start happen before the
     * path exists: during this time a restarted driver is simply
     * unavailable to its clients. */
    if (init_ms > 0) {
        usleep((useconds_t)init_ms * 1000);
    }
    static struct core_state initial;   /* size 0: no predecessor state */
    int loaded = 0;
    if (state_file) {
        int sf = open(state_file, O_RDONLY);
        if (sf != -1) {
            loaded = read(sf, &initial, sizeof initial) == (ssize_t)sizeof initial;
            close(sf);
            if (!loaded) {
                memset(&initial, 0, sizeof initial);
            }
        }
    }
    if (drv.import_state(&initial) == -1) {
        fprintf(stderr, "mono v%u: driver failed to start\n", drv.version);
        return 1;
    }
    /* First start: save the result of the initialisation for the next
     * process. Format 1 is the one every version can read. */
    if (state_file && !loaded) {
        static struct core_state out;
        int sf = open(state_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (sf == -1 || drv.export_state(1, &out) == -1 ||
            write(sf, &out, sizeof out) != (ssize_t)sizeof out) {
            fprintf(stderr, "mono v%u: cannot save state to %s\n", drv.version, state_file);
        }
        if (sf != -1) {
            close(sf);
        }
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

    if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, attach_flags,
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
