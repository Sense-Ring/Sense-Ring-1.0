/* MAX30102 PPG front end. See ARCHITECTURE.md §3 for the reasoning behind the
 * register values, the power-up ordering and the FIFO pointer arithmetic.
 */
#include "ppg.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(main);

/* MAX30102 register map */
#define REG_INT_STATUS_1 0x00
#define REG_FIFO_WR_PTR  0x04
#define REG_OVF_COUNTER  0x05
#define REG_FIFO_RD_PTR  0x06
#define REG_FIFO_DATA    0x07
#define REG_FIFO_CONFIG  0x08
#define REG_MODE_CONFIG  0x09
#define REG_SPO2_CONFIG  0x0A
#define REG_LED1_PA      0x0C
#define REG_LED2_PA      0x0D
#define REG_PART_ID      0xFF

#define PART_ID_MAX30102 0x15

#define MODE_CONFIG_SHDN  BIT(7)
#define MODE_CONFIG_RESET BIT(6)
#define MODE_SPO2         0x03 /* drives RED and IR */

/* The FIFO is 32 entries deep and packs 3 bytes per LED channel, so a
 * SpO2-mode entry is 6 bytes. Counts are 18-bit, right-justified.
 */
#define FIFO_DEPTH        32
#define FIFO_SAMPLE_BYTES 6
#define FIFO_PTR_MASK     0x1F
#define ADC_COUNT_MASK    0x03FFFFU

/* FIFO_CONFIG: SMP_AVE[7:5]=001 (average 2), FIFO_ROLLOVER_EN[4]=1,
 * FIFO_A_FULL[3:0]=0 (unused, we poll). Rollover matters: without it the FIFO
 * wedges permanently the first time we fall behind.
 */
#define FIFO_CONFIG_VALUE ((0x01 << 5) | BIT(4))

/* SPO2_CONFIG: SPO2_ADC_RGE[6:5]=01 (4096nA full scale), SPO2_SR[4:2]=000
 * (50Hz), LED_PW[1:0]=11 (411us). Pulse width and ADC resolution are the same
 * setting on this part; 411us is the only one that gives the full 18 bits, and
 * a ring reflects far less light back than a fingertip clip, so the bits are
 * worth their energy. Shortening the pulse is the next power knob if the rate
 * and the duty cycle are not enough -- 118us would cut LED energy 3.5x and
 * cost two bits.
 *
 * 50Hz against the averaging above yields 25 output samples/sec: LED on-time,
 * and so LED energy, is directly proportional to this. See ARCHITECTURE.md
 * §3.2 for why 25Hz is enough to recover a heart rate.
 */
#define SPO2_CONFIG_VALUE ((0x01 << 5) | (0x00 << 2) | 0x03)

/* LED drive current, 0.2mA per step. 0x32 = 10mA, a sane starting point for a
 * ring held against skin. Raise it if the DC level is too low to see a pulse.
 */
#define LED_CURRENT_RED 0x32
#define LED_CURRENT_IR  0x32

/* VLED+ reaches the MAX30102 only through U3, a WS4622C load switch gated by
 * LED_EN. Its enable is active high with a 4M internal pull-down, so the LEDs
 * stay dark until we drive this pin, even though VDD (1.8V) keeps the I2C side
 * of the chip fully responsive.
 */
static const struct gpio_dt_spec ppg_power_en = GPIO_DT_SPEC_GET(DT_NODELABEL(led_en_switch), gpios);
static const struct i2c_dt_spec ppg_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(max30102));

/* U3 switches in ~70us, but VLED carries ~11uF of bulk to charge. */
#define VLED_SETTLE_MS 5

/* The first conversions after the rail comes up ride the settling transient.
 * Let them happen, then throw them away by clearing the FIFO underneath them:
 * 100ms on an 8s window is a rounding error, and a ramp at the head of the
 * window would defeat the DC removal in vitals.c.
 */
#define PPG_WARMUP_MS 100

/* Zeroing all three leaves the FIFO empty and aligned. They are contiguous but
 * write as three separate registers.
 */
static int fifo_reset(void)
{
    static const uint8_t ptrs[][2] = {
        { REG_FIFO_WR_PTR, 0x00 },
        { REG_OVF_COUNTER, 0x00 },
        { REG_FIFO_RD_PTR, 0x00 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(ptrs); i++) {
        int ret = i2c_reg_write_byte_dt(&ppg_i2c, ptrs[i][0], ptrs[i][1]);

        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int max30102_reset(void)
{
    int ret = i2c_reg_write_byte_dt(&ppg_i2c, REG_MODE_CONFIG, MODE_CONFIG_RESET);

    if (ret != 0) {
        return ret;
    }

    /* The RESET bit self-clears once the register bank is back to defaults. */
    for (int i = 0; i < 20; i++) {
        uint8_t mode;

        k_msleep(5);
        ret = i2c_reg_read_byte_dt(&ppg_i2c, REG_MODE_CONFIG, &mode);
        if (ret != 0) {
            return ret;
        }
        if ((mode & MODE_CONFIG_RESET) == 0) {
            return 0;
        }
    }

    return -ETIMEDOUT;
}

/* Everything that has to be in place before the first conversion. MODE_CONFIG is
 * deliberately absent: writing it is what starts the LEDs, and that is
 * ppg_start()'s job.
 */
static const uint8_t ppg_config[][2] = {
    { REG_FIFO_CONFIG, FIFO_CONFIG_VALUE },
    { REG_SPO2_CONFIG, SPO2_CONFIG_VALUE },
    { REG_LED1_PA, LED_CURRENT_RED },
    { REG_LED2_PA, LED_CURRENT_IR },
};

static int write_config(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(ppg_config); i++) {
        int ret = i2c_reg_write_byte_dt(&ppg_i2c, ppg_config[i][0], ppg_config[i][1]);

        if (ret != 0) {
            LOG_ERR("Write 0x%02x to reg 0x%02x failed (%d)", ppg_config[i][1], ppg_config[i][0],
                    ret);
            return ret;
        }
    }

    return 0;
}

int ppg_init(void)
{
    uint8_t part_id;
    int ret;

    if (!gpio_is_ready_dt(&ppg_power_en)) {
        LOG_ERR("VLED load switch GPIO not ready");
        return -ENODEV;
    }

    if (!device_is_ready(ppg_i2c.bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&ppg_power_en, GPIO_OUTPUT_ACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to enable VLED load switch (%d)", ret);
        return ret;
    }

    k_msleep(VLED_SETTLE_MS);

    ret = i2c_reg_read_byte_dt(&ppg_i2c, REG_PART_ID, &part_id);
    if (ret != 0) {
        LOG_ERR("MAX30102 did not respond at 0x%02x (%d)", ppg_i2c.addr, ret);
        goto power_off;
    }

    if (part_id != PART_ID_MAX30102) {
        LOG_ERR("Unexpected part ID 0x%02x, expected 0x%02x", part_id, PART_ID_MAX30102);
        ret = -ENODEV;
        goto power_off;
    }

    ret = max30102_reset();
    if (ret != 0) {
        LOG_ERR("MAX30102 reset failed (%d)", ret);
        goto power_off;
    }

    ret = write_config();
    if (ret != 0) {
        goto power_off;
    }

    /* Configured, verified and dark. The rail stays down until a window wants
     * it -- there is no reason to burn LED current between measurements.
     */
    ppg_stop();
    LOG_INF("MAX30102 ready: RED+IR at %d Hz, drive 0x%02x, rail off", PPG_SAMPLE_RATE_HZ,
            LED_CURRENT_RED);
    return 0;

power_off:
    gpio_pin_set_dt(&ppg_power_en, 0);
    return ret;
}

int ppg_start(void)
{
    uint8_t drive;
    int ret = gpio_pin_set_dt(&ppg_power_en, 1);

    if (ret != 0) {
        LOG_ERR("Failed to raise VLED rail (%d)", ret);
        return ret;
    }

    k_msleep(VLED_SETTLE_MS);

    /* Check the part still holds the configuration ppg_init() gave it, and put
     * it back if it does not.
     *
     * Shutdown retains every register; a *reset* does not, and the two are
     * indistinguishable from here unless something checks. Writing the
     * configuration once at boot and never looking at it again is what this
     * read-back exists to avoid.
     *
     * What that would cost is specific and silent. On POR the MAX30102 clears
     * LED1_PA and LED2_PA to 0x00 -- **the LEDs go dark** -- and SPO2_CONFIG to
     * a 69us pulse instead of 411us. Every call here would still succeed, the
     * FIFO would still fill, and every sample would be the dark level. From the
     * outside that is indistinguishable from a ring nobody is wearing, and it
     * would stay that way until the next reboot, because the only other code
     * that writes LED_PA runs at boot.
     *
     * The part's VDD is the always-on 1.8V rail and no load switch gates it
     * (ARCHITECTURE.md 3.7), so a read-back is the only way to know the part
     * restarted underneath us. If the warning below prints, it did.
     *
     * One register read on the normal path, and only on a mismatch does it cost
     * four writes. LED1_PA is the one worth reading because it is the register
     * whose default does the damage.
     */
    ret = i2c_reg_read_byte_dt(&ppg_i2c, REG_LED1_PA, &drive);
    if (ret != 0) {
        LOG_ERR("Could not read back LED drive (%d)", ret);
        goto power_off;
    }

    if (drive != LED_CURRENT_RED) {
        LOG_WRN("MAX30102 lost its configuration (LED drive 0x%02x, expected 0x%02x) -- the part "
                "reset underneath us and the LEDs have been dark since. Reconfiguring.",
                drive, LED_CURRENT_RED);
        ret = write_config();
        if (ret != 0) {
            goto power_off;
        }
    }

    /* Clears SHDN and starts the LEDs pulsing. */
    ret = i2c_reg_write_byte_dt(&ppg_i2c, REG_MODE_CONFIG, MODE_SPO2);
    if (ret != 0) {
        LOG_ERR("Failed to start MAX30102 (%d)", ret);
        goto power_off;
    }

    k_msleep(PPG_WARMUP_MS);

    /* Drops whatever the warm-up produced, so the window begins at the first
     * settled sample.
     */
    ret = fifo_reset();
    if (ret != 0) {
        LOG_ERR("Failed to clear FIFO (%d)", ret);
        goto power_off;
    }

    return 0;

power_off:
    ppg_stop();
    return ret;
}

int ppg_read_fifo(struct ppg_sample *samples, size_t max_samples)
{
    uint8_t buf[FIFO_DEPTH * FIFO_SAMPLE_BYTES];
    uint8_t ptrs[3];
    unsigned int available;
    int ret;

    if (samples == NULL || max_samples == 0) {
        return -EINVAL;
    }

    /* WR_PTR, OVF_COUNTER and RD_PTR are contiguous, so one burst gets all
     * three and keeps them consistent with each other.
     */
    ret = i2c_burst_read_dt(&ppg_i2c, REG_FIFO_WR_PTR, ptrs, sizeof(ptrs));
    if (ret != 0) {
        return ret;
    }

    /* Modular subtraction on 5-bit pointers: the mask is what makes this give
     * the right distance once the write pointer has wrapped past the read one.
     */
    available = (ptrs[0] - ptrs[2]) & FIFO_PTR_MASK;

    /* Equal pointers are the one ambiguous state: the FIFO reads as empty when
     * it is untouched and identically empty when it wrapped exactly and is
     * full. That tie is the only thing OVF_COUNTER is needed for, and the only
     * state in which a sample can have been lost at all -- below FIFO_DEPTH
     * unread the delta above is the whole truth, whatever the counter says.
     * Reading it as a standalone overflow flag warned on every poll and, worse,
     * would have called an empty FIFO full and spliced FIFO_DEPTH stale entries
     * into the window.
     */
    if (available == 0) {
        if (ptrs[1] == 0) {
            return 0;
        }

        LOG_WRN("FIFO overflow, lost %u samples", ptrs[1]);
        available = FIFO_DEPTH;
    }

    available = MIN(available, max_samples);

    ret = i2c_burst_read_dt(&ppg_i2c, REG_FIFO_DATA, buf, available * FIFO_SAMPLE_BYTES);
    if (ret != 0) {
        return ret;
    }

    for (unsigned int i = 0; i < available; i++) {
        const uint8_t *entry = &buf[i * FIFO_SAMPLE_BYTES];

        /* 18 bits right-justified in 3 bytes: the top 6 carry no data, and left
         * unmasked they surface as sporadic huge values that mimic motion.
         */
        samples[i].red = sys_get_be24(&entry[0]) & ADC_COUNT_MASK;
        samples[i].ir = sys_get_be24(&entry[3]) & ADC_COUNT_MASK;
    }

    return available;
}

/* How many times to insist on the shutdown write before giving up on it. See
 * ppg_stop() for why this stopped being a fire-and-forget write.
 */
#define SHUTDOWN_RETRIES 3

void ppg_stop(void)
{
    int ret = -EIO;
    int attempts;

    /* SHDN first, then the rail: telling the chip to stop before yanking its
     * LED supply is politer than the reverse, and it drops the chip's own draw
     * to ~0.7uA for the gap until the next window.
     *
     * **Retried and reported rather than fired and forgotten.** VDD is the
     * always-on 1.8V rail and no load switch reaches it (ARCHITECTURE.md 3.7),
     * so this register write is the *only* thing between the part and ~600uA. A
     * write that quietly failed would leave the MAX30102 converting in SpO2
     * mode for the whole PAUSE -- 90 of every 121 seconds -- and every later
     * cycle would do the same, because nothing anywhere would have said so.
     * That is a silent ~600uA against a device budget of ~305uA (5.1.1), and it
     * is invisible in the log by construction.
     *
     * Retried *before* the rail drops rather than after only because the bus is
     * more likely to be idle here; the part answers on VDD either way, so a
     * later attempt would work equally well. Three tries because the plausible
     * failure is a momentarily busy bus, not a dead part -- if the part is
     * dead, the next ppg_start() says so far more usefully than this can.
     */
    for (attempts = 0; attempts < SHUTDOWN_RETRIES && ret != 0; attempts++) {
        ret = i2c_reg_write_byte_dt(&ppg_i2c, REG_MODE_CONFIG, MODE_CONFIG_SHDN);
    }

    /* A write that failed once and succeeded on the retry is the interesting
     * case, and it has to be said out loud: without this line "the write never
     * fails" and "it fails every time and the retry is quietly saving 600uA"
     * produce identical logs, which makes an unreliable bus invisible in
     * exactly the discharge numbers it would explain.
     */
    if (ret == 0 && attempts > 1) {
        LOG_WRN("MAX30102 shutdown write needed %d attempts -- the I2C bus is "
                "unreliable here, and before the retries this would have left the "
                "part drawing ~600uA",
                attempts);
    }

    if (ret != 0) {
        /* An error rather than a warning, and it names the current: this is the
         * one failure in this file whose whole consequence is battery life, and
         * a line that only said "I2C write failed" would be filed next to the
         * harmless ones.
         */
        LOG_ERR("MAX30102 shutdown write failed (%d) -- the part is still in SpO2 mode "
                "on the always-on 1.8V rail and will draw ~600uA until the next window",
                ret);
    }

    /* U3 actively discharges VLED once EN drops. Unconditional: whatever
     * happened above, the LEDs are the expensive half and they come down.
     */
    (void)gpio_pin_set_dt(&ppg_power_en, 0);
}
