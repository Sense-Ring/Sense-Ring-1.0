/* Switched VBAT divider on the SAADC. See ARCHITECTURE.md §2 for the divider
 * math, the settle-time derivation and why the in-tree driver is disabled.
 */
#include "battery.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>

LOG_MODULE_DECLARE(main);

#define VBAT_NODE DT_NODELABEL(vbat)

/* VBAT reaches the divider only through U4, a WS4622C load switch gated by
 * VBAT_EN. Leaving it open between readings is what keeps the divider off the
 * cell: 4.2V across 2M5 would otherwise trickle 1.7uA away around the clock.
 */
static const struct gpio_dt_spec vbat_power_en = GPIO_DT_SPEC_GET(DT_NODELABEL(vbat_en_switch), gpios);
static const struct voltage_divider_dt_spec vbat_divider = VOLTAGE_DIVIDER_DT_SPEC_GET(VBAT_NODE);

/* C21 (1nF) sits on the tap, and the divider drives it through R5||R6 = 600k.
 * One time constant is 600us, so the node needs ~3ms after the switch closes
 * before it means anything.
 */
#define VBAT_SETTLE_MS 3

/* Conversions per reading, and why more than one.
 *
 * **Measured: the cell reads 40, 38, 40, 37, 41, 42% on consecutive
 * three-second cycles.** That is not the cell moving, it is a 31mAh cell with
 * high internal resistance being sampled at an arbitrary point relative to a
 * BLE burst -- ~15mA into a couple of ohms is tens of millivolts of sag, and
 * the discharge curve runs 5% per 10mV through the middle, so a single badly
 * timed conversion is worth several percent.
 *
 * Odd, so the median is an actual sample rather than an average of two. Five
 * costs about 100us of SAADC time inside a window where the divider is already
 * powered and the 3ms settle has already been paid -- the expensive parts of a
 * reading happen once either way.
 *
 * A median rather than a mean on purpose: sag is one-sided. A burst during one
 * conversion drags a mean down with it, while the median simply ignores it
 * unless it happened during most of them.
 */
#define VBAT_SAMPLES 5

/* Discharge curve for a single LiPo cell, as open-circuit millivolts against
 * remaining charge. The knees carry the information: the middle of the range
 * is nearly flat, so a linear fit across the whole span would read ~50% for
 * most of the cell's life and then fall off a cliff without warning.
 */
static const struct {
    uint16_t mv;
    uint8_t percent;
} discharge_curve[] = {
    { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020,  80 },
    { 3980, 75 },  { 3950, 70 }, { 3910, 65 }, { 3870, 60 }, { 3850, 55 },
    { 3840, 50 },  { 3820, 45 }, { 3800, 40 }, { 3790, 35 }, { 3770, 30 },
    { 3750, 25 },  { 3730, 20 }, { 3710, 15 }, { 3690, 10 }, { 3610, 5 },
    { 3270, 0 },
};

int battery_init(void)
{
    int ret;

    if (!gpio_is_ready_dt(&vbat_power_en)) {
        LOG_ERR("VBAT divider load switch GPIO not ready");
        return -ENODEV;
    }

    if (!adc_is_ready_dt(&vbat_divider.port)) {
        LOG_ERR("SAADC not ready");
        return -ENODEV;
    }

    /* Inactive: the divider stays disconnected until a reading asks for it. */
    ret = gpio_pin_configure_dt(&vbat_power_en, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to configure VBAT divider load switch (%d)", ret);
        return ret;
    }

    ret = adc_channel_setup_dt(&vbat_divider.port);
    if (ret != 0) {
        LOG_ERR("SAADC channel setup failed (%d)", ret);
        return ret;
    }

    LOG_INF("Battery monitor ready on AIN%d", vbat_divider.port.channel_id);
    return 0;
}

/* Insertion sort, because VBAT_SAMPLES is 5. */
static void sort_small(int16_t *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        int16_t key = v[i];
        size_t j = i;

        while (j > 0 && v[j - 1] > key) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = key;
    }
}

int battery_read_mv(void)
{
    int16_t raw;
    int16_t samples[VBAT_SAMPLES];
    struct adc_sequence sequence = {
        .buffer = &raw,
        .buffer_size = sizeof(raw),
        /* Recalibrating each time costs a few hundred microseconds once every
         * 10s and holds the offset steady as the ring warms against skin.
         */
        .calibrate = true,
    };
    /* Holds the tap voltage first, then the cell voltage: both conversions
     * below rewrite it in place.
     */
    int32_t tap_mv;
    int ret;

    /* Fills in channels/resolution/oversampling only, so the initializer's
     * buffer and calibrate settings above survive. Order can't be flipped.
     */
    ret = adc_sequence_init_dt(&vbat_divider.port, &sequence);
    if (ret != 0) {
        return ret;
    }

    ret = gpio_pin_set_dt(&vbat_power_en, 1);
    if (ret != 0) {
        return ret;
    }

    k_msleep(VBAT_SETTLE_MS);

    /* All VBAT_SAMPLES inside one switch-on: the settle above is paid once and
     * the divider is not toggled between conversions, so this is the cheap part
     * of the reading rather than five readings.
     */
    for (size_t i = 0; i < VBAT_SAMPLES; i++) {
        ret = adc_read_dt(&vbat_divider.port, &sequence);
        if (ret != 0) {
            break;
        }
        samples[i] = raw;
        /* Calibrate on the first conversion only. Four more recalibrations
         * would cost more than the extra conversions they accompany, and the
         * offset cannot drift measurably inside a few hundred microseconds.
         */
        sequence.calibrate = false;
    }

    /* Drop the divider whether or not the conversion worked: returning early on
     * error would latch the switch on and leak for the rest of the cell's life.
     */
    (void)gpio_pin_set_dt(&vbat_power_en, 0);

    if (ret != 0) {
        return ret;
    }

    sort_small(samples, VBAT_SAMPLES);

    /* Single-ended conversions can land a count or two below zero on an empty
     * input; clamp so the millivolt conversion stays sane.
     */
    tap_mv = MAX(samples[VBAT_SAMPLES / 2], 0);

    ret = adc_raw_to_millivolts_dt(&vbat_divider.port, &tap_mv);
    if (ret != 0) {
        return ret;
    }

    /* Scale the tap back up to the cell voltage using R5/R6 from the DT. */
    ret = voltage_divider_scale_dt(&vbat_divider, &tap_mv);
    if (ret != 0) {
        return ret;
    }

    return (int)tap_mv;
}

uint8_t battery_percent(uint16_t mv)
{
    if (mv >= discharge_curve[0].mv) {
        return discharge_curve[0].percent;
    }

    for (size_t i = 1; i < ARRAY_SIZE(discharge_curve); i++) {
        if (mv >= discharge_curve[i].mv) {
            /* Interpolate between the bracketing points. */
            const uint16_t span_mv = discharge_curve[i - 1].mv - discharge_curve[i].mv;
            const uint8_t span_pct = discharge_curve[i - 1].percent - discharge_curve[i].percent;

            return discharge_curve[i].percent + ((mv - discharge_curve[i].mv) * span_pct) / span_mv;
        }
    }

    return 0;
}

/* ==========================================================================
 * The reported level
 *
 * `battery_percent()` above is the curve, and it is honest about a single
 * voltage. What a person reads off a phone needs two more things it cannot
 * give them.
 *
 * **It must not climb while the ring is discharging.** A gauge that goes 40,
 * 38, 41, 37 teaches the wearer that none of the numbers mean anything, and the
 * one number this device needs believed is the one that says charge it. Sag is
 * one-sided -- the cell reads low under load and recovers, never the reverse --
 * so an upward step is nearly always noise and a downward step nearly always
 * real. The clamp is asymmetric for that reason: falls are taken immediately,
 * rises have to be earned.
 *
 * **It must still notice a charger.** So a rise is accepted once the smoothed
 * voltage has genuinely moved up by more than sag accounts for, which is what
 * CHARGE_MV is. Below that a rise is absorbed into the average and changes
 * nothing.
 *
 * CHARGE_MV arms once, not once per step. It answers "is this cell recovering
 * or is this noise", and that question only has to be asked while the answer is
 * still no. Re-asking it after every accepted rise makes 40mV the smallest
 * upward movement the level can ever make, and 40mV through the flat middle of
 * the curve is a dozen percent -- a charging ring would climb 17, 29, 43, 56
 * rather than counting up. So once a rise is believed the ring stays in the
 * rising state, following the cell cycle by cycle, until it falls again.
 *
 * The EMA is over millivolts rather than percent, deliberately: averaging
 * percentages averages the curve's own steep middle, where 10mV is 5%, and
 * would smooth hardest exactly where the resolution matters most.
 * ========================================================================== */

/* Weight of each new reading in the average, as a reciprocal: 4 means the new
 * sample is a quarter of the result. At the shipping cadence a cycle is two
 * minutes, so this settles in roughly eight.
 */
#define VBAT_EMA_SHIFT 2

/* How far the smoothed voltage must rise before the level is allowed to climb.
 * Comfortably more than a BLE burst's sag and comfortably less than what a
 * charger does in a couple of minutes.
 */
#define CHARGE_MV 40

/* Most percentage points the reported level may climb in one reading.
 *
 * **This exists because arming CHARGE_MV is a cliff:** putting the ring on a
 * charger moves the gauge 5-10 points in a single step. The comment above
 * explains why the rise gate arms
 * once rather than per step -- re-arming makes 40mV the smallest upward movement
 * the level can ever make -- but arming once does not remove that first 40mV
 * worth of suppressed error, it concentrates it. Through the flat middle of the
 * curve 40mV is about a dozen points, and the moment the gate clears, all of it
 * lands at once.
 *
 * So the release is spread instead of snapped. Nothing else changes: the gate,
 * the hysteresis and the immediate-fall rule are all as they were.
 *
 * **4 is chosen to bind on the release and not on real charging.** A 31mAh cell
 * charged at 1C is full in an hour, which at the shipping cadence of one reading
 * per ~121s is about 3.4 points per reading -- so genuine charging stays under
 * this ceiling and is reported as fast as it happens, while a twelve-point
 * release becomes three steps rather than one jump.
 *
 * **It is display smoothing, not a physical model.** The gauge infers charging
 * from a voltage rise because nothing tells it the truth: the BQ25180 knows,
 * and this firmware does not address it. Given that bit, the clamp could be
 * skipped outright while charging and this ceiling would have nothing left to
 * do.
 */
#define LEVEL_RISE_MAX_PCT 4

static uint16_t s_ema_mv;      /* 0 until the first reading */
static uint8_t s_reported;
static uint16_t s_rise_floor;  /* the voltage a rise is measured against */
static bool s_rising;          /* CHARGE_MV has been cleared and not fallen back */

uint8_t battery_level(uint16_t mv)
{
    uint8_t raw;

    if (s_ema_mv == 0) {
        /* First reading of the session. Nothing to smooth against, and no
         * reason to make the wearer wait eight cycles for a number.
         */
        s_ema_mv = mv;
        s_rise_floor = mv;
        s_rising = false;
        s_reported = battery_percent(mv);
        return s_reported;
    }

    s_ema_mv = (uint16_t)(s_ema_mv + ((int32_t)mv - (int32_t)s_ema_mv) / (1 << VBAT_EMA_SHIFT));
    raw = battery_percent(s_ema_mv);

    if (raw < s_reported) {
        /* Downward is believed straight away. A cell that is actually emptying
         * must never be reported as fuller than it is, and the cost of
         * believing a spuriously low reading is an early warning rather than a
         * late one.
         */
        s_reported = raw;
        s_rise_floor = s_ema_mv;
        /* Whatever recovery was underway is over; the next rise has to earn
         * CHARGE_MV again from here.
         */
        s_rising = false;
    } else if (raw > s_reported && (s_rising || s_ema_mv > s_rise_floor + CHARGE_MV)) {
        /* Genuinely recovering: on a charger, or off a heavy load for long
         * enough that the cell has settled. Either way the rise is real, and
         * stays real until the cell turns back around -- so follow it from here
         * rather than making each further percent clear the gate again.
         */
        uint8_t gap = (uint8_t)(raw - s_reported);

        s_rising = true;
        s_reported += MIN(gap, LEVEL_RISE_MAX_PCT);

        /* The floor follows the *voltage*, not the clamped level, so a release
         * that takes several readings to play out does not have to re-earn
         * CHARGE_MV part-way through. `s_rising` already holds the gate open;
         * moving the floor here keeps the two consistent for the fall path,
         * which measures the next rise from wherever the cell actually is.
         */
        s_rise_floor = s_ema_mv;
    }

    return s_reported;
}

void battery_level_reset(void)
{
    s_ema_mv = 0;
    s_reported = 0;
    s_rise_floor = 0;
    s_rising = false;
}
