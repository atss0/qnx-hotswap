/*
 * ctl_test.c - checks that write() and driver-specific devctl() reach the
 * driver core through the shell, using /dev/counter.
 *
 * Usage: ctl_test [device]     (default /dev/counter; needs write access)
 * Exit code: 0 if every step gave the expected value.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "counter.h"

static int failures;

static void check(const char *what, uint64_t got, uint64_t want)
{
    printf("ctl_test: %-28s got %llu, want %llu: %s\n", what,
           (unsigned long long)got, (unsigned long long)want,
           got == want ? "ok" : "FAILED");
    if (got != want) {
        failures++;
    }
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : COUNTER_PATH;
    int fd = open(dev, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "ctl_test: cannot open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    uint64_t v = 1000000;
    if (write(fd, &v, sizeof v) != (ssize_t)sizeof v) {
        fprintf(stderr, "ctl_test: write failed: %s\n", strerror(errno));
        return 1;
    }

    struct counter_rec rec;
    if (read(fd, &rec, sizeof rec) != (ssize_t)sizeof rec) {
        fprintf(stderr, "ctl_test: read failed: %s\n", strerror(errno));
        return 1;
    }
    check("read after write", rec.value, 1000001);

    uint64_t n = 41;
    int rc = devctl(fd, DCMD_COUNTER_ADD, &n, sizeof n, NULL);
    if (rc != EOK) {
        fprintf(stderr, "ctl_test: devctl failed: %s\n", strerror(rc));
        return 1;
    }
    check("devctl output", n, 1000042);

    if (read(fd, &rec, sizeof rec) != (ssize_t)sizeof rec) {
        fprintf(stderr, "ctl_test: read failed: %s\n", strerror(errno));
        return 1;
    }
    check("read after devctl", rec.value, 1000043);

    close(fd);
    return failures ? 1 : 0;
}
