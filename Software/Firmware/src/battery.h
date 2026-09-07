#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

/* Sets up the SAADC channel and leaves the divider unpowered. */
int battery_init(void);

/* Powers the divider, samples the cell and drops the divider again. Returns
 * the cell voltage in millivolts, or a negative errno.
 */
int battery_read_mv(void);

/* Remaining charge for a cell voltage, 0-100. Pure: the discharge curve and
 * nothing else. Correct for one voltage, and unusable on its own for a display
 * -- see battery_level().
 */
uint8_t battery_percent(uint16_t mv);

/* What to actually show and act on: battery_percent() over a smoothed voltage,
 * clamped so it cannot climb while the cell is discharging.
 *
 * Stateful, and the state is the point. A raw reading on a 31mAh cell moves
 * several percent depending on whether a BLE burst happened to land during the
 * conversion -- measured at 40/38/40/37/41/42% on consecutive cycles. Call this
 * once per cycle, from one place.
 */
uint8_t battery_level(uint16_t mv);

/* Forgets the smoothing state. For tests, and for a deliberate re-baseline. */
void battery_level_reset(void);

#endif
