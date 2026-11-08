/*
 * accel_client.c - a 1 kHz control loop reading /dev/accel.
 *
 * Every period it reads one sample and checks what a control application
 * would care about during a driver upgrade:
 *   - failed reads (the device was unavailable),
 *   - sequence breaks (the driver lost or repeated samples / its state),
 *   - uncalibrated samples,
 *   - measurement error: the sample value minus the true acceleration at
 *     the sample's timestamp (known, because the hardware is simulated;
 *     see accel.h). A driver that lost its filter history or calibration
 *     shows up here as an error spike;
 *   - timing: wake-up lateness, read duration, missed deadlines.
 * Like client.c it reopens the device after its descriptor goes bad and
 * can record every tick to a CSV file for tools/plot.py.
 *
 * Usage: accel_client [seconds] [period_us] [priority] [samples.csv]
 *   defaults: 10 1000 50, no sample file
 *
 * When several clients read the same device (HOTSWAP_SHARED set in the
 * environment), each one sees only part of the sequence numbers, so a break
 * is then a sequence number that does not increase (a reset or repeat)
 * rather than one that is not exactly previous + 1.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#include "accel.h"

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 0)

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* One tick's measurements, kept in memory for the optional CSV file. */
struct sample {
    uint64_t t_ns;      /* wake-up time since the first tick */
    uint64_t seq;
    int32_t  value_mg;
    int32_t  truth_mg;
    uint32_t read_ns;
    uint32_t late_ns;
    uint16_t version;
    uint16_t ok;        /* 1 = a full, calibrated record was read */
};

static int write_samples(const char *path, const struct sample *s, long n)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "accel_client: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "t_ms,read_us,late_us,ok,seq,value_mg,truth_mg,err_mg,version\n");
    for (long i = 0; i < n; i++) {
        fprintf(f, "%.3f,%.2f,%.2f,%u,%llu,%d,%d,%d,%u\n",
                s[i].t_ns / 1e6, s[i].read_ns / 1e3, s[i].late_ns / 1e3, s[i].ok,
                (unsigned long long)s[i].seq, s[i].value_mg, s[i].truth_mg,
                s[i].ok ? s[i].value_mg - s[i].truth_mg : 0, s[i].version);
    }
    fclose(f);
    return 0;
}

/* Running statistics for one reporting window or the whole run. */
struct stats {
    long     reads, errors, breaks, uncal, missed;
    uint64_t max_late_ns, max_read_ns;
    int32_t  max_abs_err;
    double   sum_sq_err;
    long     n_err;
};

static void stats_print(const char *label, const struct stats *s, uint16_t ver)
{
    printf("%s reads=%ld failed=%ld seq_breaks=%ld uncal=%ld missed=%ld "
           "err_mg rms=%.1f max=%d  read_us max=%.1f  late_max_us=%.1f  v%u\n",
           label, s->reads, s->errors, s->breaks, s->uncal, s->missed,
           s->n_err ? sqrt(s->sum_sq_err / s->n_err) : 0.0, s->max_abs_err,
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
    int      shared    = getenv("HOTSWAP_SHARED") != NULL;

    /* Allocate (and touch) the sample buffer before the loop starts. */
    struct sample *samples = NULL;
    if (csv) {
        samples = calloc((size_t)total, sizeof *samples);
        if (samples == NULL) {
            fprintf(stderr, "accel_client: no memory for %ld samples\n", total);
            return 1;
        }
        memset(samples, 0, (size_t)total * sizeof *samples);
    }

    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != EOK) {
            fprintf(stderr, "accel_client: cannot set priority %d: %s\n", prio, strerror(rc));
        }
    }

    int fd = open(ACCEL_PATH, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "accel_client: cannot open %s: %s\n", ACCEL_PATH, strerror(errno));
        return 1;
    }

    /* Periodic timer -> pulse on our own channel (see examples/jitter). */
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || coid == -1) {
        perror("accel_client: channel");
        return 1;
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_TICK, 0);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("accel_client: timer_create");
        return 1;
    }
    struct itimerspec its = {
        .it_value    = { .tv_sec = 0, .tv_nsec = (long)period_ns },
        .it_interval = { .tv_sec = 0, .tv_nsec = (long)period_ns },
    };
    timer_settime(timer, 0, &its, NULL);

    struct _pulse p;
    MsgReceivePulse(chid, &p, sizeof p, NULL);
    uint64_t start = now_ns();
    uint64_t tick  = 0;

    struct stats win, all;
    memset(&win, 0, sizeof win);
    memset(&all, 0, sizeof all);
    uint64_t prev_seq = 0;
    uint16_t prev_ver = 0;
    int have_prev = 0;
    uint64_t outage_start = 0, max_outage_ns = 0;
    long outage_errors = 0, reconnects = 0;

    for (long i = 1; i <= total; i++) {
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        uint64_t woke = now_ns();
        tick++;

        /* Ticks lost during a stall: count as missed and resync. */
        uint64_t elapsed_ticks = (woke - start) / period_ns;
        long lost = 0;
        if (elapsed_ticks > tick) {
            lost = (long)(elapsed_ticks - tick);
            tick = elapsed_ticks;
        }
        uint64_t due = start + tick * period_ns;

        /* Read one sample; reopen the device if its descriptor went bad. */
        struct accel_rec rec;
        ssize_t n = -1;
        int err = 0;
        if (fd == -1) {
            fd = open(ACCEL_PATH, O_RDONLY);
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
                close(fd);
                fd = -1;
            }
        }
        uint64_t done = now_ns();

        uint64_t late    = woke > due ? woke - due : 0;
        uint64_t read_ns = done - woke;
        int      missed  = (done > due ? done - due : 0) >= period_ns;
        int      got     = n == (ssize_t)sizeof rec;
        int      valid   = got && (rec.flags & ACCEL_F_CALIBRATED);
        int32_t  truth   = got ? (int32_t)lrint(accel_truth_mg(rec.t_ns)) : 0;
        int32_t  e       = valid ? rec.value_mg - truth : 0;
        int      brk     = got && have_prev &&
                           (shared ? rec.seq <= prev_seq : rec.seq != prev_seq + 1);

        struct stats *ss[2] = { &win, &all };
        for (int k = 0; k < 2; k++) {
            struct stats *s = ss[k];
            s->reads++;
            s->missed += lost + missed;
            if (read_ns > s->max_read_ns) s->max_read_ns = read_ns;
            if (late > s->max_late_ns)    s->max_late_ns = late;
            if (!got) {
                s->errors++;
            } else if (!valid) {
                s->uncal++;
            } else {
                int32_t a = e < 0 ? -e : e;
                if (a > s->max_abs_err) s->max_abs_err = a;
                s->sum_sq_err += (double)e * e;
                s->n_err++;
            }
            if (brk) {
                s->breaks++;
            }
        }

        if (!got) {
            if (outage_start == 0) {
                outage_start = woke;
                outage_errors = 0;
                fprintf(stderr, "accel_client: read #%ld failed: %s; device unavailable\n",
                        i, n == -1 ? strerror(err) : "short read");
            }
            outage_errors++;
        } else {
            if (outage_start != 0) {
                uint64_t outage = done - outage_start;
                if (outage > max_outage_ns) max_outage_ns = outage;
                fprintf(stderr, "accel_client: device back after %.1f ms (%ld failed "
                        "reads)\n", outage / 1e6, outage_errors);
                outage_start = 0;
            }
            if (brk) {
                fprintf(stderr, "accel_client: sequence break at #%ld: %llu -> %llu\n", i,
                        (unsigned long long)prev_seq, (unsigned long long)rec.seq);
            }
            if (have_prev && rec.version != prev_ver) {
                printf("accel_client: driver v%u -> v%u at read #%ld, seq %llu, "
                       "error %d mg\n", prev_ver, rec.version, i,
                       (unsigned long long)rec.seq, e);
            }
            prev_seq  = rec.seq;
            prev_ver  = rec.version;
            have_prev = 1;
        }

        if (samples) {
            struct sample *sm = &samples[i - 1];
            sm->t_ns     = woke - start;
            sm->read_ns  = (uint32_t)read_ns;
            sm->late_ns  = (uint32_t)late;
            sm->ok       = (uint16_t)valid;
            sm->seq      = got ? rec.seq : 0;
            sm->value_mg = got ? rec.value_mg : 0;
            sm->truth_mg = truth;
            sm->version  = got ? rec.version : 0;
        }

        if (i % per_sec == 0) {
            char label[32];
            snprintf(label, sizeof label, "t=%lds", i / per_sec);
            stats_print(label, &win, prev_ver);
            memset(&win, 0, sizeof win);
        }
    }

    stats_print("TOTAL", &all, prev_ver);
    printf("TOTAL reconnects=%ld longest_outage_ms=%.1f\n", reconnects, max_outage_ns / 1e6);
    if (samples) {
        write_samples(csv, samples, total);
        free(samples);
    }
    return (all.errors || all.breaks || all.uncal || all.missed) ? 1 : 0;
}
