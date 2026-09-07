/* BMA530 accelerometer over I2C. See ARCHITECTURE.md §3A.
 *
 * Note the part number. The schematic (designator AC2) fits a **BMA530**, which
 * is a different generation from the BMA456 the DTS node and this document once
 * named -- different register map, different chip ID, different startup
 * sequence. Nothing about a BMA456 driver works on this part, and the way it
 * fails is not obvious (see the dummy read below).
 *
 * Deliberately minimal: chip ID, health, range, output rate, data registers.
 * The BMA530's step counter, tilt, orientation and any-motion features run in
 * its "feature engine", and none of them is needed to read acceleration.
 *
 * Note for whoever adds the step counter: unlike the BMA456, this part needs no
 * configuration blob -- the feature engine is enabled out of reset, and the
 * 24-bit step count is readable straight from the ordinary register map at
 * 0x57-0x59. Enabling it is FEAT_ENG_GPR_0.step_en (bit 3 of 0x55) followed by
 * FEAT_ENG_GPR_CTRL.update_gprs (bit 0 of 0x54) to commit it. Beware that the
 * datasheet's §4.9.5 prose names the wrong output registers; the register map
 * is the correct one. See ARCHITECTURE.md §3A.8.
 *
 * Register addresses and field values below are from the BMA530 datasheet,
 * document BST-BMA530-DS000-04.
 */
#include "imu.h"
#include <errno.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(main);

#define REG_CHIP_ID       0x00
#define REG_HEALTH_STATUS 0x02
#define REG_ACC_DATA      0x18 /* X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB */
#define REG_ACC_CONF_0    0x30
#define REG_ACC_CONF_1    0x31
#define REG_ACC_CONF_2    0x32
#define REG_FEAT_ENG_GPR_CTRL 0x54
#define REG_FEAT_ENG_GPR_0    0x55
#define REG_CMD           0x7E

/* The 24-bit step count, low byte first.
 *
 * NOTE: the datasheet contradicts itself about where this lives. The prose in
 * §4.9.5 names FEAT_ENG_GPR_1/2/3; the register map table and the per-register
 * descriptions both say FEAT_ENG_GPR_2/3/4 at 0x57-0x59, with explicit "Step
 * counter value byte-0/1/2" field definitions. The register map is the correct
 * one -- following the prose would read gen_int*_data_src as the low byte and
 * produce quiet nonsense.
 */
#define REG_STEP_COUNT 0x57

/* FEAT_ENG_GPR_0.step_en, the common enable for counter and detector. */
#define FEAT_ENG_GPR_0_STEP_EN BIT(3)

/* FEAT_ENG_GPR_CTRL.update_gprs: copies the host-owned first-stage GPRs into
 * the second stage. A GPR write that is never committed does nothing at all,
 * and does it silently.
 */
#define FEAT_ENG_GPR_CTRL_UPDATE BIT(0)

/* Bosch's product identifier for the BMA530. A BMA456 would answer 0x16 here,
 * which is how the part swap was found in the first place.
 */
#define CHIP_ID_BMA530 0xC2

/* HEALTH_STATUS low nibble. 0xF means the analog front end powered up cleanly;
 * anything else means it did not, and the acceleration would be meaningless.
 */
#define HEALTH_OK        0x0F
#define HEALTH_MASK      0x0F

#define CMD_SOFTRESET    0xB6

/* ACC_CONF_0.sensor_ctrl: all four bits set enables the accelerometer, all
 * clear disables it. 0xE is the part telling us it rejected the configuration.
 */
#define SENSOR_CTRL_ENABLE  0x0F
#define SENSOR_CTRL_DISABLE 0x00
#define SENSOR_CTRL_BAD_CFG 0x0E

/* ACC_CONF_1: [7] power_mode, [6:4] acc_bwp, [3:0] acc_odr.
 *
 * LPM (duty cycling), not HPM. This is the single most consequential constant
 * in the file now that the accelerometer runs continuously rather than only in
 * a bench mode, and the datasheet's own numbers are why (table 1, table 8, at
 * the 1.8V rail this board runs):
 *
 *     HPM, any ODR        ~125uA
 *     LPM, 50Hz, avg4      ~15uA
 *     suspend             ~4.75uA
 *
 * The trap is in §4.2.4: "only in LPM does the overall power consumption depend
 * strongly on the chosen ODR". HPM costs ~125uA whether it is clocked at 50Hz
 * or 1600Hz, so lowering the rate in HPM saves nothing at all -- the *mode* is
 * the knob, not the rate. Against a ~290uA device budget (ARCHITECTURE.md
 * §5.1.1) that is the difference between the IMU costing 30% of the battery and
 * costing 5%: about a day and a quarter of the four the cell is good for.
 *
 * 50Hz rather than something slower because it is the slowest rate at which the
 * BMA530's step counter will run in LPM (datasheet table 20, LPM requires
 * >= 50Hz). Dropping to 25Hz saves 4uA and closes that door.
 *
 * In LPM, acc_bwp selects how many samples are averaged rather than a filter
 * shape -- so this field is an averaging depth here, not "normal mode".
 *
 * No averaging, deliberately. The step counter is a feature-engine algorithm
 * keyed on the shape of the per-stride acceleration peak, and averaging four
 * samples at 50Hz is a 12.5Hz box filter sitting directly on top of that peak:
 * it lowers and widens exactly the feature being detected, which shows up as a
 * walk counted as fewer, later steps. Averaging is also the only field in this
 * register that touches the step path without touching the power number -- ODR
 * and power mode both do.
 *
 * It costs nothing either way: acc_bwp does not appear in the datasheet's
 * current tables, so LPM at 50Hz is ~15uA regardless. HPM is where the step
 * algorithm is properly at home, but the part costs ~125uA there.
 */
#define ACC_ODR_50HZ    0x05
#define ACC_BWP_AVG1    0x00 /* normal mode in HPM, no averaging in LPM */
#define ACC_POWER_LPM   0
#define ACC_POWER_HPM   1
#define ACC_CONF_1_VALUE ((ACC_POWER_LPM << 7) | (ACC_BWP_AVG1 << 4) | ACC_ODR_50HZ)

/* ACC_CONF_2: [7] drdy auto-clear, [4] noise mode, [3:2] IIR roll-off, [1:0] range.
 *
 * +/-4g. 2g would resolve a gentle movement twice as finely, and for the
 * movement signal alone it would be the better choice; 4g is picked because an
 * ordinary knock lands well past 2g and a clipped impact is indistinguishable
 * from a lesser one. The IIR roll-off keeps its -60dB reset default, and the noise
 * mode its low-noise default.
 */
#define ACC_RANGE_4G    0x01
#define ACC_IIR_60DB    0x03
#define ACC_CONF_2_VALUE ((ACC_IIR_60DB << 2) | ACC_RANGE_4G)

/* Datasheet table 7: at +/-4g, 1g = 8192 LSB. Unlike the BMA456 the data is a
 * full 16-bit two's-complement value, not 12 bits left-justified in 16 -- so
 * there is no shift here, and a driver carried over from that part would read
 * every value 16x too small.
 */
#define LSB_PER_G 8192

/* The datasheet's reserved "data not valid" pattern: set after power-on before
 * the first conversion completes, and when the part has rejected the
 * configuration. Worth detecting rather than reporting as -4g.
 */
#define ACC_DATA_INVALID ((int16_t)0x8000)

/* Datasheet asks for 3ms after power-on or soft reset before the first
 * transaction. 15ms is generous margin on a path that runs once at boot, and a
 * soft reset is the heavier of the two cases -- it is "largely equivalent to a
 * power cycle" (§4.7), not a register write that settles immediately.
 */
#define IMU_RESET_MS 15

/* How many times to ask for the chip ID before giving up. The first attempt is
 * expected to fail -- it is the interface-selecting transaction the part does
 * not acknowledge -- so anything less than 2 cannot work. This is what makes
 * the soft reset above safe: however many throwaway transactions the reset
 * costs, the loop absorbs them instead of the driver having to predict the
 * count. Cheap on a path that runs once.
 */
#define IMU_ID_ATTEMPTS 8

/* Health is polled rather than read once: after a reset the part needs a moment
 * to finish its internal checks, and 0x0F does not appear instantly.
 */
#define IMU_HEALTH_ATTEMPTS 10

/* How many times imu_start() asks for a sample before deciding the part is
 * enabled but not converting. Spread across IMU_WARMUP_MS.
 */
#define IMU_DATA_ATTEMPTS 5

/* How long to wait after enabling before the first sample is expected.
 *
 * 40ms was enough in HPM and is not in LPM. At 50Hz with 4-sample averaging the
 * part needs roughly four output periods (~80ms) before a first valid sample,
 * and the datasheet (§4.2.6) warns that in LPM "the time interval between the
 * first and second samples is not as expected" by design. 200ms covers both
 * with room to spare, on a path that runs once at boot.
 */
#define IMU_WARMUP_MS 200

static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(bma530));

/* Everything that must be correct before the first conversion. A table rather
 * than a run of i2c_reg_write_byte_dt() calls so the error handling lives in
 * one place -- same reasoning as ppg_init()'s config array.
 *
 * ACC_CONF_0 is deliberately absent: its sensor_ctrl field is what enables the
 * accelerometer, and enabling belongs to imu_start(). Same split as
 * MODE_CONFIG in ppg.c.
 */
static const uint8_t config[][2] = {
    { REG_ACC_CONF_1, ACC_CONF_1_VALUE },
    { REG_ACC_CONF_2, ACC_CONF_2_VALUE },
};

/* One axis, from two little-endian bytes to milli-g. */
static int16_t axis_milli_g(const uint8_t *le16, bool *valid)
{
    int16_t counts = (int16_t)sys_get_le16(le16);

    if (counts == ACC_DATA_INVALID) {
        *valid = false;
        return 0;
    }

    /* +/-4g is +/-32768 counts, so the milli-g result is at most +/-4000 and
     * stays inside int16_t. The int32_t cast is on the multiply, where a 16-bit
     * intermediate would overflow long before the divide brought it back.
     */
    return (int16_t)(((int32_t)counts * 1000) / LSB_PER_G);
}

int imu_init(void)
{
    uint8_t id, health, gpr0;
    int ret;

    if (!device_is_ready(imu_i2c.bus)) {
        LOG_ERR("I2C bus not ready for IMU");
        return -ENODEV;
    }

    /* Soft reset, and this driver does need one.
     *
     * An earlier version dropped it, on the theory that the reset write and the
     * documented "first transaction is not acknowledged" behaviour were
     * indistinguishable and the reset was wedging the interface. That was half
     * right -- they *are* indistinguishable -- but the conclusion was wrong, and
     * the retry loop below is the actual answer to it.
     *
     * Without a reset this driver is stateless across an SoC reset while the
     * part is not: the BMA530 keeps its configuration, its power state and its
     * health status through a reflash, because nothing removed its supply. So
     * every boot after the first inherited whatever the previous run left --
     * including the disabled front end imu_stop() leaves behind, which reads
     * back as an unhealthy device. The symptom was an IMU that worked exactly
     * once, on the first boot after a real power cycle, and never again.
     *
     * The datasheet calls this reset "largely equivalent to a power cycle"
     * (§4.7) and names it as the remedy for a bad health status (§4.8), which
     * is precisely the situation it is being used for here.
     *
     * The return is discarded on purpose: a soft reset issued over I2C is
     * documented as *not* acknowledged, so a NACK here is success. Treating it
     * as a failure would abort every boot on a healthy part.
     */
    (void)i2c_reg_write_byte_dt(&imu_i2c, REG_CMD, CMD_SOFTRESET);
    k_msleep(IMU_RESET_MS);

    /* The first transaction selects the serial interface and is not
     * acknowledged, so it cannot also be the one that returns the chip ID.
     * Rather than assume exactly one throwaway is needed, try a few times and
     * say how many it actually took -- the count is a fact about the part worth
     * knowing, and hard-coding a guess is what got the previous two attempts.
     */
    for (int attempt = 0; attempt < IMU_ID_ATTEMPTS; attempt++) {
        ret = i2c_reg_read_byte_dt(&imu_i2c, REG_CHIP_ID, &id);
        if (ret == 0) {
            if (attempt > 0) {
                LOG_INF("IMU answered on attempt %d", attempt + 1);
            }
            break;
        }
        k_msleep(2);
    }

    if (ret != 0) {
        LOG_ERR("IMU chip ID read failed (%d) after %d attempts at 0x%02x", ret,
                IMU_ID_ATTEMPTS, imu_i2c.addr);
        return ret;
    }
    if (id != CHIP_ID_BMA530) {
        LOG_ERR("Unexpected IMU chip ID 0x%02x (expected 0x%02x for BMA530)", id,
                CHIP_ID_BMA530);
        return -ENODEV;
    }

    /* Distinct from the ID check: the ID says the right part is on the bus, the
     * health status says its analog front end came up. A part that answers but
     * reports bad health produces plausible-looking acceleration that is not
     * measuring anything.
     */
    /* Health is a *diagnostic*, not a gate. Polled because the part needs a
     * moment after a reset to finish its internal checks -- HEALTH_STATUS reset
     * value is 0x00 and only becomes 0x0F once they pass, so reading it once
     * immediately is reading it too early.
     */
    health = 0;
    for (int attempt = 0; attempt < IMU_HEALTH_ATTEMPTS; attempt++) {
        ret = i2c_reg_read_byte_dt(&imu_i2c, REG_HEALTH_STATUS, &health);
        if (ret == 0 && (health & HEALTH_MASK) == HEALTH_OK) {
            break;
        }
        k_msleep(2);
    }

    /* Deliberately a warning and not a failure.
     *
     * The datasheet says any value but 0x0F indicates an internal error, and an
     * earlier version of this file duly returned -EIO on one. That cost the
     * whole motion feature on a part that was demonstrably working: it read its
     * chip ID correctly and, once started, produced acceleration whose
     * magnitude came out at 991mg against a true 1g.
     *
     * So health is reported and moved past. The check that actually decides
     * whether this part is usable is in imu_start(), which refuses unless a
     * real conversion arrives -- that is a measurement of the thing we care
     * about rather than a proxy for it, and it cannot pass on a dead front end.
     */
    if ((health & HEALTH_MASK) != HEALTH_OK) {
        LOG_WRN("IMU health 0x%02x, wanted low nibble 0x%02x -- continuing, "
                "imu_start() will decide on real data",
                health, HEALTH_OK);
    }

    /* Disable the accelerometer before reconfiguring it.
     *
     * This is not tidiness and it is easy to miss: ACC_CONF_0 comes out of
     * reset at 0x0F, which means **the accelerometer is already running when
     * this function first talks to the part**. The datasheet (§4.2.6) is
     * explicit that configuration should be changed with it disabled, and
     * changes are applied immediately -- so writing ODR, power mode and range
     * underneath a running conversion is how the part ends up latching
     * sensor_ctrl to 0x0E ("a wrong configuration was found") and refusing to
     * enable afterwards.
     *
     * The failure is downstream of here and looks nothing like its cause:
     * imu_start() reports a rejected configuration, imu_ready stays false, and
     * the firmware silently loses both the motion stream and the movement-gated
     * probe in §5.3.1.
     */
    ret = i2c_reg_write_byte_dt(&imu_i2c, REG_ACC_CONF_0, SENSOR_CTRL_DISABLE);
    if (ret != 0) {
        LOG_ERR("Failed to disable accelerometer before configuring (%d)", ret);
        return ret;
    }

    for (size_t i = 0; i < ARRAY_SIZE(config); i++) {
        ret = i2c_reg_write_byte_dt(&imu_i2c, config[i][0], config[i][1]);
        if (ret != 0) {
            LOG_ERR("IMU config write 0x%02x failed (%d)", config[i][0], ret);
            return ret;
        }
    }

    /* Enable the step counter.
     *
     * Two writes, and the second is the one that is easy to omit: FEAT_ENG_GPR_0
     * is a *staged* register, so setting step_en does nothing until update_gprs
     * copies the host-owned first stage into the second. There is no error for
     * forgetting it -- the feature simply never turns on.
     *
     * Read-modify-write rather than a blind store: every other bit in this
     * register enables a different feature (tilt, orientation, significant
     * motion, the three generic interrupts), and clobbering them would be a
     * silent regression the moment anything else here starts using one.
     *
     * The finer configuration -- sc_en, sd_en, watermark_level -- lives in the
     * extended register map at STEP_COUNTER (extended address 0x19), reached
     * through FEATURE_DATA_ADDR/FEATURE_DATA_TX. It needs no touching: its reset
     * value is 0x1800, which already has both sc_en and sd_en set. If the count
     * ever reads a stubborn zero, that register is the first place to look.
     */
    ret = i2c_reg_read_byte_dt(&imu_i2c, REG_FEAT_ENG_GPR_0, &gpr0);
    if (ret != 0) {
        LOG_ERR("IMU feature register read failed (%d)", ret);
        return ret;
    }

    ret = i2c_reg_write_byte_dt(&imu_i2c, REG_FEAT_ENG_GPR_0,
                                gpr0 | FEAT_ENG_GPR_0_STEP_EN);
    if (ret != 0) {
        LOG_ERR("Failed to enable step counter (%d)", ret);
        return ret;
    }

    ret = i2c_reg_write_byte_dt(&imu_i2c, REG_FEAT_ENG_GPR_CTRL, FEAT_ENG_GPR_CTRL_UPDATE);
    if (ret != 0) {
        LOG_ERR("Failed to commit step counter enable (%d)", ret);
        return ret;
    }

    /* Configured, verified and asleep -- the same posture ppg_init() leaves the
     * MAX30102 in, and for the same reason: proving the part works is not a
     * reason to start spending on it.
     */
    imu_stop();

    /* "configured, not yet measuring" -- this runs before imu_start(), which is
     * what actually enables the accelerometer. Saying "accelerometer off" here
     * was accurate for the microseconds until imu_start() ran and then read as
     * a contradiction of every sample that followed it.
     */
    LOG_INF("BMA530 configured: +/-4g, 50Hz LPM, idle until imu_start()");
    return 0;
}

int imu_start(void)
{
    uint8_t ctrl;
    int ret = i2c_reg_write_byte_dt(&imu_i2c, REG_ACC_CONF_0, SENSOR_CTRL_ENABLE);

    if (ret != 0) {
        LOG_ERR("Failed to enable accelerometer (%d)", ret);
        return ret;
    }

    k_msleep(IMU_WARMUP_MS);

    /* The part reports a rejected configuration by refusing to enable rather
     * than by failing the write, so the only way to know the settings above
     * were accepted is to read the field back.
     */
    ret = i2c_reg_read_byte_dt(&imu_i2c, REG_ACC_CONF_0, &ctrl);
    if (ret != 0) {
        LOG_ERR("Failed to read back IMU sensor control (%d)", ret);
        return ret;
    }
    if ((ctrl & 0x0F) != SENSOR_CTRL_ENABLE) {
        uint8_t c1 = 0, c2 = 0;

        /* Whatever went wrong, the three configuration registers are the whole
         * story, so print them rather than just the verdict. 0x0E specifically
         * means the part rejected the configuration; anything else means it
         * simply did not come on.
         */
        (void)i2c_reg_read_byte_dt(&imu_i2c, REG_ACC_CONF_1, &c1);
        (void)i2c_reg_read_byte_dt(&imu_i2c, REG_ACC_CONF_2, &c2);

        LOG_ERR("IMU %s: ACC_CONF_0=0x%02x (wanted 0x%02x), CONF_1=0x%02x (wrote 0x%02x), "
                "CONF_2=0x%02x (wrote 0x%02x)",
                ((ctrl & 0x0F) == SENSOR_CTRL_BAD_CFG) ? "rejected the configuration"
                                                       : "did not enable",
                ctrl, SENSOR_CTRL_ENABLE, c1, ACC_CONF_1_VALUE, c2, ACC_CONF_2_VALUE);

        return ((ctrl & 0x0F) == SENSOR_CTRL_BAD_CFG) ? -EINVAL : -EIO;
    }

    /* Enabled and configured, but that is not the same as producing data. Poll
     * until a conversion actually lands, so a start that "succeeds" and then
     * yields nothing but the 0x8000 invalid pattern is caught here rather than
     * showing up later as a motion stream that never moves.
     */
    for (int attempt = 0; attempt < IMU_DATA_ATTEMPTS; attempt++) {
        struct imu_sample probe;

        ret = imu_read(&probe);
        if (ret == 0) {
            LOG_INF("BMA530 measuring (first sample %d/%d/%d mg)", probe.x, probe.y, probe.z);
            return 0;
        }
        k_msleep(IMU_WARMUP_MS / IMU_DATA_ATTEMPTS);
    }

    LOG_ERR("IMU enabled but produced no valid sample (%d)", ret);
    return (ret == 0) ? -EIO : ret;

}

int imu_read(struct imu_sample *out)
{
    uint8_t buf[6];
    bool valid = true;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }

    /* One burst: the six data registers are contiguous, and reading them
     * separately could catch the part updating mid-sequence and mix two
     * different instants into one vector.
     */
    ret = i2c_burst_read_dt(&imu_i2c, REG_ACC_DATA, buf, sizeof(buf));
    if (ret != 0) {
        return ret;
    }

    out->x = axis_milli_g(&buf[0], &valid);
    out->y = axis_milli_g(&buf[2], &valid);
    out->z = axis_milli_g(&buf[4], &valid);

    /* Not an error the caller should treat as a fault: it means the conversion
     * after a start or a configuration change has not landed yet, and the next
     * read will have one.
     */
    return valid ? 0 : -EAGAIN;
}

int imu_read_steps(uint32_t *steps)
{
    uint8_t buf[3];
    int ret;

    if (steps == NULL) {
        return -EINVAL;
    }

    /* Three contiguous registers, low byte first, so one burst. Unlike the
     * acceleration registers there is no "invalid" pattern to check for: the
     * count is simply 0 until the first step is detected, which is a real
     * answer rather than a not-ready one.
     */
    ret = i2c_burst_read_dt(&imu_i2c, REG_STEP_COUNT, buf, sizeof(buf));
    if (ret != 0) {
        return ret;
    }

    *steps = sys_get_le24(buf);
    return 0;
}

void imu_stop(void)
{
    /* Powering down: the return is ignored deliberately, because there is no
     * recovery to attempt if the last write of a shutdown fails.
     *
     * This clears sensor_ctrl rather than using the deeper CMD_SUSPEND, which
     * would leave only CHIP_ID and the suspend register itself reachable and so
     * make waking up a more delicate operation than it needs to be here.
     */
    (void)i2c_reg_write_byte_dt(&imu_i2c, REG_ACC_CONF_0, SENSOR_CTRL_DISABLE);
}
