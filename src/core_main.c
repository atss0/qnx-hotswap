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
 * Only the shell may talk to a core: the channel is reachable by any process
 * on the machine, so every message is checked against the shell's pid (taken
 * from the kernel, on the file descriptor the core registers through). When
 * the shell goes away the core exits instead of lingering as an orphan.
 *
 * "Buggy release" builds used by the failure demos:
 *   -DCRASH_AFTER=n   crash on read number n
 *   -DHANG_ON_READ    spin forever on the first read
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

/* The only process allowed to send us requests. 0 until known: nothing is
 * accepted before that. volatile: written by the registering thread, read by
 * the main thread. */
static volatile pid_t shell_pid = 0;
static int shell_scoid = -1;  /* server connection of the shell, once seen */

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

    /* Ask the kernel who really owns this path before announcing our channel:
     * our replies must only ever go to that process. */
    struct _server_info si;
    if (ConnectServerInfo(0, fd, &si) != fd) {
        fprintf(stderr, "core v%u: cannot identify the shell: %s\n",
                drv.version, strerror(errno));
        exit(1);
    }
    shell_pid = si.pid;

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

    /* Fully buffered even on a terminal, so a driver's printf during a swap
     * is only a memory copy; output is flushed at points outside the swap. */
    setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
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

    /* Public channel so the shell (another process) can connect to it.
     * _NTO_CHF_DISCONNECT: the kernel tells us when a client process goes
     * away. There must be no _NTO_CHF_UNBLOCK here: the shell relies on a
     * timed-out MsgSend returning at once, even if we never reply. */
    int chid = ChannelCreate(_NTO_CHF_DISCONNECT);
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

#ifdef CRASH_AFTER
    long reads_served = 0;
#endif

    for (;;) {
        static struct core_msg m;   /* ~530 bytes: keep it off the stack */
        struct _msg_info info;
        int rcvid = MsgReceive(chid, &m, sizeof m, &info);
        if (rcvid == -1) {
            perror("MsgReceive");
            return 1;
        }
        if (rcvid == 0) {
            /* A pulse. The one that matters: the shell has gone away. Any
             * other process connecting and leaving must not stop us. */
            const struct _pulse *p = (const struct _pulse *)&m;
            if (p->code == _PULSE_CODE_DISCONNECT) {
                if (p->scoid == shell_scoid) {
                    printf("core v%u (pid %d): shell is gone, exiting\n",
                           drv.version, getpid());
                    fflush(stdout);
                    return 0;
                }
                ConnectDetach(p->scoid);    /* release the kernel's record */
            }
            continue;
        }
        /* The kernel fills in info.pid, so it cannot be forged. */
        if (shell_pid == 0 || info.pid != shell_pid) {
            MsgError(rcvid, EPERM);
            continue;
        }
        shell_scoid = info.scoid;

        /* Each operation needs a different part of the message. */
        size_t need = CORE_MSG_HDR_LEN;
        if (info.msglen >= (ssize_t)CORE_MSG_HDR_LEN) {
            switch (m.op) {
            case CORE_OP_EXPORT:
            case CORE_OP_CHECKPOINT:
                need = CORE_MSG_STATE_HDR_LEN;
                break;
            case CORE_OP_IMPORT:
            case CORE_OP_WRITE:
            case CORE_OP_CTL:
                need = CORE_MSG_STATE_HDR_LEN;
                if (info.msglen >= (ssize_t)need && m.state.size <= CORE_STATE_MAX) {
                    need += m.state.size;
                } else {
                    need = sizeof m + 1;    /* malformed: always rejected */
                }
                break;
            default:
                break;
            }
        }
        if (info.msglen < (ssize_t)need || m.type != CORE_MSG_TYPE) {
            MsgError(rcvid, EBADMSG);
            continue;
        }

        switch (m.op) {
        case CORE_OP_READ: {
            if (!serving) {
                MsgError(rcvid, EAGAIN);
                break;
            }
#ifdef HANG_ON_READ
            /* Simulated bug: an endless loop. The core is alive but never
             * replies, which is the case a crash detector cannot see. */
            fprintf(stderr, "core v%u (pid %d): simulated bug, hanging\n",
                    drv.version, getpid());
            for (;;) {
            }
#endif
#ifdef CRASH_AFTER
            if (++reads_served >= CRASH_AFTER) {
                fprintf(stderr, "core v%u (pid %d): simulated bug, crashing on "
                        "read %ld\n", drv.version, getpid(), reads_served);
                raise(SIGSEGV);
            }
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
        case CORE_OP_EXPORT:
        case CORE_OP_CHECKPOINT: {
            /* EXPORT hands over the state and stops serving: from now on the
             * successor owns it (unless the shell rolls back and imports it
             * into us again). CHECKPOINT is only a copy for safe keeping. */
            static struct core_state st;
            if (drv.export_state(m.state.format, &st) == -1) {
                MsgError(rcvid, EPROTO);    /* no common format: no swap */
                break;
            }
            if (m.op == CORE_OP_EXPORT) {
                serving = 0;
            }
            MsgReply(rcvid, EOK, &st, sizeof st);
            break;
        }
        case CORE_OP_IMPORT:
            if (drv.import_state(&m.state) == -1) {
                MsgError(rcvid, ENOTSUP);   /* shell keeps the old core */
                break;
            }
            serving = 1;
            MsgReply(rcvid, EOK, NULL, 0);
            /* The driver may have logged during import: write it out only
             * now, so the output does not lengthen the swap (the shell and
             * its clients wait for this reply). */
            fflush(stdout);
            break;
        case CORE_OP_WRITE: {
            ssize_t n = (!serving || drv.write == NULL) ? -1
                      : drv.write(m.state.data, m.state.size);
            if (n < 0) {
                MsgError(rcvid, serving ? ENOSYS : EAGAIN);
            } else {
                MsgReply(rcvid, (long)n, NULL, 0);
            }
            break;
        }
        case CORE_OP_CTL: {
            ssize_t n = (!serving || drv.control == NULL) ? -1
                      : drv.control(m.state.format, m.state.data, m.state.size,
                                    sizeof m.state.data);
            if (n < 0) {
                MsgError(rcvid, serving ? ENOTTY : EAGAIN);
            } else {
                MsgReply(rcvid, (long)n, m.state.data, (size_t)n);
            }
            break;
        }
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
