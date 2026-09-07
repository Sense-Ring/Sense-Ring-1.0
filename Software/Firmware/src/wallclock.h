#ifndef WALLCLOCK_H
#define WALLCLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* What time it is, on a board with no RTC.
 *
 * The nRF52832 on this ring has no real-time clock and no coin cell to keep one
 * running, so the only time source on the device is `k_uptime_get()` --
 * milliseconds since power-up, which resets to zero on every reset and has no
 * relationship to any date. Records are stamped with it (flash_store.c), which
 * is fine for ordering readings within one run and useless for anything else.
 *
 * The fix is not to give the ring a clock but to give it an **anchor**: the
 * phone knows the real time, so on connect it writes the current epoch and the
 * ring stores the one number that converts between the two,
 *
 *     epoch_at_boot = phone_epoch_now - uptime_now
 *
 * after which any record's real time is `epoch_at_boot + record.timestamp_ms`.
 *
 * **Records deliberately stay uptime-based.** They have to: a window is
 * measured and written to flash long before any phone connects, and NOR flash
 * is append-only, so a record cannot be revised once the time becomes known.
 * Storing the anchor separately means one number gets corrected instead of
 * thousands that cannot be.
 *
 * The conversion therefore happens **off the ring**, at delivery. That also
 * keeps the flash record and the BLE package byte-for-byte identical, which
 * stamping epochs into the wire format would have broken: a millisecond epoch
 * does not fit the record's uint32.
 *
 * Re-anchoring on every connection corrects drift for free: the ring's crystal
 * is whatever it is, but the error can only accumulate since the *last*
 * connection rather than since boot.
 */

/* **Uptime here is virtual.** Everything above still holds, with one
 * substitution: the ring stamps records with `wallclock_uptime()` rather than
 * `k_uptime_get()` -- the latter plus a base carried across a reset.
 *
 * The reason is that the log survives a reset. A log kept across a reset
 * holds records timestamped in the *previous* boot's uptime, while
 * `k_uptime_get()` has restarted at zero -- so new records would be stamped
 * *earlier* than old ones, time would run backwards inside one log, and a single
 * anchor could not date both. Rebasing above the newest surviving timestamp
 * keeps the whole log on one monotonic scale and therefore on one anchor, which
 * is the property this file exists to protect.
 *
 * The cost is that the seconds the ring spends dark are compressed to nothing:
 * the base resumes where the log left off, so a reset looks instantaneous in the
 * record. For a reading every ~24s that is inside the noise, and it is a far
 * smaller error than the alternative of not having the readings.
 *
 * `timestamp_ms` is a uint32, so virtual uptime wraps after ~49.7 days of
 * accumulated running. The caller refuses to rebase near that ceiling and takes
 * the erase instead -- see main.c.
 */

/* Milliseconds since boot, plus any base carried across a reset. This is the
 * timestamp source for records; `k_uptime_get()` is still the right thing for
 * measuring intervals within one boot.
 */
uint32_t wallclock_uptime(void);

/* Moves virtual uptime up to `base_ms`, so the next record lands above a log
 * kept from before a reset. Call once at boot, before anything is recorded.
 */
void wallclock_rebase(uint32_t base_ms);

/* Restores an anchor persisted across a reset, so a kept log can be dated
 * before any phone connects. Superseded by the next wallclock_set().
 */
void wallclock_restore(uint64_t epoch_at_boot_ms);

/* Anchors uptime to real time. `epoch_ms` is Unix milliseconds as the phone
 * sees them; the anchor is computed against the uptime at the moment of this
 * call, so it should be called with a freshly-read clock rather than one that
 * has been sitting in a queue.
 *
 * Idempotent and safe to call on every connection -- that is the intended use,
 * and each call replaces the anchor rather than adjusting it.
 */
void wallclock_set(uint64_t epoch_ms);

/* The anchor, if a phone has ever set it since boot. Returns false and leaves
 * *epoch_at_boot_ms untouched when it has not.
 *
 * The false case is not an error and is expected to be common: the ring
 * measures from power-up and may buffer for hours before anything connects. It
 * means "these records cannot be dated yet", which is a fact the client needs
 * rather than one to paper over with a zero.
 */
bool wallclock_anchor(uint64_t *epoch_at_boot_ms);

#endif
