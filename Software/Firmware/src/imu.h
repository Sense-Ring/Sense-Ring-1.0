#ifndef IMU_H
#define IMU_H

#include <stdint.h>

/* The BMA530 accelerometer, driven directly over I2C.
 *
 * The part is a BMA530, not the BMA456 named on the DTS node before v49 of the
 * schematic was checked. They are different generations with incompatible
 * register maps; see imu.c.
 *
 * Same shape as ppg.h and for the same reason: there is no in-tree Zephyr
 * driver bound to this part (the DTS declares it `i2c-device`, which claims the
 * address and binds nothing), so this file is the driver. See ARCHITECTURE.md
 * §3A.
 *
 * One difference from the PPG worth knowing: the BMA530 has no load switch in
 * front of it. It sits on the always-on 1.8V rail, so imu_stop() reaches its
 * power draw through a register write and nothing else -- there is no rail to
 * drop and no GPIO here at all.
 */

/* One acceleration reading, milli-g per axis. At rest the vector magnitude is
 * ~1000 (gravity), which is the cheapest possible sanity check on a reading.
 */
struct imu_sample {
    int16_t x;
    int16_t y;
    int16_t z;
};

/* Verifies the BMA530 and loads its configuration. Leaves the accelerometer
 * disabled and the part in its low-power state -- call imu_start() to measure.
 * Returns 0, or a negative errno.
 */
int imu_init(void);

/* Enables the accelerometer and waits out the first conversions. Returns 0, or
 * a negative errno.
 */
int imu_start(void);

/* Reads the current acceleration. Returns 0 with *out populated, or a negative
 * errno.
 *
 * -EAGAIN specifically means the part answered but has no conversion ready yet
 * -- it reports a reserved pattern until the first one lands after a start or a
 * configuration change. That is a wait, not a fault; the next read will have
 * data. Any other negative return is a bus or device error.
 *
 * There is no FIFO drain here as there is for the PPG: the BMA530 has a FIFO,
 * but nothing yet needs a gap-free stream, and polling the data registers is
 * both simpler and enough to see whether the part works.
 */
int imu_read(struct imu_sample *out);

/* Reads the hardware step counter: a free-running 24-bit total.
 *
 * **The count is cumulative since boot and is never reset by this driver.** The
 * soft reset in imu_init() zeroes it, so it shares an epoch with the package
 * timestamps in flash_store.c -- both count from the same power-up and both are
 * meaningless across one.
 *
 * That is deliberate, and the alternative -- resetting after each read so every
 * package carries a delta -- is worse for three reasons:
 *
 *   - a lost package loses those steps permanently, where with a running total
 *     the next package that *does* arrive still yields the correct difference
 *     across the gap;
 *   - reading would become destructive, so two consumers (BLE and anything
 *     added later) could not both have the value;
 *   - the reset is not atomic with the read, so any step falling between them
 *     is silently dropped.
 *
 * A caller wanting "steps since last time" subtracts two readings, which costs
 * one uint32_t of state and cannot lose anything.
 *
 * Returns 0 with *steps populated, or a negative errno.
 */
int imu_read_steps(uint32_t *steps);

/* Disables the accelerometer and returns the part to low power. */
void imu_stop(void);

#endif
