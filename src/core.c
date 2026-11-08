/*
 * core.c - the swappable half of the driver: logic and state.
 *
 * A core serves requests from the shell over its own QNX channel. This toy
 * driver is a counter: each read returns the next value. The same source is
 * built several times:
 *
 *   core_v1      CORE_VERSION=1, state format 1 (value)
 *   core_v2      CORE_VERSION=2, state format 2 (value + generation); it
 *                imports format 1 by converting it, and can export format 1
 *                so a downgrade back to v1 also works
 *   core_v2_bad  like core_v2 but crashes on its first read: a "buggy
 *                release" used to demonstrate automatic rollback
 *
 * Start-up: the core creates its channel, then a helper thread registers it
 * with the shell (DCMD_HOTSWAP_REGISTER on /dev/counter). The registration
 * must come from a second thread because the shell, while handling it, sends
 * CORE_OP_EXPORT/IMPORT messages that the main thread has to receive.
 *
 * Usage: core_vN [-p priority] [-i init_ms]
 *   -p  SCHED_FIFO priority of the registering thread (default 60). The shell
 *       runs the swap at this priority (QNX message passing inherits the
 *       sender's priority), so it should be above the clients' priority.
 *   -i  simulated hardware initialisation time (same option as mono_vN).
 *       It runs before the core registers, i.e. while the old core is still
 *       serving clients, so it does not interrupt them.
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

#include "hotswap_proto.h"

#ifndef CORE_VERSION
#define CORE_VERSION 1
#endif

/* State formats this build can read (import) and write (export). */
#if CORE_VERSION >= 2
#define FMT_MIN 1
#define FMT_MAX 2
#else
#define FMT_MIN 1
#define FMT_MAX 1
#endif

/* Driver state. Only the main thread touches it. */
static uint64_t counter_value;
static uint64_t generation;     /* handovers survived (format 2 only) */
static int      serving = 0;    /* 0 until state is imported, 0 after export */

struct reg_args {
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
            fprintf(stderr, "core v%d: cannot set priority %d: %s\n",
                    CORE_VERSION, ra->prio, strerror(rc));
        }
    }

    int fd = open(HOTSWAP_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "core v%d: cannot open %s: %s\n",
                CORE_VERSION, HOTSWAP_PATH, strerror(errno));
        exit(1);
    }

    struct hotswap_register reg = {
        .chid = ra->chid,
        .core_version = CORE_VERSION,
        .fmt_min = FMT_MIN,
        .fmt_max = FMT_MAX,
    };
    int rc = devctl(fd, DCMD_HOTSWAP_REGISTER, &reg, sizeof reg, NULL);
    close(fd);
    if (rc != EOK) {
        fprintf(stderr, "core v%d: registration refused: %s\n",
                CORE_VERSION, strerror(rc));
        exit(1);
    }
    printf("core v%d (pid %d): active\n", CORE_VERSION, getpid());
    fflush(stdout);
    return NULL;
}

/* Fill *st with our state in the newest format we can write that is not
 * newer than 'wanted'. Returns 0, or -1 if even our oldest format is newer
 * than what the successor accepts. */
static int export_state(uint32_t wanted, struct core_state *st)
{
    memset(st, 0, sizeof *st);
    if (wanted < FMT_MIN) {
        return -1;
    }
    st->format = wanted < FMT_MAX ? wanted : FMT_MAX;
    st->value  = counter_value;
    if (st->format >= 2) {
        st->generation = generation;
    }
    return 0;
}

/* Take over state written by a predecessor, converting older formats. */
static int import_state(const struct core_state *st)
{
    if (st->format < FMT_MIN || st->format > FMT_MAX) {
        return -1;
    }
    counter_value = st->value;
    if (st->format >= 2) {
        generation = st->generation;
    } else {
        /* Format 1 has no generation: start counting from this handover. */
        generation = 0;
    }
#if FMT_MAX >= 2
    generation++;
    printf("core v%d: imported format %u state (value %llu, generation %llu)%s\n",
           CORE_VERSION, st->format, (unsigned long long)counter_value,
           (unsigned long long)generation,
           st->format < FMT_MAX ? ", converted from older format" : "");
    fflush(stdout);
#endif
    return 0;
}

int main(int argc, char **argv)
{
    int  prio = 60;
    long init_ms = 0;
    int  opt;
    while ((opt = getopt(argc, argv, "p:i:")) != -1) {
        if (opt == 'p') {
            prio = atoi(optarg);
        } else if (opt == 'i') {
            init_ms = atol(optarg);
        } else {
            fprintf(stderr, "usage: %s [-p priority] [-i init_ms]\n", argv[0]);
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
    ra.chid = chid;
    ra.prio = prio;
    pthread_t tid;
    if (pthread_create(&tid, NULL, register_thread, &ra) != EOK) {
        perror("pthread_create");
        return 1;
    }
    pthread_detach(tid);

    for (;;) {
        struct core_msg m;
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
            fprintf(stderr, "core v%d (pid %d): simulated bug, crashing\n",
                    CORE_VERSION, getpid());
            raise(SIGSEGV);
#endif
            struct counter_rec rec = {
                .value = ++counter_value,
                .core_version = CORE_VERSION,
            };
            MsgReply(rcvid, EOK, &rec, sizeof rec);
            break;
        }
        case CORE_OP_EXPORT: {
            /* Hand over the state and stop serving: from now on the
             * successor owns it (unless the shell rolls back and imports
             * it into us again). */
            struct core_state st;
            if (export_state(m.state.format, &st) == -1) {
                MsgError(rcvid, EPROTO);    /* no common format: no swap */
                break;
            }
            serving = 0;
            MsgReply(rcvid, EOK, &st, sizeof st);
            break;
        }
        case CORE_OP_IMPORT:
            if (import_state(&m.state) == -1) {
                MsgError(rcvid, ENOTSUP);   /* shell keeps the old core */
                break;
            }
            serving = 1;
            MsgReply(rcvid, EOK, NULL, 0);
            break;
        case CORE_OP_QUIT:
            MsgReply(rcvid, EOK, NULL, 0);
            printf("core v%d (pid %d): retired at value %llu\n",
                   CORE_VERSION, getpid(), (unsigned long long)counter_value);
            return 0;
        default:
            MsgError(rcvid, ENOSYS);
            break;
        }
    }
}
