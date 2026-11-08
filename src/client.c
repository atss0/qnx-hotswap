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
 * If the descriptor goes bad (a conventional driver was restarted), the
 * client reopens /dev/counter on the next tick, as a robust application
 * would, and reports how long the device was unavailable.
 *
 * The period comes from a periodic timer delivering pulses (see
 * examples/jitter for why clock_nanosleep is not used).
 *
 * Usage: client [seconds] [period_us] [priority] [samples.csv]
 *   defaults: 10 1000 50, no sample file
 *   samples.csv: one line per tick (time, read latency, lateness, ok, value,
 *   version) for plotting with tools/plot.py. Samples are kept in memory
 *   during the run and written at the end, so file I/O never disturbs the
 *   loop.
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

#include "counter.h"

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 0)

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* One tick's measurements, kept in memory for the optional CSV file. */
struct sample {
    uint64_t t_ns;          /* wake-up time since the first tick */
    uint64_t value;         /* counter value read (0 if the read failed) */
    uint32_t read_ns;       /* duration of open()/read() this tick */
    uint32_t late_ns;       /* how late the loop woke up */
    uint32_t version;       /* driver version that answered */
    uint32_t ok;            /* 1 = a full record was read */
};

static int write_samples(const char *path, const struct sample *s, long n)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "client: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "t_ms,read_us,late_us,ok,value,version\n");
    for (long i = 0; i < n; i++) {
        fprintf(f, "%.3f,%.2f,%.2f,%u,%llu,%u\n",
                s[i].t_ns / 1e6, s[i].read_ns / 1e3, s[i].late_ns / 1e3,
                s[i].ok, (unsigned long long)s[i].value, s[i].version);
    }
    fclose(f);
    return 0;
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
    const char *csv    = argc > 4 ? argv[4] : NULL;
    long     per_sec   = (long)(1000000000ull / period_ns);
    long     total     = seconds * per_sec;
    /* Several clients on one device each see only part of the counter
     * values: then a gap means "did not increase", not "not exactly +1". */
    int      shared    = getenv("HOTSWAP_SHARED") != NULL;

    /* Allocate (and touch) the sample buffer before the loop starts, so no
     * page faults happen while measuring. */
    struct sample *samples = NULL;
    if (csv) {
        samples = calloc((size_t)total, sizeof *samples);
        if (samples == NULL) {
            fprintf(stderr, "client: no memory for %ld samples\n", total);
            return 1;
        }
        memset(samples, 0, (size_t)total * sizeof *samples);
    }

    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != EOK) {
            fprintf(stderr, "client: cannot set priority %d: %s\n", prio, strerror(rc));
        }
    }

    int fd = open(COUNTER_PATH, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "client: cannot open %s: %s\n", COUNTER_PATH, strerror(errno));
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

    /* Wait for the first tick. Tick k is then due at start + k * period. */
    struct _pulse p;
    MsgReceivePulse(chid, &p, sizeof p, NULL);
    uint64_t start = now_ns();
    uint64_t tick  = 0;     /* index of the tick we are serving */

    struct stats win = {0}, all = {0};
    uint64_t prev_value = 0;
    uint32_t prev_ver = 0;
    int have_prev = 0;

    /* Outage tracking: from the first failed read to the next good one. */
    uint64_t outage_start = 0, max_outage_ns = 0;
    long     outage_errors = 0, reconnects = 0;

    for (long i = 1; i <= total; i++) {
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        uint64_t woke = now_ns();
        tick++;

        /* If the loop was stalled for several periods, the timer could not
         * queue one pulse per expiry: those ticks are simply gone. Detect
         * them from the clock, count each as a missed deadline, and resync
         * so one stall is not reported as lateness forever after. */
        uint64_t elapsed_ticks = (woke - start) / period_ns;
        long lost = 0;
        if (elapsed_ticks > tick) {
            lost = (long)(elapsed_ticks - tick);
            tick = elapsed_ticks;
            fprintf(stderr, "client: stall before read #%ld: %ld tick(s) lost\n", i, lost);
        }
        uint64_t due = start + tick * period_ns;

        /* Like a real application, reopen the device after its descriptor
         * went bad (that happens when a conventional driver is restarted).
         * While /dev/counter does not exist yet, open() fails: that tick
         * counts as an error too. */
        struct counter_rec rec;
        ssize_t n = -1;
        int err = 0;
        if (fd == -1) {
            fd = open(COUNTER_PATH, O_RDONLY);
            if (fd != -1) {
                reconnects++;
            } else {
                err = errno;
            }
        }
        if (fd != -1) {
            n = read(fd, &rec, sizeof rec);
            if (n == -1) {
                err = errno;
                /* EAGAIN: the hot-swap shell has no core for the moment; the
                 * descriptor is still good. Anything else (a conventional
                 * driver restarted): drop it and reopen next tick. */
                if (err != EAGAIN) {
                    close(fd);
                    fd = -1;
                }
            }
        }
        uint64_t done = now_ns();

        /* Lateness is measured from when the tick was due; a tick that
         * arrives a little early (timer granularity) counts as on time. */
        uint64_t late    = woke > due ? woke - due : 0;
        uint64_t read_ns = done - woke;
        int      missed  = (done > due ? done - due : 0) >= period_ns;
        win.missed += lost;
        all.missed += lost;

        if (samples) {
            struct sample *sm = &samples[i - 1];
            sm->t_ns    = woke - start;
            sm->read_ns = (uint32_t)read_ns;
            sm->late_ns = (uint32_t)late;
            sm->ok      = n == (ssize_t)sizeof rec;
            sm->value   = sm->ok ? rec.value : 0;
            sm->version = sm->ok ? rec.core_version : 0;
        }

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
            /* Report only the first failure of an outage, not one per tick. */
            if (outage_start == 0) {
                outage_start = woke;
                outage_errors = 0;
                fprintf(stderr, "client: read #%ld failed: %s; device unavailable\n",
                        i, n == -1 ? strerror(err) : "short read");
            }
            outage_errors++;
        } else {
            if (outage_start != 0) {
                uint64_t outage = done - outage_start;
                if (outage > max_outage_ns) {
                    max_outage_ns = outage;
                }
                fprintf(stderr, "client: device back after %.1f ms (%ld failed "
                        "reads); counter now %llu\n", outage / 1e6, outage_errors,
                        (unsigned long long)rec.value);
                outage_start = 0;
            }
            if (have_prev && (shared ? rec.value <= prev_value
                                     : rec.value != prev_value + 1)) {
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
    printf("TOTAL reconnects=%ld longest_outage_ms=%.1f\n",
           reconnects, max_outage_ns / 1e6);
    if (samples) {
        write_samples(csv, samples, total);
        free(samples);
    }
    return (all.errors || all.gaps || all.missed) ? 1 : 0;
}
