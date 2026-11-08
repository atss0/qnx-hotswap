/*
 * accel.h - record format and simulated hardware for the accelerometer
 * driver (/dev/accel). Shared by the driver (accel.c) and its test client
 * (accel_client.c).
 *
 * The "hardware" is simulated, but behaves like a real MEMS accelerometer
 * in the ways that matter for a driver upgrade:
 *   - the true acceleration is a 0.5 Hz, +-1000 mg oscillation (a vibrating
 *     or swaying mount), a function of CLOCK_MONOTONIC time only, so it
 *     continues smoothly no matter which driver process samples it;
 *   - every raw reading carries a fixed zero-g offset (bias) of +137 mg,
 *     which the driver must measure at start-up (calibration, about half a
 *     second in self-test mode) and subtract;
 *   - readings are noisy (+-40 mg) and about 1 in 200 is a glitch of
 *     +-1500 mg (e.g. electrical interference).
 * Because the truth is a known function of time, the client can compute
 * the measurement error of every sample it receives.
 */
#ifndef ACCEL_H
#define ACCEL_H

#include <math.h>
#include <stdint.h>

#define ACCEL_PATH "/dev/accel"

/* One read() of /dev/accel returns exactly this record. */
struct accel_rec {
    uint64_t seq;        /* sample number; continues across driver swaps */
    uint64_t t_ns;       /* CLOCK_MONOTONIC time the sample was taken */
    int32_t  value_mg;   /* filtered, bias-corrected acceleration */
    int32_t  raw_mg;     /* the raw hardware reading */
    uint16_t flags;      /* ACCEL_F_* */
    uint16_t version;    /* driver version that produced the sample */
    uint32_t reserved;
};

#define ACCEL_F_CALIBRATED 0x1   /* bias has been measured and is applied */

/* ---- simulated physics ------------------------------------------------ */

#define SIM_AMPL_MG   1000.0
#define SIM_FREQ_HZ   0.5
#define SIM_BIAS_MG   137
#define SIM_NOISE_MG  40
#define SIM_GLITCH_MG 1500
#define SIM_GLITCH_1_IN 200

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The true acceleration at time t. */
static inline double accel_truth_mg(uint64_t t_ns)
{
    return SIM_AMPL_MG * sin(2.0 * M_PI * SIM_FREQ_HZ * ((double)t_ns / 1e9));
}

#endif /* ACCEL_H */
