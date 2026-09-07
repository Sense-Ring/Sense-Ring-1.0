/* Host tests for vitals.c -- synthetic PPG in, heart rate out.
 *
 * Choosing BPM_MIN -- the floor below which a rate is refused rather than named
 * -- cannot be done honestly without this. The floor is a filter question, and a
 * filter question is settled by measuring the filter, not by arguing about
 * octaves.
 *
 * What it generates is deliberately the ring's *bad* case rather than a clean
 * textbook pulse: perfusion down at 0.15% of DC, baseline wander from breathing
 * several times larger than the pulse itself, and converter noise on top. A
 * fingertip clip sees ten times the perfusion; this device does not.
 *
 * Two questions, and the second matters more:
 *
 *   1. Across a sweep of true rates, how often is the reported rate wrong?
 *   2. Given breathing and no pulse at all, how often does it report a pulse?
 *
 * The second is what a low floor risks. Being blind to bradycardia is bad; a
 * confident 32bpm invented out of someone's breathing is worse, because it is
 * indistinguishable downstream from the thing it is meant to detect.
 */
#include "vitals.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M_PI is POSIX, not ISO C, and the build is -std=c11 on purpose: the firmware
 * compiles as strict C and the tests should not quietly need more.
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS ((double)PPG_SAMPLE_RATE_HZ)
#define WINDOW VITALS_WINDOW_SAMPLES

/* Well above VITALS_CONTACT_IR_DC_MIN: these tests are about the rate
 * estimator, not about the contact gate.
 */
#define DC_IR 60000.0
#define DC_RED 45000.0

/* What counts as a wrong answer. The window comment in vitals.h quotes its
 * measurements in "more than 5bpm wrong", so this matches.
 */
#define ERROR_TOLERANCE_BPM 5.0

/* ---- deterministic noise ------------------------------------------------- */
/* Seeded per case so a failure can be reproduced exactly, and so two runs of
 * the same build always agree -- a sweep whose verdict moves between runs
 * cannot be used to choose a constant.
 */
static uint32_t rng_state = 1;

static void rng_seed(uint32_t seed)
{
    rng_state = seed ? seed : 1;
}

static double urand(void)
{
    /* xorshift32 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (double)rng_state / 4294967296.0;
}

static double nrand(void)
{
    double u1 = urand();
    double u2 = urand();

    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---- synthetic PPG ------------------------------------------------------- */
struct signal_spec {
    double bpm;          /* 0 means no pulse at all */
    double perfusion;    /* pulse AC RMS as a fraction of DC */
    double resp_hz;      /* breathing */
    double wander;       /* breathing amplitude as a fraction of DC */
    double drift;        /* linear DC drift across the window, fraction of DC */
    double noise_counts; /* per-sample gaussian, in ADC counts */
    double phase;        /* pulse phase, radians */
};

/* A pulse shape with harmonics rather than a bare sinusoid. The harmonics are
 * the whole reason estimate_bpm() has octave-error logic: a signal that is pure
 * fundamental cannot produce the failure that logic exists to catch, so testing
 * with one would be testing the easy case only.
 */
static double pulse_shape(double phase)
{
    return sin(phase) + 0.45 * sin(2.0 * phase + 0.7) + 0.2 * sin(3.0 * phase + 1.9);
}

static void synth(const struct signal_spec *s, struct ppg_sample *out, size_t n)
{
    double shape[WINDOW];
    double rms = 0.0;

    for (size_t i = 0; i < n; i++) {
        double t = (double)i / FS;

        shape[i] = (s->bpm > 0.0)
                       ? pulse_shape(2.0 * M_PI * (s->bpm / 60.0) * t + s->phase)
                       : 0.0;
        rms += shape[i] * shape[i];
    }
    rms = sqrt(rms / (double)n);
    if (rms < 1e-9) {
        rms = 1.0; /* no pulse: leave the shape at zero rather than dividing by it */
    }

    for (size_t i = 0; i < n; i++) {
        double t = (double)i / FS;
        double ac = (shape[i] / rms) * s->perfusion * DC_IR;
        double baseline = s->wander * DC_IR * sin(2.0 * M_PI * s->resp_hz * t + 0.4) +
                          s->drift * DC_IR * (t / ((double)n / FS));
        double ir = DC_IR + baseline + ac + s->noise_counts * nrand();
        /* Red carries the same pulse at a different amplitude, which is what
         * the SpO2 projection expects. Not what these tests measure, but a red
         * channel of pure noise would be an unrealistic input to a function
         * that filters both.
         */
        double red = DC_RED + baseline * (DC_RED / DC_IR) + ac * 0.8 +
                     s->noise_counts * nrand();

        out[i].ir = (uint32_t)(ir < 0.0 ? 0.0 : ir);
        out[i].red = (uint32_t)(red < 0.0 ? 0.0 : red);
    }
}

/* ---- sweeps -------------------------------------------------------------- */
struct tally {
    int windows;   /* cases run */
    int believed;  /* reported a rate the confidence gate would accept */
    int wrong;     /* believed, and more than ERROR_TOLERANCE_BPM out */
    double worst;  /* largest absolute error among believed windows */
    double err_sum;
    int at_floor;  /* believed answers sitting exactly on BPM_MIN */
    int too_slow;  /* refused as a pulse slower than the floor, with the reason */
};

static const double perfusions[] = {0.0015, 0.003, 0.01};
static const double wanders[] = {0.01, 0.03};
static const double resp_rates[] = {0.20, 0.25, 0.33};

static void run_case(double true_bpm, struct tally *t, uint32_t seed)
{
    struct ppg_sample samples[WINDOW];
    struct vitals v;

    for (size_t p = 0; p < sizeof(perfusions) / sizeof(perfusions[0]); p++) {
        for (size_t w = 0; w < sizeof(wanders) / sizeof(wanders[0]); w++) {
            for (size_t r = 0; r < sizeof(resp_rates) / sizeof(resp_rates[0]); r++) {
                for (int phase = 0; phase < 3; phase++) {
                    struct signal_spec s = {
                        .bpm = true_bpm,
                        .perfusion = perfusions[p],
                        .resp_hz = resp_rates[r],
                        .wander = wanders[w],
                        .drift = 0.005,
                        .noise_counts = 20.0,
                        .phase = phase * 2.1,
                    };

                    rng_seed(seed + (uint32_t)(p * 991 + w * 97 + r * 13 + phase));
                    synth(&s, samples, WINDOW);

                    t->windows++;
                    if (vitals_compute(samples, WINDOW, &v) != 0) {
                        continue;
                    }
                    /* A refusal that names its reason is not a blind spot: the
                     * record says a pulse was there and was too slow to put a
                     * number on, which is a finding and not a gap. Counted
                     * separately from a plain "nothing found" for that reason.
                     */
                    if (v.bpm_below_floor && v.confidence >= VITALS_BPM_CONFIDENCE_MIN) {
                        t->too_slow++;
                        continue;
                    }
                    if (v.bpm == 0 || v.confidence < VITALS_BPM_CONFIDENCE_MIN) {
                        continue;
                    }

                    t->believed++;
                    if (v.bpm == VITALS_BPM_MIN) {
                        t->at_floor++;
                    }
                    if (true_bpm > 0.0) {
                        double err = fabs((double)v.bpm - true_bpm);

                        t->err_sum += err;
                        if (err > t->worst) {
                            t->worst = err;
                        }
                        if (err > ERROR_TOLERANCE_BPM) {
                            t->wrong++;
                        }
                    }
                }
            }
        }
    }
}

int main(void)
{
    static const double rates[] = {25, 28, 30, 33, 35, 38, 40, 45, 50, 60,  70,
                                   80, 90, 100, 120, 140, 160, 180, 200};
    struct tally total = {0};
    int blind = 0;
    int disguised = 0;

    printf("vitals host tests\n");
    printf("  sample rate      %g Hz\n", FS);
    printf("  window           %d samples (%d s)\n", WINDOW, VITALS_WINDOW_SEC);
    printf("  BPM_MIN          %d\n", VITALS_BPM_MIN);
    printf("  high-pass corner %d bpm\n", VITALS_HP_CORNER_BPM);
    printf("  confidence gate  %d\n\n", VITALS_BPM_CONFIDENCE_MIN);

    printf("  true   cases  believed   wrong   mean err   worst   at floor   too slow\n");
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        struct tally t = {0};

        run_case(rates[i], &t, 0x5eed0000u + (uint32_t)i * 7919u);
        printf("  %5.0f   %5d   %7d   %5d   %8.2f   %5.1f   %8d   %8d\n", rates[i],
               t.windows, t.believed, t.wrong,
               t.believed ? t.err_sum / t.believed : 0.0, t.worst, t.at_floor,
               t.too_slow);

        if (rates[i] < VITALS_BPM_MIN) {
            /* Below the floor there is exactly one acceptable outcome, and it
             * is not a number: say a pulse is there and too slow to name. Two
             * ways to get it wrong, and they are not equally bad.
             *
             * blind     -- the window produced nothing at all. The wearer's
             *              bradycardia is a gap in the log, indistinguishable
             *              from a ring that has stopped reading.
             *
             * disguised -- the window produced a *believed rate*. Far worse:
             *              25bpm filed as a healthy 50 is not a gap, carries
             *              full confidence, and nothing downstream can tell.
             *              This is what the old clamp did to every sub-floor
             *              window, and it is the reason the check exists.
             */
            if (t.believed > 0) {
                disguised++;
            } else if (t.too_slow == 0) {
                blind++;
            }
        }

        total.windows += t.windows;
        total.believed += t.believed;
        total.wrong += t.wrong;
        total.err_sum += t.err_sum;
        if (t.worst > total.worst) {
            total.worst = t.worst;
        }
    }

    printf("\n  totals: %d windows, %d believed, %d wrong (>%.0f bpm), worst %.1f\n",
           total.windows, total.believed, total.wrong, ERROR_TOLERANCE_BPM,
           total.worst);
    printf("  rates below the floor reported as a believed rate: %d\n", disguised);
    printf("  rates below the floor that vanish silently:        %d\n\n", blind);

    /* The safety test. Breathing, drift and noise -- no pulse anywhere in the
     * signal. Anything believed here is a heart rate invented out of a
     * breathing artefact, which is the specific risk of lowering the floor.
     */
    {
        struct tally t = {0};

        run_case(0.0, &t, 0xb1eedu);
        printf("  no pulse at all: %d windows, %d believed a rate, %d called too slow\n",
               t.windows, t.believed, t.too_slow);
        if (t.believed) {
            printf("  *** %d false pulses -- the floor is too low for this filter\n",
                   t.believed);
        }
        printf("\n");

        return (total.wrong > 0 || t.believed > 0 || disguised > 0) ? 1 : 0;
    }
}
