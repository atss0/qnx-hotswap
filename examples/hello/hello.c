/*
 * Smoke test for the QNX toolchain and the QEMU target.
 * Prints the OS name and creates a channel, the endpoint QNX message
 * passing uses, to show that QNX-specific APIs link and run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/neutrino.h>
#include <sys/utsname.h>

int main(void)
{
    struct utsname u;
    if (uname(&u) == -1) {
        perror("uname");
        return EXIT_FAILURE;
    }

    int chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return EXIT_FAILURE;
    }

    printf("Hello from %s %s (%s), channel id %d\n", u.sysname, u.release, u.machine, chid);
    ChannelDestroy(chid);
    return EXIT_SUCCESS;
}
