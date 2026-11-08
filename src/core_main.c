/*
 * core_main.c - host process for a swappable driver core.
 *
 * Linked with one driver (counter.c, accel.c, ...), this program serves the
 * shell's requests over its own QNX channel and calls the driver's
 * callbacks (driver.h). It contains no driver logic itself.
 *
 * Start-up: optional hardware initialisation (-i), then the channel is
 * created and a helper thread registers it with the shell
 * (DCMD_HOTSWAP_REGISTER on the device path). The registration must come
 * from a second thread because the shell, while handling it, sends
 * CORE_OP_EXPORT/IMPORT messages that the main thread has to receive.
 *
 * Usage: <core> [-n path] [-p priority] [-i init_ms]
 *   -n  device path of the shell to register with (default: driver's path)
 *   -p  SCHED_FIFO priority of the registering thread (default 60). The shell
 *       runs the swap at this priority (QNX message passing inherits the
 *       sender's priority), so it should be above the clients' priority.
 *   -i  simulated hardware initialisation time. It runs before the core
 *       registers, i.e. while the old core is still serving clients, so it
 *       does not interrupt them.
 *
 * Build with -DCRASH_ON_READ to get a "buggy release" that crashes on its
 * first read (used to demonstrate rollback).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include "driver.h"

static int serving = 0;     /* 0 until state is imported, 0 again after export */

struct reg_args {
    const char *path;
    int chid;
    int prio;
};

/* Helper thread: tell the shell about our channel. The devctl returns only
 * after the shell has finished the whole swap. */
static void *register_thread(void *arg)
{
    struct reg_args *ra = arg;

    if (ra->prio > 0) {
        struct sched_param sp = { .sched_priority = ra->prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != EOK) {
            fprintf(stderr, "core v%u: cannot set priority %d: %s\n",
                    drv.version, ra->prio, strerror(rc));
        }
    }

    int fd = open(ra->path, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "core v%u: cannot open %s: %s\n",
                drv.version, ra->path, strerror(errno));
        exit(1);
    }

    struct hotswap_register reg = {
        .chid = ra->chid,
        .core_version = drv.version,
        .fmt_min = drv.fmt_min,
        .fmt_max = drv.fmt_max,
    };
    int rc = devctl(fd, DCMD_HOTSWAP_REGISTER, &reg, sizeof reg, NULL);
    close(fd);
    if (rc != EOK) {
        fprintf(stderr, "core v%u: registration refused: %s\n",
                drv.version, strerror(rc));
        exit(1);
    }
    printf("core v%u (pid %d): active on %s\n", drv.version, getpid(), ra->path);
    fflush(stdout);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *path = drv.path;
    int  prio = 60;
    long init_ms = 0;
    int  opt;
    while ((opt = getopt(argc, argv, "n:p:i:")) != -1) {
        switch (opt) {
        case 'n': path = optarg; break;
        case 'p': prio = atoi(optarg); break;
        case 'i': init_ms = atol(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-n path] [-p priority] [-i init_ms]\n", argv[0]);
            return 2;
        }
    }

    /* Hardware initialisation, done before registering: the predecessor
     * keeps serving clients meanwhile. */
    if (init_ms > 0) {
        usleep((useconds_t)init_ms * 1000);
    }

    /* Public channel so the shell (another process) can connect to it. */
    int chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }

    static struct reg_args ra;
    ra.path = path;
    ra.chid = chid;
    ra.prio = prio;
    pthread_t tid;
    if (pthread_create(&tid, NULL, register_thread, &ra) != EOK) {
        perror("pthread_create");
        return 1;
    }
    pthread_detach(tid);

    for (;;) {
        static struct core_msg m;   /* ~530 bytes: keep it off the stack */
        struct _msg_info info;
        int rcvid = MsgReceive(chid, &m, sizeof m, &info);
        if (rcvid == -1) {
            perror("MsgReceive");
            return 1;
        }
        if (rcvid == 0) {
            continue;   /* a pulse (kernel notification): nothing to do */
        }
        if (info.msglen < (ssize_t)sizeof m || m.type != CORE_MSG_TYPE) {
            MsgError(rcvid, EBADMSG);
            continue;
        }

        switch (m.op) {
        case CORE_OP_READ: {
            if (!serving) {
                MsgError(rcvid, EAGAIN);
                break;
            }
#ifdef CRASH_ON_READ
            /* Simulated bug in a new release: crash on the first request. */
            fprintf(stderr, "core v%u (pid %d): simulated bug, crashing\n",
                    drv.version, getpid());
            raise(SIGSEGV);
#endif
            uint8_t buf[CORE_MAX_REPLY];
            ssize_t n = drv.read(buf, sizeof buf);
            if (n < 0) {
                MsgError(rcvid, EIO);
            } else {
                /* The reply status is the record size: the shell's MsgSend
                 * returns it, so the shell can forward any record type. */
                MsgReply(rcvid, (long)n, buf, (size_t)n);
            }
            break;
        }
        case CORE_OP_EXPORT: {
            /* Hand over the state and stop serving: from now on the
             * successor owns it (unless the shell rolls back and imports
             * it into us again). */
            static struct core_state st;
            if (drv.export_state(m.state.format, &st) == -1) {
                MsgError(rcvid, EPROTO);    /* no common format: no swap */
                break;
            }
            serving = 0;
            MsgReply(rcvid, EOK, &st, sizeof st);
            break;
        }
        case CORE_OP_IMPORT:
            if (m.state.size > CORE_STATE_MAX || drv.import_state(&m.state) == -1) {
                MsgError(rcvid, ENOTSUP);   /* shell keeps the old core */
                break;
            }
            serving = 1;
            MsgReply(rcvid, EOK, NULL, 0);
            break;
        case CORE_OP_QUIT:
            MsgReply(rcvid, EOK, NULL, 0);
            printf("core v%u (pid %d): retired\n", drv.version, getpid());
            return 0;
        default:
            MsgError(rcvid, ENOSYS);
            break;
        }
    }
}
