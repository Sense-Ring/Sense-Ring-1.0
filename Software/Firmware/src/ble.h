#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_store.h"

/* The ring as a connectable BLE peripheral. It advertises as "SenseRing" and
 * exposes three GATT services: the standard Heart Rate (0x180D) and Battery
 * (0x180F) services, plus a custom service carrying the vitals package the
 * flash log stores, a {timestamp, x, y, z, steps} acceleration package, a
 * control channel for commands to the ring, and the buffered log replayed on
 * request -- which is the path most of a wearer's data actually travels.
 * See ARCHITECTURE.md §5B.
 *
 * flash_store.h is included for `struct flash_sample`, which is the vitals
 * package: the log record and the notification are deliberately one layout, so
 * the type that describes it belongs to whichever of the two defines it and is
 * shared rather than duplicated.
 */

/* Brings up the controller, registers the services and starts advertising.
 * Returns 0, or a negative errno. BLE is a bonus on top of the RTT line, so a
 * failure here is not fatal to measuring -- main() carries on without it.
 */
int ble_init(void);

/* Pushes one measurement to any subscribed client: the bpm to the Heart Rate
 * service and the full 16-byte vitals package to the custom vitals service --
 * byte-for-byte the flash record, little-endian, with the sample's `contact`
 * in bit 0 of byte 14 and byte 15 (the record's repeat count) always zero.
 * A no-op when nothing is connected or subscribed.
 *
 * Only readings the firmware vouches for reach this. Refused windows are
 * stored with their reason but not notified -- the live view carries readings,
 * and the history replay carries the gaps.
 */
void ble_notify_vitals(const struct flash_sample *sample);

/* Pushes one motion reading to any subscribed client, as a 14-byte
 * {timestamp_ms, x, y, z, steps} package: uint32, three int16, then uint32 --
 * all little-endian, axes in signed milli-g, steps a free-running total since
 * boot (see imu_read_steps()). A no-op when nothing is subscribed.
 *
 * A separate characteristic from the vitals package rather than extra fields on
 * it, because that package is byte-for-byte what the flash log stores and these
 * values are not stored. See ble.c.
 */
void ble_notify_motion(uint32_t timestamp_ms, int16_t x, int16_t y, int16_t z, uint32_t steps);

/* Tells a subscribed client that something worth collecting just happened, so
 * it pulls the buffer now rather than at its next poll. A no-op when nothing is
 * subscribed -- the records are in flash either way.
 *
 * This exists because the ring cannot push its buffer. The log is read over
 * BLE, so the ring needs a way to say "come and get it" or a full buffer would
 * wait for the phone's next connection.
 *
 * Three things send it: the backlog crossing BACKLOG_NUDGE_HIGH_PCT, the
 * ten-minute uncollected backstop, and **a client subscribing to history
 * without asking for its backlog** (`OFFER_AFTER_SUBSCRIBE_MS` in ble.c). The
 * last is the "the app was just opened" path; the others are the ring deciding
 * for itself that a wait has gone on too long.
 */
void ble_notify_event(void);

/* Reasons for ble_notify_alert(). Mirrors ALERT_* in ble.c, and whatever the
 * client decodes on the other side of the wire.
 */
#define BLE_ALERT_WORN             0x01u
#define BLE_ALERT_NOT_WORN         0x02u
#define BLE_ALERT_BATTERY_LOW      0x03u
#define BLE_ALERT_BATTERY_CRITICAL 0x04u
#define BLE_ALERT_BATTERY_OK       0x05u

/* Tells the phone something changed that a person should be told about now:
 * the ring came off, or the cell is running out.
 *
 * Distinct from ble_notify_event(), which asks the phone to *collect*. This one
 * asks it to *say something*, carries no cursor, and is sent on transitions
 * only -- an alert repeated every cycle is an alert nobody reads.
 */
void ble_notify_alert(uint8_t reason);

/* Tells ble.c what this boot was, so the control read can hand it to the phone.
 *
 * `reset_cause` is RESETREAS exactly as `hwinfo_get_reset_cause()` returned it,
 * unshifted and undecoded -- the naming belongs on the phone, and a bit this
 * firmware has no name for is still a bit worth carrying. `cause_valid` is
 * false only when the read itself failed: **a zero cause with `cause_valid`
 * true is a finding, not a gap**, because this part latches no bit for
 * power-on or brownout, so a supply that went away reads clear. See
 * log_reset_cause() in main.c.
 *
 * Called once, from main(), before ble_init(). It exists because the cause is
 * unreadable by then any other way: main.c clears RESETREAS as soon as it has
 * read it, so a later boot cannot report an earlier one's cause.
 *
 * **This is the only field-visible account of a reset.** `Reset cause:` goes to
 * RTT, RTT means tethered, and a reset on a worn ring is not. Without this a
 * client can see *that* the ring restarted -- uptime going backwards -- and
 * never why.
 */
void ble_set_boot_report(uint32_t reset_cause, bool cause_valid, uint32_t records_destroyed);

/* What the cell said on the way out, if the previous run ended with it empty.
 *
 * **This is the only way a phone can ever learn that the cell reached zero.**
 * The ring stops measuring at 5%, spends the rest of the charge delivering its
 * backlog and then loses the rail; the moment the cell is actually empty is the
 * moment nothing can transmit, and the next phone to reach the ring finds it
 * charged and reading 60%. So the report is made on the boot *after* the death,
 * out of a mark left in NVS on the way down and the reset cause found on the way
 * up -- see the flat mark in main.c for why neither alone is sufficient.
 *
 * [percent] and [millivolts] are the last reading before the mark was armed, and
 * [epoch_ms] when that was, or 0 if no phone had set the clock during that run.
 * All three are meaningless unless [went_flat]; a phone must test the flag
 * rather than the percentage, because 0% is not a value this ring can report
 * having measured.
 *
 * Called once, from main(), before ble_init(), for the same reason as
 * ble_set_boot_report(): the first control read after a reset must already carry
 * it.
 */
void ble_set_flat_report(bool went_flat, uint8_t percent, uint16_t millivolts,
                         uint64_t epoch_ms);

/* Updates the Battery service level (0-100), notifying subscribers. */
void ble_set_battery(uint8_t percent);

/* Cell level and free-running step total in one packet, on the battery's
 * cadence: {u32 uptime_ms, u8 percent, u16 mV, u32 steps}, little-endian.
 *
 * Sent alongside ble_set_battery() rather than through it, because the standard
 * Battery service is a single percent byte with nowhere to carry a step count.
 * Steps are a *total*, so two notifications can be differenced for a rate --
 * see the note at the definition for why this survives BLE_LIVE_STREAM being 0
 * when the vitals and motion packets do not.
 */
void ble_notify_status(uint32_t timestamp_ms, uint8_t percent, uint16_t millivolts,
                       uint32_t steps);

#endif
