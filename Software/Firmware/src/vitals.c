/* Heart rate and SpO2 from a window of raw PPG counts. All fixed point: the
 * nRF52832 has an FPU but nothing else here needs it, and keeping this integer
 * avoids pulling FP context into the build for one calculation a minute.
 * See ARCHITECTURE.md §4 for the algorithm reasoning.
 */
#include "vitals.h"
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

/* Rates we are willing to believe, and the filter corner, both from vitals.h.
 *
 * The floor was 40 for years on the reasoning that the high-pass had to
 * separate the pulse from breathing at ~0.25Hz and 30bpm (0.5Hz) sat too close
 * to it. That reasoning was sound about the *filter* and wrong about the
 * *band*: the two had been tied together only because the filter width was
 * computed from LAG_MAX. Untied (VITALS_HP_CORNER_BPM), the correlator searches
 * down to 30bpm while the filter goes on rejecting breathing exactly as hard as
 * it did before. Measured in tests/host -- see ARCHITECTURE.md §4.2.
 */
#define BPM_MIN VITALS_BPM_MIN
#define BPM_MAX VITALS_BPM_MAX

/* Lag in samples for a given rate: lag = 60 * fs / bpm. */
#define LAG_FOR_BPM(bpm) ((60 * PPG_SAMPLE_RATE_HZ) / (bpm))
#define LAG_MIN LAG_FOR_BPM(BPM_MAX)
#define LAG_MAX LAG_FOR_BPM(BPM_MIN)

/* Width of the high-pass, from its own corner rather than from LAG_MAX. */
#define HP_LAG LAG_FOR_BPM(VITALS_HP_CORNER_BPM)

/* The autocorrelation needs at least two periods of the slowest rate. */
BUILD_ASSERT(VITALS_MIN_SAMPLES >= 2 * LAG_MAX, "window too short for BPM_MIN");
BUILD_ASSERT(LAG_MIN >= 2, "sample rate too low for BPM_MAX");

/* The sub-harmonic guard in estimate_bpm() looks one octave below the slowest
 * rate in the band, so the window has to reach a lag of 2*LAG_MAX+1 and still
 * leave something to correlate against.
 */
BUILD_ASSERT(VITALS_MIN_SAMPLES > 2 * LAG_MAX + 1, "window too short for the sub-harmonic guard");

/* A corner above the floor attenuates the slowest rates the correlator is
 * allowed to find. That is deliberate and safe -- autocorrelation measures
 * periodicity, not amplitude -- but a corner *below* the floor would be pure
 * loss: breathing passed through for no gain in range.
 */
BUILD_ASSERT(VITALS_HP_CORNER_BPM >= BPM_MIN, "high-pass corner below the floor");
BUILD_ASSERT(VITALS_MIN_SAMPLES >= PPG_SAMPLE_RATE_HZ / 2, "window shorter than a contact block");

/* Bench-tunable gates. All three want checking against real optics on a real
 * finger before they are trusted; the numbers below are starting points.
 */
/* Both live in vitals.h: a caller that refuses to measure needs to quote them.
 * The gap between them is the hysteresis -- a quarter of the threshold is wider
 * than the noise on any level we have seen and still far narrower than the
 * on-to-off step, which is most of a decade.
 */
#define CONTACT_IR_DC_MIN VITALS_CONTACT_IR_DC_MIN
#define CONTACT_IR_DC_ON  VITALS_CONTACT_IR_DC_ON

/* Contact is decided on this much of the newest signal rather than on the
 * window mean. Half a second is long enough to average out sample noise and
 * short enough that taking the ring off registers within one FIFO poll.
 */
#define CONTACT_BLOCK_SAMPLES (PPG_SAMPLE_RATE_HZ / 2)
/* There is deliberately no heart-rate confidence threshold in this file. This
 * code reports what it measured and how much it believes it; whether that is
 * good enough to act on depends on what the caller is about to do with it, and
 * only the caller knows that. main.c owns the gate (VITALS_BPM_CONFIDENCE_MIN)
 * and stores what it refuses, with the reason attached.
 *
 * SpO2 is different, and the asymmetry is the point: the ratio-of-ratios below
 * is not merely less certain on a weak pulse, it is *biased* by the residue
 * left in the filtered signal. A caller cannot correct for that with a
 * threshold of its own, so this file declines to compute it at all.
 */
#define SPO2_CONFIDENCE_MIN 600 /* stricter than HR: the ratio needs a clean pulse */

/* SpO2 only, and the reason for the number is measured rather than guessed.
 * Whatever baseline wander survives the high-pass is common to both channels,
 * so it correlates in exactly the way the projection below cannot distinguish
 * from a pulse, and it drags R toward 1.0 -- which reads as low saturation.
 * The weaker the pulse, the more the residue dominates.
 *
 * This used to be 50 (0.5% of DC) on the strength of a measured bias of -1.0%
 * at 0.4% perfusion and -2.2% at 0.25%. Almost all of that residue turned out
 * to be the moving average's edge artefact (see highpass() below), not its
 * frequency response, and with the edges fixed the same synthetic sweep gives
 * ~0.0% bias down to 0.4% perfusion, -0.3% at 0.15% and -0.7% at 0.05%. 0.12%
 * is the lowest setting where every window in the sweep stayed inside the same
 * half a point the old threshold was chosen for; below it the windows that
 * creep past are the ones whose perfusion is mostly noise.
 *
 * That matters because a ring is not a fingertip clip: it sees a small fraction
 * of the perfusion a clip does, and 0.5% rejected essentially every real
 * window. It is the reason SpO2 read 0 rather than anything at all.
 *
 * Parts per ten thousand, not per thousand like the reported field: at ring
 * signal levels the reported figure quantises to 0, 1 or 2, which is too coarse
 * to put a threshold on.
 */
#define SPO2_PERFUSION_MIN 12 /* parts per ten thousand of DC */

/* A peak within 80% of the best one, at a shorter lag, wins. See the octave
 * error note in ARCHITECTURE.md §4.3.
 */
#define PEAK_COMPETITIVE_PERMILLE 800

/* How much taller a peak at twice the chosen lag has to be before we conclude
 * the chosen one is only its second harmonic.
 *
 * Why any such test can work: correlation here is divided by the window energy
 * rather than by the shrinking overlap (see corr_at), so a genuine period's
 * peaks *decay* at each multiple. A taller peak one octave down therefore
 * cannot be a multiple of the rate we picked -- it has to be the rate itself.
 *
 * Measured as corr[2L]/corr[L] across the whole tests/host sweep, 51-54 windows
 * per rate over the three perfusions, two wanders and three breathing rates:
 *
 *   true bpm    min    mean    max
 *      25      1.169   1.310  1.720   <- fundamental is out of band
 *      28      1.149   1.252  1.428   <- fundamental is out of band
 *      30      0.825   0.850  0.956
 *      35      0.847   0.899  1.168
 *      45      0.886   0.935  1.460
 *      58      0.901   0.921  0.952
 *
 * The means separate cleanly; the tails overlap, because on a 0.15% perfusion
 * window the "pulse" is barely above the converter noise and any ratio is
 * possible. So this threshold is not a clean split and cannot be -- it is a
 * choice about which way to be wrong. Over the 572 windows of the sweep:
 *
 *   1.05   0 sub-floor rates missed, 17/470 in-band windows refused
 *   1.10   0 missed, 13 refused
 *   1.15   1 missed,  6 refused
 *   1.20   4 missed,  3 refused
 *   1.25  41 missed,  3 refused
 *
 * 1.10 buys the last of the detection at 2.8% of in-band windows near the
 * floor, and those two costs are not comparable. A false refusal is one window
 * of a 15-second cadence saying "could not read this", which the next window
 * almost always undoes and which the record format already carries. A missed
 * one is a slow pulse filed as an ordinary rate, silently, for as long as it
 * lasts. Detection is bought at the shallow end of this curve -- past 1.20 it
 * collapses -- so buy all of it.
 */
#define SUBHARMONIC_WINS_PERMILLE 1100

/* Working buffers. 375 samples x 4 bytes x 2 channels is 3KB, plus 1.5KB of
 * filter scratch. Static because main's stack is 2KB and this would not fit.
 */
static int32_t ac_ir[VITALS_WINDOW_SAMPLES];
static int32_t ac_red[VITALS_WINDOW_SAMPLES];
static int32_t filter_tmp[VITALS_WINDOW_SAMPLES];

/* Mean IR over the newest CONTACT_BLOCK_SAMPLES, and the smallest such mean
 * anywhere in the window. Blocks are laid out backwards from the newest sample
 * so the last one is always whole and always the most recent half-second; any
 * remainder is left at the old end, where it matters least.
 *
 * The two answers are different questions and the caller needs both:
 *
 *   tail  -- is the ring on a finger *now*. This is what "worn" means, and it
 *            has to come from the newest samples only. Judging it by the mean
 *            of the whole window, which is what this used to do, cannot report
 *            a removal until half the window has flushed: measured at 15s of
 *            latency, of which the first 12s were spent reporting a confident
 *            53bpm from a ring sitting on a desk. The DC ramp as it slid off
 *            was slow enough to autocorrelate.
 *
 *   min   -- was it on a finger for *all* of the window. Vitals are computed
 *            across the whole window, so one contaminated stretch invalidates
 *            them even though the ring is back on now.
 */
static uint32_t contact_dc(const struct ppg_sample *samples, size_t n, uint32_t *tail_dc)
{
    uint32_t min_dc = UINT32_MAX;
    size_t end = n;

    *tail_dc = 0;

    while (end >= CONTACT_BLOCK_SAMPLES) {
        uint64_t sum = 0;
        uint32_t dc;

        for (size_t i = end - CONTACT_BLOCK_SAMPLES; i < end; i++) {
            sum += samples[i].ir;
        }
        dc = (uint32_t)(sum / CONTACT_BLOCK_SAMPLES);

        if (end == n) {
            *tail_dc = dc;
        }
        if (dc < min_dc) {
            min_dc = dc;
        }
        end -= CONTACT_BLOCK_SAMPLES;
    }

    return min_dc;
}

uint32_t vitals_ir_dc(const struct ppg_sample *samples, size_t n)
{
    uint64_t sum = 0;

    if (samples == NULL || n == 0) {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        sum += samples[i].ir;
    }

    return (uint32_t)(sum / n);
}

bool vitals_contact(const struct ppg_sample *samples, size_t n, bool worn)
{
    uint32_t dc;

    if (samples == NULL || n == 0) {
        return worn; /* nothing new to judge on: believe what we believed */
    }

    dc = vitals_ir_dc(samples, n);

    return worn ? (dc >= CONTACT_IR_DC_MIN) : (dc >= CONTACT_IR_DC_ON);
}

/* Integer square root, bit by bit. Newton's method would need a 64-bit divide
 * per iteration; this needs none.
 */
static uint32_t isqrt64(uint64_t x)
{
    uint64_t rem = 0;
    uint64_t root = 0;

    for (int i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (x >> 62);
        x <<= 2;
        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }

    return (uint32_t)(root >> 1);
}

/* The sample at j, with the window extended past either end by reflecting it
 * through the endpoint: v[-k] = 2*v[0] - v[k]. That is an *odd* reflection, and
 * the choice matters more than it looks.
 *
 * The moving average below is centred, so within half a window of each end it
 * needs samples that do not exist. Averaging over whatever fits instead --
 * which is what this used to do -- computes the mean of a shorter, off-centre
 * stretch, and on a signal with any slope at all that mean is simply wrong. The
 * error is the size of the drift, not the size of the pulse, and each pass
 * smears it another `half` samples inward: three passes contaminate ~54 samples
 * at each end with an artefact several times larger than the heartbeat. It
 * correlates over long lags, so it puts a broad hump at the far end of the lag
 * band, and estimate_bpm() locks onto that instead of the pulse. Measured on a
 * 0.2% perfusion window with 3% baseline wander, it took the post-filter RMS
 * from 26 counts (pulse only) to 55, and the reported rate to exactly half the
 * true one.
 *
 * Odd reflection continues the local trend rather than folding it back, so the
 * average of a locally straight signal stays exactly on the trend and the ends
 * come out as clean as the middle. An even reflection (v[-k] = v[k]) would put
 * a corner at each endpoint and leave half the artefact behind.
 */
static inline int32_t sample_at(const int32_t *v, size_t n, ptrdiff_t j)
{
    if (j < 0) {
        return 2 * v[0] - v[-j];
    }
    if ((size_t)j >= n) {
        return 2 * v[n - 1] - v[2 * (n - 1) - (size_t)j];
    }

    return v[j];
}

/* Subtract a centred moving average of 2*half+1 samples. That is a zero-phase
 * high-pass with a null at DC and unity gain at fs/(2*half+1); at half =
 * HP_LAG/2 that corner is VITALS_HP_CORNER_BPM, so whatever survives is faster
 * than breathing, which is what dominates a raw PPG trace. The width is a
 * parameter rather than baked in because the corner is only meaningful relative
 * to a rate, and a caller that has measured one may want to say so.
 *
 * Applied HP_PASSES times for a steeper roll-off. One pass leaves ~20% of the
 * drift through, which is still several times the pulse and swamps it; each
 * further pass squares what is left. Three costs nothing at the slow end --
 * 40bpm still comes back exact -- and halves the SpO2 bias that the residue
 * causes, which is why it is not two.
 *
 * The sum is carried rather than recomputed: 750 samples times a 37-wide window
 * times three passes times two channels is 166k multiply-free adds done the
 * naive way, and the running form makes it 750 per pass regardless of width.
 */
#define HP_PASSES 3
static void highpass(int32_t *v, size_t n, size_t half)
{
    const size_t width = 2 * half + 1;
    int64_t sum = 0;

    for (ptrdiff_t j = -(ptrdiff_t)half; j <= (ptrdiff_t)half; j++) {
        sum += sample_at(v, n, j);
    }

    for (size_t i = 0; i < n; i++) {
        filter_tmp[i] = v[i] - (int32_t)(sum / (int64_t)width);
        /* Slide the window one sample: drop the trailing edge, add the leading
         * one. v is untouched until the memcpy, so both reads see the input.
         */
        sum += sample_at(v, n, (ptrdiff_t)i + (ptrdiff_t)half + 1) -
               sample_at(v, n, (ptrdiff_t)i - (ptrdiff_t)half);
    }

    memcpy(v, filter_tmp, n * sizeof(v[0]));
}

/* 3-tap boxcar. At 25Hz its first null is 8.3Hz, well clear of the 3.7Hz top
 * of the heart-rate band, so it takes out sample noise without touching the
 * pulse. Both channels get it, so its gain cancels in the SpO2 ratio.
 */
static void smooth3(int32_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        size_t a = (i == 0) ? 0 : i - 1;
        size_t b = (i + 1 < n) ? i + 1 : i;

        filter_tmp[i] = (v[a] + v[i] + v[b]) / 3;
    }

    memcpy(v, filter_tmp, n * sizeof(v[0]));
}

static uint32_t rms(const int32_t *v, size_t n)
{
    uint64_t acc = 0;

    /* 18-bit counts squared is 2^36, times 200 samples is 2^44: 64-bit or it
     * silently wraps.
     */
    for (size_t i = 0; i < n; i++) {
        acc += (uint64_t)((int64_t)v[i] * v[i]);
    }

    return isqrt64(acc / n);
}

/* Correlation of the signal with itself `lag` samples later, as parts per
 * thousand of the window's energy.
 *
 * Divide by n, not by the overlap (n - lag). Normalising away the shrinking
 * overlap sounds fairer but removes the decay with lag that keeps the peak at T
 * above the one at 2T -- it made every rate above 90bpm come back halved. The
 * sub-harmonic guard below then leans on that same decay in the other
 * direction, so this is load-bearing twice over.
 *
 * sum*1000/energy is that ratio -- (sum/n) / (energy/n) -- in one division
 * instead of three, which matters because the two truncating divisions it
 * replaces each threw away up to a whole count. On a weak pulse, where energy
 * per sample is small, that quantised the whole correlation curve and with it
 * the interpolated peak.
 *
 * Split out from the band scan because the guard needs the same number at a few
 * lags outside the band, and three extra evaluations cost far less than widening
 * the band, the array that holds it, and the stack it sits on.
 */
static int32_t corr_at(const int32_t *ac, size_t n, size_t lag, int64_t energy)
{
    int64_t sum = 0;

    for (size_t i = 0; i + lag < n; i++) {
        sum += (int64_t)ac[i] * ac[i + lag];
    }

    return (int32_t)(sum * 1000 / energy);
}

/* Rate by autocorrelation rather than peak detection: we want the period, not
 * individual beat times, and correlation degrades gracefully where threshold
 * crossing falls off a cliff. It also hands us a confidence number for free.
 *
 * Returns 0 when it will not name a rate. `*below_floor` separates the two
 * reasons for that: a pulse that is really there and really slower than BPM_MIN
 * (set), versus nothing periodic worth the name (clear). The caller needs the
 * difference -- see the note on the band edge below.
 */
static uint16_t estimate_bpm(const int32_t *ac, size_t n, uint16_t *confidence,
                             bool *below_floor)
{
    int32_t corr[LAG_MAX + 2];
    int64_t energy = 0;
    int32_t best = 0;
    size_t best_lag = 0;
    size_t chosen;
    int32_t cm, c0, cp, denom, lag_milli, bpm;

    *confidence = 0;
    *below_floor = false;

    for (size_t i = 0; i < n; i++) {
        energy += (int64_t)ac[i] * ac[i];
    }
    if (energy < (int64_t)n) {
        return 0; /* signal below one count RMS: nothing to correlate */
    }

    /* One lag either side of the band, because the interpolation below reads
     * the winner's neighbours.
     */
    for (size_t lag = LAG_MIN - 1; lag <= LAG_MAX + 1; lag++) {
        corr[lag] = corr_at(ac, n, lag, energy);
    }

    for (size_t lag = LAG_MIN; lag <= LAG_MAX; lag++) {
        if (corr[lag] > best) {
            best = corr[lag];
            best_lag = lag;
        }
    }
    if (best_lag == 0 || best <= 0) {
        return 0;
    }

    /* The autocorrelation peaks at every multiple of the true period, so the
     * tallest peak is not reliably the first one. Walk up from the shortest
     * lag and take the first local maximum that is competitive with the best:
     * that is T, where the global maximum might be 2T.
     */
    chosen = best_lag;
    for (size_t lag = LAG_MIN; lag <= LAG_MAX; lag++) {
        if (corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1] &&
            (int64_t)corr[lag] * 1000 >= (int64_t)best * PEAK_COMPETITIVE_PERMILLE) {
            chosen = lag;
            break;
        }
    }

    /* Everything from here to the return used to end in one line:
     *
     *     return CLAMP(rate, BPM_MIN, BPM_MAX);
     *
     * which quietly promised that whatever the correlator had found, the answer
     * would land inside the band of rates we are willing to believe. It kept
     * that promise by moving answers rather than by rejecting them, and a moved
     * answer is indistinguishable downstream from a measured one -- same field,
     * same confidence, no mark on it. Two things came out of that, both of them
     * bradycardia hiding in plain sight, and both measured in tests/host:
     *
     *   - Breathing with no pulse anywhere in it reported a confident 220bpm in
     *     27 of 54 windows. The correlation just falls away from the short-lag
     *     end of the band, so the tallest point in the band is its edge; that
     *     edge is 250bpm, and the clamp filed it as 220.
     *
     *   - A true 25bpm pulse reported a confident 50bpm, and 28bpm reported 55.
     *     Its period is 60 samples and the band stops at 50, so the correlator
     *     never sees the fundamental and locks onto the second harmonic instead.
     *
     * The second is the dangerous one. A rate the ring cannot see is a gap in
     * the record, and a gap is visible; a rate the ring reports as an ordinary
     * 50 is not a gap, and nothing downstream has any way to know.
     *
     * So: no clamping. An estimate outside the band means we do not know the
     * rate, and where we can tell that the truth is *below* the floor we say
     * that specifically, because "too slow to measure" on a well-perfused finger
     * is itself the finding.
     */

    /* A peak sitting on the edge of the band is not a peak. The correlation is
     * still climbing where the band stops, so whatever it climbs towards is
     * outside -- and which edge it is tells us which side. (The octave walk
     * above can only ever pick a genuine interior maximum; this catches the
     * fall-back to best_lag, which is the tallest point in a closed interval and
     * has no such guarantee.)
     */
    if (corr[chosen] < corr[chosen - 1] || corr[chosen] < corr[chosen + 1]) {
        *confidence = (uint16_t)CLAMP(corr[chosen], 0, 1000);
        *below_floor = (chosen == LAG_MAX);
        return 0;
    }

    /* An interior peak, and still possibly the second harmonic of a pulse whose
     * own period is longer than the band. Look one octave down -- three lags, to
     * allow the period a sample of slack -- and if the correlation there is
     * clearly taller, that is the real pulse and it is below the floor.
     *
     * Only where that octave lands at or beyond the end of the band. Inside it,
     * a taller peak at 2L is the ordinary octave ambiguity that the walk above
     * already resolves in favour of the shorter lag, and it resolves it
     * correctly across 33-200bpm in the sweep; two rules pointing opposite ways
     * at the same lag would be one rule too many.
     *
     * At LAG_MAX exactly -- a pulse at the floor itself, whose harmonic sits at
     * BPM_MIN*2 -- the walk has no such record. It prefers the shorter lag, and
     * for a floor-rate pulse the shorter lag is the harmonic: one window in 54
     * of the true-30bpm sweep came back as a confident 60. That is the same
     * failure as the 25bpm case and deserves the same answer, so the boundary is
     * >= rather than >.
     */
    if (2 * chosen >= LAG_MAX) {
        int32_t sub = 0;

        for (size_t lag = 2 * chosen - 1; lag <= 2 * chosen + 1; lag++) {
            int32_t c = corr_at(ac, n, lag, energy);

            if (c > sub) {
                sub = c;
            }
        }

        if ((int64_t)sub * 1000 > (int64_t)corr[chosen] * SUBHARMONIC_WINS_PERMILLE) {
            /* The sub-harmonic's own height, not the harmonic's: the periodicity
             * we are refusing to name a rate for is the one down there, and a
             * refusal is only worth storing with the evidence that produced it.
             */
            *confidence = (uint16_t)CLAMP(sub, 0, 1000);
            *below_floor = true;
            return 0;
        }
    }

    /* Sub-sample the peak by fitting a parabola through it and its neighbours.
     * Without this, 25Hz quantises the rate into steps of 1500/lag bpm -- 2.4
     * bpm apart at rest and far worse higher up. This is what buys back the
     * resolution the lowered sample rate gave away.
     */
    cm = corr[chosen - 1];
    c0 = corr[chosen];
    cp = corr[chosen + 1];
    denom = cm - 2 * c0 + cp;
    lag_milli = (int32_t)chosen * 1000;
    if (denom < 0) { /* a real peak curves downward; a flat one divides by ~0 */
        int32_t delta = (500 * (cm - cp)) / denom;

        /* A parabola through three points cannot legitimately place its vertex
         * more than half a sample from the middle one.
         */
        lag_milli += CLAMP(delta, -500, 500);
    }
    if (lag_milli <= 0) {
        return 0;
    }

    *confidence = (uint16_t)CLAMP(c0, 0, 1000);

    /* Rounded rather than truncated. The interpolation is allowed to place the
     * period half a sample past LAG_MAX, which at the floor is 29.7bpm -- a
     * legitimate at-floor reading that truncation would turn into 29 and the
     * band check below would then throw away.
     */
    bpm = (60 * PPG_SAMPLE_RATE_HZ * 1000 + lag_milli / 2) / lag_milli;

    if (bpm < BPM_MIN) {
        *below_floor = true;
        return 0;
    }
    if (bpm > BPM_MAX) {
        return 0;
    }

    return (uint16_t)bpm;
}

int vitals_compute(const struct ppg_sample *samples, size_t n, struct vitals *out)
{
    int64_t sum_ir = 0;
    int64_t sum_red = 0;
    uint32_t dc_ir, dc_red, rms_ir, perfusion_p10k, tail_dc, min_block_dc;

    if (samples == NULL || out == NULL) {
        return -EINVAL;
    }
    if (n < VITALS_MIN_SAMPLES || n > VITALS_WINDOW_SAMPLES) {
        return -ENODATA;
    }

    memset(out, 0, sizeof(*out));

    /* The LEDs reflect off whatever is in front of them, including nothing.
     * A DC level this low means the ring is off the finger, and every number
     * below it would be noise dressed up as a vital sign.
     *
     * Reported as the newest half-second rather than the window mean, because
     * that is the level someone looking at the log wants to know: what the
     * sensor sees now, not what it averaged over the last 15 seconds.
     */
    min_block_dc = contact_dc(samples, n, &tail_dc);
    out->ir_dc = tail_dc;

    if (tail_dc < CONTACT_IR_DC_MIN) {
        return 0;
    }
    out->contact = true;

    /* On a finger now, but not for all of the window. Everything below averages
     * across the whole of it, so the vitals would be part pulse and part empty
     * air -- report the contact and nothing else until the window has refilled.
     */
    if (min_block_dc < CONTACT_IR_DC_MIN) {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        sum_ir += samples[i].ir;
        sum_red += samples[i].red;
    }
    dc_ir = (uint32_t)(sum_ir / (int64_t)n);
    dc_red = (uint32_t)(sum_red / (int64_t)n);

    if (dc_red == 0) {
        return 0;
    }

    /* Both channels get identical filtering. That matters for SpO2: the ratio
     * below divides one channel's AC by the other's, so any gain the filters
     * apply cancels only if it is the same gain.
     */
    for (size_t i = 0; i < n; i++) {
        ac_ir[i] = (int32_t)samples[i].ir - (int32_t)dc_ir;
        ac_red[i] = (int32_t)samples[i].red - (int32_t)dc_red;
    }

    for (int pass = 0; pass < HP_PASSES; pass++) {
        highpass(ac_ir, n, HP_LAG / 2);
        highpass(ac_red, n, HP_LAG / 2);
    }
    smooth3(ac_ir, n);
    smooth3(ac_red, n);

    rms_ir = rms(ac_ir, n);

    /* Kept at ten-thousandths for the SpO2 gate below and reported at
     * thousandths, which is what the field is documented in.
     */
    perfusion_p10k = (uint32_t)MIN((uint64_t)rms_ir * 10000 / dc_ir, UINT32_MAX);
    out->perfusion_milli = (uint16_t)MIN(perfusion_p10k / 10, UINT16_MAX);

    /* IR carries the pulse: it penetrates deeper than red and is far less
     * affected by skin tone.
     */
    out->bpm = estimate_bpm(ac_ir, n, &out->confidence, &out->bpm_below_floor);

    /* SpO2 by ratio-of-ratios: R = (AC_red/DC_red) / (AC_ir/DC_ir). Oxygenated
     * blood absorbs red and infrared differently, so R tracks saturation.
     *
     * AC_red/AC_ir comes from a least-squares projection of the red trace onto
     * the IR one, not from each channel's RMS. Both channels carry the same
     * pulse shape, so red ~= alpha * ir and alpha is exactly the amplitude
     * ratio we need. The channels' noise is independent -- separate LED pulses,
     * separate conversions -- so it cancels in the cross term, where taking two
     * RMS values instead lets it inflate both and drag R toward 1. That showed
     * up as a systematic ~1% under-read of SpO2, worst on dim optics.
     */
    if (out->confidence >= SPO2_CONFIDENCE_MIN && perfusion_p10k >= SPO2_PERFUSION_MIN) {
        int64_t cross = 0;
        int64_t ir_energy = 0;

        for (size_t i = 0; i < n; i++) {
            cross += (int64_t)ac_red[i] * ac_ir[i];
            ir_energy += (int64_t)ac_ir[i] * ac_ir[i];
        }

        /* Negative cross means the two channels move opposite ways, which no
         * pulse does: that is an artefact, not a measurement.
         */
        if (ir_energy > 0 && cross > 0) {
            int64_t alpha_milli = cross * 1000 / ir_energy;
            int32_t r_milli = (int32_t)MIN(alpha_milli * dc_ir / dc_red, (int64_t)INT16_MAX);
            int32_t spo2;

            /* Maxim's empirical fit from the MAX30102 reference design:
             *   SpO2 = -45.06*R^2 + 30.354*R + 94.845
             * scaled here to tenths of a percent with R in thousandths.
             */
            spo2 = 948 + (30354 * r_milli) / 100000 -
                   (int32_t)((4506LL * r_milli * r_milli) / 10000000);

            /* The fit is only meaningful over roughly 70-100%; outside that it
             * is extrapolation, not measurement.
             */
            out->spo2_tenths = (uint16_t)CLAMP(spo2, 700, 1000);
        }
    }

    return 0;
}
