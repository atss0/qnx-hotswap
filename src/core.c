/*
 * core.c - the swappable half of the driver: logic and state.
 *
 * A core serves requests from the shell over its own QNX channel. This toy
 * driver is a counter: each read returns the next value. CORE_VERSION is set
 * at build time (core_v1, core_v2) so the swap is visible to clients.
 *
 * Start-up: the core creates its channel, then a helper thread registers it
 * with the shell (DCMD_HOTSWAP_REGISTER on /dev/counter). The registration
 * must come from a second thread because the shell, while handling it, sends
 * CORE_OP_EXPORT/IMPORT messages that the main thread has to receive.
 *
 * Usage: core_vN [-p priority]
 *   -p  SCHED_FIFO priority of the registering thread (default 60). The shell
 *       runs the swap at this priority (QNX message passing inherits the
 *       sender's priority), so it should be above the clients' priority.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include "hotswap_proto.h"

#ifndef CORE_VERSION
#define CORE_VERSION 1
#endif

/* Driver state. Only the main thread touches it. */
static uint64_t counter_value;
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

    struct hotswap_register reg = { .chid = ra->chid, .core_version = CORE_VERSION };
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

int main(int argc, char **argv)
{
    int prio = 60;
    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') {
            prio = atoi(optarg);
        } else {
            fprintf(stderr, "usage: %s [-p priority]\n", argv[0]);
            return 2;
        }
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
            struct counter_rec rec = {
                .value = ++counter_value,
                .core_version = CORE_VERSION,
            };
            MsgReply(rcvid, EOK, &rec, sizeof rec);
            break;
        }
        case CORE_OP_EXPORT: {
            /* Hand over the state and stop serving: from now on the
             * successor owns it. */
            struct core_state st = {
                .format = CORE_STATE_FORMAT,
                .value  = counter_value,
            };
            serving = 0;
            MsgReply(rcvid, EOK, &st, sizeof st);
            break;
        }
        case CORE_OP_IMPORT:
            if (m.state.format != CORE_STATE_FORMAT) {
                MsgError(rcvid, ENOTSUP);   /* shell keeps the old core */
                break;
            }
            counter_value = m.state.value;
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
