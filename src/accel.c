/*
 * accel.c - accelerometer driver (simulated hardware, see accel.h).
 *
 * What the driver does on every read:
 *   1. take a raw sample from the "hardware",
 *   2. subtract the zero-g bias measured during calibration,
 *   3. (v2 only) reject glitches: a sample that jumps more than
 *      SPIKE_LIMIT_MG away from the last output is replaced by that output
 *      (unless the jump persists, then the filter restarts),
 *   4. smooth with a moving average over the last 8 samples,
 *   5. return the result with a sequence number.
 *
 * Driver state, i.e. what a successor must inherit to continue seamlessly:
 *   - the calibration (bias), which costs ~0.5 s of self-test to redo,
 *   - the sample sequence number,
 *   - the filter history (8 samples); without it the output restarts from
 *     a single noisy sample,
 *   - v2 also: the last output (for glitch rejection) and a counter of
 *     rejected glitches.
 *
 * State formats:
 *   1 (v1): bias, calibrated, seq, window[8], idx, count
 *   2 (v2): format 1 + last_out_mg, spikes_rejected
 * v2 converts format 1 when upgrading and writes format 1 when a
 * downgrade to v1 is requested.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>

#include "accel.h"
#include "driver.h"

#ifndef DRV_VERSION
#define DRV_VERSION 1
#endif

#if DRV_VERSION >= 2
#define FMT_MAX 2
#else
#define FMT_MAX 1
#endif

#define WINDOW          8
#define CAL_SAMPLES     250     /* self-test samples, ~1 ms apart */
#define SPIKE_LIMIT_MG  400     /* v2 glitch threshold; the true signal moves
                                   at most ~3 mg per ms */
#define SPIKE_RUN       3       /* longer runs of "glitches" are real changes */

/* ---- state ------------------------------------------------------------ */

struct accel_state_f1 {
    int32_t  bias_mg;
    uint32_t calibrated;
    uint64_t seq;
    int32_t  window[WINDOW];    /* bias-corrected samples, ring buffer */
    uint32_t idx;               /* next slot to write */
    uint32_t count;             /* valid samples in window (<= WINDOW) */
};

struct accel_state_f2 {
    struct accel_state_f1 f1;
    int32_t  last_out_mg;
    uint32_t reserved;
    uint64_t spikes_rejected;
};

static struct accel_state_f2 st;    /* v1 uses only st.f1 */

/* ---- simulated hardware --------------------------------------------------- */

static uint32_t rng = 2463534242u;

static uint32_t xorshift32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* One raw reading. In self-test mode the sensor reports zero g, so the
 * reading is just bias + noise: that is how the bias is calibrated. */
static int32_t hw_read(uint64_t t_ns, int selftest)
{
    int32_t noise = (int32_t)(xorshift32() % (2 * SIM_NOISE_MG + 1)) - SIM_NOISE_MG;
    int32_t glitch = 0;
    if (xorshift32() % SIM_GLITCH_1_IN == 0) {
        glitch = (xorshift32() & 1) ? SIM_GLITCH_MG : -SIM_GLITCH_MG;
    }
    double truth = selftest ? 0.0 : accel_truth_mg(t_ns);
    return (int32_t)lrint(truth) + SIM_BIAS_MG + noise + glitch;
}

/* Measure the zero-g bias: average CAL_SAMPLES self-test readings, one per
 * millisecond. Real sensors need this after every power-up or driver restart
 * unless the calibration is handed over.
 *
 * The spacing comes from a periodic timer delivering pulses: a loop around
 * nanosleep(1 ms) would really take about 2 ms per sample, because QNX rounds
 * sleeps up to the next clock tick. */
static void calibrate(void)
{
    uint64_t t0 = now_ns();
    int64_t sum = 0;

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_MINAVAIL, 0);
    timer_t timer;
    timer_create(CLOCK_MONOTONIC, &ev, &timer);
    struct itimerspec its = {
        .it_value    = { .tv_sec = 0, .tv_nsec = 1000000 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 1000000 },
    };
    timer_settime(timer, 0, &its, NULL);

    for (int i = 0; i < CAL_SAMPLES; i++) {
        struct _pulse p;
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        sum += hw_read(0, 1);
    }

    timer_delete(timer);
    ConnectDetach(coid);
    ChannelDestroy(chid);

    st.f1.bias_mg = (int32_t)(sum / CAL_SAMPLES);
    st.f1.calibrated = 1;
    printf("accel v%d: calibrated, bias %d mg (true %d), took %.0f ms\n",
           DRV_VERSION, st.f1.bias_mg, SIM_BIAS_MG, (now_ns() - t0) / 1e6);
    fflush(stdout);
}

/* ---- driver callbacks -------------------------------------------------- */

static int accel_import(const struct core_state *cs)
{
    memset(&st, 0, sizeof st);
    rng ^= (uint32_t)getpid() * 2654435761u;    /* different noise per process */

    if (cs->size == 0) {
        calibrate();                /* fresh start: no inherited calibration */
        return 0;
    }

    if (cs->format == 1 && cs->size >= sizeof(struct accel_state_f1)) {
        memcpy(&st.f1, cs->data, sizeof st.f1);
#if FMT_MAX >= 2
        /* Convert format 1: seed the glitch filter with the current
         * filter output so it is active from the first sample. */
        int64_t sum = 0;
        for (uint32_t i = 0; i < st.f1.count; i++) {
            sum += st.f1.window[i];
        }
        st.last_out_mg = st.f1.count ? (int32_t)(sum / st.f1.count) : 0;
        st.spikes_rejected = 0;
#endif
    } else if (cs->format == 2 && FMT_MAX >= 2 &&
               cs->size >= sizeof(struct accel_state_f2)) {
        memcpy(&st, cs->data, sizeof st);
    } else {
        return -1;
    }
    if (st.f1.count > WINDOW || st.f1.idx >= WINDOW) {
        return -1;                  /* corrupt state: refuse the swap */
    }

    printf("accel v%d: imported format %u state: bias %d mg, seq %llu, "
           "filter %u samples%s\n", DRV_VERSION, cs->format, st.f1.bias_mg,
           (unsigned long long)st.f1.seq, st.f1.count,
           cs->format < FMT_MAX ? " (converted from older format)" : "");
    /* No fflush here: the host flushes after replying (part of a swap). */
    return 0;
}

static int accel_export(uint32_t wanted, struct core_state *cs)
{
    if (wanted < 1) {
        return -1;
    }
    memset(cs, 0, sizeof *cs);
    cs->format = wanted < FMT_MAX ? wanted : FMT_MAX;
    if (cs->format >= 2) {
        memcpy(cs->data, &st, sizeof st);
        cs->size = sizeof st;
    } else {
        memcpy(cs->data, &st.f1, sizeof st.f1);   /* drop v2-only fields */
        cs->size = sizeof st.f1;
    }
    return 0;
}

static ssize_t accel_read(void *buf, size_t max)
{
    uint64_t t = now_ns();
    int32_t raw = hw_read(t, 0);
    int32_t x = raw - st.f1.bias_mg;

#if DRV_VERSION >= 2
    /* Glitch rejection: physically impossible jumps are replaced by the
     * last output instead of being fed into the average. A glitch is a
     * single sample; if the jump persists for more than SPIKE_RUN samples,
     * the signal really is somewhere else (e.g. the filter history is old
     * because the driver was down and resumed from a checkpoint), so the
     * filter restarts from the new sample instead of holding a stale value.
     * The run length is deliberately not part of the state. */
    static uint32_t rejected_in_a_row;
    if (st.f1.count > 0 && (x - st.last_out_mg > SPIKE_LIMIT_MG ||
                            st.last_out_mg - x > SPIKE_LIMIT_MG)) {
        if (++rejected_in_a_row <= SPIKE_RUN) {
            x = st.last_out_mg;
            st.spikes_rejected++;
        } else {
            st.f1.count = 0;
            st.f1.idx = 0;
            rejected_in_a_row = 0;
        }
    } else {
        rejected_in_a_row = 0;
    }
#endif

    /* Moving average over the last WINDOW samples. */
    st.f1.window[st.f1.idx] = x;
    st.f1.idx = (st.f1.idx + 1) % WINDOW;
    if (st.f1.count < WINDOW) {
        st.f1.count++;
    }
    int64_t sum = 0;
    for (uint32_t i = 0; i < st.f1.count; i++) {
        sum += st.f1.window[i];
    }
    int32_t out = (int32_t)(sum / st.f1.count);
    st.last_out_mg = out;

    struct accel_rec rec = {
        .seq      = ++st.f1.seq,
        .t_ns     = t,
        .value_mg = out,
        .raw_mg   = raw,
        .flags    = st.f1.calibrated ? ACCEL_F_CALIBRATED : 0,
        .version  = DRV_VERSION,
    };
    size_t n = max < sizeof rec ? max : sizeof rec;
    memcpy(buf, &rec, n);
    return (ssize_t)n;
}

const struct driver drv = {
    .path         = ACCEL_PATH,
    .version      = DRV_VERSION,
    .fmt_min      = 1,
    .fmt_max      = FMT_MAX,
    .import_state = accel_import,
    .export_state = accel_export,
    .read         = accel_read,
};
