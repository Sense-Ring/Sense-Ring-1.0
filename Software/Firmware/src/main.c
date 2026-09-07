/* Boot and the measurement cycle. See ARCHITECTURE.md §5 for the duty-cycle
 * budget and the degraded-mode reasoning.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include "battery.h"
#include "ble.h"
#include "flash_store.h"
#include "imu.h"
#include "ppg.h"
#include "vitals.h"
#include "wallclock.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define PPG_POLL_INTERVAL_MS 200

/* ==========================================================================
 * Bench switches.
 *
 * These modify the real firmware rather than replacing it, and that is the
 * whole point. 
 * ========================================================================== */

/* Shorten the pause between bursts so the pipeline turns over in seconds.
 *
 * **This switch alone moves nothing else.** PREPARE and REPORT are the
 * measurement itself and keep their shipping lengths, so the readings are the
 * real ones and only their spacing changes. The pause is the one phase that
 * trades against nothing but battery (see PAUSE_MS below), which is exactly what
 * makes it safe to compress here and pointless to compress in a shipping
 * build.
 *
 * BENCH_SYNTHETIC_VITALS *does* also compress the burst, because with fabricated
 * readings there is no measurement left to distort -- see REPORT_MS below.
 */
#define BENCH_FAST_CYCLE 0

/* Feed the pipeline fabricated readings instead of the optics.
 *
 * No LEDs, no finger, no contact gate: the PPG is never started. What does run
 * is everything downstream of vitals_compute() -- the confidence gate, the
 * flash append, the refusal path and the BLE notification. That is the half of
 * this firmware a bench test cannot otherwise reach without somebody wearing
 * the ring for hours, and it is the half that buffer-and-flush actually
 * consists of.
 */
#define BENCH_SYNTHETIC_VITALS 0

#if BENCH_SYNTHETIC_VITALS
/* A plausible resting rate, swung slowly, so a console full of fabricated
 * readings still looks like something a wearer could have produced. The range
 * matters only for readability: nothing downstream judges the number.
 */
#define BENCH_SWEEP_MIN_BPM  58
#define BENCH_SWEEP_MAX_BPM  82
#define BENCH_SWEEP_STEP_BPM 4
#endif

#define BENCH_ANY (BENCH_FAST_CYCLE || BENCH_SYNTHETIC_VITALS)

/* ==========================================================================
 * Live streaming
 *
 * With this at 0 the radio carries **nothing per reading**. Data leaves the ring
 * in page-sized batches: the log fills a 4KB page, main.c nudges, the phone asks
 * for the replay, and the phone acknowledges it. One page is 256
 * records, about 1h43m at the shipping cadence.
 *
 * Turn it on for bench work and for anything that wants to watch readings
 * arrive in real time. The app's live tiles are driven by this stream and by
 * nothing else -- replayed records are charted and stored but deliberately do
 * not move the live tiles, because they are history rather than the wearer's
 * current state.
 * ========================================================================== */
#define BLE_LIVE_STREAM 0

/* One measurement cycle, in phases.
 *
 *   PREPARE  fill the window; nothing can be judged until it is full
 *   REPORT   a package every RECORD_INTERVAL_MS, each from the newest window
 *   PAUSE    LEDs down
 *
 * PREPARE is not idle time -- it is the window itself. vitals_compute() reads a
 * whole VITALS_WINDOW_SEC of signal, so the first package of a cycle cannot
 * exist until that much has been collected, and the assert says so.
 */
#define PREPARE_MS 15000

/* **The burst is compressed only when the readings are fabricated**, and the
 * distinction is the whole safety of doing it at all.
 *
 * With real optics these two numbers are not pacing, they are the measurement:
 * a package is computed from a 15s window of signal, and RECORD_INTERVAL_MS is
 * how far that window slides between packages. Shortening it there would not
 * produce packages faster, it would produce the *same* window reported more
 * often -- five near-identical readings that look like five measurements.
 *
 * Under BENCH_SYNTHETIC_VITALS there is no window and no signal; record_burst()
 * invents each reading, so the interval is pure spacing and can be anything.
 * Making it 100ms turns a cycle from 18s into 3s and a full 4KB page from ~15.5
 * minutes into ~2.6, which is the difference between watching the page nudge
 * happen and waiting for it.
 */
#if BENCH_SYNTHETIC_VITALS
#define REPORT_MS          500
#define RECORD_INTERVAL_MS 100
#else
#define REPORT_MS          15000
#define RECORD_INTERVAL_MS 3000
#endif

/* ]The pause is the only knob that trades battery against nothing
 * else at all: the burst above still produces its five packages, they just
 * arrive every two minutes instead of every one. 90s is 25% duty.
 */
#if BENCH_SYNTHETIC_VITALS
#define PAUSE_MS 2500
#elif BENCH_FAST_CYCLE
#define PAUSE_MS 3000
#else
#define PAUSE_MS 90000
#endif

BUILD_ASSERT(PREPARE_MS >= VITALS_WINDOW_MS,
             "prepare phase shorter than the window: the first package would be computed "
             "from a partly-filled window");
BUILD_ASSERT(REPORT_MS % RECORD_INTERVAL_MS == 0, "report phase is not a whole number of packages");

/* Successive packages inside a burst come from windows that overlap by
 * VITALS_WINDOW_SEC - RECORD_INTERVAL_MS. That is deliberate and worth being
 * explicit about: they are five views of a sliding 15s window, not five
 * independent measurements, so five agreeing packages are weaker evidence than
 * five independent ones would be. Anything reading this log downstream should
 * treat a burst as one measurement seen five times, not as five.
 */
BUILD_ASSERT(RECORD_INTERVAL_MS < VITALS_WINDOW_MS, "packages would have gaps between them");

/* The sliding window and the drain buffer, and everything that fills them, is
 * compiled out under BENCH_SYNTHETIC_VITALS -- that mode never starts the
 * optics, so this would be 3.2KB of RAM held for a sensor it does not read.
 * On a part with 64KB total that is worth reclaiming rather than trusting the
 * linker to notice.
 */
#if !BENCH_SYNTHETIC_VITALS
static struct ppg_sample window[VITALS_WINDOW_SAMPLES];
static size_t window_filled;

static struct ppg_sample batch[32];
#endif

/* Whether the ring is on a finger, as of the last drain. vitals_contact() is
 * stateless and hysteretic, so the belief lives here.
 */
static bool worn;

/* The one place `worn` changes, so the transition can be announced exactly
 * once. A ring coming off a finger is the difference between "no readings
 * because there is nothing to read" and "no readings because something is
 * wrong", and the phone cannot tell those apart on silence alone.
 */
static void set_worn(bool now)
{
    if (now == worn) {
        return;
    }
    worn = now;
    ble_notify_alert(now ? BLE_ALERT_WORN : BLE_ALERT_NOT_WORN);
}

/* Set once battery_init() succeeds; read back by the flush callback below. */
static bool battery_ready;

/* Set once the IMU came up and started. Unlike the PPG the accelerometer is
 * left running for the whole session rather than duty-cycled: in LPM at 50Hz it
 * costs ~15uA against a ~290uA budget (ARCHITECTURE.md §3A.5, §5.1.1), which is
 * far less than the bookkeeping to switch it on and off would be worth. It is
 * also what would let a future wake-on-motion path exist at all.
 */
static bool imu_ready;

/* The median of `n` per-drain movement deltas, in milli-g, or 0 when there are
 * none. Shared by the two places that ask "was the hand moving for most of
 * this?" -- the gate that judges a filled window, and the watch that decides
 * when to start one -- so the two cannot answer the same question differently.
 *
 * Computed by bisecting the value space rather than by sorting a copy: 16
 * passes over 75 uint16s beats 150 bytes of stack and a qsort on a device that
 * has caught a stack overflow before. The caller's array is not disturbed.
 *
 * On an even count this returns the *upper* of the two middle values, which
 * errs toward refusing. That is the same direction the threshold itself errs
 * in.
 */
static uint16_t median_milli_g(const uint16_t *slot, size_t n)
{
    uint32_t lo = 0;
    uint32_t hi = UINT16_MAX;

    if (n == 0) {
        return 0;
    }

    /* The smallest value v for which more than half the samples sit at or
     * below v. Invariant: the answer is always within [lo, hi].
     */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        size_t at_or_below = 0;

        for (size_t i = 0; i < n; i++) {
            if (slot[i] <= mid) {
                at_or_below++;
            }
        }

        if (at_or_below > n / 2u) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }

    return (uint16_t)lo;
}

#if !BENCH_SYNTHETIC_VITALS
/* ==========================================================================
 * How much the hand moved while the window was filling
 *
 * **Why this exists: without it a worn session comes back bimodal.** In one
 * measured session, 88 of 102 accepted readings formed a smooth trace at
 * 71-83bpm and 14 sat in an island at 52-59 -- and the islands are not slow
 * stretches of the same trace, they are discontinuities. Two readings one PAUSE
 * apart gave 57 and 82, with the step counter frozen either side. Some of the
 * islands also arrived with perfusion at 0.9-1.1% against 0.2-0.3% for the
 * whole rest of the session: a pulse does not get five times stronger for four
 * windows and then return, but a hand moving does exactly that to the AC
 * term.
 *
 * **Nothing in the signal path could reject any of it, and that is structural
 * rather than a tuning miss.** `estimate_bpm()` measures periodicity, and
 * rhythmic hand movement is periodic -- every one of those windows cleared
 * VITALS_BPM_CONFIDENCE_MIN, at 526 to 637. The high-pass cannot help either:
 * VITALS_HP_CORNER_BPM is 40, so the filter is deliberately flat across
 * 0.7-1.5Hz, which is 42-90bpm -- where both hand movement and ordinary resting
 * rates live. There is no filter setting that separates them, because in the
 * optics they are the same signal.
 *
 * So the evidence has to come from a different part. The accelerometer already
 * runs continuously (see imu_ready) and `wait_until_worth_probing()` already
 * uses the Manhattan delta between consecutive reads as its movement measure;
 * this is that same quantity, accumulated across the window the optics filled
 * rather than sampled once per package.
 *
 * Sampled in drain_once(), which is the only thing that paces a window, so the
 * movement history and the sample history advance together by construction.
 * ========================================================================== */

/* One slot per drain, covering a whole VITALS_WINDOW_MS at PPG_POLL_INTERVAL_MS
 * each. Rounded up, so the movement history is never shorter than the sample
 * history it has to explain.
 */
#define MOVE_SLOTS ((VITALS_WINDOW_MS + PPG_POLL_INTERVAL_MS - 1) / PPG_POLL_INTERVAL_MS)

static uint16_t move_slot[MOVE_SLOTS]; /* Manhattan delta, milli-g, per drain */
static size_t move_next;               /* where the next delta lands */
static size_t move_filled;             /* slots written since the window reset */
static struct imu_sample move_prev;    /* the reading the next delta is against */
static bool move_have_prev;

static void movement_reset(void)
{
    move_next = 0;
    move_filled = 0;
    move_have_prev = false;
}

/* One drain's worth of movement. Silent on every failure -- a bus glitch or an
 * -EAGAIN is not evidence that the hand was still, and recording a zero for it
 * would be exactly the wrong way to be wrong: it would dilute the mean below
 * and make a contaminated window look usable. A missed read simply does not
 * appear in the history, and the mean is over what was actually measured.
 */
static void movement_sample(void)
{
    struct imu_sample now;
    uint32_t delta;

    if (!imu_ready || imu_read(&now) != 0) {
        return;
    }

    if (move_have_prev) {
        delta = (uint32_t)(abs(now.x - move_prev.x) + abs(now.y - move_prev.y) +
                           abs(now.z - move_prev.z));

        move_slot[move_next] = (uint16_t)MIN(delta, UINT16_MAX);
        move_next = (move_next + 1u) % ARRAY_SIZE(move_slot);
        if (move_filled < ARRAY_SIZE(move_slot)) {
            move_filled++;
        }
    }

    move_prev = now;
    move_have_prev = true;
}

/* Mean Manhattan delta over the window, in milli-g, or 0 when nothing was
 * measured. Zero is also what a perfectly still ring reads, and the two are
 * deliberately not distinguished: both mean "no evidence of movement", and the
 * gate below is only ever asked whether there is evidence *of* movement. An
 * IMU that never came up therefore never refuses a window, which is the same
 * degraded-not-broken posture every optional subsystem here takes.
 *
 * A mean rather than a peak. The failure being caught is sustained rhythmic
 * movement -- that is what an autocorrelator mistakes for a pulse -- and a
 * single jolt in an otherwise still 15 seconds leaves 14 usable seconds that
 * the correlator will weight far more heavily than the jolt. A peak would
 * refuse that window; the mean lets it through, which is the right answer.
 */
static uint16_t window_movement_milli_g(void)
{
    uint32_t sum = 0;

    if (move_filled == 0) {
        return 0;
    }

    for (size_t i = 0; i < move_filled; i++) {
        sum += move_slot[i];
    }

    return (uint16_t)(sum / move_filled);
}

/* Median Manhattan delta over the window, in milli-g, or 0 when nothing was
 * measured. **This is the figure the gate below judges, and the mean above is
 * the figure the record carries** -- see WINDOW_MOVE_MILLI_G for why the two
 * are deliberately different quantities.
 *
 * The mean's own comment argues for a mean over a peak on the grounds that "a
 * single jolt in an otherwise still 15 seconds leaves 14 usable seconds". That
 * goal is right and a mean is the wrong instrument for it: one gesture of
 * 900 mg across 75 drains adds 12 mg to the mean, and four of them carry an
 * otherwise still window over a 100 mg line. A median asks the question the
 * comment actually poses -- was the hand moving for *most* of the window -- and
 * it answers "no" for a hand that gestured and "yes" for a hand that is
 * walking, which is the separation the gate exists to make.
 *
 * The arithmetic is median_milli_g() above, which the stillness watch in
 * pause_until_still() shares: move_slot[] is not disturbed, so the mean above
 * can be taken before or after this in any order, and on an even count the
 * *upper* of the two middle values is returned -- erring toward refusing, the
 * same direction the threshold errs in. At 75 slots that case arises only when
 * a drain was missed.
 */
static uint16_t window_movement_median_milli_g(void)
{
    return median_milli_g(move_slot, move_filled);
}
#else
/* No optics on the synthetic path, so no window, no drains and no movement
 * history to go with them. Reporting "still" keeps the gate out of the way of a
 * bench run whose whole purpose is to exercise the accepting path -- the same
 * reason that path forces `worn` true.
 */
static inline uint16_t window_movement_milli_g(void)
{
    return 0;
}

static inline uint16_t window_movement_median_milli_g(void)
{
    return 0;
}
#endif

/* Mean Manhattan delta per drain, in milli-g, above which the window is thrown
 * away rather than measured.
 *
 * **This is a measured value, not a guessed one.** A ring lying on a desk reads
 * 2-5 mg, but a worn hand is a different object entirely. Console capture on
 * hardware gives three cleanly separated populations:
 *
 *   hand deliberately still          36-45 mg
 *   stationary, hands in use        122-217 mg
 *   walking                         632-938 mg
 *
 * 100 mg is 2.2x the top of the still band and well under the bottom of the
 * next one, so it clears the noise floor without reaching into anything that is
 * merely worn. A threshold at 40 -- inside the still band -- costs real
 * readings: a window at 41 mg with confidence 620, well clear of
 * VITALS_BPM_CONFIDENCE_MIN, refused by one milli-g.
 *
 * **One threshold is enough for both artefacts.** Walking and hands-in-use look
 * like they might need separate treatment, and the step counter is already in
 * the record and needs no calibration -- but both populations sit far above
 * 100, so the milli-g test subsumes a step test and a second rule would only
 * add a way to be wrong.
 *
 * **The comparison is against the *median* per-drain delta, not the mean**, and
 * that matters more than the number does. At PPG_POLL_INTERVAL_MS a window is
 * 75 drains. A hand still at 40 mg needs only **six contaminated drains -- 1.2
 * seconds out of fifteen** -- to carry the mean over 100, because one 900 mg
 * gesture adds 11 mg to it. So a mean asks for fifteen continuous seconds of
 * near-stillness, and no threshold in the still band survives that: raising the
 * line to tolerate six gestures also clears sustained motion, which is the
 * artefact. The median asks a different question -- was the hand moving for
 * *most* of the window -- so a gesturing hand passes and a walking hand does
 * not, at the same 100 mg. See window_movement_median_milli_g() for the
 * argument, and record_vitals() for why the record still carries the mean.
 *
 * ⚠️ **This no longer refuses a window on its own.** record_vitals() consults
 * it only for windows the optics had already given up on; a moving window whose
 * pulse cleared VITALS_BPM_CONFIDENCE_MIN is stored as a reading, with its
 * movement bucket attached. Two things follow.
 *
 * **The asymmetry that justified an aggressive threshold now points the other
 * way.** The case for erring aggressive was that a false refusal costs one
 * window while a false acceptance files a movement artefact as a heart rate,
 * and the two are not comparable. That case is sound, and this ring now errs
 * the other way at exactly the confidences where the observed artefacts lived
 * (526-637, *above* the bar). What holds the line is the confidence bar itself,
 * and vitals.h is explicit that the bar never caught them.
 *
 * **What bought the relaxation was reading the refusals rather than counting
 * them.** Every capture carrying both a movement figure and a confidence -- 26
 * MOVING windows -- checked against the accept bar:
 *
 *   100-149 mg   n=3   max confidence 400
 *   150-249 mg   n=7   max confidence 390
 *   250-499 mg   n=5   max confidence 290
 *   500-999 mg   n=9   max confidence 340
 *   >= 1000 mg   n=2   max confidence 230
 *
 * **None reached VITALS_BPM_CONFIDENCE_MIN.** So raising this number to 250 --
 * the obvious relaxation, and the one the `stationary, hands in use` band
 * argues for -- buys zero readings out of those windows; it relabels them
 * `pulse too weak`. The threshold is not what the yield is spent on. (n=26 from
 * one session, and that session was the movement calibration, so the motion in
 * it was deliberate. Suggestive rather than a field distribution.)
 *
 * **Refusals arrive in runs, which is what makes a false refusal expensive.**
 * "The next window undoes it" assumes an isolated event. An all-day worn
 * capture accepted **27 windows out of 462 -- 5.8%** -- with whole transfers at
 * 0%, 3%, 6% and 7% yield, against 44% in one transfer taken sitting still. The
 * next window is refused for the same reason, because the hand is still
 * attached to the same person.
 *
 * Every record carries its movement bucket (flash_store.h), accepted records
 * included, so this gate can be reapplied or reargued from the log alone: the
 * clause in record_vitals() is what would put it back.
 */
#define WINDOW_MOVE_MILLI_G 100

/* Last step total successfully read from the IMU. The hardware counter is
 * free-running and never reset by this firmware (imu.h), so this only ever
 * grows -- it exists so a failed read repeats the previous total instead of
 * reporting zero and making a monotonic counter appear to jump backwards.
 */
static uint32_t last_steps;

/* The step total as of the last record written. Separate from last_steps
 * because the two answer different questions: last_steps is "the best total we
 * know", refreshed whenever a read succeeds, while this one only moves when a
 * record actually consumes a delta. Folding them together would drop the steps
 * taken during any window that produced no record.
 */
static uint32_t steps_at_last_record;

/* Steps since the previous record, saturating at what the record can hold.
 *
 * The flash record stores a delta rather than the free-running total (see
 * flash_store.c), and this is where the subtraction happens -- here, while both
 * totals are still in hand, rather than downstream by differencing adjacent
 * records that a transfer may have separated or an erase may have discarded.
 *
 * Two things it has to survive. The counter is 24-bit and free-running, so it
 * wraps eventually; an unsigned subtraction gives the right answer across one
 * wrap and a nonsense one across a reset, and 65535 is a less misleading
 * nonsense than a near-4-billion delta. A ceiling of 65535 is also far above
 * anything real -- a sprinter manages a few hundred steps in a 121s cycle.
 */
static uint16_t step_delta(void)
{
    uint32_t delta = last_steps - steps_at_last_record;

    steps_at_last_record = last_steps;
    return (delta > UINT16_MAX) ? UINT16_MAX : (uint16_t)delta;
}

/* Battery level for the flash-log flush. Returns cell mV, or a negative errno
 * when the monitor never came up. Handed to flash_store_init().
 */
static int flush_battery_mv(void)
{
    if (!battery_ready) {
        return -ENODEV;
    }
    return battery_read_mv();
}

/* The free-running step total, read live. Handed to flash_store_init() for the
 * flush's battery package, and used by power_check() for the status packet --
 * both want the same thing, so neither gets its own copy of it.
 *
 * Reads the counter rather than returning `last_steps`, because the two are not
 * the same claim: last_steps is the total as of the last measurement window,
 * and neither caller here runs on that cadence. Deliberately does not refresh
 * last_steps either -- that variable is paired with steps_at_last_record to
 * produce the per-record deltas, and a second writer on an unrelated cadence
 * would move steps between records for no reason. This is a pure read; the
 * delta path is untouched by it.
 */
static int step_total(void)
{
    uint32_t steps;
    int rc;

    if (!imu_ready) {
        return -ENODEV;
    }

    rc = imu_read_steps(&steps);
    if (rc != 0) {
        return rc;
    }

    /* The counter is 24-bit, so this cannot overflow the int the callback
     * returns and cannot collide with the negative-errno convention.
     */
    return (int)steps;
}

/* Defined further down, past the collection helpers that BENCH_SYNTHETIC_VITALS
 * compiles out. Declared above them because it is the one thing on this side of
 * the split that both paths call.
 */
static void record_vitals(const struct vitals *v);

/* Everything from here to record_one() is how a reading is *obtained* from the
 * optics: the sliding window, the FIFO drains, the contact probe. None of it
 * has a caller under BENCH_SYNTHETIC_VITALS, which fabricates readings instead,
 * so it is compiled out there rather than left for the linker to strip -- an
 * unreferenced function is a warning, and warnings that are expected are
 * warnings nobody reads.
 */
#if !BENCH_SYNTHETIC_VITALS

static void window_reset(void)
{
    window_filled = 0;
    /* The movement history describes the samples, so it is only ever as valid
     * as they are. Discarding one without the other would let a window that
     * started after a removal be judged on movement from before it.
     */
    movement_reset();
}

/* Appends a drained batch, dropping as much of the front as it has to. Linear
 * and memmove'd rather than a ring buffer because vitals_compute() wants one
 * contiguous run: a ring would need linearising into a second 3KB buffer.
 */
static void window_append(const struct ppg_sample *src, size_t n)
{
    if (n > ARRAY_SIZE(window)) {
        src += n - ARRAY_SIZE(window);
        n = ARRAY_SIZE(window);
    }

    if (window_filled + n > ARRAY_SIZE(window)) {
        size_t drop = window_filled + n - ARRAY_SIZE(window);

        memmove(window, &window[drop], (window_filled - drop) * sizeof(window[0]));
        window_filled -= drop;
    }

    memcpy(&window[window_filled], src, n * sizeof(window[0]));
    window_filled += n;
}

/* One FIFO drain into the sliding window. Returns the sample count, or -1. */
static int drain_once(void)
{
    int n;

    k_msleep(PPG_POLL_INTERVAL_MS);

    /* Before the FIFO read rather than after it, so the accelerometer is
     * sampled on the poll interval and not on the poll interval plus however
     * long the I2C burst took. The two reads share a bus, so that difference is
     * real and it is the sampling jitter of the movement history.
     */
    movement_sample();

    n = ppg_read_fifo(batch, ARRAY_SIZE(batch));
    if (n < 0) {
        LOG_ERR("PPG read failed (%d)", n);
        return -1;
    }

    window_append(batch, (size_t)n);
    return n;
}

/* A settled look at the optics before committing the LEDs to a whole window.
 * Sets `worn`, and on a refusal says what it saw.
 *
 * The judgement is deliberately not made on the first drain. Doing that turned
 * every cycle into a ~200ms flicker of the LEDs and back to sleep, which is
 * both invisible on a bench and undiagnosable: the refusal path is silent when
 * the ring was already off, so the console showed nothing but an empty dump
 * every couple of minutes. A second of signal is enough for a stable decision
 * and long enough to see the red LED light, which is the cheapest possible
 * confirmation that the rail, the load switch and the part all work.
 *
 * Reporting the DC is the point of the whole function. "Not worn" alone cannot
 * be told apart from "threshold set wrong", and VITALS_CONTACT_IR_DC_ON is
 * still a bench guess that has never been checked against a finger on these
 * optics -- so it gates whether this device measures at all while being the
 * least verified number in it. Printing the measured level beside the required
 * one is what makes that fixable rather than mysterious.
 */
#define CONTACT_PROBE_MS 1000

static bool probe_contact(void)
{
    const int64_t until = k_uptime_get() + CONTACT_PROBE_MS;
    uint32_t dc;

    window_reset();

    while (k_uptime_get() < until) {
        if (drain_once() < 0) {
            return false;
        }
    }

    if (window_filled == 0) {
        LOG_WRN("PPG delivered no samples -- sensor stalled");
        return false;
    }

    dc = vitals_ir_dc(window, window_filled);

    if (!vitals_contact(window, window_filled, worn)) {
        if (worn) {
            LOG_INF("Ring removed (IR DC %u)", dc);
        } else {
            LOG_INF("Not worn (IR DC %u, needs %u)", dc, VITALS_CONTACT_IR_DC_ON);
        }
        set_worn(false);
        window_reset();
        return false;
    }

    if (!worn) {
        LOG_INF("Ring on (IR DC %u) -- preparing", dc);
        set_worn(true);
    }

    return true;
}

/* Drains the FIFO for `ms`, keeping the sliding window and the contact belief
 * up to date. Returns false if the ring came off, in which case the window has
 * been discarded and there is nothing to measure.
 */
static bool collect_for(int64_t ms)
{
    const int64_t until = k_uptime_get() + ms;

    while (k_uptime_get() < until) {
        int n = drain_once();

        if (n < 0) {
            return false;
        }

        /* Contact is judged per drain, not per package: a ring that comes off
         * mid-burst must not have the rest of the burst measured through it.
         * A drain that yielded nothing carries no evidence either way, and
         * vitals_contact() returns the existing belief for it.
         */
        if (!vitals_contact(batch, (size_t)n, worn)) {
            LOG_INF("Ring removed (IR DC %u)", vitals_ir_dc(batch, (size_t)n));
            set_worn(false);
            window_reset();
            return false;
        }
    }

    return true;
}

/* One package: measure the current window, and -- if the reading is one this
 * firmware is prepared to vouch for -- store it, send it, and decide whether
 * the rate belongs to this wearer. Returns true if it did not.
 */
static void record_one(void)
{
    struct vitals v;
    int rc;

    /* Both of these earn a line for the same reason the refusal below does:
     * they end the window with nothing stored and nothing sent, which from the
     * outside is indistinguishable from a refusal, from an unworn ring, and
     * from a ring that has stopped working. This path used to be the silent
     * one -- the only gap with no console line at all -- so a short window and
     * a broken part looked identical even with a debugger attached.
     */
    if (window_filled < VITALS_MIN_SAMPLES) {
        LOG_INF("Window too short to measure (%u of %u samples)", (unsigned)window_filled,
                (unsigned)VITALS_MIN_SAMPLES);
        return;
    }

    rc = vitals_compute(window, window_filled, &v);
    if (rc != 0) {
        /* vitals_compute() only rejects on its argument contract, so this is a
         * bug in this file rather than a wearer or an optics problem -- hence
         * a warning where the others are informational.
         */
        LOG_WRN("vitals_compute failed (%d) on %u samples", rc, (unsigned)window_filled);
        return;
    }

    record_vitals(&v);
}

#endif /* !BENCH_SYNTHETIC_VITALS -- end of the optics-side collection path */

/* Everything a package does once a reading exists: timestamp it, attach the
 * motion that explains it, gate it, store it and send it.
 *
 * Split from record_one() so that a reading's *origin* and its *handling* are
 * separable. Nothing in here cares where the numbers came from, and that is
 * what lets BENCH_SYNTHETIC_VITALS drive the real storage and transport path
 * with fabricated input instead of a second copy of it that could drift.
 */
static void record_vitals(const struct vitals *v)
{
    struct flash_sample sample;
    uint32_t ts;
    uint16_t moved;
    uint16_t moved_median;

    /* Virtual, so a log kept across a reset stays ordered and stays on one
     * anchor -- see wallclock.h. Identical to k_uptime_get() on a cold boot.
     */
    ts = wallclock_uptime();

    /* Acceleration rides the same timestamp as the vitals package, so a client
     * receiving both can line them up without guessing. Sampled here rather
     * than on its own timer for exactly that reason -- the point of the IMU on
     * this device is context for a heart rate -- 120bpm from climbing stairs and
     * 120bpm at rest are the same number without it -- and context that is not
     * aligned to the reading it explains is worth much less.
     *
     * Deliberately *above* the confidence gate below, and not moved down with
     * the vitals package: movement is measured by a different part and is no
     * less true because the pulse was unreadable. Gating it on the PPG would
     * take the motion stream down for every weak-perfusion stretch -- and a
     * window with no believable pulse is exactly when knowing whether the
     * wearer was moving is worth most. So the two packages arrive as a pair
     * when the pulse is good, and motion arrives alone when it is not.
     *
     * The running total is what goes on the wire, and a per-record *delta* is
     * what goes to flash -- see the step_delta() call below and the note on
     * `steps` in flash_store.c for why the same number is right in one place
     * and wrong in the other.
     */
    if (imu_ready) {
        struct imu_sample motion;
        uint32_t steps;
        int rc;

        /* Read the counter first, and independently of the accelerometer.
         *
         * This used to sit inside the `rc == 0` branch below, which tied it to
         * the accelerometer having a *fresh conversion* -- and the two have
         * nothing to do with each other. The step count is maintained by the
         * feature engine and read from its own registers (0x57-0x59); it is
         * current whether or not a new sample has landed since the last look.
         * Gating it on drdy meant every -EAGAIN left the total stale, which
         * pushed the steps taken in that window onto whichever later record
         * happened to catch a good read. No steps were lost -- last_steps is a
         * total and step_delta() differences it -- but they were attributed to
         * the wrong window, which is exactly what makes a step trace look
         * lumpy and wrong next to a walk somebody counted.
         *
         * On a read error keep the last total rather than sending 0. The count
         * is monotonic by contract (imu.h), and a consumer computing a rate by
         * differencing would read a transient glitch as the wearer having
         * walked backwards -- or, worse, as a counter reset followed by a huge
         * positive jump on the next package.
         */
        if (imu_read_steps(&steps) == 0) {
            last_steps = steps;
        }

        rc = imu_read(&motion);
        if (rc == 0) {
            /* The step count is kept either way -- step_delta() puts it in the
             * record below, which is the path that survives BLE_LIVE_STREAM
             * being off. Only the axes depend on this notification, and only
             * the axes are lost when it is compiled out.
             */
#if BLE_LIVE_STREAM
            ble_notify_motion(ts, motion.x, motion.y, motion.z, last_steps);
#else
            ARG_UNUSED(ts);
#endif
        } else if (rc != -EAGAIN) {
            /* -EAGAIN just means no conversion has landed yet; anything else
             * is the part or the bus, and is worth one line rather than a
             * silent gap in the motion stream.
             */
            LOG_WRN("IMU read failed (%d)", rc);
        }
    }

    /* Everything the window measured, in the one shape flash and BLE both take.
     * Built before the gate because both sides of the gate need it -- a refusal
     * is stored with the same evidence an accepted reading is, and that is what
     * makes the two comparable to whoever reads them later.
     */
    /* Read here rather than at the gate below, because movement is evidence
     * before it is a verdict. The record carries it either way -- see the
     * movement bucket in flash_store.h -- and a `moving` refusal that cannot
     * say how far over the threshold it landed is the gap that was worth
     * closing.
     */
    moved = window_movement_milli_g();

    /* Two figures from one history, and they are not interchangeable. The mean
     * goes in the record because the bucket boundaries in flash_store.h are
     * calibrated against means -- 36-45, 122-217, 632-938 are all means from
     * hardware capture -- so a stored bucket stays comparable to that
     * calibration and to every record ever written. The median is what the gate
     * judges, because "was the hand moving for most of this window" is the
     * question and the mean answers a different one.
     *
     * Keeping them separate is what makes the gate measurable from its own
     * output: recording the mean and gating on the median moves only the gate,
     * so the accepted/refused split *per stored bucket* reads out exactly what
     * judging on the median buys, band by band.
     */
    moved_median = window_movement_median_milli_g();

    sample.timestamp_ms = ts;
    sample.bpm = v->bpm;
    sample.spo2_tenths = v->spo2_tenths;
    sample.confidence = v->confidence;
    sample.perfusion_milli = v->perfusion_milli;
    sample.steps = step_delta();
    sample.contact = v->contact;
    sample.movement_milli_g = moved;

    /* Only a rate we believe is reported as a rate. A wrong rate stored as a
     * measurement is worse than no rate at all: it cannot be told apart from a
     * real one by anything downstream, and the whole point of the confidence
     * figure is that this file is the last place that can act on it.
     *
     * This check used to sit *below* the append and the notify, so the log and
     * the phone carried rates nothing had vouched for. Moving the two calls
     * below it is the whole of that fix.
     *
     * What *is* stored on this path is the refusal itself: the reading is not
     * promoted to a vital sign, but the fact that the ring looked and found
     * nothing believable is real data and used to be thrown away. Storing it
     * with the reason attached is not a reversal of the gate -- the record is
     * flagged refused, so nothing downstream can read it as a pulse -- it is
     * the difference between a log with a hole in it and a log that says why.
     *
     * **Movement is not part of this test.** It is the only condition in the
     * reason chain below whose evidence does not come from the optics -- see
     * the movement block above imu_ready -- and it names windows that were
     * moving *and* had nothing believable to keep, rather than disqualifying a
     * window on its own. A window whose pulse cleared
     * VITALS_BPM_CONFIDENCE_MIN is kept even if the hand was moving for most of
     * it.
     *
     * ⚠️ **The population that admits is measured, and it is the artefact
     * island.** The note on the bar in vitals.h records movement artefacts at
     * confidence 526-637 -- above this bar, and caught only by the movement
     * test that no longer fires on them. The MOVING windows that carry both
     * figures top out at confidence 400 and are unaffected either way. So on
     * the evidence this tree holds, the windows this promotes are the
     * artefacts.
     *
     * **What still separates them downstream.** Every record carries its
     * movement bucket in flags bits 5-7, accepted records included and by
     * design (flash_store.h), so an accepted reading taken at 500-999 mg is
     * distinguishable from one taken at <50 mg by anything that reads the
     * record -- a consumer can reapply the stricter gate exactly.
     */
    if (!v->contact || v->bpm == 0 || v->confidence < VITALS_BPM_CONFIDENCE_MIN) {
        /* One classification, two consumers: the console line and the stored
         * record say the same word, so a reader of either is looking at the
         * same judgement rather than two that can drift apart. "not worn",
         * "moving", "no pulse found", "pulse too weak" and "pulse below the
         * band" are five different things to do something about -- the wearer,
         * the wearer's hand, the placement or optics, the perfusion, and the
         * wearer's heart -- and reconstructing which one fired from five
         * printed values is something a reader gets wrong eventually.
         *
         * **The order of this chain is load-bearing, and "moving" sits second
         * on purpose.** Contact is first because nothing else means anything
         * without it. Movement is next because it describes the window better
         * than anything below it can: whatever the correlator returned came
         * out of a signal the hand was contaminating, so filing that answer
         * under any of the three reasons below would be an assertion about the
         * optics or about the wearer's heart made on evidence that supports
         * neither. **Above TOO_SLOW specifically.** That reason is the one
         * refusal here that is a finding about the wearer, and a movement
         * artefact that happens to land under VITALS_BPM_MIN would otherwise be
         * stored as this ring's bradycardia signal -- the exact silent misfile
         * the band-edge note in vitals.c exists to prevent.
         *
         * **That is the only thing movement still disqualifies.** Reaching
         * this chain at all means the pulse was already unbelievable -- no
         * contact, no rate, or below the bar -- so "moving" does not override a
         * reading, it names why there was none. The one window it takes away
         * from another reason is the moving one that cleared the bar with a
         * rate under VITALS_BPM_MIN, and that is deliberate: a bradycardia
         * claim needs a still hand.
         *
         * The last of them is not really a refusal to measure: the window found
         * a pulse and believed it, and the only thing missing is a number,
         * because the rate is below the band vitals.c will name. It is held to
         * the same confidence bar as a reading for exactly that reason -- it is
         * an assertion about the wearer, so it needs the evidence a reading
         * needs. Below that bar it is just noise, and says "no pulse found".
         */
        bool too_slow = v->bpm_below_floor && v->confidence >= VITALS_BPM_CONFIDENCE_MIN;
        enum flash_refusal why = !v->contact ? FLASH_REFUSAL_NOT_WORN
                                 : moved_median >= WINDOW_MOVE_MILLI_G ? FLASH_REFUSAL_MOVING
                                 : too_slow      ? FLASH_REFUSAL_TOO_SLOW
                                 : v->bpm == 0   ? FLASH_REFUSAL_NO_PULSE
                                                 : FLASH_REFUSAL_WEAK_PULSE;
        /* Indexed by `why`, so it has to cover the reserved gap at 4 as well --
         * see flash_store.h. Bounds-checked below rather than trusted: this
         * array and the enum are two things that have to agree, and the one
         * time they did not it was found in the field.
         */
        static const char *const names[] = {"not worn",
                                            "no pulse found",
                                            "pulse too weak",
                                            "pulse below the band",
                                            "moving (old numbering)",
                                            "moving"};
        const char *name = ((unsigned int)why < ARRAY_SIZE(names)) ? names[why] : "unknown";

        /* Both movement figures, because they disagree by design and a reader
         * who sees only one cannot tell which of them refused the window. The
         * median is the one that judged it; the mean is the one in the record.
         */
        LOG_INF("Package refused: %s (contact %s, HR %u bpm, confidence %u%%, PI %u.%u%%, "
                "movement %u mg median, %u mg mean)",
                name, v->contact ? "yes" : "no", v->bpm, v->confidence / 10,
                v->perfusion_milli / 10, v->perfusion_milli % 10, moved_median, moved);

        flash_store_append_refusal(&sample, why);
        return;
    }

    /* One line when a window is kept on a hand that was moving, because it is
     * the only accepted record whose provenance a console reader cannot see
     * from the numbers. The record carries its movement bucket to the phone
     * either way; RTT carries whatever is printed here and nothing else.
     */
    if (moved_median >= WINDOW_MOVE_MILLI_G) {
        LOG_INF("Package kept while moving: HR %u bpm, confidence %u%%, "
                "movement %u mg median, %u mg mean",
                v->bpm, v->confidence / 10, moved_median, moved);
    }

    /* The flash append is the delivery path, not a backup of one. See
     * BLE_LIVE_STREAM: the reading reaches the phone when its page fills, or
     * when the backlog marks ask for a collection.
     */
    flash_store_append(&sample);
#if BLE_LIVE_STREAM
    ble_notify_vitals(&sample);
#endif
}

/* How full the log is, and whether the phone is draining it.
 *
 * The one number buffer-and-flush lives or dies on. `cursor` is everything ever
 * appended this generation and `head` is the oldest record still held, so the
 * gap between them is what the phone has not acknowledged yet -- and whether
 * that gap grows or holds steady across cycles is the whole question. A ring
 * that measures perfectly and never gets its buffer collected is a ring with no
 * data path, and from the vitals lines alone the two look identical.
 *
 * Compiled out entirely off the bench: the shipping console is meant to be
 * quiet between flushes, and this would put a line on it every cycle forever.
 */
#if BENCH_ANY
static void bench_report_backlog(void)
{
    uint32_t head = flash_store_head();
    uint32_t cursor = flash_store_cursor();
    uint32_t held = cursor - head;

    LOG_INF("Bench backlog: %u record(s) held (%u B), head %u, cursor %u, generation %u",
            held / FLASH_RECORD_SIZE, held, head, cursor, flash_store_generation());
}
#else
static inline void bench_report_backlog(void)
{
}
#endif

/* Ask the phone to collect once the backlog gets deep, and re-arm only when it
 * has come back down. This is what makes "buffer, and flush when full" a
 * complete sentence.
 *
 * On a link that simply stays up nobody else was ever going to start the
 * conversation -- the phone asks for a backfill when it connects, on a nudge,
 * and on a stale cursor, and that is the whole list. Without a nudge of its own
 * a ring filled its buffer with records the phone had already seen live,
 * acknowledged none of them, reclaimed no flash, and eventually lapped and
 * warned about dropping data the phone in fact held. Not urgent -- the buffer
 * is ~4 days deep and the battery is ~3 -- but it is a path that only ever ends
 * by losing something.
 *
 * **Two marks, and the routine one is a page.** A single mark at half the
 * buffer is the wrong unit twice over: it is ~2 days of backlog away, and *any*
 * threshold in between frees nothing, because flash cannot erase less than a
 * page and so neither can an acknowledgement. A collection of 100 records that
 * the phone confirms perfectly reclaims exactly zero bytes. One page -- 256 records, about 1h43m at the shipping cadence -- is the
 * smallest amount of collecting that buys any space at all, and it is therefore
 * the natural rhythm for a link that is up and working.
 *
 * Half the buffer stays as a second, louder mark. It is not the rhythm any more;
 * it is the backstop that says the rhythm has not been working, which is a
 * different event and deserves to read differently on the console.
 *
 * Hysteresis on both, for the same reason the contact gate has it: at a single
 * threshold a backlog sitting on the mark would nudge every cycle forever. The
 * page mark gets its hysteresis for free, because the acknowledgement that
 * answers it moves the head a whole page by construction and so cannot leave the
 * backlog on the mark it just fired at.
 *
 * The backstop is expressed as a fraction of flash_store_capacity() rather than
 * a record count, because a record count is a number that quietly stops meaning
 * what it said the next time the storage partition is resized.
 */
#if BENCH_ANY
/* 50% of 14592 records is ~42 hours away at the shipping cadence, which makes
 * the shipping thresholds a thing you can only reason about rather than watch.
 *
 * Only the backstop is scaled. The page mark is a page on the bench too -- it is
 * a property of the part, not a policy -- and at the fast cycle it arrives in
 * ~2.6 minutes.
 *
 * **10%, not the 2% this used to be.** 2% of capacity is 4669 bytes against a
 * 4096-byte page, so the backstop sat ~36 records behind the page mark: about
 * 18 seconds at the fast cadence, and it would trip on any collection that was
 * merely slow. Since the whole point of a bench run is to watch the page mark
 * keep the backlog down *without* the backstop firing, a backstop that close is
 * measuring the wrong thing. At 10% it is ~15 minutes away, so if it fires the
 * phone genuinely is not collecting.
 */
#define BACKLOG_NUDGE_HIGH_PCT 10
#define BACKLOG_NUDGE_LOW_PCT  5
#else
#define BACKLOG_NUDGE_HIGH_PCT 50
#define BACKLOG_NUDGE_LOW_PCT  25
#endif

BUILD_ASSERT(BACKLOG_NUDGE_LOW_PCT < BACKLOG_NUDGE_HIGH_PCT,
             "backlog nudge marks inverted: it would re-arm the moment it fired");

/* **A third mark, on time rather than volume.**
 *
 * Both marks above are volume marks, and both are sized by what flash can
 * *reclaim*: a page is the smallest acknowledgement that frees any bytes at
 * all, which is why the page is the routine rhythm. That reasoning is about
 * space, and space is not the only thing at stake.
 *
 * The two come apart whenever the ring restarts faster than a page fills. A
 * ring resetting every ~28 minutes against a ~104-minute page never reaches its
 * only routine mark, never nudges, and loses every record to the boot erase --
 * observed with the phone connected and in range throughout, 1637 seconds of
 * continuous connection containing not one transfer.
 *
 * The phone cannot cover this from its side: it asks for a backfill when it
 * connects, on a nudge, and on a stale cursor, and that is the whole list. On a
 * link that simply stays up, nobody starts the conversation.
 *
 * So: if records have been waiting longer than this, ask -- however few they
 * are. It reclaims nothing, because a partial page cannot, and that is fine.
 * **Reclaiming space and not losing records are different jobs, and one trigger
 * cannot do both.**
 *
 * **The cost is connection events, not bytes.** Ten minutes is ~25 records, 400
 * bytes; a page is 4096. Over a day the two policies move the same total, one in
 * ~144 small collections rather than ~14 large ones. The marginal cost is the
 * per-collection overhead, which is why this is minutes and not seconds.
 *
 * **The interval is chosen against how often the ring restarts, not against the
 * battery.** It bounds a reset's cost to the records taken since the last ask,
 * rather than to all of them.
 */
#if BENCH_ANY
#define DELIVERY_MAX_HOLD_MS (2 * 60 * 1000)
#else
#define DELIVERY_MAX_HOLD_MS (200 * 60 * 1000)
#endif

static bool page_nudged;    /* a page of unacknowledged records is waiting */
static bool backlog_nudged; /* the backstop: the buffer is half full */
static uint32_t held_since_head;  /* the head we last saw move */
static int64_t  held_since_ms;    /* when it last moved, or we last asked */

/* ==========================================================================
 * Running out of battery
 *
 * **Measured drain is about 5mA** -- 79% to 63% in one hour -- so roughly six
 * hours a charge on the bench build and somewhere near half a day at the
 * shipping cadence. At that runtime an empty cell is something the wearer meets
 * daily, so it needs behaviour rather than a footnote.
 *
 * Two marks, and they do different things.
 *
 *   LOW      measure less often. The pause is the only knob that trades
 *            battery against nothing but latency (see PAUSE_MS), so stretching
 *            it is the cheapest hour available. The wearer is told, because a
 *            ring quietly sampling a third as often is a ring whose data has
 *            changed meaning without saying so.
 *
 *   CRITICAL stop measuring; keep the radio. The buffer holds up to four days
 *            of readings that exist nowhere else, and the worst possible end to
 *            a wear test is the ring dying with all of them still on it. New
 *            readings are what costs the battery, so they are what stops --
 *            and what is bought with the remaining charge is time for the
 *            backlog to reach the phone.
 *
 * The recovery mark sits well above the low mark on purpose. The gauge is
 * *known* to be noisy off-charger -- the discharge curve runs 5% per 10mV
 * through the middle, and a BLE burst on a 31mAh cell sags it further -- so
 * marks a couple of points apart would flap, and every flap is an alert.
 * ========================================================================== */
#define POWER_LOW_PCT       15u
#define POWER_CRITICAL_PCT   5u
#define POWER_RECOVER_PCT   30u
#define POWER_CRITICAL_SLEEP_MS 60000

BUILD_ASSERT(POWER_CRITICAL_PCT < POWER_LOW_PCT && POWER_LOW_PCT < POWER_RECOVER_PCT,
             "power marks inverted: the ring would oscillate between states");

enum power_state {
    POWER_OK,
    POWER_LOW,
    POWER_CRITICAL,
};

static enum power_state s_power = POWER_OK;

/* How long to pause this cycle. Longer when the cell is low: at the shipping
 * cadence this takes the duty cycle from 30s of optics in 120 to 30 in 300.
 */
static int32_t power_pause_ms(void)
{
    return (s_power == POWER_LOW) ? (PAUSE_MS + 2 * PAUSE_MS) : PAUSE_MS;
}

/* ==========================================================================
 * Telling the phone the cell ran out
 *
 * **The ring cannot report 0%, and no amount of firmware makes it able to.**
 * It stops measuring at POWER_CRITICAL_PCT, spends what is left keeping the
 * radio up for the backlog, and then the rail drops. The instant at which the
 * cell is empty is the instant nothing can transmit, and by the time a phone
 * reaches the ring again somebody has charged it and the gauge reads 60%. A
 * phone that only ever shows what it was told therefore shows the last live
 * reading -- 40%, indefinitely, for a ring that has been dead since Tuesday.
 *
 * So the report has to be made *afterwards*, and it needs two things that only
 * survive in different places.
 *
 *   The mark    -- what the cell said on the way down, in NVS. Written at the
 *                  transition into CRITICAL, which is the last moment the ring
 *                  both knows it is in trouble and still has the charge to do a
 *                  flash write. Disarmed on recovery, so a ring that was
 *                  charged in time never claims to have died.
 *
 *   The cause   -- RESETREAS, read at the top of main(). **A clear register is
 *                  the finding on this part**, not a missing one: the nRF52832
 *                  latches no bit for power-on and none for brownout, so a
 *                  supply that actually went away leaves it at zero. See
 *                  log_reset_cause().
 *
 * Either alone proves nothing. A mark with a *named* reset cause is a ring that
 * was low and then hit the debugger's reset pin, which is not the same event. A
 * clear cause with no mark is somebody plugging in a healthy ring. **Together
 * they say the rail dropped while the cell was already empty, which is as close
 * to "it ran out" as this hardware can get.**
 *
 * The verdict is computed once at boot and then lives in RAM for the rest of
 * it, exactly like the reset cause it is built from and for the same reason:
 * the phone may not connect for hours, and an answer that expired before anyone
 * asked would be no answer. The NVS mark is cleared as soon as it is read, so
 * it can never convict the boot after this one.
 * ========================================================================== */
#define FLAT_MARK_KEY    "sense/flat"

/* Bumped if `struct flat_mark` changes. A mark of the wrong format is ignored
 * rather than reinterpreted -- the failure it would otherwise produce is a
 * fabricated claim that the wearer's ring died, which is worse than silence.
 */
#define FLAT_MARK_FORMAT 1u

struct flat_mark {
    uint32_t format;
    uint64_t epoch_ms;    /* when it was written; 0 if no phone had anchored us */
    uint32_t uptime_ms;   /* virtual uptime at the same instant, always valid */
    uint16_t millivolts;
    uint8_t percent;
    uint8_t armed;        /* 0 once the cell recovered, or once a boot has read it */
};

/* What the previous run ended as, decided once by flat_mark_check(). */
static bool s_went_flat;
static struct flat_mark s_flat_mark;

static int flat_mark_read(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                          void *param)
{
    struct flat_mark *dst = param;

    ARG_UNUSED(key);

    /* Same rule as boot_state_read(): a different length is a different build's
     * struct, and reading it as this one's would put arbitrary bytes into a
     * percentage the phone is about to show somebody.
     */
    if (len != sizeof(*dst)) {
        return -EINVAL;
    }
    return read_cb(cb_arg, dst, sizeof(*dst)) < 0 ? -EIO : 0;
}

/* Arms or disarms the mark. Disarming writes a record rather than deleting one,
 * so there is exactly one code path and one thing that can fail.
 */
static void flat_mark_set(bool armed, uint8_t percent, uint16_t millivolts)
{
    struct flat_mark mark = {
        .format = FLAT_MARK_FORMAT,
        .uptime_ms = wallclock_uptime(),
        .millivolts = millivolts,
        .percent = percent,
        .armed = armed ? 1u : 0u,
    };
    uint64_t anchor;

    /* Only datable if a phone has set the clock this boot. Zero is left in
     * place otherwise and means exactly that -- the uptime beside it still
     * pins the event to a point within the boot.
     */
    if (wallclock_anchor(&anchor)) {
        mark.epoch_ms = anchor + (uint64_t)mark.uptime_ms;
    }

    if (settings_save_one(FLAT_MARK_KEY, &mark, sizeof(mark)) != 0) {
        /* Not fatal, and deliberately not retried. The cost is one death the
         * phone cannot account for; the alternative is spending a dying cell's
         * last charge on flash writes instead of on delivering the backlog.
         */
        LOG_WRN("Could not %s the flat mark -- a flat cell may go unreported",
                armed ? "arm" : "clear");
        return;
    }

    if (armed) {
        LOG_INF("Flat mark armed: %u%% (%umV) at uptime %ums", percent, millivolts,
                (unsigned)mark.uptime_ms);
    } else {
        LOG_DBG("Flat mark cleared");
    }
}

/* Reads the mark left by the previous run and decides what it means.
 *
 * Takes the cause rather than reading the file-scope copy, because that copy is
 * declared further down beside log_reset_cause() -- and because a function whose
 * verdict depends on exactly two inputs is easier to be sure of when both of
 * them are in its signature.
 */
static void flat_mark_check(uint32_t reset_cause, bool cause_valid)
{
    struct flat_mark mark;

    memset(&mark, 0, sizeof(mark));
    (void)settings_load_subtree_direct(FLAT_MARK_KEY, flat_mark_read, &mark);

    if (mark.format != FLAT_MARK_FORMAT || mark.armed == 0U) {
        /* No mark, a stale one, or the cell recovered before it mattered. */
        return;
    }

    /* Cleared before the verdict, not after: every path below this line has
     * finished with the mark, and leaving it armed on the one path that returns
     * early would have it convict some later boot of a death it did not have.
     */
    flat_mark_set(false, 0, 0);

    if (!cause_valid) {
        LOG_WRN("Cell was at %u%% (%umV) and this boot's cause is unknown -- "
                "cannot say whether it ran out", mark.percent, mark.millivolts);
        return;
    }

    if (reset_cause != 0U) {
        /* Low, and then something specific reset it: the pin, a software
         * request, the debugger. That is not the cell running out.
         */
        LOG_WRN("Cell was at %u%% (%umV), but the last reset had a named cause "
                "(0x%08x) -- not reporting it as flat", mark.percent, mark.millivolts,
                (unsigned)reset_cause);
        return;
    }

    s_went_flat = true;
    s_flat_mark = mark;
    LOG_WRN("*** The cell ran out. Last reading %u%% (%umV) at uptime %ums; "
            "the rail then dropped with RESETREAS clear.",
            mark.percent, mark.millivolts, (unsigned)mark.uptime_ms);
}

/* ==========================================================================
 * How much of the time the CPU is actually awake
 *
 * **This is the only quantity that can account for the measured drain.**
 * Observed discharge is ~3.3mA, and everything this firmware can be held
 * responsible for has been costed against it without coming close: the optics
 * are ~0.28mA at the 26% duty the cycle actually runs, the radio is ~22uA as
 * observed and cannot exceed ~200uA at this interval even if peripheral latency
 * were entirely broken, the accelerometer is ~15uA, the flash scrub is bounded
 * at four erases a cycle, and the divider and the SAADC are both switched off
 * on every path including the error ones.
 *
 * **The nRF52832 core is the exception.** On this board there is no DC/DC
 * inductor, so the part runs from the LDO and draws ~7mA at 64MHz when it is
 * running. 3.3mA is about 47% of that. Nothing else on the board is in the
 * right order of magnitude, and nothing has ever checked whether this ring
 * sleeps -- CONFIG_TICKLESS_KERNEL is on, but that says the kernel *can* idle,
 * not that it does.
 *
 * `execution_cycles` is idle + non-idle, `total_cycles` is non-idle alone, so
 * the ratio of the deltas is the duty since the last report. Deltas rather than
 * since-boot totals because boot is unrepresentative and would dominate a
 * running average for hours.
 *
 * **Read it knowing what it does not count.** Usage is accounted at context
 * switches, so an interrupt that does not cause one is charged to whatever
 * thread it interrupted -- and an ISR that fires while the idle thread is
 * current is therefore charged to *idle*. Radio and timer ISRs are exactly that
 * case. **So a high reading convicts the CPU and a low reading does not clear
 * it**; it moves the remaining suspicion into interrupt context, which is a
 * much smaller place to look than "somewhere on the board".
 * ========================================================================== */
static void cpu_duty_report(void)
{
    static uint64_t prev_exec, prev_busy;
    k_thread_runtime_stats_t stats;
    uint64_t exec, busy;

    if (k_thread_runtime_stats_cpu_get(0, &stats) != 0) {
        return;
    }

    exec = stats.execution_cycles - prev_exec;
    busy = stats.total_cycles - prev_busy;
    prev_exec = stats.execution_cycles;
    prev_busy = stats.total_cycles;

    if (exec == 0) {
        return;
    }

    /* Tenths, because the number this is looking for is either a few percent or
     * tens of percent and the difference between 0.3% and 3% is the whole
     * question. Integer maths on a 64-bit count that cannot overflow at 32kHz.
     */
    LOG_INF("CPU awake %u.%u%% since the last cycle (%llu of %llu cycles)",
            (unsigned)(busy * 1000U / exec) / 10U, (unsigned)(busy * 1000U / exec) % 10U,
            (unsigned long long)busy, (unsigned long long)exec);
}

/* Reads the cell and moves the state machine. Called every cycle, including
 * cycles that measure nothing -- see the note at the call site.
 */
static void power_check(void)
{
    enum power_state was = s_power;
    int mv;
    uint8_t pct;

    if (!battery_ready) {
        return;
    }

    mv = battery_read_mv();
    if (mv < 0) {
        /* A failed read is not evidence of a flat cell, and treating it as one
         * would stop the ring measuring over a bad ADC conversion.
         */
        LOG_ERR("Battery read failed (%d)", mv);
        return;
    }

    pct = battery_level((uint16_t)mv);
    LOG_DBG("Battery %u%% (%d mV)", pct, mv);
    ble_set_battery(pct);

    /* On the battery's cadence deliberately: the discharge slope and the CPU
     * duty that is supposed to explain it then sit in the same stretch of log,
     * a couple of lines apart, and can be read against each other without
     * lining up two timebases.
     */
    cpu_duty_report();

    /* The step total rides out on the same cadence, in its own packet.
     *
     * Here rather than in the measurement path because this is the one place
     * that still talks to the phone in the shipping build: BLE_LIVE_STREAM is
     * 0, so the vitals and motion notifications are compiled out and steps
     * would otherwise reach a client only inside replayed records. A total on
     * the battery's cadence gives the app something live to show without
     * reopening the live stream and the radio time that comes with it.
     *
     * A failed step read skips only the step packet -- the battery has already
     * gone out above, and the power state machine below does not depend on it.
     */
    {
        int steps = step_total();

        if (steps >= 0) {
            ble_notify_status((uint32_t)k_uptime_get(), pct, (uint16_t)mv, (uint32_t)steps);
        }
    }

    if (pct <= POWER_CRITICAL_PCT) {
        s_power = POWER_CRITICAL;
    } else if (pct <= POWER_LOW_PCT) {
        /* Only ever downward from OK. Coming back up needs the recovery mark,
         * which is what stops a sagging reading from clearing a real warning.
         */
        if (s_power == POWER_OK) {
            s_power = POWER_LOW;
        }
    } else if (pct >= POWER_RECOVER_PCT) {
        s_power = POWER_OK;
    }

    if (s_power == was) {
        return;
    }

    switch (s_power) {
    case POWER_CRITICAL:
        LOG_WRN("Battery %u%% -- measuring stopped, delivering the buffer", pct);
        /* Before the alert, deliberately. The alert needs a phone listening and
         * this does not, and of the two it is the one that still means something
         * if nobody is there -- which is the case it exists for.
         */
        flat_mark_set(true, pct, (uint16_t)mv);
        ble_notify_alert(BLE_ALERT_BATTERY_CRITICAL);
        /* Ask now rather than waiting for the next backlog check: at this point
         * every remaining minute is one the phone might not get.
         */
        ble_notify_event();
        break;
    case POWER_LOW:
        LOG_WRN("Battery %u%% -- measuring less often to stretch it", pct);
        ble_notify_alert(BLE_ALERT_BATTERY_LOW);
        break;
    case POWER_OK:
        LOG_INF("Battery %u%% -- back to the normal cadence", pct);
        /* Charged in time. Whatever the mark said is now a thing that did not
         * happen, and leaving it armed would have the next reset -- any reset --
         * reported as the cell running out.
         */
        flat_mark_set(false, 0, 0);
        ble_notify_alert(BLE_ALERT_BATTERY_OK);
        break;
    }
}


/* ==========================================================================
 * Surviving a reset
 *
 * Erasing the log at boot is defensible on its own terms: uptime restarts at
 * zero, so every stored timestamp belongs to an epoch nothing can date, and
 * undatable readings are worse than none. That trade holds when a reset means
 * end-of-charge. When resets are frequent it makes the firmware the largest
 * destroyer of data in the system.
 *
 * Two numbers are all it takes to make the records datable again, and both are
 * small enough to keep in NVS beside the bonds:
 *
 *   generation     -- so the kept log keeps its identity and the phone's cursor
 *                     stays valid, and it resumes instead of restarting from 0.
 *   epoch_at_boot  -- the anchor, so the kept records can be dated before any
 *                     phone has connected this boot.
 *
 * **The high-water uptime is deliberately not persisted.** It is recovered from
 * the part itself -- flash_store_restore() reports the newest timestamp it
 * found, which is by definition above every kept record and cannot disagree
 * with them. A persisted copy could, and the failure would be silent.
 *
 * **Churn is a non-issue because neither number changes in normal running.** The
 * generation is constant for the life of a log and the anchor is constant modulo
 * drift, so this writes roughly once per boot rather than once per record.
 * ==========================================================================
 */
#define BOOT_STATE_KEY "sense/boot"

/* **What the kept records have to mean, for keeping them to be safe.**
 *
 * `west flash` does not erase the storage partition and does not clear NVS, so
 * **a reflash restores records written by the previous firmware.** That is
 * harmless while the record layout is unchanged, and it is exactly the accident
 * that stops being harmless the first time it changes: old-format records would
 * be resurrected under new firmware and read as though they meant something,
 * with nothing anywhere to notice.
 *
 * `FLASH_RECORD_SIZE` is folded in so a size change invalidates on its own.
 * `RECORD_FORMAT_VERSION` covers the case the size cannot see: the same 16 bytes
 * meaning something different. **Bump it whenever a field in
 * `struct flash_record` changes meaning, units, or position.**
 *
 * The cost of getting it wrong in the safe direction is one erased log.
 */
#define RECORD_FORMAT_VERSION 1u
#define BOOT_STATE_FORMAT     ((RECORD_FORMAT_VERSION << 16) | FLASH_RECORD_SIZE)

struct boot_state {
    uint32_t generation;
    uint32_t format; /* BOOT_STATE_FORMAT; 0 in anything written before it existed */
    uint64_t epoch_at_boot_ms;
};

static struct boot_state s_boot_saved; /* what is currently in NVS */

static int boot_state_read(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                           void *param)
{
    struct boot_state *dst = param;

    /* A short or long value is a different build's idea of this struct. Ignore
     * it and cold-boot; the alternative is restoring a log under a generation
     * that means something else.
     */
    if (len != sizeof(*dst)) {
        return -EINVAL;
    }
    return read_cb(cb_arg, dst, sizeof(*dst)) < 0 ? -EIO : 0;
}

static void boot_state_load(struct boot_state *out)
{
    memset(out, 0, sizeof(*out));

    /* Idempotent, and deliberately before ble_init()'s settings_load(): the log
     * has to be opened before Bluetooth, so the boot state has to be readable
     * before Bluetooth too.
     */
    if (settings_subsys_init() != 0) {
        LOG_WRN("Settings unavailable; the log cannot survive a reset this boot");
        return;
    }
    (void)settings_load_subtree_direct(BOOT_STATE_KEY, boot_state_read, out);

    /* Records written by a build whose idea of a record differs from this one's
     * must not be kept, however intact they are. Zeroing the generation is all
     * it takes: flash_store_init() treats that as a cold boot and erases.
     */
    if (out->generation != 0 && out->format != BOOT_STATE_FORMAT) {
        LOG_WRN("Stored log is format 0x%08x, this build wants 0x%08x -- erasing it",
                (unsigned)out->format, (unsigned)BOOT_STATE_FORMAT);
        memset(out, 0, sizeof(*out));
    }
}

static void boot_state_save_if_changed(void)
{
    struct boot_state now = s_boot_saved;
    uint64_t anchor;

    now.generation = flash_store_generation();
    now.format = BOOT_STATE_FORMAT;
    if (wallclock_anchor(&anchor)) {
        now.epoch_at_boot_ms = anchor;
    }

    /* The anchor is recomputed on every connection and moves by the crystal's
     * drift each time, so an exact comparison would write on every connect for
     * no benefit. A second is far below anything a vitals timestamp needs and
     * far above the drift between two connections.
     */
    if (now.generation == s_boot_saved.generation && now.format == s_boot_saved.format &&
        (now.epoch_at_boot_ms > s_boot_saved.epoch_at_boot_ms
             ? now.epoch_at_boot_ms - s_boot_saved.epoch_at_boot_ms
             : s_boot_saved.epoch_at_boot_ms - now.epoch_at_boot_ms) < 1000ULL) {
        return;
    }

    if (settings_save_one(BOOT_STATE_KEY, &now, sizeof(now)) != 0) {
        LOG_WRN("Could not persist the boot state; a reset would erase the log");
        return;
    }
    s_boot_saved = now;
}

static void nudge_if_backlog_deep(void)
{
    uint32_t capacity = flash_store_capacity();
    uint32_t page = flash_store_page_size();
    int64_t now = k_uptime_get();
    uint32_t head;
    uint32_t held;

    if (capacity == 0 || page == 0) {
        return; /* no log this session; nothing to be behind on */
    }

    head = flash_store_head();
    held = flash_store_cursor() - head;

    /* The head only moves when the phone acknowledges, so its movement is the
     * one honest signal that collection is happening. Anything else -- a link
     * that is up, a nudge that was sent -- can be true while nothing arrives.
     */
    if (head != held_since_head) {
        held_since_head = head;
        held_since_ms = now;
    }

    /* The backstop first, so a buffer that is genuinely filling says so rather
     * than reporting the page mark it also crossed on the way past.
     */
    if (!backlog_nudged && held >= capacity / 100U * BACKLOG_NUDGE_HIGH_PCT) {
        LOG_WRN("Buffer %u%% full (%u of %u records) -- asking the phone to collect",
                (unsigned)(held / (capacity / 100U)), held / FLASH_RECORD_SIZE,
                capacity / FLASH_RECORD_SIZE);
        ble_notify_event();
        backlog_nudged = true;
        page_nudged = true; /* the same nudge answers both marks */
        return;
    }

    if (backlog_nudged && held <= capacity / 100U * BACKLOG_NUDGE_LOW_PCT) {
        backlog_nudged = false;
    }

    /* A whole page is now reclaimable, which is the smallest collection worth
     * asking for. Info rather than a warning: this is the system working.
     */
    if (!page_nudged && held >= page) {
        LOG_INF("%u record(s) unacknowledged (%u page(s)) -- asking the phone to collect",
                held / FLASH_RECORD_SIZE, held / page);
        ble_notify_event();
        page_nudged = true;
        return;
    }

    /* Below a page again: the phone collected and acknowledged, the head moved
     * over whole pages, and the next page-full gets its own nudge. A backlog
     * that never comes back under is a phone that is not answering, and the
     * backstop above is what covers that.
     */
    if (page_nudged && held < page) {
        page_nudged = false;
    }

    /* The hold mark. Last, because the two above already asked if they fired,
     * and re-armed on the timer rather than on the acknowledgement -- a phone
     * that is not answering should be asked again, which is exactly the case
     * the volume marks cannot see.
     */
    if (held > 0 && now - held_since_ms >= DELIVERY_MAX_HOLD_MS) {
        LOG_WRN("%u record(s) held %u min without collection -- asking the phone",
                held / FLASH_RECORD_SIZE,
                (unsigned)((now - held_since_ms) / 60000));
        ble_notify_event();
        held_since_ms = now;
    }
}

#if BENCH_SYNTHETIC_VITALS
/* A fabricated reading, in place of the optics.
 *
 * Deliberately a believable one: contact true, confidence well clear of
 * VITALS_BPM_CONFIDENCE_MIN, perfusion in the range a real ring produces. The
 * point is to exercise the accepting path, and a reading the gate would refuse
 * exercises only the refusal. (The refusal path is not untested for it --
 * tests/host drives that side directly, on real arithmetic.)
 *
 * The rate walks the sweep and turns round at each end, so successive packages
 * differ the way real ones do and a console full of identical numbers never
 * gets mistaken for a working stream.
 */
static void bench_synthesise(struct vitals *v)
{
    static uint16_t bpm = BENCH_SWEEP_MIN_BPM;
    static int16_t step = BENCH_SWEEP_STEP_BPM;

    memset(v, 0, sizeof(*v));
    v->contact = true;
    v->confidence = 820;
    v->spo2_tenths = 972;
    v->perfusion_milli = 4;
    v->ir_dc = 42000;

    v->bpm = bpm;

    if (bpm + step > BENCH_SWEEP_MAX_BPM || bpm + step < BENCH_SWEEP_MIN_BPM) {
        step = (int16_t)-step;
    }
    bpm = (uint16_t)(bpm + step);
}
#endif

/* A burst of packages, one every RECORD_INTERVAL_MS. */
static void record_burst(int64_t ms)
{
    for (int64_t elapsed = 0; elapsed < ms; elapsed += RECORD_INTERVAL_MS) {
#if BENCH_SYNTHETIC_VITALS
        struct vitals v;

        /* Still spaced by RECORD_INTERVAL_MS rather than emitted flat out. The
         * timestamps are what the phone reassembles the stream by, and a burst
         * of packages sharing one millisecond would test the transport against
         * data no wearer can produce.
         */
        k_msleep(RECORD_INTERVAL_MS);
        bench_synthesise(&v);
        record_vitals(&v);
#else
        if (!collect_for(RECORD_INTERVAL_MS)) {
            break; /* ring came off; nothing left to measure this cycle */
        }
        record_one();
#endif
    }
}

/* Ending a cycle with the LEDs down. Separate from the bare ppg_stop() calls
 * inside the measuring phases because on the synthetic path there is nothing to
 * stop: the part was never started, and may not even be fitted to whatever the
 * bench build is running on.
 */
#if BENCH_SYNTHETIC_VITALS
static inline void cycle_ppg_stop(void)
{
}
#else
static inline void cycle_ppg_stop(void)
{
    ppg_stop();
}
#endif

/* One cycle: probe for a finger, fill a window, and report a burst of packages
 * from it.
 */
static void measurement_cycle(void)
{
#if BENCH_SYNTHETIC_VITALS
    /* No optics at all on this path: no LED current, no contact gate, no
     * PREPARE. The window those phases exist to fill is never read, because
     * record_burst() below fabricates its readings rather than computing them.
     *
     * `worn` is forced true because it is what the caller's housekeeping turns
     * on -- the battery read, the log dump and the backlog line all sit behind
     * it, and they are most of what a bench run is for. Leaving it false would
     * send every cycle into wait_until_worth_probing() instead.
     */
    worn = true;
#else
    if (ppg_start() != 0) {
        LOG_ERR("PPG window failed to start");
        ppg_stop();
        return;
    }

    /* PROBE. A second of signal to decide whether there is a finger here at
     * all, before spending a whole window's worth of LED current finding out.
     */
    if (!probe_contact()) {
        ppg_stop();
        return;
    }

    /* PREPARE. Nothing is measured until there is a whole window to measure. */
    if (!collect_for(PREPARE_MS)) {
        ppg_stop();
        return;
    }
#endif

    /* REPORT. */
    record_burst(REPORT_MS);

    cycle_ppg_stop();
}

/* How often the accelerometer is checked while the ring is off. Cheap: the part
 * is already running (see imu_ready), so this is one 6-byte I2C read and a
 * subtraction, a few hundred microseconds every quarter second.
 */
#define UNWORN_POLL_MS 250

/* Floor on how often the LEDs may be spent probing for a finger while unworn.
 * Without it a ring carried in a moving pocket would see movement continuously
 * and probe as fast as it could -- a 1.1s probe every 1.1s. Five seconds bounds
 * that at ~18% duty in the worst case while still feeling immediate to someone
 * putting the ring on.
 */
#define UNWORN_MIN_RETRY_MS 5000

/* Manhattan delta between consecutive readings, in milli-g, that counts as
 * "something moved". At rest the ring measures 2-5; picking it up produces
 * hundreds. 100 sits well clear of the noise without needing the movement to be
 * vigorous.
 */
#define DON_MOVE_MILLI_G 100

/* Waits until it is worth spending LED current looking for a finger again.
 *
 * The problem this solves: a ring that has just been taken off used to sleep
 * the full PAUSE_MS before probing, so putting it back on went unnoticed for up
 * to 90 seconds. Simply shortening the pause is the obvious fix and the wrong
 * one -- a probe costs ~1.1s of LEDs at ~1mA, so retrying every 5s unconditionally
 * is ~18% duty forever, and a ring left in a drawer would flatten the cell in
 * under a week instead of lasting a month.
 *
 * The accelerometer resolves that, and it is the first thing on this device to
 * earn its keep by *not* measuring: a ring that is not moving is not being put
 * on, so there is nothing to look for. Movement gates the probe, the floor above
 * bounds how often it can fire, and PAUSE_MS remains a backstop so a donning too
 * gentle to register still costs at most the old latency.
 *
 * Falls back to a plain sleep when the IMU never came up -- degraded, not
 * broken, which is the same posture every other optional subsystem here takes.
 */
static void wait_until_worth_probing(void)
{
    const int64_t start = k_uptime_get();
    struct imu_sample prev;
    bool have_prev = false;
    bool moved = false;

    unsigned int reads = 0, good = 0;

    if (!imu_ready) {
        k_msleep(PAUSE_MS);
        return;
    }

    while (k_uptime_get() - start < PAUSE_MS) {
        struct imu_sample s;

        k_msleep(UNWORN_POLL_MS);

        reads++;
        if (imu_read(&s) != 0) {
            continue; /* -EAGAIN or a bus glitch; neither is evidence */
        }
        good++;

        if (have_prev) {
            uint32_t move = (uint32_t)(abs(s.x - prev.x) + abs(s.y - prev.y) +
                                       abs(s.z - prev.z));

            if (move >= DON_MOVE_MILLI_G) {
                /* Latched rather than acted on immediately, because the floor
                 * below may not have passed yet -- and a ring picked up, moved
                 * and set down again inside those five seconds still deserves a
                 * probe. Forgetting it because the movement stopped is how this
                 * misses the exact case it exists for.
                 */
                moved = true;
            }
        }

        prev = s;
        have_prev = true;

        if (moved && k_uptime_get() - start >= UNWORN_MIN_RETRY_MS) {
            LOG_INF("Movement while off -- checking for a finger");
            return;
        }
    }

    /* Fell through to the backstop. If the accelerometer was answering the
     * whole time this is just a genuinely still ring; if it was not, the
     * movement gate has been silently inoperative and the ring has been running
     * on the 90s timer alone -- which looks exactly the same from outside and
     * is the difference between "nothing moved" and "nothing could be seen".
     */
    if (good == 0) {
        LOG_WRN("IMU answered none of %u reads while off -- movement gate inoperative", reads);
    }
}

/* ==========================================================================
 * When to spend the window, as opposed to whether to keep it
 *
 * **The problem this solves, measured.** A worn walk returns `yield 0% (0 of 54
 * windows), 53 moving`, with mean per-drain movement distributed 100-149:1,
 * 150-249:7, 250-499:12, 500-999:34 -- not one window under the gate, across
 * eleven minutes and 338 steps. A whole worn day is the same shape at 5.8%.
 *
 * **The gate is not what needs relaxing for that.** It is judging those windows
 * correctly; a walking hand is exactly what it exists to refuse. What is wrong
 * is *when the window is taken*, and on a fixed cadence that is not a decision
 * at all: PAUSE_MS elapses, the LEDs go up, and the ring finds out afterwards
 * whether the fifteen seconds it just paid for were still.
 *
 * `WINDOW_MOVE_MILLI_G` says what the gate is really asking of a wearer --
 * fifteen continuous seconds of near-stillness, once per cycle, as the price
 * of being measured at all. **A fixed cadence asks for it at an arbitrary
 * moment.** The accelerometer that will judge the answer is already running
 * through the whole pause (see imu_ready), costing ~15uA whether it is read or
 * not, so the ring can wait for a still moment instead of hoping for one.
 *
 * This is the same instrument `wait_until_worth_probing()` uses, pointed the
 * other way: that one waits for movement before looking for a finger, this one
 * waits for the absence of it before measuring a finger it already has.
 *
 * **What it does not do.** It does not relax the gate by one milli-g, and it
 * does not change what any stored record means. A hand that never goes still is
 * probed anyway when the deferral runs out, so the refusal stream keeps
 * arriving -- fewer of them per day, and every one still carrying its bucket,
 * bpm and confidence.
 *
 * **It matters more given that record_vitals() keeps a moving window whose
 * pulse cleared the confidence bar.** The cost of probing a moving hand is then
 * not a refusal but a reading taken through contaminated signal, which is the
 * more expensive mistake. Waiting for stillness is what keeps those readings
 * rare and the still ones the norm.
 *
 * ⚠️ **It buys nothing during a continuous walk.** What it buys is the boundary
 * case: the wearer who is still for two minutes in ten, whose stillness a fixed
 * 121.5s cadence samples only by luck. How much that is worth depends on the
 * wearer's day, which is why the deferral count is logged.
 * ========================================================================== */

/* How much recent history the watch judges, and at what cadence.
 *
 * PPG_POLL_INTERVAL_MS rather than a rate of its own, and that is load-bearing:
 * the quantity being compared against WINDOW_MOVE_MILLI_G is a Manhattan delta
 * between consecutive reads, so its magnitude scales with the gap between them.
 * Sampling at UNWORN_POLL_MS would put the same still hand at 250/200 of the
 * number the threshold was calibrated on and quietly make the watch stricter
 * than the gate it is predicting.
 *
 * Three seconds of it. The watch is a *predictor* -- it says the hand is still
 * now, and the window it starts runs for fifteen seconds after that -- so the
 * history it judges is a trade: too short and it fires on a gap between two
 * gestures, too long and a hand that has just settled waits out its own past.
 * Three seconds is one full cycle of the slowest movement that matters here (a
 * ~0.7Hz arm swing is 1.4s) and costs at most that much cadence.
 */
#define STILL_SLOTS 15

/* One read longer than the history it fills, because the first read of the
 * watch has no predecessor and so produces no delta. Getting this wrong is not
 * visible on a console -- it just probes a fifth of a second late, every cycle,
 * forever.
 */
#define STILL_WATCH_MS ((STILL_SLOTS + 1) * PPG_POLL_INTERVAL_MS)

/* How long the probe may be held back waiting for a still moment.
 *
 * The bound is what keeps this a scheduling change rather than a coverage one.
 * At 60s the cycle stretches from 121.5s to at most 181.5s, and only ever on a
 * cycle whose window the gate was going to throw away -- so the worst case is
 * fewer refusals recorded per hour and *less* LED current spent, never fewer
 * readings. Unbounded, it would be a ring that stops measuring a wearer who is
 * having a busy day, which is the opposite of what this device is for.
 */
#define STILL_DEFER_MAX_MS 60000

/* The pause, then a look at the hand before the LEDs go up.
 *
 * Sampling starts STILL_WATCH_MS *before* the pause would have ended, so a hand
 * that is already still is probed on exactly the old cadence and pays nothing
 * for the watch. Everything after that is deferral.
 *
 * Falls back to a plain sleep when the IMU never came up, and gives up on the
 * watch if no read succeeds inside the first window -- degraded, not broken,
 * which is the posture every optional subsystem here takes. A failed read is
 * never recorded as a zero: "no evidence" and "no movement" are different
 * statements, and conflating them here would probe a moving hand on the
 * strength of a broken bus.
 */
static void pause_until_still(int32_t pause_ms)
{
    uint16_t slot[STILL_SLOTS];
    size_t filled = 0;
    size_t next = 0;
    struct imu_sample prev;
    bool have_prev = false;
    unsigned int good = 0;
    int64_t start;
    int64_t deadline;

    if (!imu_ready || pause_ms <= (int32_t)STILL_WATCH_MS) {
        k_msleep(pause_ms);
        return;
    }

    k_msleep(pause_ms - (int32_t)STILL_WATCH_MS);

    start = k_uptime_get();
    deadline = start + STILL_WATCH_MS + STILL_DEFER_MAX_MS;

    for (;;) {
        struct imu_sample s;
        int64_t waited;

        k_msleep(PPG_POLL_INTERVAL_MS);

        if (imu_read(&s) == 0) {
            good++;

            if (have_prev) {
                uint32_t delta = (uint32_t)(abs(s.x - prev.x) + abs(s.y - prev.y) +
                                            abs(s.z - prev.z));

                slot[next] = (uint16_t)MIN(delta, UINT16_MAX);
                next = (next + 1u) % ARRAY_SIZE(slot);
                if (filled < ARRAY_SIZE(slot)) {
                    filled++;
                }
            }

            prev = s;
            have_prev = true;
        }

        waited = k_uptime_get() - start;

        /* Only ever judged on a full history. A partly-filled one is the first
         * moments after the watch opened, and half a second of stillness is not
         * the claim this is making.
         */
        if (filled == ARRAY_SIZE(slot)) {
            uint16_t median = median_milli_g(slot, filled);

            if (median < WINDOW_MOVE_MILLI_G) {
                if (waited > STILL_WATCH_MS) {
                    LOG_INF("Probe deferred %ds -- hand still now (%u mg median)",
                            (int)((waited - STILL_WATCH_MS) / 1000), median);
                }
                return;
            }
        } else if (waited >= STILL_WATCH_MS && good == 0) {
            /* Same failure `wait_until_worth_probing()` warns about, and the
             * same reason to say so out loud: from outside, an inoperative
             * watch and a wearer who never stops moving look identical.
             */
            LOG_WRN("IMU answered no reads while pausing -- stillness watch inoperative");
            return;
        }

        if (k_uptime_get() >= deadline) {
            LOG_INF("Hand still moving after %ds of deferral -- probing anyway",
                    STILL_DEFER_MAX_MS / 1000);
            return;
        }
    }
}

/* Why the last boot happened.
 *
 * An unexplained reset destroys records, and without this nothing in the system
 * can say what caused one. Most candidates are ruled out by configuration --
 * `RESET_ON_FATAL_ERROR=n` means a software fault halts rather than reboots,
 * `WATCHDOG=n` means nothing bites -- so RESETREAS holds what is left, and it
 * is cleared on every boot unless something reads it first.
 *
 * **A zero cause is the informative case on this part, not a missing one.**
 * nRF52832 RESETREAS latches only RESETPIN, DOG, SREQ, LOCKUP and the wake
 * sources (OFF/LPCOMP/DIF/NFC); it has no bit for power-on and none for
 * brownout. So a supply that actually went away -- a genuine brownout, a loose
 * cell contact, the PMIC dropping the rail -- leaves the register clear, and
 * reading 0 here *is* the finding rather than the absence of one, which is why
 * the zero case gets its own line below instead of being reported as
 * "unknown".
 *
 * It is logged at warning level even though a deliberate power-up produces the
 * same zero. On a charger that line is noise; appearing in the middle of a worn
 * session it is the whole answer, and there is no way to tell the two apart
 * from inside the chip. Noise on the boot nobody is watching is a fair price
 * for a loud line on the boot somebody is.
 *
 * Cleared after reading. Without that the bits accumulate across resets and the
 * third boot cannot tell which cause belonged to it.
 */
/* What log_reset_cause() found, kept for ble.c.
 *
 * RESETREAS is cleared as soon as it is read -- it has to be, or the third boot
 * in a row reports the second one's cause alongside its own -- so this is the
 * only copy after that line runs. `s_reset_cause_valid` is separate because
 * zero is a real answer on this part and "hwinfo could not tell us" is not the
 * same statement.
 */
static uint32_t s_reset_cause;
static bool s_reset_cause_valid;

static void log_reset_cause(void)
{
    static const struct {
        uint32_t flag;
        const char *name;
    } causes[] = {
        { RESET_PIN,            "pin" },
        { RESET_SOFTWARE,       "software request" },
        { RESET_BROWNOUT,       "brownout" },
        { RESET_POR,            "power-on" },
        { RESET_WATCHDOG,       "watchdog" },
        { RESET_DEBUG,          "debug" },
        { RESET_SECURITY,       "security" },
        { RESET_LOW_POWER_WAKE, "low-power wake" },
        { RESET_CPU_LOCKUP,     "cpu lockup" },
        { RESET_PARITY,         "parity" },
        { RESET_PLL,            "pll" },
        { RESET_CLOCK,          "clock" },
        { RESET_HARDWARE,       "hardware" },
        { RESET_USER,           "user" },
        { RESET_TEMPERATURE,    "temperature" },
    };
    char list[96];
    size_t used = 0;
    uint32_t cause = 0;
    int rc;

    rc = hwinfo_get_reset_cause(&cause);
    if (rc != 0) {
        LOG_WRN("Reset cause unavailable (%d) -- an unexplained reset will stay unexplained", rc);
        return;
    }

    /* Kept before the clear below and before any of the early returns, because
     * every one of them is a case the phone still needs the answer for -- a
     * zero cause most of all. See ble_set_boot_report().
     */
    s_reset_cause = cause;
    s_reset_cause_valid = true;

    /* Before any early return below, so a cause is never carried into the next
     * boot's report.
     */
    (void)hwinfo_clear_reset_cause();

    if (cause == 0) {
        LOG_WRN("Reset cause: power-on or brownout (RESETREAS clear). If the ring was "
                "not deliberately powered up, the supply went away.");
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(causes); i++) {
        int n;

        if ((cause & causes[i].flag) == 0U) {
            continue;
        }

        n = snprintk(list + used, sizeof(list) - used, "%s%s", used > 0 ? ", " : "",
                     causes[i].name);
        if (n < 0 || (size_t)n >= sizeof(list) - used) {
            used = sizeof(list) - 1;
            break;
        }
        used += (size_t)n;
    }

    /* A bit outside the table above: report the raw word rather than claiming
     * nothing was set, because "no name for it" and "nothing there" are
     * different answers and only one of them is a firmware gap.
     */
    if (used == 0) {
        LOG_WRN("Reset cause: 0x%08x (no name for these bits)", cause);
        return;
    }

    list[used] = '\0';
    LOG_WRN("Reset cause: %s (0x%08x)", list, cause);
}

int main(void)
{
    LOG_INF("SenseRing v29 System Booting...");

    /* Second line out, before anything else can fail and bury it. Whatever else
     * this boot turns out to be, why it happened is the thing a log read after
     * the fact needs first.
     */
    log_reset_cause();

#if BENCH_ANY
    /* Said once, loudly, because everything after it is a bench run and not the
     * product: a console read from the middle of a session should never leave
     * anyone guessing which one they are looking at.
     */
    LOG_WRN("BENCH BUILD -- fast cycle %s, synthetic vitals %s. Not the shipping cadence.",
            BENCH_FAST_CYCLE ? "on" : "off", BENCH_SYNTHETIC_VITALS ? "on" : "off");
#endif

    /* Bring the battery monitor up first so the boot flush can log a level.
     * A dead battery monitor is no reason to stop reporting a pulse.
     */
    battery_ready = (battery_init() == 0);
    if (!battery_ready) {
        LOG_ERR("Battery monitor unavailable, continuing without it");
    }

    /* Erases last run's log before we start a new one, and says how many
     * records that cost (flash_store.c, count_records_on_part).
     *
     * **It does not dump them first.** "Dumped and erased" and "erased" are the
     * difference between a reset that costs a console scroll and one that costs
     * the data outright -- and with RTT unavailable on a worn ring, a dump here
     * would reach nobody anyway.
     */
    {
        struct boot_state prev;
        uint32_t newest = 0;

        boot_state_load(&prev);

        if (flash_store_init(flush_battery_mv, step_total, prev.generation, &newest) != 0) {
            LOG_ERR("Vitals log unavailable, continuing without it");
        } else if (newest > 0) {
            /* The log was kept. Rebase above its newest record so the next one
             * written cannot collide with or precede it, then put the anchor
             * back so those records are datable before any phone connects.
             * A cold boot leaves `newest` at 0 and both of these untouched.
             */
            wallclock_rebase(newest + 1);
            wallclock_restore(prev.epoch_at_boot_ms);
        }

        s_boot_saved = prev;
        boot_state_save_if_changed();
    }

    /* Everything about this boot that a phone can be told, handed over before
     * the link exists so the very first control read after a reset already
     * carries it. Both terms are ready by here: the cause was stashed at the
     * top of main(), and the erase count is only known once flash_store_init()
     * has decided whether the log survived.
     *
     * This is the only field-visible account of a reset: `Reset cause:` goes to
     * RTT, and a reset on a worn ring is not tethered.
     */
    /* Needs settings, which boot_state_load() above has already brought up, and
     * needs the reset cause, which was stashed at the top of main(). Both are
     * true by here and neither is anywhere else in this function.
     */
    flat_mark_check(s_reset_cause, s_reset_cause_valid);

    ble_set_boot_report(s_reset_cause, s_reset_cause_valid, flash_store_boot_destroyed());
    ble_set_flat_report(s_went_flat, s_flat_mark.percent, s_flat_mark.millivolts,
                        s_flat_mark.epoch_ms);

    /* Connectivity is a bonus on top of the RTT line, never a gate on it. */
    if (ble_init() != 0) {
        LOG_ERR("Bluetooth unavailable, continuing without it");
    }

    /* Same posture as the battery monitor and the log: a degraded feature, not
     * a dead device. A ring that reports a pulse but no movement is worth far
     * more than one that refuses to boot. Started once and left running -- see
     * imu_ready above for why it is not duty-cycled.
     */
    /* Split rather than `imu_init() == 0 && imu_start() == 0` so the console
     * says which half failed. The two have completely different causes -- init
     * is "is the right part there and healthy", start is "did it accept the
     * configuration and begin converting" -- and collapsing them into one line
     * throws away the only clue.
     */
    if (imu_init() != 0) {
        LOG_ERR("IMU init failed -- no motion data this session");
    } else if (imu_start() != 0) {
        LOG_ERR("IMU start failed -- no motion data this session");
    } else {
        imu_ready = true;
    }

    if (ppg_init() != 0) {
        LOG_ERR("PPG Initialization failed!");
#if !BENCH_SYNTHETIC_VITALS
        return 0;
#else
        /* Not fatal here, and that is the point of the mode: it exercises the
         * pipeline downstream of the optics, so it has to run on a board whose
         * optics are absent, unpowered or broken. Refusing to boot would make
         * the one part still under test untestable.
         */
        LOG_WRN("Continuing anyway -- synthetic vitals do not need the optics");
#endif
    }

    while (1) {
        /* Whatever the phone acknowledged while we were measuring or paused:
         * take the space back, and erase a few of the pages it freed.
         *
         * **At the top of the loop, deliberately, and not beside the log dump
         * below.** A ring that is off the finger `continue`s before reaching
         * the dump, and that is the path that matters: a ring in a pocket
         * overnight measures nothing for hours while
         * a connected phone goes on collecting and acknowledging the backlog.
         * Housekeeping that only runs on cycles that measured something would
         * leave those pages unerased for exactly as long as the wearer was not
         * wearing it.
         *
         * The LEDs are down at this point and the sensor is stopped, so the
         * erases cost measurement nothing beyond delaying the next burst. They
         * are bounded per call for that reason (flash_store.h).
         */
        flash_store_service();

        /* **Battery first, and on every cycle.** It used to be read after the
         * measurement and only when the ring was worn, which made it invisible
         * on exactly the two paths that matter: a ring off the finger, and a
         * ring that has stopped measuring *because* the cell is low. The second
         * is self-locking -- stop measuring on a low battery, never read the
         * battery again, never notice it was charged.
         */
        power_check();

        /* Out of cell. Measuring is over; delivering what was already measured
         * is not.
         *
         * The ring holds up to four days of readings that exist nowhere else,
         * so the worst end to a wear test is dying with a full buffer. Below
         * the critical mark it stops taking new readings -- which is what costs
         * the battery -- and keeps only the radio, nudging the phone so the
         * backlog has every chance to leave before the lights go out. Readings
         * lost to not measuring are readings that were never taken; readings
         * lost with the buffer are readings taken and then destroyed.
         */
        if (s_power == POWER_CRITICAL) {
            nudge_if_backlog_deep();
            boot_state_save_if_changed();
            k_msleep(POWER_CRITICAL_SLEEP_MS);
            continue;
        }

        measurement_cycle();

        /* Nothing was measured, so there is nothing to report on and no reason
         * to spend a battery reading or a log dump on it. Wait for the ring to
         * be picked up instead -- that wait is where the responsiveness on
         * re-donning comes from, and skipping the housekeeping is also what
         * keeps a fast retry from filling the console with empty dumps.
         */
        if (!worn) {
            wait_until_worth_probing();
            continue;
        }

        /* Read the log back without erasing it, once per cycle. A liveness
         * check, not part of the storage contract: it proves packages are being
         * measured and stored and that the battery still reads, on a console
         * that is otherwise silent for hours.
         *
         * Only what is new since the last dump -- a handful of lines a cycle,
         * forever. It cannot print the whole log instead: the RTT up-buffer is
         * 1KB in NO_BLOCK_SKIP mode, about 13 of these lines, and a burst past
         * that is dropped with no marker at all.
         */
        (void)flash_store_dump();
        bench_report_backlog();
        nudge_if_backlog_deep();
        boot_state_save_if_changed();

        /* LEDs are already down: this is the PAUSE phase. Not anchored to
         * absolute time, because the cycle length is not fixed -- a cycle that
         * finds no finger is much shorter than one that measures -- so there is
         * no drift to correct against, only a gap to leave between bursts.
         *
         * The pause ends when the hand is still rather than when the clock says
         * so: the last three seconds of it are spent watching the accelerometer,
         * and a moving hand holds the LEDs down for up to STILL_DEFER_MAX_MS
         * longer rather than buying a window the gate would refuse. See
         * pause_until_still().
         */
        pause_until_still(power_pause_ms());
    }

    return 0;
}
