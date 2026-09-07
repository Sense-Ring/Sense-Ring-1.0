#ifndef VITALS_H
#define VITALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ppg.h"

/* One measurement window. Long enough to hold several beats at the slowest rate
 * we accept, short enough that a reading is not stale by the time it arrives.
 *
 * 15s is a measured floor, not a preference. Below it the estimate does not
 * degrade gently -- on a weak, drifting signal (0.2% perfusion, the ring's
 * worst realistic case) the rate of windows coming back more than 5bpm wrong
 * across a 45-200bpm sweep goes 0 at 15s, 4/63 at 12s, 4/63 at 10s and 6/63 at
 * 8s, and those wrong answers arrive with confidence above BPM_CONFIDENCE_MIN,
 * so no gate catches them. SpO2 degrades over the same range for the same
 * reason: its bias on a ring-grade pulse runs -0.11% at 15s and -0.32% at 8s.
 *
 * Above 15s nothing measurably improves, and everything gets slower to react:
 * on a sliding window the reported rate lags a real change by most of a window.
 * See ARCHITECTURE.md §4.1.
 */
#define VITALS_WINDOW_SEC     15
#define VITALS_WINDOW_MS      (VITALS_WINDOW_SEC * 1000)
#define VITALS_WINDOW_SAMPLES (PPG_SAMPLE_RATE_HZ * VITALS_WINDOW_SEC)

/* Below this the autocorrelation has too little overlap to mean anything. */
#define VITALS_MIN_SAMPLES (VITALS_WINDOW_SAMPLES / 2)

/* The band of rates this code is willing to report.
 *
 * Public because the floor is not a private implementation detail: it is the
 * lowest heart rate the device can see at all, every plausibility band
 * downstream has to be at or below it to avoid discarding believed readings,
 * and it is the single number that decides whether bradycardia is visible.
 *
 * Overridable so the host tests can sweep it (tests/host). The firmware value
 * is the one below and is chosen from those measurements -- see ARCHITECTURE.md
 * §4.2.
 */
#ifndef VITALS_BPM_MIN
#define VITALS_BPM_MIN 30
#endif
#define VITALS_BPM_MAX 220

/* Where the high-pass corner sits, quoted as a rate so it can be compared with
 * the band above.
 *
 * This used to be *implicitly* VITALS_BPM_MIN, because the filter width was
 * derived from the longest lag the correlator searched. Two different jobs had
 * ended up sharing one number: how slow a pulse we are willing to look for, and
 * how slow a wander we are willing to pass. Separating them is what makes a
 * lower floor possible -- the correlator can search down to 30bpm while the
 * filter keeps rejecting breathing as hard as it did at 40.
 */
#ifndef VITALS_HP_CORNER_BPM
#define VITALS_HP_CORNER_BPM 40
#endif

/* Confidence below which a rate is not worth acting on: not stored as a rate,
 * not notified. Lives here rather than inside vitals.c because the decision
 * belongs to whoever consumes a reading -- vitals.c can say how much a window
 * looked like itself one beat later, but only the caller knows what it is about
 * to do with the answer.
 *
 * **Measured separation, on the bench.** Real signals sit at 525-960 even on a
 * weak, drifting ring-grade pulse, while windows with no pulse in them at all
 * -- empty air, motion artefacts -- top out around 360. The observed values are
 * 159/318/367/425 for noise and 550/585/622/796 for signal, with nothing
 * between 425 and 550.
 *
 * **That gap does not exist on a finger, which is why the bar is 450 and not
 * 500.** Worn captures put a dense cluster of refusals immediately under 500
 * -- 498, 498, 497, 490, 486, 486, 482, 480 -- against an accepted minimum of
 * 502. The distribution is continuous across the line rather than the bimodal
 * one the bench measured, so a bar at 500 sits in the middle of the data
 * instead of in a gap.
 *
 * The near-misses were checked against their neighbours before the bar moved:
 * every window refused at 400-499 with an accepted window within 20s, compared
 * against it.
 *
 *   confidence 450-499   10 windows,  7 testable,  7 agree within 5 bpm
 *   confidence 400-449    7 windows,  6 testable,  6 agree within 5 bpm
 *
 * **13 of 13, mean error 1.4 bpm.** Those are the wearer's own rate, measured
 * and thrown away. The untestable four had no accepted neighbour inside 20s,
 * which is a gap in the evidence rather than a disagreement.
 *
 * **Why 450 is safe here.** The separation argument lists motion artefacts
 * among the things a low bar admits, and that is the real risk -- but the
 * observed artefact island came in at confidence 526-637, *above* this gate, so
 * this number never caught them. WINDOW_MOVE_MILLI_G does, on the median. This
 * gate guards only against noise, whose measured ceiling is 360, and 450 keeps
 * 90 counts of margin over it.
 *
 * **400 is defensible on the same evidence and is deliberately not taken.** The
 * 400-449 band passed the neighbour test 6 for 6, but 400 leaves only 40 counts
 * over the noise ceiling.
 *
 * Overridable so the host tests can sweep it (`make CONF=450 vitals`). The
 * bench cost of every value between 360 and 500 is identical, because the
 * synthetic suite has no windows in the gap this note is about -- that is the
 * measurement's limit, not a clean bill of health.
 */
#ifndef VITALS_BPM_CONFIDENCE_MIN
#define VITALS_BPM_CONFIDENCE_MIN 450
#endif

/* Everything a window yields. Fields are only meaningful when the flag above
 * them is set: no contact means no vitals, and either vital can come back
 * unavailable on its own.
 */
struct vitals {
    bool contact;             /* is the ring actually against skin */
    uint16_t bpm;             /* heart rate, 0 if not determined */
    /* Why bpm is 0: a pulse was found and its period is longer than BPM_MIN
     * allows, rather than nothing periodic being there at all. `confidence` is
     * that pulse's, so a caller can tell a strong slow pulse from noise.
     *
     * This exists because the alternative to reporting it is reporting nothing,
     * and the two failures look identical in a log: a wearer whose heart rate
     * has dropped below the floor and a ring that has stopped reading. The one
     * thing this must never be is a rate -- see the band-edge note in vitals.c.
     */
    bool bpm_below_floor;
    uint16_t confidence;      /* pulse periodicity, 0-1000 */
    uint16_t spo2_tenths;     /* SpO2 x 10 (976 = 97.6%), 0 if not determined */
    uint16_t perfusion_milli; /* pulse AC as parts-per-thousand of DC */
    uint32_t ir_dc;           /* IR DC over the newest half-second, for diagnostics */
};

/* Below this IR DC there is nothing in front of the LEDs. Coming back on needs
 * a clearly better level than staying on does, or a DC sitting on the threshold
 * chatters between worn and not once per poll.
 *
 * Public because a caller that refuses to measure on the strength of these
 * needs to be able to say so with the numbers attached -- "not worn" on its own
 * is indistinguishable from "threshold set wrong", and these are still the
 * bench guesses they started as, never checked against a finger on this ring's
 * optics.
 */
#define VITALS_CONTACT_IR_DC_MIN 30000u
#define VITALS_CONTACT_IR_DC_ON  (VITALS_CONTACT_IR_DC_MIN + VITALS_CONTACT_IR_DC_MIN / 4)

/* Mean IR count over a batch: what the contact decision is actually made on,
 * exposed so a refusal can report it.
 */
uint32_t vitals_ir_dc(const struct ppg_sample *samples, size_t n);

/* Is the sensor against skin, judged on this batch of samples alone?
 *
 * Cheap enough to call on every FIFO drain, which is the point: vitals_compute()
 * only runs once a window, and a ring that has been taken off should say so in
 * one poll interval rather than at the next report.
 *
 * Stateless like the rest of this file, so the caller owns the answer and
 * passes it back in as `worn`. That is not a formality -- the threshold is
 * hysteretic, and which side applies depends on what we currently believe.
 */
bool vitals_contact(const struct ppg_sample *samples, size_t n, bool worn);

/* Derives heart rate and SpO2 from one window of raw samples. Both come out of
 * the same filtered signal, which is why this is one call and not two.
 * Returns 0 with *out populated, or a negative errno.
 */
int vitals_compute(const struct ppg_sample *samples, size_t n, struct vitals *out);

#endif
