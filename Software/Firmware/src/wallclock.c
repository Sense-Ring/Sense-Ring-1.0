/* The uptime-to-epoch anchor. See wallclock.h for why the ring keeps an offset
 * rather than a clock.
 *
 * Two variables and no timer: nothing here runs periodically, nothing wakes the
 * CPU, and the cost of having a wall clock on this device is one subtraction
 * per connection. On a 31mAh cell that matters more than the precision an RTC
 * would buy.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "wallclock.h"

LOG_MODULE_REGISTER(wallclock, LOG_LEVEL_INF);

static uint64_t s_epoch_at_boot;
static bool s_anchored;

/* Carried across a reset so a kept log stays on one monotonic scale. Zero on a
 * cold boot, which is the historical behaviour exactly.
 */
static uint32_t s_uptime_base;

uint32_t wallclock_uptime(void)
{
    return s_uptime_base + (uint32_t)k_uptime_get();
}

void wallclock_rebase(uint32_t base_ms)
{
    s_uptime_base = base_ms;
    LOG_INF("Uptime rebased to %u ms to sit above the kept log", (unsigned)base_ms);
}

void wallclock_restore(uint64_t epoch_at_boot_ms)
{
    /* Same floor as wallclock_set(): a persisted anchor is only as trustworthy
     * as the anchor it was persisted from, and a stored zero must not become a
     * valid-looking 1970.
     */
    if (epoch_at_boot_ms < 1577836800000ULL) {
        return;
    }

    s_epoch_at_boot = epoch_at_boot_ms;
    s_anchored = true;
    LOG_INF("Clock anchor restored across the reset: boot was at epoch %llu ms",
            (unsigned long long)s_epoch_at_boot);
}

void wallclock_set(uint64_t epoch_ms)
{
    /* Read the uptime as close to the caller's timestamp as possible: every
     * millisecond between the phone sampling its clock and this line is error
     * that goes straight into the anchor. It is small next to a BLE connection
     * interval, which is the real floor on how good this can be, and that floor
     * is tens of milliseconds -- far below anything a vitals timestamp needs.
     */
    /* Virtual, not raw: records are stamped on the virtual scale, so the anchor
     * has to be computed against the same one or a kept log would be dated by an
     * offset that only fits the records written since the reset.
     */
    uint64_t uptime = (uint64_t)wallclock_uptime();

    /* A phone that has not been told the time itself can send an epoch of 0 or
     * something near it. Anchoring to that would date every record to 1970 and,
     * worse, would look like a valid anchor to everything downstream. 2020 is
     * comfortably before this device existed and comfortably after any plausible
     * uninitialised value.
     */
    if (epoch_ms < 1577836800000ULL) { /* 2020-01-01T00:00:00Z */
        LOG_WRN("Refusing implausible epoch %llu ms", (unsigned long long)epoch_ms);
        return;
    }

    /* Unsigned, and epoch_ms is vastly larger than any uptime this device can
     * reach, so this cannot underflow in practice -- but the guard above is
     * what actually guarantees it.
     */
    s_epoch_at_boot = epoch_ms - uptime;

    if (!s_anchored) {
        LOG_INF("Clock anchored: boot was at epoch %llu ms",
                (unsigned long long)s_epoch_at_boot);
    }
    s_anchored = true;
}

bool wallclock_anchor(uint64_t *epoch_at_boot_ms)
{
    if (!s_anchored || epoch_at_boot_ms == NULL) {
        return false;
    }

    *epoch_at_boot_ms = s_epoch_at_boot;
    return true;
}
