/* Vitals log kept in the "storage" flash partition (see the board DTS), as a
 * **ring** of fixed-size records addressed by an ever-increasing sequence
 * number: the slot for sequence `s` is `s % fa_size`.
 *
 * Two pointers describe it. `s_head` is the oldest record still stored, and
 * `s_cursor` is where the next one goes; everything between them is live.
 *
 * **Erasing is driven by what the phone has acknowledged.** It used to be
 * unconditional and total -- the whole partition, on every "flush" -- which
 * threw the buffer away without delivering it, and spent an erase cycle on
 * every page whether or not it held anything. Now the head only advances past
 * records the phone has confirmed, and a page is erased exactly once, when the
 * cursor laps round and needs it back.
 *
 * The one place records still die undelivered is the ring lapping itself: when
 * the cursor catches the head, the oldest page goes so recording can continue.
 * That is a deliberate choice and it is logged loudly -- see write_record().
 *
 * **Acknowledgement drives the erase, not the lap.** The alternative -- advance
 * the head and let the bytes sit until the cursor needs the page -- leaves
 * acknowledged records physically on the part for up to four days at the
 * shipping cadence, and indefinitely if the ring is switched off and put in a
 * drawer. Nothing in the firmware can read them, but a debugger on the part
 * can, and on a device holding health data that should be a decision rather
 * than a side effect. See scrub_pages(); the write path only erases pages the
 * scrub never got to.
 *
 * ---------------------------------------------------------------------------
 * **Who may write what, and it is one sentence: main writes everything except
 * s_acked, and only the Bluetooth RX thread writes s_acked.**
 *
 * Four contexts reach this module -- the measurement loop, the Bluetooth RX
 * thread through flash_store_release(), the replay work queue through
 * flash_store_foreach(), and the console dump. There is no lock, and the reason
 * there does not need to be one is that rule. Everything else here is
 * read-only from another thread: a walk reads s_head and s_cursor, ble.c reads
 * the cursor and the generation, and all of them are single aligned words, which
 * this core cannot tear.
 *
 * That rule is why flash_store_release() is one store and why the work it
 * implies -- reclaiming pages, erasing them -- is done by flash_store_service()
 * on the measurement thread instead. It costs up to one cycle of latency on an
 * erase whose deadline is measured in days. What it buys is that the pointers
 * describing where a wearer's vitals live have exactly one writer, on a module
 * that fails by corrupting records rather than by crashing.
 *
 * **So: anything added here runs on main unless it demonstrably cannot.** A
 * second writer is not a small change to this file, whatever it looks like in
 * the diff.
 * ---------------------------------------------------------------------------
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include "battery.h"
#include "flash_store.h"
#include "ble.h"
#include "vitals.h"
#include "wallclock.h"

LOG_MODULE_REGISTER(flash_store, LOG_LEVEL_INF);

#define STORAGE_PARTITION_ID FIXED_PARTITION_ID(storage_partition)

/* The record layout lives in flash_store.h -- the replay path and the console
 * dump both need it. What stays here is why it looks like that.
 *
 * The three quality fields are the point of the widening: every window computed
 * `contact`, `confidence` and `perfusion` and then threw them away, which left
 * the log full of readings nothing downstream could weigh. A consumer of the
 * log cannot treat a 500-confidence reading and a 950-confidence one as the
 * same evidence, and without those fields it has no choice.
 *
 * `steps` is a *delta*, not the running total imu_read_steps() returns. The
 * total is the right thing on the wire for a live consumer (a lost packet costs
 * nothing, see imu.h) but the wrong thing in a log: stored per record it is
 * 4 bytes of mostly-repeated value, and once the ring laps there is no earlier
 * record left to difference against. main.c does the subtraction once, while it
 * still has both totals.
 */
BUILD_ASSERT(sizeof(struct flash_record) == FLASH_RECORD_SIZE,
             "record must stay 16 bytes and unpadded");

/* bit 0 is the wearer, bit 1 is the verdict, bits 2-4 are the reason behind it,
 * and bits 5-7 are how much the hand was moving. The reason is only meaningful
 * when REFUSED is set; the movement bucket is meaningful on every record.
 *
 * Three reason bits rather than two, because FLASH_REFUSAL_MOVING is the fifth
 * value. The mask is what the run-length compression below matches on, so the
 * width here is also what stops a MOVING run being absorbed into a NOT_WORN
 * one.
 *
 * **The byte is full.** There are no spare bits, so any further field has to
 * make a case for widening the record -- which is a wire-format change, a
 * RECORD_FORMAT_VERSION bump, and a boot erase of whatever log is on the part.
 *
 * Note what the collapse below does *not* match on. A run absorbs a refusal
 * whose reason and contact bits agree, and takes no notice of the movement
 * bucket, so the run keeps the bucket of the refusal that opened it. That is
 * deliberate and consistent with everything else the run keeps -- the first
 * refusal's timestamp, its confidence, its perfusion -- and the alternative,
 * matching on movement too, would break a long run into one record per bucket
 * transition and cost more flash than the resolution is worth.
 *
 * **One class of refusal escapes the collapse entirely:** a MOVING window that
 * still found a believable pulse is written standalone. See
 * refusal_keeps_evidence() for why that is not a reversal of the paragraph
 * above -- the windows it exempts are the ones that are *not* interchangeable
 * with their neighbours, which is the condition the collapse rests on.
 */
#define FLAG_CONTACT      BIT(0)
#define FLAG_REFUSED      BIT(1)
#define FLAG_REASON_SHIFT 2
#define FLAG_REASON_MASK  (0x7u << FLAG_REASON_SHIFT)
#define FLAG_MOVE_SHIFT   5
#define FLAG_MOVE_MASK    (0x7u << FLAG_MOVE_SHIFT)

#define RECORD_SIZE  ((off_t)FLASH_RECORD_SIZE)

/* A refusal run cannot be extended in place. NOR flash only ever clears bits,
 * so a `repeat` already written as 3 cannot become 4 without erasing the page
 * it sits on -- and erasing a page to save a byte is exactly backwards on a
 * part rated for ~10k cycles. The run is therefore accumulated in RAM and
 * written once, when something ends it.
 */
#define REPEAT_MAX 0xFFu

/* Erased NOR flash reads as 0xFF, so a timestamp of all-ones marks a free
 * slot. Uptime in ms would take ~49 days to reach this, and a record that
 * lands on it exactly is nudged down by one below rather than written raw.
 */
#define EMPTY_MARKER 0xFFFFFFFFu

/* Offsets in this file and on the wire are **absolute byte sequence numbers**,
 * not positions in the partition. The log is a ring: the physical slot for a
 * sequence number is `seq % fa_size`, and both s_head and s_cursor only ever
 * increase.
 *
 * That is what makes a cursor safe to hand to a phone. A physical offset gets
 * reused every time the ring laps the partition, so "resume from byte 800"
 * would eventually mean a different record with nothing to mark the change. A
 * sequence number is unique for 4GB of records -- 268 million of them, or
 * roughly two centuries at this ring's rate -- so within one boot it simply
 * cannot repeat. Across boots, the generation counter covers it.
 *
 * A record never straddles a page or the wrap, because 16 divides both the
 * 4096-byte page and the 232KB partition. flash_store_init() checks it.
 */
static const struct flash_area *s_fa;
static uint32_t s_head;   /* oldest record still stored */
static uint32_t s_cursor; /* where the next record goes */
static uint32_t s_acked;  /* the phone has confirmed everything below this */
static uint32_t s_dumped; /* offset a periodic console dump has already shown */

/* How far ahead of the cursor flash is erased and writable.
 *
 * **Boot clears the whole partition; a lap clears one page at a time.** Erasing
 * lazily in both places is cheaper at boot -- 58 pages at ~85ms is a stall long
 * enough to notice -- but flash_store_restore() cannot work that way. It walks
 * the raw part rather than the bounded log, so it needs "blank" to mean
 * "nothing was ever here", and lazily-erased space past the cursor still holds
 * the *previous* session's records. The stall therefore falls only on a boot
 * that is discarding the log anyway; a boot that keeps it erases nothing at
 * all.
 *
 * This also bounds the exposure. Records left physically in flash are
 * unreadable through this module -- s_head and s_cursor bound every walk -- but
 * recoverable by anyone with a debugger on the part. A boot that erases erases
 * everything, a boot that keeps the log keeps only records it can account for,
 * and this session's are scrubbed as soon as they are acknowledged, by
 * s_scrub_next below.
 */
static uint32_t s_clean_upto;

/* The oldest sequence number whose page this session has written and not yet
 * erased. Everything below it is physically gone from flash.
 *
 * The second erase frontier, and it runs behind the log rather than ahead of
 * it: s_clean_upto is "how far the flash is ready to be written", s_scrub_next
 * is "how far the acknowledged records have actually been destroyed". They are
 * genuinely two regions and one pointer cannot describe both -- until the
 * cursor has lapped once, the pages ahead of it hold the *previous boot's*
 * records (lazily erased when the cursor reaches them) while the pages behind
 * the head hold this session's acknowledged ones (erased as soon as the phone
 * confirms them). Those are a whole partition apart in sequence space.
 *
 * The two frontiers meet at exactly one place, and it is what keeps a page from
 * being erased twice per lap: when the cursor laps onto a page, write_record()
 * asks whether the scrub already cleared it and skips its own erase if so.
 */
static uint32_t s_scrub_next;

static size_t s_page_size;
static flash_store_battery_mv_fn s_battery_mv; /* NULL until init, may stay NULL */
static flash_store_steps_fn s_steps;           /* NULL until init, may stay NULL */

/* The refusal run being accumulated, if any. Its timestamp is the *first*
 * refusal of the run; `repeat` counts the ones that followed, so the run covers
 * repeat+1 windows starting at that timestamp.
 */
static struct flash_record s_run;
static bool s_run_open;

/* Records the boot erase destroyed this time round, 0 when the log was kept.
 * Read once per connection by the control characteristic; see ble.c.
 */
static uint32_t s_boot_destroyed;

/* Which run of the log a sequence number belongs to. Offsets only mean anything
 * within one generation -- boot restarts them at zero -- so a client resuming a
 * transfer has to be able to notice that the ground moved.
 *
 * Zero is reserved to mean "I have never spoken to this ring", so a phone with
 * no stored cursor is never mistaken for one that is up to date.
 */
static uint32_t s_generation;

/* A generation for a fresh log, drawn at random.
 *
 * The obvious implementation -- start at 1 and increment -- is broken here, and
 * subtly: this counter lives in RAM, so every boot starts from the same
 * constant and every boot produces the same "new" generation. A phone holding a
 * cursor from before a reset would find its generation still matched, and the
 * one guard against resuming into a log that had restarted underneath it would
 * silently pass. So the value has to come from somewhere that differs per boot
 * by construction.
 *
 * **Something does persist** -- main.c keeps the generation in NVS so a log can
 * survive a reset. That does not change this function, and the distinction is
 * worth keeping straight: a *kept* log keeps its old generation
 * precisely because its offsets still mean what they meant, and this is only
 * reached when the log was thrown away, which is exactly when they must not.
 *
 * A 32-bit random value collides with the previous boot's about one time in
 * four billion, and the cursor bounds check catches even that.
 */
static uint32_t next_generation(void)
{
    uint32_t g = sys_rand32_get();

    return (g == 0U) ? 1U : g;
}

/* Defined with the append path below, where the run logic it belongs to lives,
 * but needed by the dump and the flush above it -- both have to commit an open
 * run before they can show or destroy the log.
 */
static int close_run(void);

/* The read-only walk every reader of the log goes through -- the console dump,
 * the flush, and the BLE replay path (ble.c). One traversal, so "what is in the
 * log" cannot mean two different things depending on who is asking.
 *
 * Stops at the first empty slot, at the end of the partition, or when `cb`
 * returns false. Returns the offset it stopped at.
 */
uint32_t flash_store_foreach(uint32_t from, flash_store_cb cb, void *user_data)
{
    struct flash_record rec;
    uint32_t limit;
    uint32_t seq;

    if (s_fa == NULL || cb == NULL) {
        return from;
    }

    /* Clamped forward, never backward. A caller asking for records older than
     * the ring still holds is not an error -- it is a phone that was away while
     * the buffer lapped -- and the honest answer is "here is what survives",
     * starting where it actually starts. The caller sees the gap because the
     * offset it gets back is higher than the one it asked for.
     */
    if (from < s_head) {
        from = s_head;
    }

    /* Snapshotted, not re-read each iteration. A record is four flash words and
     * is not written atomically, so walking towards a cursor another thread is
     * advancing could read a record half-way through being written. Anything
     * appended during the walk is simply picked up by the next one.
     */
    limit = s_cursor;

    for (seq = from; seq + FLASH_RECORD_SIZE <= limit; seq += FLASH_RECORD_SIZE) {
        off_t phys = (off_t)(seq % s_fa->fa_size);

        if (flash_area_read(s_fa, phys, &rec, RECORD_SIZE) != 0) {
            LOG_ERR("Flash read failed at 0x%lx", (long)phys);
            break;
        }
        /* Belt and braces. s_head and s_cursor already bound the live range, so
         * an erased slot inside it would mean the two had drifted from what is
         * actually in flash -- worth stopping on rather than walking 0xFF as if
         * it were a reading.
         */
        if (rec.timestamp_ms == EMPTY_MARKER) {
            LOG_WRN("Empty slot inside the live range at 0x%lx", (long)phys);
            break;
        }
        if (!cb(seq, &rec, user_data)) {
            break;
        }
    }

    return seq;
}

/* How many records a logging walk printed. */
struct dump_ctx {
    uint32_t count;
};

static bool dump_one(uint32_t offset, const struct flash_record *rec, void *user_data)
{
    struct dump_ctx *ctx = user_data;

    ARG_UNUSED(offset);

    if (rec->flags & FLAG_REFUSED) {
        /* Three reason bits index this, so a corrupted flags byte can name a
         * value no reason was ever assigned to. It used to be two bits against
         * four entries -- exactly covered, no check possible or needed -- and
         * widening the field is what put a gap between the values the byte can
         * hold and the ones this array has names for. A record read back off
         * flash is not trusted input -- a torn write or a stale page can put
         * anything in this byte -- so an unnamed value says so rather than
         * reading past the end of the table.
         */
        static const char *const reasons[] = {"not worn",  "no pulse found",
                                              "pulse too weak", "pulse below the band",
                                              "moving (old numbering)", "moving"};
        unsigned int why = (rec->flags & FLAG_REASON_MASK) >> FLAG_REASON_SHIFT;
        const char *reason = (why < ARRAY_SIZE(reasons)) ? reasons[why] : "unknown reason";

        /* The run is printed as one line with its span, because that is what it
         * is -- printing it repeat+1 times would put the thing this collapsing
         * exists to avoid back on the console.
         */
        /* Movement rides on the refused line as well as the accepted one, and
         * on this line it is the whole point: a `moving` refusal that cannot
         * say how far over WINDOW_MOVE_MILLI_G it landed is a verdict with its
         * evidence removed. On a collapsed run it is the opening refusal's
         * bucket, same as every other field here.
         */
        LOG_INF("{timestamp: %u, refused: %s, windows: %u, bpm: %u, conf: %u, "
                "pi: %u.%u, steps: %u, move: %s mg}",
                rec->timestamp_ms, reason, (unsigned)rec->repeat + 1u, rec->bpm,
                rec->confidence, rec->perfusion_milli / 10, rec->perfusion_milli % 10,
                rec->steps, flash_movement_label(flash_record_movement(rec)));
    } else {
        LOG_INF("{timestamp: %u, bpm: %u, spo2: %u.%u, conf: %u, pi: %u.%u, steps: %u, "
                "move: %s mg}",
                rec->timestamp_ms, rec->bpm, rec->spo2_tenths / 10, rec->spo2_tenths % 10,
                rec->confidence, rec->perfusion_milli / 10, rec->perfusion_milli % 10,
                rec->steps, flash_movement_label(flash_record_movement(rec)));
    }

    ctx->count++;
    return true;
}

/* Walks records from `from`, logging each, and prints a summary line.
 * Returns the offset walked to.
 *
 * Two parameters, both there for the same reason -- the two callers differ in
 * ways a reader of the console must not have to guess:
 *
 *   from -- the flush shows the whole history because it is about to destroy
 *           it. The periodic dump shows only what is new, because it runs every
 *           cycle forever and reprinting the log each time grows without bound.
 *           That is not just untidy: the RTT up-buffer is 1KB in NO_BLOCK_SKIP
 *           mode, which is about 13 of these lines, and past that the overflow
 *           is dropped *silently*. A dump that grows is a dump that quietly
 *           stops being complete.
 *
 *   verb -- "flushed" and "dumped" differ in whether the log still exists
 *           afterwards.
 */
static uint32_t dump_records(uint32_t from, const char *verb)
{
    struct dump_ctx ctx = {0};
    uint32_t off = flash_store_foreach(from, dump_one, &ctx);

    /* The running total is the live range, not the cursor: with the log now a
     * ring that drops its oldest page when it laps, "how many records exist"
     * and "how many have ever been written" stopped being the same number.
     *
     * MAX because s_acked is the one value here written by another thread: an
     * ACK validated a microsecond before the head lapped past it lands just
     * below the head, and the subtraction would print four billion. Advisory on
     * this line, and clamped where it matters (write_record()).
     */
    LOG_INF("%s %u package(s), %u stored, %u acknowledged", verb, ctx.count,
            (unsigned)((s_cursor - s_head) / FLASH_RECORD_SIZE),
            (unsigned)((MAX(s_acked, s_head) - s_head) / FLASH_RECORD_SIZE));
    return off;
}

/* Emits the battery level as its own package, separate from the vitals ones.
 * Read live at flush time rather than stored per-window: the flush is rare and
 * the cell level barely moves between them, so one fresh reading is enough.
 *
 * The step total rides along on the same package for the same reason -- it is a
 * free-running counter read straight from the BMA530, so a fresh read at flush
 * time is the whole of it. It is reported even when the battery read fails, and
 * that ordering is deliberate: the two come from different parts on different
 * buses, and letting a bad ADC conversion take the step figure down with it
 * would be a gap in the movement record caused by something unrelated to
 * movement.
 */
static void dump_battery(void)
{
    int mv = (s_battery_mv != NULL) ? s_battery_mv() : -ENODEV;
    int steps = (s_steps != NULL) ? s_steps() : -ENODEV;

    if (mv < 0 && steps < 0) {
        return;
    }

    if (mv < 0) {
        LOG_WRN("Battery level unavailable for flush (%d)", mv);
        LOG_INF("{timestamp: %u, steps: %d}", wallclock_uptime(), steps);
        return;
    }

    /* No warning on a missing step total, unlike a missing battery level. The
     * boot flush runs before imu_start() and so has no counter to read yet,
     * which is expected rather than notable -- and a genuine IMU failure has
     * already been reported once, at boot, by whoever failed to bring it up.
     * Warning here would put a line on every boot for a condition that is
     * correct, and say nothing new on the one where it is not.
     */
    if (steps < 0) {
        LOG_INF("{timestamp: %u, battery: %u%%, mv: %d}", wallclock_uptime(),
                battery_percent((uint16_t)mv), mv);
        return;
    }

    LOG_INF("{timestamp: %u, battery: %u%%, mv: %d, steps: %d}", wallclock_uptime(),
            battery_percent((uint16_t)mv), mv, steps);
}

/* What the boot erase is about to destroy, in records.
 *
 * Without it a reset destroys records silently: the loss can only be
 * reconstructed afterwards from the *client's* log, by reading the byte range
 * of a transfer that never completed, and that only works if a transfer
 * happened to be in flight. A reset between collections destroys the same data
 * and leaves no trace anywhere.
 *
 * So the count is taken from the part itself, by walking it for records that
 * are not erased. There is no persistent header to ask instead: `s_head` and
 * `s_cursor` live in RAM and die with the reset, which is the whole reason the
 * boot erase exists.
 *
 * **An upper bound, deliberately reported as one.** A record still on the part
 * may already have reached the phone and been acknowledged -- the scrub erases
 * lazily, four pages per service call, so acknowledged pages linger by design
 * (see SCRUB_PAGES_PER_SERVICE). The count cannot separate those from records
 * nobody ever saw. It is still the difference between "a reset happened" and "a
 * reset happened and it cost at most this much", and the second is what makes a
 * reset rate mean something.
 *
 * Reads in 256-byte chunks rather than a record at a time: 16 records per call
 * turns ~14.8k driver calls into ~928, and on memory-mapped internal flash the
 * whole walk is a few milliseconds once per boot.
 */
static uint32_t count_records_on_part(void)
{
    uint8_t buf[256];
    uint32_t found = 0;

    BUILD_ASSERT(sizeof(buf) % FLASH_RECORD_SIZE == 0,
                 "read chunk is not a whole number of records: the walk would straddle one");

    for (off_t off = 0; off < (off_t)s_fa->fa_size; off += (off_t)sizeof(buf)) {
        size_t chunk = MIN(sizeof(buf), (size_t)((off_t)s_fa->fa_size - off));

        if (flash_area_read(s_fa, off, buf, chunk) != 0) {
            /* A partial count beats abandoning the boot. Whatever was found so
             * far is still a floor under the loss, and this path has no way to
             * make the erase below any less correct.
             */
            LOG_WRN("Could not finish counting the old log at offset %u", (unsigned)off);
            return found;
        }

        for (size_t i = 0; i < chunk; i += FLASH_RECORD_SIZE) {
            for (size_t b = 0; b < FLASH_RECORD_SIZE; b++) {
                if (buf[i + b] != 0xFF) {
                    found++;
                    break;
                }
            }
        }
    }

    return found;
}

/* Wipes the whole partition and restarts the sequence. Only used at boot.
 *
 * Boot is the one moment a total erase is right. `k_uptime_get()` restarts at
 * zero, so every timestamp already in flash belongs to an epoch no anchor can
 * date (wallclock.h) -- the records are not merely old, they are unreadable.
 * Keeping them would mean handing a phone readings it cannot place in time.
 *
 * Every other erase in this file is now page-at-a-time and driven by what the
 * phone has acknowledged.
 */
static int reset_log(void)
{
    int rc;

    s_head = 0;
    s_cursor = 0;
    s_acked = 0;
    s_dumped = 0;
    s_clean_upto = 0;
    s_scrub_next = 0;

    /* **The whole partition, not just the first page.**
     *
     * Erasing page 0 alone and clearing the rest as the cursor reaches them is
     * sound on its own terms: every walk is bounded by s_head and s_cursor, so
     * nothing can read the stale pages.
     *
     * It is not sound alongside flash_store_restore(), which walks the raw part
     * rather than the bounded log and needs "blank" to mean "nothing was ever
     * here". Under a lazy erase the space past the cursor holds records from
     * *previous* sessions, so a restore would see the kept log, then a run of
     * stale records, and refuse -- on any ring that had been running, keeping
     * the log would quietly never work.
     *
     * The cost lands where it is cheapest: a full erase is ~85ms a page, so
     * ~4.8s across this partition, and it happens only on a boot that is going
     * to throw the log away regardless. A boot that *keeps* the log does not
     * erase at all.
     */
    rc = flash_area_erase(s_fa, 0, s_fa->fa_size);
    if (rc != 0) {
        LOG_ERR("Flash erase failed (%d)", rc);
        return rc;
    }
    s_clean_upto = (uint32_t)s_fa->fa_size;

    /* Every offset a client may be holding now refers to something else. */
    s_generation = next_generation();
    return 0;
}

/* Rebuilds the log from what is on the part, in place of the boot erase.
 *
 * **Why not simply erase.** The erase is right when a reset is rare: uptime
 * restarts at zero, so every stored timestamp belongs to an epoch nothing can
 * date, and handing a client undatable readings is worse than handing it none.
 * When resets are frequent that same erase becomes the largest destroyer of
 * data in the system, and firmware doing it deliberately.
 *
 * The way out is to make the records datable again rather than to keep them
 * blindly. The caller persists the anchor and the generation across the reset
 * (main.c) and passes the generation back in; this function rebuilds the
 * pointers, and reports the newest timestamp it found so the caller can rebase
 * uptime above it and keep the whole log on one anchor.
 *
 * **Keeping the generation is what makes it seamless.** A restored log holds
 * the same records at the same offsets, so the phone's cursor is still valid
 * and it resumes rather than restarting from zero. Drawing a fresh generation
 * here would be correct but would re-send everything the phone already had.
 *
 * **What it refuses to restore, and why refusing is cheap.** Anything it cannot
 * prove coherent falls back to the erase, which is exactly today's behaviour:
 *
 *   - a written record after a blank one -- a hole, so either a wrap or
 *     corruption, and the two are indistinguishable from here;
 *   - a timestamp that goes backwards -- the same test from the other side, and
 *     the one that catches a partially written record at the tail;
 *   - a log that has ever wrapped, which the caller screens by refusing to
 *     restore a persisted cursor at or beyond the partition size. Sequence
 *     numbers grow past fa_size and carry a wrap count that physical offsets
 *     alone cannot reconstruct. A lap needs ~4 days of continuous uptime, so
 *     refusing here costs very little in practice.
 *
 * Leading blanks are expected, not a fault: the scrub erases acknowledged pages,
 * so a healthy log routinely starts partway into the partition. The shape being
 * checked is blanks, then a monotonic written run, then blanks -- and the run is
 * the log.
 */
int flash_store_restore(uint32_t generation, uint32_t *newest_ts_ms)
{
    uint8_t buf[256];
    uint32_t first = 0;      /* start of the written run */
    uint32_t past = 0;       /* one past its end */
    uint32_t newest = 0;
    bool started = false;    /* the written run has begun */
    bool ended = false;      /* ...and has been followed by a blank */

    BUILD_ASSERT(sizeof(buf) % FLASH_RECORD_SIZE == 0,
                 "read chunk is not a whole number of records: the walk would straddle one");

    if (s_fa == NULL) {
        return -ENODEV;
    }

    /* Zero is reserved for "I have never spoken to this ring" and must never
     * become a live generation, or the stale-cursor guard loses its meaning.
     */
    if (generation == 0) {
        return -EINVAL;
    }

    for (off_t off = 0; off < (off_t)s_fa->fa_size; off += (off_t)sizeof(buf)) {
        size_t chunk = MIN(sizeof(buf), (size_t)((off_t)s_fa->fa_size - off));

        if (flash_area_read(s_fa, off, buf, chunk) != 0) {
            LOG_WRN("Could not read the old log at offset %u -- erasing instead",
                    (unsigned)off);
            return -EIO;
        }

        for (size_t i = 0; i < chunk; i += FLASH_RECORD_SIZE) {
            uint32_t at = (uint32_t)off + (uint32_t)i;
            bool blank = true;

            for (size_t b = 0; b < FLASH_RECORD_SIZE; b++) {
                if (buf[i + b] != 0xFF) {
                    blank = false;
                    break;
                }
            }

            if (blank) {
                if (started) {
                    ended = true;
                }
                continue;
            }

            /* A record after the run closed is a hole. Wrap or corruption; from
             * here they look identical, and both mean do not trust this log.
             */
            if (ended) {
                LOG_WRN("Old log has a hole at offset %u -- erasing instead", (unsigned)at);
                return -EINVAL;
            }

            {
                struct flash_record rec;

                memcpy(&rec, &buf[i], sizeof(rec));

                /* Records are appended in time order, so a fall is impossible in
                 * a coherent log. It catches a wrap whose hole happened to land
                 * off-partition, and a half-written record at the tail.
                 */
                if (started && rec.timestamp_ms < newest) {
                    LOG_WRN("Old log runs backwards at offset %u (%u after %u) -- erasing instead",
                            (unsigned)at, (unsigned)rec.timestamp_ms, (unsigned)newest);
                    return -EINVAL;
                }
                newest = rec.timestamp_ms;
            }

            if (!started) {
                started = true;
                first = at;
            }
            past = at + FLASH_RECORD_SIZE;
        }
    }

    if (!started) {
        return -ENOENT; /* nothing to keep; the caller resets as usual */
    }

    /* Timestamps are uint32 milliseconds -- ~49.7 days -- and the caller rebases
     * virtual uptime above `newest`, so the scale only ever climbs. Near the
     * ceiling, refuse: a wrapped timestamp would date new records *before* the
     * ones they follow, which is the single failure this mechanism exists to
     * prevent. 40 days leaves room for a log no phone ever collects, and the
     * erase resets the scale to zero.
     */
    if (newest > (uint32_t)(40ULL * 24 * 60 * 60 * 1000)) {
        LOG_WRN("Kept log's clock is near the uint32 ceiling (%u ms) -- erasing instead",
                (unsigned)newest);
        return -ERANGE;
    }

    s_head = first;
    s_cursor = past;
    s_dumped = first;

    /* Nothing is known to have been acknowledged: the phone's confirmations
     * lived in RAM. This costs only reclamation, and only until the next ack --
     * the phone resumes from its own cursor, so no record is re-sent.
     */
    s_acked = first;
    s_scrub_next = first;

    /* Everything above the run was verified blank on the way past, so the rest
     * of the partition is erased and ready to be written.
     */
    s_clean_upto = (uint32_t)s_fa->fa_size;

    /* A run being collapsed at the reset cannot be continued -- its state was in
     * RAM. The next refusal opens a new one.
     */
    s_run_open = false;

    s_generation = generation;

    if (newest_ts_ms != NULL) {
        *newest_ts_ms = newest;
    }

    LOG_WRN("Log kept across the reset: %u record(s), bytes %u..%u, generation %u",
            (past - first) / FLASH_RECORD_SIZE, (unsigned)first, (unsigned)past,
            (unsigned)generation);
    return 0;
}

/* Erases the one page that sequence number `seq` falls in. */
static int erase_page_at(uint32_t seq)
{
    off_t phys = (off_t)(seq % s_fa->fa_size);
    off_t page_start = phys - (phys % (off_t)s_page_size);

    return flash_area_erase(s_fa, page_start, s_page_size);
}

/* How many pages one flash_store_service() call will erase before leaving the
 * rest for the next cycle.
 *
 * There is no hurry and there is a reason not to hurry. The *space* is already
 * reclaimed the moment the head moves -- the erase only decides how long
 * acknowledged vitals go on existing on the part -- so the deadline is measured
 * in minutes against an exposure measured in days. What a bound buys is a
 * predictable cycle: a phone returning after four days away acknowledges the
 * whole buffer at once, and 57 pages back to back is ~4.8 seconds added to one
 * measurement cycle. The blind gap between bursts is time the ring is not
 * measuring, so lengthening it unpredictably is the one cost worth avoiding
 * here. Four pages is ~340ms, and the deepest possible
 * backlog drains inside half an hour.
 *
 * Falling behind is free, and self-correcting: a page the scrub has not reached
 * by the time the cursor laps onto it is erased by the write path instead, and
 * skipped here afterwards.
 */
#define SCRUB_PAGES_PER_SERVICE 4u

/* Erases up to `budget` acknowledged pages, oldest first. Returns how many.
 *
 * Erasing strictly in sequence order is what lets a single pointer describe the
 * result: everything below s_scrub_next is gone from the part.
 *
 * One test decides whether a page is this function's to erase, and getting it
 * wrong destroys live records rather than freeing dead ones. The page at
 * s_scrub_next will next be *written* at sequence `next_use`, one lap on. If
 * the erased-and-ready frontier has already passed that point, the write path
 * has cleared this page and may have been writing into it since -- stepping
 * over it without erasing is then not a shortcut, it is the only correct
 * action. Otherwise the page is untouched since the records in it were
 * acknowledged, and erasing it here saves the write path the work later.
 */
static unsigned int scrub_pages(unsigned int budget)
{
    unsigned int done = 0;

    if (s_fa == NULL) {
        return 0;
    }

    /* Whole pages the head has moved past, and nothing else. A partially
     * acknowledged page is still holding records the phone never got.
     */
    while (done < budget && s_scrub_next + s_page_size <= s_head) {
        uint32_t next_use = s_scrub_next + (uint32_t)s_fa->fa_size;

        if (s_clean_upto > next_use) {
            s_scrub_next += s_page_size; /* the write path got there first */
            continue;
        }

        if (flash_area_erase(s_fa, (off_t)(s_scrub_next % s_fa->fa_size), s_page_size) != 0) {
            /* Left where it is deliberately: a failed erase must not advance
             * the frontier, or write_record() would later believe a dirty page
             * was clean and write records into flash that cannot hold them.
             */
            LOG_ERR("Scrubbing acknowledged page at 0x%lx failed",
                    (long)(s_scrub_next % s_fa->fa_size));
            return done;
        }
        /* **Read it back before believing it.**
         *
         * The driver returning 0 says the erase was accepted, not that the part
         * did it. Those differ exactly once in the life of a flash chip and the
         * consequence is unbounded: the write path trusts s_scrub_next, so a
         * page that reports erased and is not gets records written into it
         * un-erased, which on NOR is a bitwise AND -- readings that are not
         * either value and still parse. Nothing downstream could tell.
         *
         * Three 16-byte samples rather than the whole page: start, middle and
         * end catch a partial erase, which is the realistic failure, and cost
         * three reads against an operation that just took ~85ms.
         *
         * It is also the only positive evidence that the scrub runs at all. A
         * successful scrub used to log nothing, so "acknowledged vitals stop
         * existing on the part" -- the entire point of doing this on the ack
         * path -- was a claim no console could support.
         */
        {
            off_t phys = (off_t)(s_scrub_next % s_fa->fa_size);
            /* Start, middle and end -- a partial erase is the realistic
             * failure, and three samples catch it for three reads against an
             * operation that just took ~85ms.
             */
            const off_t sample[] = {phys, phys + (off_t)(s_page_size / 2),
                                    phys + (off_t)s_page_size - 16};

            for (size_t p = 0; p < ARRAY_SIZE(sample); p++) {
                uint8_t probe[16];

                if (flash_area_read(s_fa, sample[p], probe, sizeof(probe)) != 0) {
                    LOG_ERR("Scrubbed page at 0x%lx but could not read it back", (long)phys);
                    return done;
                }
                for (size_t i = 0; i < sizeof(probe); i++) {
                    if (probe[i] != 0xFFu) {
                        /* **Deliberately does not advance s_scrub_next.**
                         *
                         * The frontier is what write_record() trusts when it
                         * decides to skip its own erase. Advancing it over a
                         * page that is not actually erased would hand the write
                         * path a dirty page it believes is clean -- and on NOR
                         * that does not fail, it ANDs, producing records that
                         * are neither value and still parse. Leaving the
                         * frontier put means the write path erases this page
                         * itself when the cursor reaches it, which is slower and
                         * correct.
                         */
                        LOG_ERR("Page at 0x%lx reports erased but byte 0x%lx is 0x%02x -- "
                                "not advancing the scrub; the write path will clear it",
                                (long)phys, (long)(sample[p] + (off_t)i), probe[i]);
                        return done;
                    }
                }
            }

            LOG_INF("Scrubbed acknowledged page at 0x%lx; %u byte(s) of vitals gone from the part",
                    (long)phys, (unsigned)s_page_size);
        }

        s_scrub_next += s_page_size;
        done++;
    }

    return done;
}

/* Reclaims whole pages the phone has finished with, oldest first.
 *
 * **Advances the head; the erase follows separately.** Moving the head is what
 * buys the space back: once it has passed a record the ring may overwrite it
 * without warning that it destroyed something undelivered, which is the whole
 * difference between a buffer that loses data and one that recycles it. That
 * part is pure arithmetic and happens the instant it can. Erasing is what makes
 * the bytes actually leave the part, and it is slow, so it is paid for out of
 * the measurement cycle's own time (flash_store_service()).
 *
 * Each page is still erased exactly once per lap -- scrub_pages() and the write
 * path agree on who owns which page through s_scrub_next -- so none of this
 * costs wear on a part rated for ~10k cycles.
 *
 * Only *whole* acknowledged pages move. A page holding one unacknowledged
 * record stays, because flash cannot erase less than a page and the alternative
 * is destroying a reading the phone never got.
 */
static void reclaim_acked_pages(void)
{
    while (true) {
        uint32_t page_end = s_head - (s_head % s_page_size) + s_page_size;

        if (page_end > s_acked) {
            break;
        }
        s_head = page_end;
    }
}

uint32_t flash_store_cursor(void)
{
    return s_cursor;
}

uint32_t flash_store_head(void)
{
    return s_head;
}

uint32_t flash_store_capacity(void)
{
    if (s_fa == NULL) {
        return 0;
    }

    return (uint32_t)s_fa->fa_size - (uint32_t)s_page_size;
}

uint32_t flash_store_page_size(void)
{
    return (uint32_t)s_page_size;
}

int flash_store_release(uint32_t upto)
{
    if (s_fa == NULL) {
        return -ENODEV;
    }
    /* An acknowledgement of something never sent, or of something already
     * dropped, is a client that has lost track. Refusing beats erasing on it.
     */
    if (upto > s_cursor || upto < s_head) {
        return -EINVAL;
    }
    if (upto <= s_acked) {
        return 0; /* nothing new; acknowledgements are allowed to repeat */
    }

    /* **One store, and that is the whole function.** Everything that follows
     * from an acknowledgement -- moving the head, erasing the pages -- happens
     * on the measurement thread in flash_store_service().
     *
     * This is a threading rule, not an optimisation. This call runs on the
     * Bluetooth RX thread, and it is the only entry point into this module that
     * does not run on main. Doing the work here would make s_head mutable from
     * two threads at once, which on a log whose failure mode is silently
     * corrupting a wearer's vitals is not a trade worth taking for the ~121s of
     * latency it saves. See the ownership note at the top of this file.
     */
    s_acked = upto;
    return 0;
}

void flash_store_service(void)
{
    if (s_fa == NULL) {
        return;
    }

    reclaim_acked_pages();
    (void)scrub_pages(SCRUB_PAGES_PER_SERVICE);
}

uint32_t flash_store_generation(void)
{
    return s_generation;
}

uint32_t flash_store_boot_destroyed(void)
{
    return s_boot_destroyed;
}

bool flash_record_refused(const struct flash_record *rec)
{
    return rec != NULL && (rec->flags & FLAG_REFUSED) != 0;
}

bool flash_record_contact(const struct flash_record *rec)
{
    return rec != NULL && (rec->flags & FLAG_CONTACT) != 0;
}

enum flash_refusal flash_record_reason(const struct flash_record *rec)
{
    if (rec == NULL) {
        return FLASH_REFUSAL_NOT_WORN;
    }
    return (enum flash_refusal)((rec->flags & FLAG_REASON_MASK) >> FLAG_REASON_SHIFT);
}

uint8_t flash_record_movement(const struct flash_record *rec)
{
    if (rec == NULL) {
        return FLASH_MOVEMENT_UNKNOWN;
    }
    return (uint8_t)((rec->flags & FLAG_MOVE_MASK) >> FLAG_MOVE_SHIFT);
}

/* The lower edge of each bucket above FLASH_MOVEMENT_UNKNOWN, so the encoder
 * and the labels below cannot disagree about where a boundary is. Indexed from
 * bucket 1; the table and the strings are read in the same order.
 */
static const uint16_t movement_floor_milli_g[] = {0, 50, 100, 150, 250, 500, 1000};

uint8_t flash_movement_bucket(uint16_t milli_g)
{
    /* Walks down from the top rather than binary-searching seven entries: this
     * runs once per record, and the loop is shorter than the comment explaining
     * a search would be.
     */
    for (size_t i = ARRAY_SIZE(movement_floor_milli_g); i > 0; i--) {
        if (milli_g >= movement_floor_milli_g[i - 1]) {
            return (uint8_t)i;
        }
    }

    /* Unreachable while the table's first floor is 0, which is why the walk
     * above returns directly instead of defaulting a variable to 1 -- a default
     * that can never survive the loop reads as the guarantee and is not one.
     *
     * If the table ever stops starting at zero this becomes reachable, and
     * UNKNOWN is the right answer rather than the still bucket: a value the
     * table cannot encode has not been measured as far as anything downstream
     * is concerned, and saying "still" would be inventing evidence. The host
     * test asserts a measured window never lands here.
     */
    return FLASH_MOVEMENT_UNKNOWN;
}

const char *flash_movement_label(uint8_t bucket)
{
    static const char *const labels[] = {"unrecorded", "<50",    "50-99",  "100-149",
                                         "150-249",    "250-499", "500-999", ">=1000"};

    BUILD_ASSERT(ARRAY_SIZE(labels) == FLASH_MOVEMENT_BUCKETS,
                 "one label per value the three bits can hold");
    BUILD_ASSERT(ARRAY_SIZE(movement_floor_milli_g) == FLASH_MOVEMENT_BUCKETS - 1,
                 "one floor per bucket above UNKNOWN");

    return (bucket < ARRAY_SIZE(labels)) ? labels[bucket] : "unknown";
}

int flash_store_init(flash_store_battery_mv_fn battery_mv, flash_store_steps_fn steps,
                     uint32_t restore_generation, uint32_t *newest_ts_ms)
{
    int rc;

    s_battery_mv = battery_mv;
    s_steps = steps;
    s_run_open = false;
    /* Cleared per call rather than per boot: the restore path below returns
     * before the erase, and a kept log must not inherit the last attempt's
     * count.
     */
    s_boot_destroyed = 0;

    if (newest_ts_ms != NULL) {
        *newest_ts_ms = 0;
    }

    rc = flash_area_open(STORAGE_PARTITION_ID, &s_fa);
    if (rc != 0) {
        LOG_ERR("flash_area_open failed (%d)", rc);
        s_fa = NULL;
        return rc;
    }

    /* Asked, not assumed. Every erase in this file is now page-granular, so a
     * wrong page size would either fail the erase or silently wipe a neighbour.
     * It is 4096 on this part; reading it from the driver means the ring buffer
     * arithmetic stays correct if the storage ever moves to a different one.
     */
    {
        struct flash_pages_info info;
        const struct device *dev = flash_area_get_device(s_fa);

        if (dev == NULL || flash_get_page_info_by_offs(dev, s_fa->fa_off, &info) != 0) {
            LOG_ERR("Cannot determine flash page size");
            s_fa = NULL;
            return -ENODEV;
        }
        s_page_size = info.size;
    }

    /* The ring arithmetic assumes records tile pages and the partition exactly,
     * so a record never straddles either boundary. Both hold on this board
     * (16 divides 4096, and 4096 divides the 232KB partition); checking beats
     * discovering it as a corrupt record after a wrap.
     */
    if ((s_page_size % FLASH_RECORD_SIZE) != 0U || (s_fa->fa_size % s_page_size) != 0U) {
        LOG_ERR("Partition %u / page %u do not tile a %u-byte record", (unsigned)s_fa->fa_size,
                (unsigned)s_page_size, FLASH_RECORD_SIZE);
        s_fa = NULL;
        return -EINVAL;
    }

    /* Keep the log if the caller carried a generation across the reset and what
     * is on the part backs it up. Anything doubtful falls through to the erase,
     * which is the behaviour this had before there was an alternative.
     */
    if (restore_generation != 0) {
        rc = flash_store_restore(restore_generation, newest_ts_ms);
        if (rc == 0) {
            return 0;
        }
        if (rc != -ENOENT) {
            LOG_WRN("Could not keep the old log (%d) -- erasing", rc);
        }
    }

    /* Before the erase, obviously, and after the geometry checks so the walk
     * can trust the record/page arithmetic it is built on.
     */
    {
        uint32_t survivors = count_records_on_part();

        if (survivors > 0) {
            LOG_WRN("Boot erase: destroying %u record(s) left on the part by the last run "
                    "(upper bound -- some may already have reached the phone)",
                    survivors);
        }

        rc = reset_log();
        if (rc != 0) {
            return rc;
        }

        /* Kept for the control read (ble.c) so the phone learns what a reset
         * cost it, not only that one happened. Recorded *after* the erase and
         * not before: this is a report of what was destroyed, and a reset_log()
         * that failed destroyed nothing.
         */
        s_boot_destroyed = survivors;
    }

    LOG_INF("Vitals log reset: %u bytes, %u-byte pages, %u records (%u usable), generation %u",
            (unsigned)s_fa->fa_size, (unsigned)s_page_size,
            (unsigned)(s_fa->fa_size / FLASH_RECORD_SIZE),
            (unsigned)((s_fa->fa_size - s_page_size) / FLASH_RECORD_SIZE), s_generation);
    return 0;
}

int flash_store_dump(void)
{
    if (s_fa == NULL) {
        return -ENODEV;
    }

    /* Deliberately does *not* close an open run, unlike the flush.
     *
     * The temptation is to close it so the console shows the refusals as they
     * happen, but main.c already logs every refused window as it is refused --
     * the console is not silent during a run and does not need this. What
     * closing it here would cost is the collapsing itself: this runs once per
     * ~121s cycle, so it would cap every run at one cycle and turn a wearer
     * whose perfusion is poor all evening into one record per cycle forever.
     * The run is bounded anyway by `repeat` saturating, so nothing accumulates
     * without limit.
     */

    /* From where the last dump stopped, so the output is a constant few lines a
     * minute however long the log gets. The running total on the summary line
     * is what says the history behind it is still there.
     */
    s_dumped = dump_records(s_dumped, "Dumped");
    dump_battery();

    return 0;
}

/* Writes one already-built record at the cursor, flushing first if the
 * partition has no room for it. The single place a record reaches flash.
 */
static int write_record(const struct flash_record *rec)
{
    int rc;

    /* First, take account of anything the phone acknowledged since the last
     * record. A comparison and at most a few page-sized additions.
     *
     * **Not for safety -- for freshness.** Nothing is lost without it: the
     * lap-drop reads s_acked directly, so it still declines to warn about
     * destroying records the phone holds, and a page dropped after being
     * acknowledged is a page nobody needed. What it buys is that an ACK
     * arriving in the middle of a measurement cycle frees its space on the
     * *next record* rather than on the next cycle. Otherwise the ring can spend
     * a cycle lapping and dropping pages it was already free to reuse, and
     * main.c's backlog nudge spends that cycle looking at a backlog that has
     * already been collected.
     */
    reclaim_acked_pages();

    /* Out of erased flash: clear one more page ahead of the cursor.
     *
     * The invariants, and every branch below keeps them:
     *
     *     head <= acked <= cursor <= clean_upto <= head + fa_size
     *
     * Erasing the page at `clean_upto` destroys whatever the ring wrote there
     * one lap ago -- the records at [clean_upto - fa_size, clean_upto - fa_size
     * + page_size). If the head has already moved past them, because the phone
     * acknowledged them, nothing is lost and this is pure recycling.
     */
    if (s_cursor + FLASH_RECORD_SIZE > s_clean_upto) {
        uint32_t dropped_end = s_clean_upto + s_page_size - s_fa->fa_size;

        /* Before the first lap there is nothing behind this page to destroy. */
        if (s_clean_upto >= s_fa->fa_size && s_head < dropped_end) {
            /* **This is where unacknowledged data dies, and it is deliberate.**
             * The buffer has lapped and the phone has not kept up -- out of
             * range, asleep, or the app swiped away. The choice is to drop the
             * oldest page or to stop recording, and this is a ring buffer:
             * the newest data wins, because a log that has stopped recording
             * has also stopped being a record of anything.
             *
             * It earns a warning rather than silence because a consumer of this
             * log needs to know it happened -- a gap with a reason attached
             * beats a gap without one.
             */
            if (s_acked < dropped_end) {
                LOG_WRN("Log lapped: dropping %u unacknowledged byte(s)",
                        dropped_end - MAX(s_head, s_acked));
            }
            s_head = dropped_end;
            if (s_acked < s_head) {
                s_acked = s_head;
            }
            if (s_dumped < s_head) {
                s_dumped = s_head;
            }
        }

        /* Unless the acknowledgement already did it. The page the cursor is
         * lapping onto held the records at [cursor - fa_size, ... + page_size),
         * and s_scrub_next says how far the ack path has erased -- so if it
         * covers them, this page is erased flash already and erasing it again
         * would be a second ~85ms and a second wear cycle for nothing.
         *
         * The test is written against the *previous lap's* sequence numbers on
         * purpose. A page-boundary comparison against the cursor would be one
         * lap out and would skip the erase on a page still holding records.
         */
        if (s_clean_upto < s_fa->fa_size ||
            s_clean_upto + s_page_size - s_fa->fa_size > s_scrub_next) {
            rc = erase_page_at(s_cursor);
            if (rc != 0) {
                LOG_ERR("Flash erase failed (%d)", rc);
                return rc;
            }
        }

        /* The page is now erased flash -- by the branch above, or by the scrub
         * before it -- and the invariant at the top of this block says so. This
         * advances either way, which is the point: it is a statement about what
         * is in the page, not about who cleared it. **Omitting it was
         * catastrophic**, and
         * quietly: s_clean_upto stayed at one page forever, so from the moment
         * the cursor reached 0x1000 the test above was true on *every* append.
         * Each record then erased the page it was about to be written into --
         * destroying the records already there -- and wrote itself into the
         * cleared page. A page therefore held exactly one record, preceded by
         * erased flash, and flash_store_foreach() stopped dead on that hole:
         *
         *     <wrn> Empty slot inside the live range at 0x1000
         *     <inf> Dumped 0 package(s), 310 stored, 0 acknowledged
         *
         * Every walk was affected, but the replay is where it did the damage. It
         * delivered the first page, hit the hole, made no progress on the next
         * slice and abandoned -- so no END frame was ever sent, the phone never
         * acknowledged, the head never moved and nothing was ever reclaimed.
         *
         * It was also an erase per record on a part rated for ~10k cycles.
         */
        s_clean_upto += s_page_size;
    }

    rc = flash_area_write(s_fa, (off_t)(s_cursor % s_fa->fa_size), rec, RECORD_SIZE);
    if (rc != 0) {
        LOG_ERR("Flash write failed (%d)", rc);
        return rc;
    }

    s_cursor += RECORD_SIZE;
    return 0;
}

/* Commits the refusal run being accumulated, if there is one.
 *
 * Two paths must call this and only two: anything appending a record *after*
 * the run (or the log would claim the run happened later than it did), and the
 * flush (or the run is erased having never been written). The periodic dump
 * deliberately does not -- see flash_store_dump().
 */
static int close_run(void)
{
    if (!s_run_open) {
        return 0;
    }

    /* Cleared before the write, not after: on a write failure the run is gone
     * either way, and leaving it open would retry it on every subsequent call.
     */
    s_run_open = false;

    return write_record(&s_run);
}

void flash_record_from_sample(struct flash_record *rec, const struct flash_sample *s)
{
    if (rec == NULL || s == NULL) {
        return;
    }

    /* Uptime in ms would take ~49 days to reach the empty marker, but a reading
     * that lands on it exactly would look like a free slot and truncate the
     * whole log behind it. One millisecond is a cheaper lie than that.
     */
    rec->timestamp_ms = (s->timestamp_ms == EMPTY_MARKER) ? (EMPTY_MARKER - 1) : s->timestamp_ms;
    rec->bpm = s->bpm;
    rec->spo2_tenths = s->spo2_tenths;
    rec->confidence = s->confidence;
    rec->perfusion_milli = s->perfusion_milli;
    rec->steps = s->steps;
    rec->flags = (uint8_t)((s->contact ? FLAG_CONTACT : 0) |
                           (flash_movement_bucket(s->movement_milli_g) << FLAG_MOVE_SHIFT));
    rec->repeat = 0;
}

int flash_store_append(const struct flash_sample *sample)
{
    struct flash_record rec;
    int rc;

    if (s_fa == NULL) {
        return -ENODEV;
    }
    if (sample == NULL) {
        return -EINVAL;
    }

    /* A believed reading ends any refusal run: the wearer's pulse came back, so
     * the run is over and has to be on the far side of this record in the log.
     */
    rc = close_run();
    if (rc != 0) {
        return rc;
    }

    flash_record_from_sample(&rec, sample);
    return write_record(&rec);
}

/* Whether this refusal is worth its own record instead of a slot in a run.
 *
 * **The run collapse would otherwise destroy the only data that can settle the
 * movement threshold.** The collapse is right for the case it was written for
 * -- "two windows that are both `not worn` differ only in noise" -- and a
 * MOVING window that still found a believable pulse is not that case. It is a
 * measurement the ring made, refused on evidence from a different part, and a
 * run keeps only the opening window's numbers: on one measured day that meant
 * 361 refused windows storing roughly thirty windows' worth of bpm, confidence
 * and perfusion between them.
 *
 * It also makes the threshold undecidable from the log. Comparing refused
 * windows' bpm against their accepted neighbours, per movement bucket, needs a
 * bpm per refused window, and a collapsed run does not have one. The movement
 * bits put the bucket on every record; this puts the reading back beside it.
 *
 * **Narrow on purpose, and the narrowness is what makes it affordable.** Only
 * MOVING, and only when the window found a pulse it would otherwise have
 * believed -- so a bpm and a confidence at or above the bar a reading has to
 * clear. Everything else still collapses: NOT_WORN overnight, and the weak and
 * absent pulses that carry nothing to preserve. At 16 bytes a record, a worn
 * day that refused every one of its ~460 windows this way costs ~7 KB, and the
 * windows that qualify are by definition the minority the gate is arguable
 * about.
 *
 * Note what this deliberately does *not* do: the record is still flagged
 * refused, so nothing downstream reads it as a vital sign. This changes what
 * the log remembers, not what the ring asserts.
 */
static bool refusal_keeps_evidence(const struct flash_sample *sample, enum flash_refusal why)
{
    return why == FLASH_REFUSAL_MOVING && sample->bpm != 0 &&
           sample->confidence >= VITALS_BPM_CONFIDENCE_MIN;
}

int flash_store_append_refusal(const struct flash_sample *sample, enum flash_refusal why)
{
    struct flash_record rec;
    int rc;

    if (s_fa == NULL) {
        return -ENODEV;
    }
    /* Bound by the last reason rather than by the mask: the reason has to
     * survive the three bits the flags byte gives it, and an out-of-range value
     * would otherwise be shifted straight into the spare bits above it.
     *
     * FLASH_REFUSAL_MOVING_LEGACY is refused as well, not merely unused. It is
     * the one value a two-bit reader turns into NOT_WORN, which is what made the
     * 08-19 numbering cost a diagnosis; a caller reaching it is either replaying
     * an old log into the wrong function or has renumbered the enum without
     * reading why 4 is empty. Both should stop here rather than write a record
     * that lies to every short reader downstream.
     */
    if (sample == NULL || (unsigned int)why > FLASH_REFUSAL_MOVING ||
        why == FLASH_REFUSAL_MOVING_LEGACY) {
        return -EINVAL;
    }

    flash_record_from_sample(&rec, sample);
    rec.flags |= FLAG_REFUSED | ((uint8_t)why << FLAG_REASON_SHIFT);

    /* Same kind of refusal as the run in progress, and room left in the count:
     * absorb it. Only the count moves -- the run keeps the first refusal's
     * timestamp and its evidence, so the record says "this started here and
     * held for this many windows" rather than drifting to describe the last
     * one. The evidence discarded here is the whole reason the reasons are
     * matched rather than the fields: two windows that are both "not worn"
     * differ only in noise, and collapsing them loses nothing worth a record.
     */
    if (!refusal_keeps_evidence(sample, why) && s_run_open &&
        (s_run.flags & FLAG_REASON_MASK) == (rec.flags & FLAG_REASON_MASK) &&
        (s_run.flags & FLAG_CONTACT) == (rec.flags & FLAG_CONTACT) && s_run.repeat < REPEAT_MAX) {
        s_run.repeat++;

        /* Steps still accumulate across the run. A wearer who is walking with
         * the ring in a pocket is refusing every window and stepping through
         * all of them, and that is worth knowing on the record that covers it.
         */
        if (UINT16_MAX - s_run.steps > sample->steps) {
            s_run.steps += sample->steps;
        } else {
            s_run.steps = UINT16_MAX;
        }
        return 0;
    }

    /* A different kind of refusal, a saturated count, or a refusal carrying
     * evidence: the old run is finished either way.
     */
    rc = close_run();
    if (rc != 0) {
        return rc;
    }

    /* An evidence-bearing refusal is written standalone rather than opening a
     * run, so one record means one window and nothing later can be absorbed
     * into it. Opening a run with it would keep its numbers -- a run keeps the
     * opener's evidence -- but would also report them under a repeat count
     * covering windows that did not measure them, and the whole point of
     * writing this record is that it is read as a single window's measurement.
     */
    if (refusal_keeps_evidence(sample, why)) {
        return write_record(&rec);
    }

    s_run = rec;
    s_run_open = true;
    return 0;
}
