/* Host tests for flash_store.c -- the ring arithmetic, against a modelled part.
 *
 * Two of these paths are the riskiest things in the tree under change: the ring
 * wrap arithmetic, which fails by corrupting records rather than by crashing,
 * and acknowledge-then-erase, which is the one path in the firmware that
 * destroys data on purpose. Neither announces itself, and both have been wrong
 * on hardware -- a log with a hole one page in that every walk stopped dead on,
 * reported as "transfer complete".
 *
 * On hardware a single lap is 14,592 records: 77 minutes of unbroken finger
 * contact even with the cycle shortened as far as the BUILD_ASSERTs allow. Here
 * it is a fraction of a second, so it can be run on every change, which is the
 * only kind of test that gets run at all.
 *
 * **What makes this worth trusting is the flash model, not the assertions.** It
 * only clears bits, exactly like the part: writing a record into a slot nobody
 * erased first does not fail and does not read back as either value. Every test
 * asserts that never happened.
 *
 * Every assertion here has been checked by seeding the fault back in and
 * confirming it fails. Five of them: the one-page hole above (s_clean_upto
 * never advancing) fails 37 checks; erasing a page twice per lap
 * fails on the erase counts; a head that follows the acknowledgement rather than
 * whole pages fails immediately; a scrub that does not step over pages the
 * cursor has reclaimed destroys live records; and moving the reclaim out of
 * write_record() costs a cycle of buffer space. **A test nobody has watched fail
 * is a test nobody should trust** -- this module can report success while
 * losing data, which is how three faults survived a hardware sign-off.
 *
 * ---------------------------------------------------------------------------
 * **On threads.** The module has one writer -- the measurement loop -- for every
 * piece of its state except `s_acked`, which only the Bluetooth RX thread
 * writes. That is what makes a single-threaded harness an honest model of it
 * rather than a convenient one.
 *
 * The two entry points that are not on main are exercised anyway, through the
 * flash model's hook: it runs a callback at the start of any flash operation, so
 * a test can put `flash_store_release()` inside an append the way the RX thread
 * does, or an append inside a replay walk the way the work queue does. See the
 * interleaving section below.
 *
 * What it still cannot reach is a preemption *between two ordinary statements*.
 * If a second writer is ever added to this module, that gap stops being
 * theoretical and this harness stops being sufficient.
 * ---------------------------------------------------------------------------
 *
 * Usage: make -f Makefile flash   (or just `make all`)
 */
#include "flash_store.h"
/* For VITALS_BPM_CONFIDENCE_MIN -- the bar a refusal has to clear to be worth
 * its own record. The same constant flash_store.c decides on, so the test moves
 * with the gate rather than pinning a number the firmware no longer uses.
 */
#include "vitals.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/storage/flash_map.h>

/* A deliberately small part, in the same shape as the real one: 16 pages, 16
 * records to a page. Small enough that a lap is a few hundred appends and the
 * page-level bookkeeping can be checked page by page; the real geometry gets
 * its own test at the bottom.
 */
#define TEST_PAGE  256u
#define TEST_PAGES 16u
#define TEST_SIZE  (TEST_PAGE * TEST_PAGES)

#define RECS_PER_PAGE (TEST_PAGE / FLASH_RECORD_SIZE)

static unsigned int failures;
static unsigned int checks;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                                          \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(got, want, what)                                                                  \
    CHECK((unsigned long)(got) == (unsigned long)(want), "%s: got %lu, want %lu", (what),           \
          (unsigned long)(got), (unsigned long)(want))

/* ---- the harness --------------------------------------------------------- */

static uint32_t next_index; /* every record's timestamp is its own index */

static void fresh(size_t size, size_t page)
{
    shim_flash_init(size, page);
    shim_log_reset();
    next_index = 0;
    CHECK_EQ(flash_store_init(NULL, NULL, 0, NULL), 0, "flash_store_init");
}

/* One believed reading. The values are arbitrary and constant except the
 * timestamp, which is the record's index: that is what lets a walk say not just
 * "how many records" but "which ones, and in what order".
 */
static int append_one(void)
{
    struct flash_sample s = {
        .timestamp_ms = ++next_index,
        .bpm = 72,
        .spo2_tenths = 976,
        .confidence = 800,
        .perfusion_milli = 15,
        .steps = 1,
        .contact = true,
    };

    return flash_store_append(&s);
}

static void append_n(unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        CHECK_EQ(append_one(), 0, "append");
    }
}

/* What a walk found. `ordered` is the property that matters: a bad `% fa_size`
 * shows up as records that are individually valid and collectively nonsense.
 */
struct walk {
    unsigned int count;
    uint32_t first;
    uint32_t last;
    bool ordered;
};

static bool walk_cb(uint32_t offset, const struct flash_record *rec, void *user_data)
{
    struct walk *w = user_data;

    (void)offset;
    if (w->count == 0) {
        w->first = rec->timestamp_ms;
    } else if (rec->timestamp_ms != w->last + 1) {
        w->ordered = false;
    }
    w->last = rec->timestamp_ms;
    w->count++;
    return true;
}

static struct walk walk_from(uint32_t from, uint32_t *stopped_at)
{
    struct walk w = {.ordered = true};
    uint32_t at = flash_store_foreach(from, walk_cb, &w);

    if (stopped_at != NULL) {
        *stopped_at = at;
    }
    return w;
}

/* Everything the log currently holds, walked from the head. */
static struct walk walk_all(void)
{
    return walk_from(flash_store_head(), NULL);
}

static unsigned int held_records(void)
{
    return (flash_store_cursor() - flash_store_head()) / FLASH_RECORD_SIZE;
}

/* Acknowledge everything written so far, then run the housekeeping the next
 * measurement cycle would. Two steps because on the ring they are two threads:
 * the acknowledgement arrives on the Bluetooth RX thread and does nothing but
 * record itself, and main does the work. The tests that care about the gap
 * between them drive the two halves separately.
 */
static void ack_all(void)
{
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");
    flash_store_service();
}

static void expect_clean_writes(const char *where)
{
    CHECK(shim_flash_write_to_dirty == 0, "%s: %u byte(s) written to unerased flash", where,
          shim_flash_write_to_dirty);
}

/* A reset, modelled as what a reset actually is: every static in the module
 * starts over while the partition keeps whatever was on it. Deliberately not
 * shim_flash_init(), which would wipe the flash and defeat the point.
 */
static void reboot(uint32_t generation, uint32_t *newest)
{
    shim_log_reset();
    CHECK_EQ(flash_store_init(NULL, NULL, generation, newest), 0, "init after reset");
}

/* Writes `rec` straight to the part, behind the store's back, to build the
 * shapes a healthy log cannot produce -- a hole, or time running backwards.
 */
static void poke_record(uint32_t offset, const struct flash_record *rec)
{
    const struct flash_area *fa = NULL;

    CHECK_EQ(flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa), 0, "open for poke");
    CHECK_EQ(flash_area_write(fa, (off_t)offset, rec, sizeof(*rec)), 0, "poke");
}

/* ---- tests --------------------------------------------------------------- */

/* Phase 0 of the hardware checklist, on the host: the numbers the boot line
 * prints, and the one property the generation exists for.
 */
static void test_geometry(void)
{
    uint32_t g1, g2;

    fresh(TEST_SIZE, TEST_PAGE);
    CHECK_EQ(flash_store_page_size(), TEST_PAGE, "page size");
    CHECK_EQ(flash_store_capacity(), TEST_SIZE - TEST_PAGE, "capacity is one page short");
    CHECK_EQ(flash_store_cursor(), 0, "cursor at boot");
    CHECK_EQ(flash_store_head(), 0, "head at boot");
    g1 = flash_store_generation();
    CHECK(g1 != 0, "generation 0 means 'never spoken to this ring'");

    fresh(TEST_SIZE, TEST_PAGE);
    g2 = flash_store_generation();
    CHECK(g1 != g2, "generation repeated across a reset (%u) -- the stale-cursor guard is blind",
          g1);

    /* **The whole partition, not one page.** A lazy erase clears page 0 only,
     * to avoid ~5s of stalled boot.
     *
     * flash_store_restore() reversed the trade. It walks the raw part rather
     * than the bounded log, so it needs "blank" to mean "nothing was ever
     * here" -- and under the lazy erase the space past the cursor held records
     * from previous sessions, which made keeping a log impossible on any ring
     * that had been running. The stall now falls only on a boot that was
     * discarding the log anyway; a boot that keeps it erases nothing at all and
     * is faster than this ever was.
     */
    CHECK_EQ(shim_flash_total_erases(), TEST_PAGES, "pages erased at boot");
}

/* The plain path: write records, read them back, get the same records. */
static void test_append_and_walk(void)
{
    struct walk w;
    uint32_t stopped = 0;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(100);

    w = walk_from(0, &stopped);
    CHECK_EQ(w.count, 100, "records walked");
    CHECK_EQ(w.first, 1, "first record");
    CHECK_EQ(w.last, 100, "last record");
    CHECK(w.ordered, "records came back out of order");
    CHECK_EQ(stopped, flash_store_cursor(), "walk stopped at the cursor");
    CHECK_EQ(held_records(), 100, "held");
    expect_clean_writes("append_and_walk");
}

/* **The N2 headline.** Acknowledging a whole page frees it *and* erases it;
 * acknowledging less than a page frees nothing, because flash cannot erase less.
 *
 * Also the threading rule: `flash_store_release()` runs on the Bluetooth RX
 * thread and must do nothing but record the offset. Everything it implies
 * happens later, on the measurement thread.
 */
static void test_ack_frees_and_scrubs_whole_pages(void)
{
    fresh(TEST_SIZE, TEST_PAGE);

    /* Less than a page. */
    append_n(RECS_PER_PAGE - 1);
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");
    flash_store_service();
    CHECK_EQ(flash_store_head(), 0, "a partial page must free nothing");
    CHECK(!shim_flash_page_is_erased(0), "page 0 still holds unacknowledged records");

    /* One record more, and the page is whole. */
    append_n(1);
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");

    /* Nothing has happened yet, and that is the point: this is the RX thread,
     * and the head is main's to move.
     */
    CHECK_EQ(flash_store_head(), 0, "release() must not touch the head");
    CHECK(!shim_flash_page_is_erased(0), "release() must not erase");

    flash_store_service();
    CHECK_EQ(flash_store_head(), TEST_PAGE, "head should move exactly one page");
    CHECK(shim_flash_page_is_erased(0), "acknowledged records are still readable on the part");
    expect_clean_writes("ack_frees_and_scrubs");
}

/* A deep backlog acknowledged in one go -- a phone back after days away. The
 * cycle takes a few pages and leaves the rest, so the burst cadence stays
 * predictable instead of absorbing ~4.8s of erasing in one go.
 */
static void test_deep_ack_is_paced(void)
{
    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 10);
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");

    flash_store_service();
    CHECK_EQ(flash_store_head(), TEST_PAGE * 10, "the head is arithmetic -- all of it, at once");

    /* Four pages a cycle: the space came back immediately, the erasing is
     * spread out behind it.
     */
    for (unsigned int p = 0; p < 4; p++) {
        CHECK(shim_flash_page_is_erased(p), "page %u should have been scrubbed this cycle", p);
    }
    CHECK(!shim_flash_page_is_erased(4), "page 4 should have been left for the next cycle");

    flash_store_service();
    flash_store_service();
    for (unsigned int p = 0; p < 10; p++) {
        CHECK(shim_flash_page_is_erased(p), "page %u not scrubbed after three cycles", p);
    }
    expect_clean_writes("deep_ack");
}

/* A failed erase must not advance the frontier. If it did, the write path would
 * later believe a dirty page was clean and write records into flash that cannot
 * hold them -- which is silent, because NOR just ANDs the bits.
 */
static void test_failed_scrub_does_not_advance(void)
{
    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 2);
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");

    shim_flash_erase_fail_next = 1;
    shim_log_verbosity = 0; /* the error below is the point of the test */
    flash_store_service();
    shim_log_verbosity = 1;
    CHECK(!shim_flash_page_is_erased(0), "the erase was supposed to fail");
    CHECK_EQ(shim_log_count[SHIM_LOG_ERR], 1, "a failed scrub must say so");

    /* The next acknowledgement picks it up again, from the same page. */
    append_n(RECS_PER_PAGE);
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");
    flash_store_service();
    CHECK(shim_flash_page_is_erased(0), "the retry did not go back for the page it missed");
    CHECK(shim_flash_page_is_erased(1), "page 1 not scrubbed");
    expect_clean_writes("failed_scrub");
}

/* An erase that reports success and does nothing must not move the frontier.
 *
 * This is the failure a driver return code cannot express, and it is the one
 * that ends worst: s_scrub_next is what write_record() trusts when it skips its
 * own erase, so believing a lying erase means records get written into a page
 * that was never cleared. NOR does not refuse that -- it ANDs -- so the result
 * is readings that are neither value and still parse, with nothing anywhere
 * reporting a problem.
 *
 * The firmware reads the page back rather than trusting the driver, which is
 * what this checks.
 */
static void test_lying_erase_is_caught(void)
{
    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 2);
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");

    shim_flash_erase_noop_next = 1;
    shim_log_verbosity = 0; /* the error below is the point of the test */
    flash_store_service();
    shim_log_verbosity = 1;

    CHECK(shim_log_count[SHIM_LOG_ERR] >= 1, "a page that did not erase must say so");
    CHECK(!shim_flash_page_is_erased(0), "the model was supposed to leave page 0 dirty");

    /* The frontier must not have moved past it: the proof is that a later
     * scrub goes back for the same page rather than stepping over it.
     */
    flash_store_service();
    CHECK(shim_flash_page_is_erased(0), "the scrub stepped over a page it never erased");
    expect_clean_writes("lying_erase");
}

/* Lapping with the phone keeping up: no data is lost, nothing warns, and -- the
 * property that pays for the whole split between the scrub and the write path --
 * no page is erased twice in a lap.
 */
static void test_lap_with_acks(void)
{
    unsigned int *before;
    unsigned int lap_erases = 0;
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    shim_log_watch("Log lapped");

    /* Lap once to get past the first-lap erases: until the cursor has been
     * round, the pages ahead of it hold the previous session's records and have
     * to be cleared before they can be written, whoever else erased them since.
     */
    for (unsigned int i = 0; i < TEST_PAGES * RECS_PER_PAGE; i++) {
        append_n(1);
        if (flash_store_cursor() % TEST_PAGE == 0) {
            ack_all();
        }
    }

    before = calloc(shim_flash_page_count, sizeof(*before));
    memcpy(before, shim_flash_erases, shim_flash_page_count * sizeof(*before));

    /* And again, this time counting. */
    for (unsigned int i = 0; i < TEST_PAGES * RECS_PER_PAGE; i++) {
        append_n(1);
        if (flash_store_cursor() % TEST_PAGE == 0) {
            ack_all();
        }
    }

    for (size_t p = 0; p < shim_flash_page_count; p++) {
        unsigned int delta = shim_flash_erases[p] - before[p];

        CHECK(delta <= 1, "page %zu erased %u times in one lap", p, delta);
        lap_erases += delta;
    }
    CHECK_EQ(lap_erases, TEST_PAGES, "erases in a steady-state lap");
    free(before);

    CHECK_EQ(shim_log_watch_hits, 0, "the ring dropped data the phone had acknowledged");

    w = walk_all();
    CHECK_EQ(w.count, held_records(), "walk did not reach the cursor");
    CHECK(w.ordered, "records out of order after a lap -- the %% fa_size is wrong");
    expect_clean_writes("lap_with_acks");
}

/* Lapping with no phone at all: the oldest page goes, loudly, one page at a
 * time, and what survives is still a contiguous run ending at the newest record.
 *
 * **Where the first drop actually falls is worth pinning down**, because the
 * hardware checklist expects it one page earlier than the code delivers it. The
 * first lap fills the partition *completely* -- every slot, TEST_SIZE bytes --
 * because until the cursor has been all the way round there is no page behind it
 * holding anything, and nothing to drop. Only the record after that has to
 * displace one. From then on the live range oscillates between capacity and the
 * full partition as pages are dropped and refilled, which is why
 * flash_store_capacity() is the conservative of the two and the right number for
 * a policy to be written against.
 */
static void test_lap_without_acks(void)
{
    struct walk w;
    uint32_t head_before;
    unsigned int fits = TEST_SIZE / FLASH_RECORD_SIZE;
    unsigned int drops;

    fresh(TEST_SIZE, TEST_PAGE);
    shim_log_watch("Log lapped");
    /* A lap with no phone is one warning per page, by design. Counted, not
     * printed -- shim_log_watch_hits is what the assertions read.
     */
    shim_log_verbosity = 0;

    append_n(fits);
    CHECK_EQ(shim_log_watch_hits, 0, "warned before the partition was actually full");
    CHECK_EQ(flash_store_head(), 0, "nothing should have been dropped yet");
    CHECK_EQ(held_records(), fits, "the first lap should fill every slot");

    head_before = flash_store_head();
    append_n(1);
    CHECK_EQ(shim_log_watch_hits, 1, "the lap-drop must warn -- it is the record of the loss");
    CHECK_EQ(flash_store_head() - head_before, TEST_PAGE, "a lap-drop must take exactly one page");

    /* Round again. One page dropped per page written, and the head moves in
     * whole pages only -- a partial drop would leave the walk reading into
     * flash the cursor is about to write.
     */
    head_before = flash_store_head();
    shim_log_watch("Log lapped");
    append_n(fits);
    drops = shim_log_watch_hits;
    CHECK_EQ(drops, TEST_PAGES, "drops in a full lap");
    CHECK_EQ(flash_store_head() - head_before, drops * TEST_PAGE, "head moved by a partial page");
    CHECK(held_records() <= fits, "holding %u records in a %u-slot partition", held_records(),
          fits);
    CHECK(held_records() >= flash_store_capacity() / FLASH_RECORD_SIZE,
          "holding %u records, below the capacity the ring promises", held_records());

    w = walk_all();
    CHECK_EQ(w.count, held_records(), "walk did not reach the cursor after wrapping");
    CHECK(w.ordered, "records out of order after wrapping");
    CHECK_EQ(w.last, next_index, "the newest record is not the last one written");
    shim_log_verbosity = 1;
    expect_clean_writes("lap_without_acks");
}

/* A scrub that has fallen a long way behind must step over the pages the cursor
 * has already taken back, not erase them.
 *
 * This is the state a phone returning after days away leaves: nothing was
 * acknowledged, so the scrub never ran, while the ring lapped repeatedly and the
 * write path erased and refilled every page underneath it. The scrub's frontier
 * is then pointing at pages that are physically full of *live* records — and it
 * is holding a sequence number one or more laps out of date, which is exactly
 * the kind of stale pointer that reads as valid. Erasing on it destroys the
 * newest data in the log while believing it is clearing the oldest.
 */
static void test_scrub_far_behind_skips_reused_pages(void)
{
    unsigned int fits = TEST_SIZE / FLASH_RECORD_SIZE;
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    shim_log_verbosity = 0; /* lap warnings, all expected */

    /* Two laps with no acknowledgement and no housekeeping at all. The head
     * advances only by lap-drops; the scrub frontier never moves off zero.
     */
    append_n(fits * 2);

    /* Now the phone reappears and acknowledges part of what survives. */
    CHECK_EQ(flash_store_release(flash_store_head() + TEST_PAGE * 2), 0, "release");
    for (unsigned int i = 0; i < TEST_PAGES; i++) {
        flash_store_service();
    }
    shim_log_verbosity = 1;

    expect_clean_writes("scrub_far_behind");
    w = walk_all();
    CHECK_EQ(w.count, held_records(), "the scrub erased pages that still held live records");
    CHECK(w.ordered, "records out of order after a late scrub");
    CHECK_EQ(w.last, next_index, "the newest record is not the last one written");

    /* And it did eventually catch up rather than giving up. */
    append_n(RECS_PER_PAGE);
    ack_all();
    expect_clean_writes("scrub_far_behind (after catching up)");
}

/* A request for records older than the ring still holds is a phone that was away
 * while the buffer lapped, not an error. It gets what survives, and the offset
 * it gets back is higher than the one it asked for, which is how it learns.
 */
static void test_walk_below_head_clamps(void)
{
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 4);
    ack_all();
    append_n(3); /* written after the acknowledgement, so they survive it */
    CHECK(flash_store_head() > 0, "nothing was reclaimed, so there is nothing to clamp against");

    w = walk_from(0, NULL);
    CHECK_EQ(w.count, held_records(), "a walk from 0 should return what survives");
    CHECK_EQ(w.count, 3, "the surviving records");
    CHECK(w.ordered, "clamped walk came back out of order");
    CHECK_EQ(w.first, flash_store_head() / FLASH_RECORD_SIZE + 1, "clamped to the wrong record");
}

/* An acknowledgement outside the live range is a client that has lost track.
 * Refusing beats erasing on it.
 */
static void test_release_bounds(void)
{
    uint32_t head;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 3);

    CHECK(flash_store_release(flash_store_cursor() + FLASH_RECORD_SIZE) != 0,
          "acknowledged a record that was never sent");

    ack_all();
    head = flash_store_head();

    /* Repeats are fine and must change nothing. */
    CHECK_EQ(flash_store_release(head), 0, "a repeated acknowledgement is not an error");
    CHECK_EQ(flash_store_head(), head, "head moved on a repeat");

    CHECK(flash_store_release(head - FLASH_RECORD_SIZE) == 0 || flash_store_head() == head,
          "an acknowledgement below the head must not move it backwards");
    CHECK_EQ(flash_store_head(), head, "head moved backwards");
}

/* Refusal runs collapse in RAM and land as one record. Not part of N2, but it
 * shares write_record() with everything above, and a run that lands twice or
 * not at all is a hole in the log by another route.
 */
static void test_refusal_runs_collapse(void)
{
    struct flash_sample s = {.timestamp_ms = 1000, .contact = true};
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);

    for (unsigned int i = 0; i < 5; i++) {
        s.timestamp_ms = 1000 + i;
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_WEAK_PULSE), 0, "refusal");
    }
    CHECK_EQ(flash_store_cursor(), 0, "an open run must stay in RAM");

    /* A believed reading closes the run and lands after it. */
    next_index = 2000;
    append_n(1);
    CHECK_EQ(flash_store_cursor(), 2 * FLASH_RECORD_SIZE, "the run should be exactly one record");

    w = walk_from(0, NULL);
    CHECK_EQ(w.count, 2, "records after a collapsed run");
    CHECK_EQ(w.first, 1000, "a run keeps the first refusal's timestamp");
    CHECK_EQ(w.last, 2001, "the reading that closed the run should be last");
    expect_clean_writes("refusal_runs");
}

/* The numbering has to survive a two-bit reader.
 *
 * This is the test the numbering rule needs. The header says in words
 * that a reader masking two bits reads MOVING as NOT_WORN, the companion app was
 * that reader, and a comment stopped nobody -- 361 windows of a walking wearer
 * came back as "not worn" and the ring was diagnosed with a collapsed contact
 * gate. Words in a header are not a constraint. This is.
 *
 * The property is not "MOVING is 5". It is that **no reason a worn window can
 * carry truncates to NOT_WORN**, so that a client which has not caught up
 * misreports the heading and never the wear state.
 */
static void test_reasons_survive_a_two_bit_reader(void)
{
    static const enum flash_refusal worn_reasons[] = {
        FLASH_REFUSAL_NO_PULSE,
        FLASH_REFUSAL_WEAK_PULSE,
        FLASH_REFUSAL_TOO_SLOW,
        FLASH_REFUSAL_MOVING,
    };
    struct flash_sample s = {.timestamp_ms = 1000, .contact = true};
    struct flash_record rec;

    for (size_t i = 0; i < ARRAY_SIZE(worn_reasons); i++) {
        CHECK(FLASH_REFUSAL_TRUNCATED(worn_reasons[i]) != FLASH_REFUSAL_NOT_WORN,
              "a worn window's reason must never truncate to NOT_WORN");
    }

    /* And the specific degradation that was chosen, so a renumber that keeps the
     * property above but picks a worse landing spot still has to be argued for.
     * WEAK_PULSE would point a reader at the fit; TOO_SLOW would file movement
     * as a finding about the wearer's heart.
     */
    CHECK_EQ(FLASH_REFUSAL_TRUNCATED(FLASH_REFUSAL_MOVING), FLASH_REFUSAL_NO_PULSE,
             "a short read of MOVING should say `worn, nothing believable`");

    /* NOT_WORN itself must keep truncating to itself: the whole point is that a
     * short reader can still be trusted about wear state.
     */
    CHECK_EQ(FLASH_REFUSAL_TRUNCATED(FLASH_REFUSAL_NOT_WORN), FLASH_REFUSAL_NOT_WORN,
             "the one reason that means not worn must survive truncation");

    /* 4 is reserved. Nothing may write it, because it is the value that
     * truncates to NOT_WORN -- refused at the door rather than left to be
     * rediscovered by a wrong diagnosis.
     */
    CHECK_EQ(FLASH_REFUSAL_TRUNCATED(FLASH_REFUSAL_MOVING_LEGACY), FLASH_REFUSAL_NOT_WORN,
             "4 aliases to NOT_WORN -- which is exactly why it is reserved");
    fresh(TEST_SIZE, TEST_PAGE);
    CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING_LEGACY), -EINVAL,
             "the reserved reason must not be writable");
    CHECK_EQ(flash_store_cursor(), 0, "a refused append must not have opened a run");

    /* The whole range still has to fit the three bits it is carried in, and a
     * reason must not bleed into the movement bucket above it.
     */
    s.movement_milli_g = 0;
    for (size_t i = 0; i < ARRAY_SIZE(worn_reasons); i++) {
        flash_record_from_sample(&rec, &s);
        rec.flags |= (uint8_t)(0x02u | ((unsigned int)worn_reasons[i] << 2));
        CHECK_EQ(flash_record_reason(&rec), worn_reasons[i], "reason round-trip");
        CHECK_EQ(flash_record_movement(&rec), flash_movement_bucket(0),
                 "a reason must not reach the movement bits");
    }
    expect_clean_writes("two_bit_reader");
}

/* Grabs the movement bucket off the first record a walk sees. `struct walk`
 * above deliberately carries only timestamps; this needs the flags byte.
 */
static bool first_movement_cb(uint32_t offset, const struct flash_record *rec, void *user_data)
{
    uint8_t *out = user_data;

    ARG_UNUSED(offset);
    if (*out == 0xffu) {
        *out = flash_record_movement(rec);
    }
    return true;
}

/* The movement bucket in flags bits 5-7.
 *
 * Four claims, and the first two are the ones that would be expensive to get
 * wrong in the field. The boundaries decide what a day of wear says about
 * WINDOW_MOVE_MILLI_G, and bucket 0 being reserved is the whole reason this
 * change did not need a RECORD_FORMAT_VERSION bump and a boot erase of a log
 * that has just proven it survives resets.
 */
static void test_movement_buckets(void)
{
    static const struct {
        uint16_t milli_g;
        uint8_t bucket;
    } edges[] = {
        {0, 1},     {49, 1},   {50, 2},   {99, 2},   {100, 3},  {149, 3},  {150, 4},
        {249, 4},   {250, 5},  {499, 5},  {500, 6},  {999, 6},  {1000, 7}, {UINT16_MAX, 7},
        /* The measured populations from main.c:486, which are what the
         * boundaries were placed around. `hands in use` straddling 3 and 4 is
         * the point of having both.
         */
        {36, 1},    {45, 1},   /* hand deliberately still */
        {122, 3},   {217, 4},  /* stationary, hands in use */
        {632, 6},   {938, 6},  /* walking */
        /* WINDOW_MOVE_MILLI_G is 100 and lives in main.c, so it cannot be
         * included from here. These two are it: the last accepted window and
         * the first refused one. If a boundary ever moves through the gate,
         * this is where it should be argued about rather than discovered.
         */
        {99, 2},    {100, 3},
    };
    struct flash_sample s = {.timestamp_ms = 1000, .contact = true};
    struct flash_record rec;
    uint8_t seen = 0xffu;

    for (size_t i = 0; i < ARRAY_SIZE(edges); i++) {
        CHECK_EQ(flash_movement_bucket(edges[i].milli_g), edges[i].bucket, "bucket edge");
    }

    /* Every value round-trips through the record, never lands on UNKNOWN, and
     * does not disturb the two bits below it.
     */
    for (uint32_t mg = 0; mg <= 1400; mg += 7) {
        s.movement_milli_g = (uint16_t)mg;
        flash_record_from_sample(&rec, &s);
        CHECK_EQ(flash_record_movement(&rec), flash_movement_bucket((uint16_t)mg), "round-trip");
        CHECK(flash_record_movement(&rec) != FLASH_MOVEMENT_UNKNOWN,
              "a measured window must never encode as unrecorded");
        CHECK(flash_record_contact(&rec), "the movement bits must not disturb contact");
        CHECK(!flash_record_refused(&rec), "the movement bits must not disturb the verdict");
    }

    /* A record written by firmware older than the field: bits 5-7 clear. It has
     * to read as unknown and not as "definitely still", which is the entire
     * argument for reserving bucket 0.
     */
    s.movement_milli_g = 800;
    flash_record_from_sample(&rec, &s);
    rec.flags &= 0x1fu;
    CHECK_EQ(flash_record_movement(&rec), FLASH_MOVEMENT_UNKNOWN,
             "a record with no bucket recorded must not claim to have been still");

    /* Labels are bounds-checked, because a torn write can put anything here. */
    for (uint8_t b = 0; b < FLASH_MOVEMENT_BUCKETS; b++) {
        CHECK(flash_movement_label(b) != NULL, "every bucket needs a label");
    }
    CHECK_EQ(strcmp(flash_movement_label(FLASH_MOVEMENT_BUCKETS), "unknown"), 0,
             "an out-of-range bucket must say so rather than read past the table");

    /* The run-length collapse matches on the reason and contact bits only, so a
     * run absorbs refusals whose movement differs and keeps the bucket of the
     * one that opened it. Without this the field would fragment every run and
     * cost flash the resolution is not worth.
     */
    fresh(TEST_SIZE, TEST_PAGE);
    /* Refusals that found nothing: bpm 0, so none of them is evidence-bearing
     * and all five are interchangeable. That is the condition the collapse is
     * for -- see test_refusals_that_carry_evidence() for the exception, which
     * is the same five records with a pulse in them.
     */
    s.bpm = 0;
    s.confidence = 0;
    for (unsigned int i = 0; i < 5; i++) {
        s.timestamp_ms = 1000 + i;
        s.movement_milli_g = (uint16_t)(60 + i * 300); /* buckets 2, 5, 6, 7, 7 */
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING), 0, "refusal");
    }
    next_index = 2000;
    append_n(1);
    CHECK_EQ(flash_store_cursor(), 2 * FLASH_RECORD_SIZE,
             "a run must collapse across differing movement buckets");

    (void)flash_store_foreach(0, first_movement_cb, &seen);
    CHECK_EQ(seen, 2, "a run keeps the opening refusal's bucket, as it keeps its timestamp");
    expect_clean_writes("movement_buckets");
}

/* Collects bpm, confidence and bucket off every record a walk sees. */
struct evidence {
    unsigned int count;
    uint16_t bpm[8];
    uint16_t confidence[8];
    uint8_t bucket[8];
    uint8_t repeat[8];
};

static bool evidence_cb(uint32_t offset, const struct flash_record *rec, void *user_data)
{
    struct evidence *e = user_data;

    ARG_UNUSED(offset);
    if (e->count < ARRAY_SIZE(e->bpm)) {
        e->bpm[e->count] = rec->bpm;
        e->confidence[e->count] = rec->confidence;
        e->bucket[e->count] = flash_record_movement(rec);
        e->repeat[e->count] = rec->repeat;
    }
    e->count++;
    return true;
}

/* A MOVING refusal that still found a believable pulse keeps its own record.
 *
 * The collapse above is right for refusals that are interchangeable, and a
 * moving window with a bpm and a confidence over the bar is not one of those:
 * it is a measurement, refused on evidence from a different part, and the run
 * would keep only the opening window's numbers. On one measured day that is
 * 361 refused windows stored as roughly thirty windows' evidence, which makes
 * the movement threshold undecidable from the log -- arguing it needs a bpm per
 * refused window, per movement bucket.
 *
 * Four claims, and the last two are what keep the exception from eating the
 * flash the collapse was written to save.
 */
static void test_refusals_that_carry_evidence(void)
{
    struct flash_sample s = {.contact = true, .movement_milli_g = 300};
    struct evidence e = {0};
    struct walk w;

    /* 1. Five consecutive believable MOVING windows are five records, each with
     *    its own numbers, none of them absorbed.
     */
    fresh(TEST_SIZE, TEST_PAGE);
    for (unsigned int i = 0; i < 5; i++) {
        s.timestamp_ms = 1000 + i;
        s.bpm = (uint16_t)(70 + i);
        s.confidence = (uint16_t)(VITALS_BPM_CONFIDENCE_MIN + i);
        s.movement_milli_g = (uint16_t)(120 + i * 200); /* buckets 3, 4, 5, 6, 6 */
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING), 0, "refusal");
    }
    CHECK_EQ(flash_store_cursor(), 5 * FLASH_RECORD_SIZE,
             "an evidence-bearing refusal must not wait in RAM for a run to close");

    (void)flash_store_foreach(0, evidence_cb, &e);
    CHECK_EQ(e.count, 5, "five measured windows, five records");
    for (unsigned int i = 0; i < 5; i++) {
        CHECK_EQ(e.bpm[i], 70 + i, "each record keeps its own bpm");
        CHECK_EQ(e.confidence[i], VITALS_BPM_CONFIDENCE_MIN + i, "each record keeps its own confidence");
        CHECK_EQ(e.repeat[i], 0, "a standalone refusal covers exactly one window");
    }
    CHECK_EQ(e.bucket[0], 3, "and its own movement bucket");
    CHECK_EQ(e.bucket[4], 6, "and its own movement bucket");

    w = walk_from(0, NULL);
    CHECK_EQ(w.count, 5, "records");
    CHECK(w.ordered, "standalone refusals must stay in order");
    expect_clean_writes("refusals_that_carry_evidence");

    /* 2. It closes an open run first, so the log does not report the run as
     *    having happened after the window that ended it.
     */
    fresh(TEST_SIZE, TEST_PAGE);
    s.bpm = 0;
    s.confidence = 0;
    for (unsigned int i = 0; i < 3; i++) {
        s.timestamp_ms = 1000 + i;
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING), 0, "refusal");
    }
    CHECK_EQ(flash_store_cursor(), 0, "an open run must stay in RAM");
    s.timestamp_ms = 1003;
    s.bpm = 71;
    s.confidence = VITALS_BPM_CONFIDENCE_MIN;
    CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING), 0, "refusal");
    CHECK_EQ(flash_store_cursor(), 2 * FLASH_RECORD_SIZE,
             "the run and the window that ended it, in that order");
    w = walk_from(0, NULL);
    CHECK_EQ(w.first, 1000, "the run keeps the first refusal's timestamp");
    CHECK_EQ(w.last, 1003, "the evidence-bearing window lands after the run it closed");

    /* 3. Under the bar, it still collapses. A refused window with nothing to
     *    preserve is exactly the case the run-length compression is for, and
     *    this is what stops the exception from costing a record per window.
     */
    fresh(TEST_SIZE, TEST_PAGE);
    for (unsigned int i = 0; i < 5; i++) {
        s.timestamp_ms = 1000 + i;
        s.bpm = 71;
        s.confidence = (uint16_t)(VITALS_BPM_CONFIDENCE_MIN - 1);
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING), 0, "refusal");
    }
    /* And a believable confidence with no rate to attach it to is not evidence
     * either -- bpm 0 is what "no pulse found" looks like in this field.
     */
    for (unsigned int i = 0; i < 5; i++) {
        s.timestamp_ms = 1005 + i;
        s.bpm = 0;
        s.confidence = 900;
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_MOVING), 0, "refusal");
    }
    next_index = 2000;
    append_n(1);
    CHECK_EQ(flash_store_cursor(), 2 * FLASH_RECORD_SIZE,
             "ten unbelievable refusals are still one run");

    /* 4. Narrow to MOVING. NOT_WORN is the reason the collapse exists for --
     *    the ring spends nights in it -- and a stray confidence on a window
     *    that was not against skin must not fragment that run.
     */
    fresh(TEST_SIZE, TEST_PAGE);
    s.contact = false;
    s.bpm = 71;
    s.confidence = 900;
    for (unsigned int i = 0; i < 5; i++) {
        s.timestamp_ms = 1000 + i;
        CHECK_EQ(flash_store_append_refusal(&s, FLASH_REFUSAL_NOT_WORN), 0, "refusal");
    }
    next_index = 3000;
    append_n(1);
    CHECK_EQ(flash_store_cursor(), 2 * FLASH_RECORD_SIZE,
             "the exception is MOVING only -- a NOT_WORN run must still collapse");
    expect_clean_writes("refusals_that_carry_evidence (narrowness)");
}

/* ---- interleaving -------------------------------------------------------- *
 *
 * Everything above calls the module the way one thread would. These three put
 * the *other* threads' calls inside main's, using the flash model's hook: the
 * callback runs at the start of a flash operation, before the model does the
 * work, so from the module's point of view a pointer moved underneath an
 * operation already in progress.
 *
 * Two entry points do not run on main, and these are them:
 *   flash_store_release() -- the Bluetooth RX thread, on every acknowledgement
 *   flash_store_foreach() -- the replay work queue, for the length of a transfer
 */

/* The RX thread, acknowledging whatever is in the log right now. */
static void ack_from_rx_thread(void *arg)
{
    ARG_UNUSED(arg);
    (void)flash_store_release(flash_store_cursor());
}

/* An acknowledgement landing in the middle of an append -- inside the record
 * write, and inside the page erase that sometimes precedes it.
 *
 * This is the interleaving that actually happens on the ring, thousands of times
 * a day: main is writing, the phone acknowledges, and `s_acked` changes under a
 * decision main is part-way through making. The one that matters is the lap-drop
 * — deciding whether a page is safe to overwrite reads `s_acked`, and reading a
 * stale one either destroys records the phone never got or warns that it did.
 */
static void test_ack_during_append(void)
{
    struct walk w;
    unsigned int fits = TEST_SIZE / FLASH_RECORD_SIZE;

    fresh(TEST_SIZE, TEST_PAGE);
    shim_log_watch("Log lapped");
    shim_flash_hook(SHIM_FLASH_WRITE | SHIM_FLASH_ERASE, ack_from_rx_thread, NULL);

    /* Three laps, with an acknowledgement inside every single flash operation. */
    for (unsigned int i = 0; i < fits * 3; i++) {
        append_n(1);
        if ((i % RECS_PER_PAGE) == 0) {
            flash_store_service(); /* main's housekeeping, once a cycle */
        }
    }
    shim_flash_hook(0, NULL, NULL);

    CHECK(shim_flash_hook_fired > 0, "the hook never fired -- this test proved nothing");
    expect_clean_writes("ack_during_append");

    /* Nothing may be dropped as unacknowledged: the phone was acknowledging
     * continuously, so every page that lapped had been confirmed. A stale read
     * of s_acked is exactly what would break this.
     */
    CHECK_EQ(shim_log_watch_hits, 0, "dropped records the phone had acknowledged");

    CHECK(flash_store_head() <= flash_store_cursor(), "head overran the cursor");
    w = walk_all();
    CHECK_EQ(w.count, held_records(), "walk did not reach the cursor");
    CHECK(w.ordered, "records out of order after interleaved acknowledgements");
    CHECK_EQ(w.last, next_index, "the newest record is not the last one written");
}

/* An acknowledgement mid-cycle frees its space on the next record, not on the
 * next cycle.
 *
 * `flash_store_service()` runs once per measurement cycle, but an ACK can land
 * at any point inside one, so `write_record()` reclaims too. Nothing is unsafe
 * without that -- a page dropped after being acknowledged is a page nobody
 * needed -- but the ring would spend a cycle lapping and dropping pages it was
 * already free to reuse, and main.c's nudge would spend it looking at a backlog
 * that had already been collected.
 */
static void test_ack_frees_space_before_the_next_cycle(void)
{
    unsigned int fits = TEST_SIZE / FLASH_RECORD_SIZE;

    fresh(TEST_SIZE, TEST_PAGE);
    shim_log_watch("Log lapped");

    /* Fill it completely, with no housekeeping at all -- one long cycle. */
    append_n(fits);
    CHECK_EQ(held_records(), fits, "the buffer should be full");

    /* The phone collects and acknowledges the lot, mid-cycle. */
    CHECK_EQ(flash_store_release(flash_store_cursor()), 0, "release");
    CHECK_EQ(flash_store_head(), 0, "release() must not touch the head");

    /* One more record, still no service() call. The space must already be back. */
    append_n(1);
    CHECK_EQ(shim_log_watch_hits, 0, "dropped a page it was free to reuse");
    CHECK(held_records() <= RECS_PER_PAGE, "still holding %u records after acknowledging all of them",
          held_records());
}

/* The same, but landing inside the scrub's own erase. This is the dangerous
 * shape -- with the erase on a work queue, an acknowledgement could move the
 * head while a page was part-way through being cleared. It is
 * now two calls on one thread and cannot interleave at all; this test is what
 * says so, and what would notice if the erase were ever moved off main again.
 */
static void test_ack_during_scrub(void)
{
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 6);
    CHECK_EQ(flash_store_release(TEST_PAGE * 3), 0, "release");

    shim_flash_hook(SHIM_FLASH_ERASE, ack_from_rx_thread, NULL);
    flash_store_service();
    flash_store_service();
    shim_flash_hook(0, NULL, NULL);

    CHECK(shim_flash_hook_fired > 0, "no erase happened -- this test proved nothing");
    for (unsigned int p = 0; p < 3; p++) {
        CHECK(shim_flash_page_is_erased(p), "page %u not scrubbed", p);
    }

    append_n(RECS_PER_PAGE);
    expect_clean_writes("ack_during_scrub");
    w = walk_all();
    CHECK_EQ(w.count, held_records(), "walk did not reach the cursor");
    CHECK(w.ordered, "records out of order");
}

/* A replay walk on the work queue while main goes on measuring.
 *
 * The property under test is the one `flash_store_foreach()`'s comment claims:
 * the cursor is snapshotted on entry, so a record appended *during* the walk is
 * not visited. It has to be that way -- a 16-byte record is four flash words and
 * is not written atomically, so walking towards a live cursor could read a
 * record half-way through being written and hand a wearer's phone a reading that
 * never existed.
 */
static void append_from_main(void *arg)
{
    unsigned int *left = arg;

    if (*left > 0) {
        (*left)--;
        (void)append_one();
    }
}

static void test_append_during_walk(void)
{
    unsigned int budget = 40;
    unsigned int before;
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(60);
    before = held_records();

    /* Every record the walk reads, main writes another. */
    shim_flash_hook(SHIM_FLASH_READ, append_from_main, &budget);
    w = walk_all();
    shim_flash_hook(0, NULL, NULL);

    CHECK(shim_flash_hook_fired > 0, "the hook never fired -- this test proved nothing");
    CHECK_EQ(w.count, before, "the walk visited records appended after it started");
    CHECK(w.ordered, "records out of order under an interleaved append");
    expect_clean_writes("append_during_walk");

    /* And the records written during the walk are all there afterwards. */
    w = walk_all();
    CHECK_EQ(w.count, held_records(), "records appended during a walk went missing");
    CHECK(w.ordered, "records out of order after the walk");
    CHECK_EQ(w.last, next_index, "the newest record is not the last one written");
}

/* The real part, at the real geometry, for two laps. This is the soak the
 * hardware checklist asks for -- 29,000 writes and the lap warning landing on
 * 14,592 records rather than 14,848 -- and here it costs a fraction of a second.
 */
static void test_soak_real_geometry(void)
{
    const size_t size = 237568; /* 232KB, the storage partition */
    const size_t page = 4096;
    const unsigned int capacity_records = (unsigned int)((size - page) / FLASH_RECORD_SIZE);
    const unsigned int fits = (unsigned int)(size / FLASH_RECORD_SIZE);
    struct walk w;

    fresh(size, page);
    shim_log_verbosity = 0; /* 57 lap warnings a lap, all of them expected */
    CHECK_EQ(flash_store_capacity() / FLASH_RECORD_SIZE, 14592, "usable records on the real part");
    CHECK_EQ(fits, 14848, "records that physically fit");

    shim_log_watch("Log lapped");
    append_n(fits);
    CHECK_EQ(shim_log_watch_hits, 0, "warned before the partition was full");
    append_n(1);
    /* 14,849, not the 14,593 the hardware checklist expects -- see
     * test_lap_without_acks() for why the first lap fills every slot.
     */
    CHECK_EQ(shim_log_watch_hits, 1, "the first lap-drop landed somewhere other than record 14849");

    append_n(capacity_records);
    w = walk_all();
    CHECK_EQ(w.count, held_records(), "walk did not reach the cursor");
    CHECK(w.ordered, "records out of order after two laps");
    CHECK_EQ(w.last, next_index, "the newest record is not the last one written");
    expect_clean_writes("soak");

    /* Now the same again with the phone keeping up, which is the configuration
     * that is meant to lose nothing at all.
     */
    fresh(size, page);
    shim_log_watch("Log lapped");
    for (unsigned int i = 0; i < capacity_records * 2; i++) {
        append_n(1);
        if (flash_store_cursor() % page == 0) {
            ack_all();
        }
    }
    CHECK_EQ(shim_log_watch_hits, 0, "dropped records the phone had acknowledged");
    CHECK(held_records() < (unsigned int)(page / FLASH_RECORD_SIZE),
          "the phone acknowledged everything but %u records are still held", held_records());
    shim_log_verbosity = 1;
    expect_clean_writes("soak_with_acks");
}

/* The point of the whole mechanism: a reset stops costing the buffer.
 *
 * Every property here is one the phone depends on. The generation surviving is
 * what stops it restarting from zero and re-reading everything; the cursor and
 * head surviving are what make its stored cursor still mean something; and the
 * walk coming back ordered is what says the records themselves are intact.
 */
static void test_reset_keeps_an_unwrapped_log(void)
{
    uint32_t gen, cursor, newest = 0;
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(100);
    gen = flash_store_generation();
    cursor = flash_store_cursor();

    reboot(gen, &newest);

    CHECK_EQ(flash_store_generation(), gen, "generation must survive or the phone restarts from 0");
    CHECK_EQ(flash_store_cursor(), cursor, "cursor after reset");
    CHECK_EQ(flash_store_head(), 0, "head after reset");
    CHECK_EQ(newest, 100, "newest timestamp reported to the caller");
    CHECK_EQ(held_records(), 100, "records held after reset");

    w = walk_all();
    CHECK_EQ(w.count, 100, "records walkable after reset");
    CHECK_EQ(w.first, 1, "first record after reset");
    CHECK_EQ(w.last, 100, "last record after reset");
    CHECK(w.ordered, "the kept log must still be in order");
}

/* The scrub erases acknowledged pages, so a healthy log routinely starts partway
 * into the partition. Leading blanks are the normal case, not a fault, and a
 * restore that rejected them would fall back to the erase on every ring that had
 * ever collected anything -- which is every ring.
 */
static void test_reset_keeps_a_log_with_reclaimed_pages(void)
{
    uint32_t gen, head, cursor, newest = 0;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(RECS_PER_PAGE * 2);
    ack_all();
    append_n(10);

    gen = flash_store_generation();
    head = flash_store_head();
    cursor = flash_store_cursor();
    CHECK(head > 0, "the test needs a head that has actually moved");

    reboot(gen, &newest);

    CHECK_EQ(flash_store_generation(), gen, "generation across a reclaimed-page reset");
    CHECK_EQ(flash_store_head(), head, "head must land where the data starts");
    CHECK_EQ(flash_store_cursor(), cursor, "cursor across a reclaimed-page reset");
    CHECK_EQ(walk_all().count, 10, "the surviving records");
}

/* A written record after a blank one cannot happen in a coherent log, so it is
 * either a wrap or corruption -- and from here those are the same thing. Erase.
 */
static void test_reset_refuses_a_holed_log(void)
{
    struct flash_record stray = {.timestamp_ms = 9999, .bpm = 70};
    uint32_t gen, newest = 0;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(10);
    gen = flash_store_generation();

    /* Well past the cursor, with blank records in between. */
    poke_record(FLASH_RECORD_SIZE * 40, &stray);

    shim_log_verbosity = 0; /* the refusal warns, and that is the point */
    reboot(gen, &newest);
    shim_log_verbosity = 1;

    CHECK_EQ(flash_store_cursor(), 0, "a holed log must be erased, not kept");
    CHECK_EQ(newest, 0, "no timestamp may be reported from a log that was erased");
    CHECK(flash_store_generation() != gen, "an erased log must take a fresh generation");
}

/* The same test from the other side, and the one that catches a record left
 * half-written by a reset that landed mid-append: records are appended in time
 * order, so a fall is impossible in a log worth keeping.
 */
static void test_reset_refuses_a_log_that_runs_backwards(void)
{
    struct flash_record older = {.timestamp_ms = 1, .bpm = 70};
    uint32_t gen, cursor, newest = 0;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(10);
    gen = flash_store_generation();
    cursor = flash_store_cursor();

    /* Appended in sequence, but dated before the record it follows. */
    poke_record(cursor, &older);

    shim_log_verbosity = 0;
    reboot(gen, &newest);
    shim_log_verbosity = 1;

    CHECK_EQ(flash_store_cursor(), 0, "a log that runs backwards must be erased");
    CHECK(flash_store_generation() != gen, "an erased log must take a fresh generation");
}

/* Generation zero means "do not keep it", which is how a ring with no persisted
 * state behaves.
 */
static void test_cold_boot_still_erases(void)
{
    uint32_t newest = 0;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(50);

    shim_log_verbosity = 0; /* the boot erase reports what it destroyed */
    reboot(0, &newest);
    shim_log_verbosity = 1;

    CHECK_EQ(flash_store_cursor(), 0, "generation 0 must erase");
    CHECK_EQ(newest, 0, "an erase reports no timestamp");
}

/* Nothing on the part is not a failure, it is a cold start: the caller gets the
 * erase path and a zero timestamp, so it does not rebase uptime over nothing.
 */
static void test_reset_of_a_blank_part_is_a_cold_start(void)
{
    uint32_t newest = 123;

    fresh(TEST_SIZE, TEST_PAGE);
    reboot(4242, &newest);

    CHECK_EQ(flash_store_cursor(), 0, "cursor on a blank part");
    CHECK_EQ(newest, 0, "a blank part must report no timestamp");
}

/* A kept log has to go on working, not merely survive being counted. */
static void test_kept_log_still_appends_and_acknowledges(void)
{
    uint32_t gen, newest = 0;
    struct walk w;

    fresh(TEST_SIZE, TEST_PAGE);
    append_n(20);
    gen = flash_store_generation();

    reboot(gen, &newest);
    next_index = newest; /* the caller rebases uptime; the test stamps the same way */
    append_n(5);

    w = walk_all();
    CHECK_EQ(w.count, 25, "old and new records together");
    CHECK(w.ordered, "new records must continue the kept log's order");
    CHECK_EQ(w.last, 25, "the newest record");

    /* Acknowledgement frees whole pages and nothing less, so the remainder
     * below a page boundary stays held -- the same rule as on a log that never
     * saw a reset. What matters here is that the head moves at all.
     */
    ack_all();
    CHECK(flash_store_head() > 0, "a kept log must still be acknowledgeable");
    CHECK_EQ(held_records(), (25 * FLASH_RECORD_SIZE % TEST_PAGE) / FLASH_RECORD_SIZE,
             "only the part-page remainder stays held");
    expect_clean_writes("kept_log");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        shim_log_verbosity = 2;
    }
    shim_rand_seed(0x5e11ee);

    printf("flash_store: ring arithmetic, acknowledgement and erase\n");

    test_geometry();
    test_append_and_walk();
    test_ack_frees_and_scrubs_whole_pages();
    test_deep_ack_is_paced();
    test_failed_scrub_does_not_advance();
    test_lying_erase_is_caught();
    test_lap_with_acks();
    test_lap_without_acks();
    test_scrub_far_behind_skips_reused_pages();
    test_walk_below_head_clamps();
    test_release_bounds();
    test_refusal_runs_collapse();
    test_movement_buckets();
    test_reasons_survive_a_two_bit_reader();
    test_refusals_that_carry_evidence();
    test_ack_during_append();
    test_ack_frees_space_before_the_next_cycle();
    test_ack_during_scrub();
    test_append_during_walk();
    test_soak_real_geometry();

    test_reset_keeps_an_unwrapped_log();
    test_reset_keeps_a_log_with_reclaimed_pages();
    test_reset_refuses_a_holed_log();
    test_reset_refuses_a_log_that_runs_backwards();
    test_cold_boot_still_erases();
    test_reset_of_a_blank_part_is_a_cold_start();
    test_kept_log_still_appends_and_acknowledges();

    shim_flash_free();

    printf("%u checks, %u failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
