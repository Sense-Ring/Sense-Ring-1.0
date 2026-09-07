/* Bring-up bench modes, kept out of src/main.c. NOT COMPILED -- CMakeLists.txt
 * lists its sources explicitly and nothing under tests/ is in it.
 *
 * These two modes each replaced main() entirely and brought up one part and
 * nothing else: no BLE, no flash, no battery. That was the right shape when the
 * console was the only output and the open question was "does this part answer
 * at all". Both questions are answered on hardware -- the IMU axes and step
 * counter are integrated, and the optics produced a confirmed 71bpm finger
 * capture -- so the bench switches in main.c exercise the buffer-and-deliver
 * pipeline instead.
 *
 * Kept because if a new board revision ever needs bring-up, i2c_bus_scan() and
 * imu_probe_shapes() below are the "what is actually on the bus, and does it
 * answer in the shape we expect" tools, and they are the hardest part to write
 * again from memory. Paste back into main.c, restore the #if/#else scaffolding
 * around the real firmware, and re-add the dispatch in main().
 */
/* ==========================================================================
 * DELETE LATER, JUST FOR TESTING
 *
 * Live vitals bench mode. Set to 0 to restore the real firmware -- battery,
 * flash and BLE are compiled out while this is 1, and the console carries
 * nothing but a rate and a saturation.
 *
 * What it does: brings the MAX30102 up, leaves it running, and keeps a sliding
 * VITALS_WINDOW_SEC of samples that it re-analyses every LIVE_REPORT_MS. The
 * LED rail stays up the whole time, so this draws far more than the duty-cycled
 * firmware -- it is a bench mode, not something to wear around.
 *
 * The window is the same length the real firmware uses, so the numbers here are
 * the numbers it would report; only the cadence differs. That also sets the
 * warm-up: vitals_compute() wants VITALS_MIN_SAMPLES, so the first half-window
 * reports "filling" rather than a rate.
 * ========================================================================== */
#define PPG_LIVE_VITALS_MODE 0

/* ==========================================================================
 * DELETE LATER, JUST FOR TESTING
 *
 * Live IMU bench mode. Set to 0 to restore the real firmware -- PPG, battery,
 * flash and BLE are all compiled out while this is 1, and the console carries
 * nothing but acceleration.
 *
 * What it does: brings the BMA530 up, leaves it running, and prints one line
 * every IMU_LIVE_POLL_MS with the three axes in milli-g, the vector magnitude,
 * and how much the vector moved since the previous line.
 *
 * Deliberately not a step counter and not a fall detector. This is the "does
 * the part answer, are the axes sane, does the number move when I move" check
 * that has to pass before either of those is worth writing. Two things to look
 * for: lying still on a desk the magnitude reads ~1000 (one g, gravity) and
 * `move` sits near 0; picked up and waved, `move` jumps by hundreds.
 * ========================================================================== */
#define IMU_LIVE_MODE 0

#if PPG_LIVE_VITALS_MODE && IMU_LIVE_MODE
#error "PPG_LIVE_VITALS_MODE and IMU_LIVE_MODE are alternative bench modes: enable one"
#endif

#if IMU_LIVE_MODE

/* DELETE LATER, JUST FOR TESTING
 *
 * Poll and print interval. 200ms is not about the sensor -- the BMA530 is
 * producing samples at 50Hz and we are reading one in ten. It is about the
 * console: five lines a second is readable while something is being waved
 * about, and it keeps well inside the RTT up-buffer, which is 1KB in
 * NO_BLOCK_SKIP mode and drops anything past ~13 lines a burst *silently*
 * (see ARCHITECTURE.md 5.5.1). A tighter loop here would be a loop whose
 * output you cannot trust.
 */
#define IMU_LIVE_POLL_MS 200

/* DELETE LATER, JUST FOR TESTING
 *
 * Integer square root for the vector magnitude, restoring shift-and-subtract:
 * two bits of input per bit of output, no divides. Same reasoning as
 * vitals.c's isqrt64() -- the Cortex-M4 has no 64-bit divide and this build has
 * no floating point at all, so sqrt() from libm would drag FP into a firmware
 * that deliberately has none.
 */
static uint32_t isqrt32(uint32_t x)
{
    uint32_t root = 0;
    uint32_t bit = 1u << 30;

    while (bit > x) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }

    return root;
}

/* DELETE LATER, JUST FOR TESTING
 *
 * Every 7-bit address that acknowledges, printed on one line. Runs only when
 * imu_init() has already failed, because that is the moment the distinction
 * matters and nothing else can make it: "the IMU did not answer" and "nothing
 * at all is on this bus" look identical from a single failed transaction, and
 * they have completely different causes. Seeing 0x57 here (the MAX30102) proves
 * the bus, the pull-ups and the pin assignment are all fine and the problem is
 * the IMU alone.
 */
static void i2c_bus_scan(void)
{
    const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    uint8_t found[8];
    size_t n = 0;

    if (!device_is_ready(bus)) {
        LOG_ERR("I2C bus not ready -- cannot scan");
        return;
    }

    for (uint8_t addr = 0x08; addr < 0x78 && n < ARRAY_SIZE(found); addr++) {
        uint8_t discard;

        /* A one-byte read is the cheapest probe that distinguishes an address
         * that acknowledges from one that does not.
         */
        if (i2c_read(bus, &discard, 1, addr) == 0) {
            found[n++] = addr;
        }
    }

    if (n == 0) {
        LOG_ERR("I2C scan: nothing answered on the whole bus");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        LOG_INF("I2C scan: device at 0x%02x", found[i]);
    }
}

/* DELETE LATER, JUST FOR TESTING
 *
 * Which *shape* of transaction does the IMU accept?
 *
 * The bus scan above answers "is it there" (it is: it acknowledges a bare
 * read). This answers the next question, which the scan cannot: a bare read is
 * the only I2C form with no write phase at all, and every register access needs
 * one. So the part acknowledging a read while refusing a register read narrows
 * the fault to the write phase -- and these five probes say which part of it.
 *
 * Each is logged with its return whether it succeeds or fails, because the
 * pattern across them is the diagnostic, not any single result:
 *
 *   A works, B fails         -> the part is refusing writes outright
 *   A and B work, C fails    -> it dislikes the repeated START specifically
 *   C fails but D works      -> same, and D is the workaround
 *   C fails then E works     -> the first transaction is simply being eaten
 */
static void imu_probe_shapes(void)
{
    const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    const uint8_t addr = 0x18;
    uint8_t reg = 0x00;
    uint8_t val;
    int ret;

    if (!device_is_ready(bus)) {
        return;
    }

    ret = i2c_read(bus, &val, 1, addr);
    LOG_INF("probe A bare read          ret=%d val=0x%02x", ret, val);
    k_msleep(10);

    ret = i2c_write(bus, &reg, 1, addr);
    LOG_INF("probe B pointer write only ret=%d", ret);
    k_msleep(10);

    val = 0;
    ret = i2c_write_read(bus, addr, &reg, 1, &val, 1);
    LOG_INF("probe C write_read (Sr)    ret=%d val=0x%02x", ret, val);
    k_msleep(10);

    val = 0;
    ret = i2c_write(bus, &reg, 1, addr);
    if (ret == 0) {
        ret = i2c_read(bus, &val, 1, addr);
    }
    LOG_INF("probe D write+STOP+read    ret=%d val=0x%02x", ret, val);
    k_msleep(10);

    val = 0;
    ret = i2c_write_read(bus, addr, &reg, 1, &val, 1);
    LOG_INF("probe E write_read again   ret=%d val=0x%02x", ret, val);

    LOG_INF("(CHIP_ID should read 0xc2)");
}

/* DELETE LATER, JUST FOR TESTING */
static void imu_live_loop(void)
{
    struct imu_sample prev = { 0, 0, 0 };
    bool have_prev = false;

    LOG_INF("Live IMU: reporting every %d ms", IMU_LIVE_POLL_MS);

    if (imu_init() != 0) {
        LOG_ERR("IMU initialization failed -- scanning the bus to see what is there");
        i2c_bus_scan();
        imu_probe_shapes();
        return;
    }
    if (imu_start() != 0) {
        LOG_ERR("IMU failed to start");
        return;
    }

    while (1) {
        struct imu_sample s;
        uint32_t mag, move;
        int ret;

        k_msleep(IMU_LIVE_POLL_MS);

        ret = imu_read(&s);
        if (ret == -EAGAIN) {
            /* Answered, but the first conversion has not landed yet. Expected
             * once or twice right after the start, not a fault.
             */
            LOG_INF("IMU warming up");
            continue;
        }
        if (ret != 0) {
            LOG_ERR("IMU read failed (%d)", ret);
            continue;
        }

        /* Each axis is at most a few thousand milli-g, so the squares and their
         * sum stay far inside 32 bits and no widening is needed here -- unlike
         * vitals.c, where 18-bit counts squared genuinely do overflow.
         */
        mag = isqrt32((uint32_t)((int32_t)s.x * s.x + (int32_t)s.y * s.y +
                                 (int32_t)s.z * s.z));

        /* Manhattan distance from the previous vector rather than the Euclidean
         * one: it needs no second square root, and as a "did this move" number
         * the two are within a small constant factor of each other. The first
         * reading has nothing to compare against and reports 0 rather than the
         * distance from an imaginary origin at rest.
         */
        move = have_prev ? (uint32_t)(abs(s.x - prev.x) + abs(s.y - prev.y) +
                                      abs(s.z - prev.z))
                         : 0;

        LOG_INF("IMU x=%6d y=%6d z=%6d mg  |a|=%5u  move=%5u", s.x, s.y, s.z, mag, move);

        prev = s;
        have_prev = true;
    }
}

#elif PPG_LIVE_VITALS_MODE

/* DELETE LATER, JUST FOR TESTING
 *
 * One measurement, in words. Bench mode only.
 *
 * The real firmware deliberately does not call this. Its console stays silent
 * between flushes -- every window goes to flash instead, and the flush dumps
 * the lot in one burst (see flash_store.c and ARCHITECTURE.md 5.5). Reading a
 * ring over RTT means holding a debugger against it, which is exactly the
 * situation where you want the packages batched rather than trickling out one
 * line at a time.
 */
static void report(const struct vitals *v)
{
    if (!v->contact) {
        LOG_INF("No contact (IR DC %u)", v->ir_dc);
        return;
    }

    if (v->bpm == 0) {
        LOG_INF("Pulse too weak to read (confidence %u%%, PI %u.%u%%)",
                v->confidence / 10, v->perfusion_milli / 10, v->perfusion_milli % 10);
        return;
    }

    if (v->spo2_tenths == 0) {
        LOG_INF("HR %u bpm (confidence %u%%), SpO2 unavailable", v->bpm, v->confidence / 10);
        return;
    }

    LOG_INF("HR %u bpm, SpO2 %u.%u%% (confidence %u%%, PI %u.%u%%)", v->bpm,
            v->spo2_tenths / 10, v->spo2_tenths % 10, v->confidence / 10,
            v->perfusion_milli / 10, v->perfusion_milli % 10);
}

/* DELETE LATER, JUST FOR TESTING */
#define LIVE_REPORT_MS 3000

/* The FIFO holds 32 entries, so that is the most a drain can ever yield. */
static struct ppg_sample live_batch[32];

/* The sliding window, oldest sample first, and how much of it is live. */
static struct ppg_sample live_window[VITALS_WINDOW_SAMPLES];
static size_t live_filled;

/* Whether the ring is currently on a finger, as of the last drain. Owned here
 * because vitals_contact() is stateless and its threshold is hysteretic.
 * Starts false so the first batch has to earn contact rather than assume it.
 */
static bool live_worn;

/* DELETE LATER, JUST FOR TESTING
 *
 * Appends a drained batch, dropping as much of the front as it has to. Kept
 * linear and memmove'd rather than made a proper ring buffer because
 * vitals_compute() wants one contiguous run of samples: a ring would need
 * linearising into a second 6KB buffer, where the shift costs a ~6KB copy per
 * poll -- a few hundred microseconds every 200ms, on a core that is otherwise
 * idle here.
 */
static void live_window_append(const struct ppg_sample *src, size_t n)
{
    if (n > ARRAY_SIZE(live_window)) {
        /* More than a whole window at once cannot happen from a 32-entry FIFO,
         * but the arithmetic below would underflow if it ever did.
         */
        src += n - ARRAY_SIZE(live_window);
        n = ARRAY_SIZE(live_window);
    }

    if (live_filled + n > ARRAY_SIZE(live_window)) {
        size_t drop = live_filled + n - ARRAY_SIZE(live_window);

        memmove(live_window, &live_window[drop],
                (live_filled - drop) * sizeof(live_window[0]));
        live_filled -= drop;
    }

    memcpy(&live_window[live_filled], src, n * sizeof(live_window[0]));
    live_filled += n;
}

/* DELETE LATER, JUST FOR TESTING */
static void live_vitals_loop(void)
{
    int64_t next_report;

    LOG_INF("Live vitals: %d s window, reported every %d ms", VITALS_WINDOW_SEC,
            LIVE_REPORT_MS);

    if (ppg_init() != 0) {
        LOG_ERR("PPG Initialization failed!");
        return;
    }
    if (ppg_start() != 0) {
        LOG_ERR("PPG failed to start");
        return;
    }

    next_report = k_uptime_get() + LIVE_REPORT_MS;

    while (1) {
        int n;

        k_msleep(PPG_POLL_INTERVAL_MS);

        n = ppg_read_fifo(live_batch, ARRAY_SIZE(live_batch));
        if (n < 0) {
            LOG_ERR("PPG read failed (%d)", n);
        } else {
            bool worn = vitals_contact(live_batch, (size_t)n, live_worn);

            /* Checked here, on every drain, rather than at report time: this
             * runs every PPG_POLL_INTERVAL_MS, so removal shows up in a poll
             * interval instead of at the next report.
             */
            if (live_worn && !worn) {
                /* Everything in the window was measured through a finger that
                 * is no longer there. Keeping it would mean a stale rate for
                 * the rest of the window, and a mixed one for a window after
                 * it goes back on.
                 */
                LOG_INF("Ring removed");
                live_filled = 0;
            } else if (!live_worn && worn) {
                LOG_INF("Ring on -- filling window");
            }
            live_worn = worn;

            if (worn) {
                live_window_append(live_batch, (size_t)n);
            }
        }

        if (k_uptime_get() < next_report) {
            continue;
        }

        /* Anchored to absolute time so the cadence does not drift by however
         * long the DSP took.
         */
        next_report += LIVE_REPORT_MS;

        if (!live_worn) {
            /* Said once per report rather than once per poll: the transition is
             * logged the instant it happens, this is only the steady state.
             */
            LOG_INF("Not worn");
        } else if (live_filled < VITALS_MIN_SAMPLES) {
            LOG_INF("Filling window (%u/%u samples)", (unsigned)live_filled,
                    (unsigned)VITALS_MIN_SAMPLES);
        } else {
            struct vitals v;

            if (vitals_compute(live_window, live_filled, &v) == 0) {
                report(&v);
            } else {
                LOG_WRN("Window rejected (%u samples)", (unsigned)live_filled);
            }
        }
    }
}
