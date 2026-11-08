/*
 * client.c - a 1 kHz "control loop" that reads /dev/counter every period.
 *
 * It plays the part of a real-time application that must not notice a
 * driver upgrade. Every period it reads one record and checks:
 *   - errors:  read() failed or returned a short record
 *   - gaps:    the counter did not advance by exactly 1 (state was lost or
 *              duplicated across a swap)
 *   - timing:  how late the loop woke up, how long read() took, and whether
 *              the cycle overran its period (missed deadline)
 * It prints one line per second and a summary, and reports every change of
 * core version as it happens.
 *
 * The period comes from a periodic timer delivering pulses (see
 * examples/jitter for why clock_nanosleep is not used).
 *
 * Usage: client [seconds] [period_us] [priority]   (defaults: 10 1000 50)
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>

#include "hotswap_proto.h"

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 0)

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Running statistics for one reporting window or the whole run. */
struct stats {
    long     reads, errors, gaps, missed;
    uint64_t max_late_ns, max_read_ns, sum_read_ns;
};

static void stats_print(const char *label, const struct stats *s, uint32_t ver)
{
    printf("%s reads=%ld errors=%ld gaps=%ld missed=%ld "
           "read_us avg=%.1f max=%.1f  late_max_us=%.1f  core=v%u\n",
           label, s->reads, s->errors, s->gaps, s->missed,
           s->reads ? s->sum_read_ns / 1000.0 / s->reads : 0.0,
           s->max_read_ns / 1000.0, s->max_late_ns / 1000.0, ver);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    long     seconds   = argc > 1 ? atol(argv[1]) : 10;
    uint64_t period_ns = (uint64_t)(argc > 2 ? atol(argv[2]) : 1000) * 1000u;
    int      prio      = argc > 3 ? atoi(argv[3]) : 50;
    long     per_sec   = (long)(1000000000ull / period_ns);
    long     total     = seconds * per_sec;

    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != EOK) {
            fprintf(stderr, "client: cannot set priority %d: %s\n", prio, strerror(rc));
        }
    }

    int fd = open(HOTSWAP_PATH, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "client: cannot open %s: %s\n", HOTSWAP_PATH, strerror(errno));
        return 1;
    }

    /* Periodic timer -> pulse on our own channel. */
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || coid == -1) {
        perror("client: channel");
        return 1;
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_TICK, 0);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("client: timer_create");
        return 1;
    }
    struct itimerspec its = {
        .it_value    = { .tv_sec = 0, .tv_nsec = (long)period_ns },
        .it_interval = { .tv_sec = 0, .tv_nsec = (long)period_ns },
    };
    timer_settime(timer, 0, &its, NULL);

    /* Wait for the first tick; every later tick is due one period after the
     * previous due time. */
    struct _pulse p;
    MsgReceivePulse(chid, &p, sizeof p, NULL);
    uint64_t due = now_ns();

    struct stats win = {0}, all = {0};
    uint64_t prev_value = 0;
    uint32_t prev_ver = 0;
    int have_prev = 0;

    for (long i = 1; i <= total; i++) {
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        due += period_ns;

        uint64_t woke = now_ns();
        struct counter_rec rec;
        ssize_t n = read(fd, &rec, sizeof rec);
        uint64_t done = now_ns();

        /* Lateness is measured from when the tick was due; a tick that
         * arrives a little early (timer granularity) counts as on time. */
        uint64_t late    = woke > due ? woke - due : 0;
        uint64_t read_ns = done - woke;
        int      missed  = (done > due ? done - due : 0) >= period_ns;

        struct stats *ss[2] = { &win, &all };
        for (int k = 0; k < 2; k++) {
            struct stats *s = ss[k];
            s->reads++;
            s->sum_read_ns += read_ns;
            if (read_ns > s->max_read_ns) s->max_read_ns = read_ns;
            if (late > s->max_late_ns)    s->max_late_ns = late;
            if (missed)                   s->missed++;
        }

        if (n != (ssize_t)sizeof rec) {
            win.errors++;
            all.errors++;
            fprintf(stderr, "client: read #%ld failed: %s\n", i,
                    n == -1 ? strerror(errno) : "short read");
        } else {
            if (have_prev && rec.value != prev_value + 1) {
                win.gaps++;
                all.gaps++;
                fprintf(stderr, "client: gap at #%ld: %llu -> %llu\n", i,
                        (unsigned long long)prev_value, (unsigned long long)rec.value);
            }
            if (have_prev && rec.core_version != prev_ver) {
                printf("client: core v%u -> v%u at read #%ld, value %llu, "
                       "that read took %.1f us\n", prev_ver, rec.core_version, i,
                       (unsigned long long)rec.value, read_ns / 1000.0);
            }
            prev_value = rec.value;
            prev_ver   = rec.core_version;
            have_prev  = 1;
        }

        if (i % per_sec == 0) {
            char label[32];
            snprintf(label, sizeof label, "t=%lds", i / per_sec);
            stats_print(label, &win, prev_ver);
            memset(&win, 0, sizeof win);
        }
    }

    stats_print("TOTAL", &all, prev_ver);
    return (all.errors || all.gaps || all.missed) ? 1 : 0;
}
