/*
 * jitter.c - measure wakeup latency of a periodic loop on QNX.
 *
 * Runs a loop that sleeps until an absolute deadline every PERIOD_NS
 * (default 1 ms) and records how late each wakeup was. Prints min / avg /
 * max lateness and how many wakeups missed the next deadline entirely.
 *
 * This is the baseline for the hotswap demo: the control loop must keep
 * these numbers unchanged while the driver is upgraded underneath it.
 *
 * Two modes:
 *   sleep - clock_nanosleep() to an absolute deadline. On QNX a sleep is
 *           rounded up to the next clock tick plus one tick, so this mode
 *           shows ~1 tick of built-in lateness.
 *   timer - a periodic POSIX timer that delivers a pulse to a channel and the
 *           loop blocks in MsgReceivePulse(). This is the idiomatic QNX way
 *           to run a periodic task.
 *
 * Usage: jitter [iterations] [period_us] [priority] [sleep|timer]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 0)

static uint64_t ts_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

int main(int argc, char **argv)
{
    long iters     = argc > 1 ? atol(argv[1]) : 5000;
    long period_ns = (argc > 2 ? atol(argv[2]) : 1000) * 1000L;
    int  prio      = argc > 3 ? atoi(argv[3]) : 0;

    /* Optionally run as a real-time FIFO thread, like a real control loop. */
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0) {
            fprintf(stderr, "pthread_setschedparam(FIFO, %d) failed: %d\n", prio, rc);
        }
    }

    const char *mode = argc > 4 ? argv[4] : "timer";
    int use_timer = strcmp(mode, "sleep") != 0;

    /* Report the system clock tick: it bounds timer resolution. */
    struct _clockperiod cp;
    if (ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0) == 0) {
        printf("clock tick=%luns\n", (unsigned long)cp.nsec);
    }

    /* Timer mode: a channel plus a periodic timer that sends us a pulse. */
    int chid = -1;
    if (use_timer) {
        chid = ChannelCreate(_NTO_CHF_PRIVATE);
        int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
        if (chid == -1 || coid == -1) { perror("channel"); return 1; }

        struct sigevent ev;
        SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_TICK, 0);

        timer_t tid;
        if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) { perror("timer_create"); return 1; }

        struct itimerspec its = {
            .it_value    = { .tv_sec = 0, .tv_nsec = period_ns },
            .it_interval = { .tv_sec = 0, .tv_nsec = period_ns },
        };
        if (timer_settime(tid, 0, &its, NULL) == -1) { perror("timer_settime"); return 1; }
    }

    struct timespec next, now;
    clock_gettime(CLOCK_MONOTONIC, &next);
    if (use_timer) {
        /* Sync to the first pulse; later expiries are measured against it. */
        struct _pulse p;
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        clock_gettime(CLOCK_MONOTONIC, &next);
    }

    uint64_t min = UINT64_MAX, max = 0, sum = 0;
    long missed = 0;

    for (long i = 0; i < iters; i++) {
        /* Advance the absolute deadline by one period. */
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        if (use_timer) {
            struct _pulse p;
            MsgReceivePulse(chid, &p, sizeof p, NULL);
        } else {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* In timer mode the expiry can land slightly before our computed
         * deadline (tick alignment); count that as zero lateness. */
        if (ts_ns(&now) < ts_ns(&next)) { now = next; }

        uint64_t late = ts_ns(&now) - ts_ns(&next);
        if (late < min) min = late;
        if (late > max) max = late;
        sum += late;
        if (late >= (uint64_t)period_ns) missed++;
    }

    printf("mode=%s iterations=%ld period=%ldus prio=%d\n", mode, iters, period_ns / 1000, prio);
    printf("lateness us: min=%.1f avg=%.1f max=%.1f  missed_deadlines=%ld\n",
           min / 1000.0, (double)sum / iters / 1000.0, max / 1000.0, missed);
    return 0;
}
