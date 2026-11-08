/*
 * rogue.c - a test: can an unrelated process command a driver core?
 *
 * A core's channel is reachable by any process on the machine, because the
 * shell (another process) has to connect to it. This program plays the
 * attacker: it connects to a running core's channels and tries to stop the
 * core (QUIT), take its state (EXPORT) and overwrite it (IMPORT). Every
 * attempt must be refused, with EPERM, without disturbing the core.
 *
 * Usage: rogue <core_pid>
 * Exit code: 0 if everything was refused, 1 if any request was accepted.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>

#include "hotswap_proto.h"

#define MAX_CHID 8      /* a core has very few channels */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <core_pid>\n", argv[0]);
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    static const struct { enum core_op op; const char *name; } tries[] = {
        { CORE_OP_EXPORT, "EXPORT" },
        { CORE_OP_IMPORT, "IMPORT" },
        { CORE_OP_QUIT,   "QUIT"   },
    };

    int attempts = 0, refused = 0;
    for (int chid = 1; chid <= MAX_CHID; chid++) {
        int coid = ConnectAttach(0, pid, chid, _NTO_SIDE_CHANNEL, 0);
        if (coid == -1) {
            continue;
        }
        for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
            static struct core_msg m;
            static struct core_state reply;
            memset(&m, 0, sizeof m);
            m.type = CORE_MSG_TYPE;
            m.op = (uint16_t)tries[i].op;
            m.state.format = 1;
            m.state.size = 0;   /* an empty import would reset the driver */

            attempts++;
            if (MsgSend(coid, &m, sizeof m, &reply, sizeof reply) == -1) {
                if (errno == EPERM) {
                    refused++;
                }
                printf("rogue: chid %d %s: %s\n", chid, tries[i].name, strerror(errno));
            } else {
                printf("rogue: chid %d %s: ACCEPTED\n", chid, tries[i].name);
            }
        }
        ConnectDetach(coid);
    }
    printf("rogue: %d of %d attempts refused with EPERM\n", refused, attempts);
    return (attempts > 0 && refused == attempts) ? 0 : 1;
}
