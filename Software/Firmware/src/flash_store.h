#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include <stdbool.h>
#include <stdint.h>

/* Append-only vitals log in the "storage" flash partition. Each record is one
 * measured window, carrying both the reading and the evidence for it. Records
 * are read back over BLE and reclaimed only once the client acknowledges them;
 * the console dump is a diagnostic view and erases nothing. See
 * ARCHITECTURE.md §5A.
 */

/* Why a window produced no reading this firmware would vouch for. Classified by
 * main.c, which owns the confidence policy, and carried in the record so the
 * reader does not have to re-derive it from a threshold it may not share.
 *
 * These are five different things to do something about -- the wearer, the
 * placement or the optics, the perfusion, the wearer's hand, and the wearer's
 * heart -- and the whole point of storing a refusal is that they stop looking
 * identical from the outside.
 *
 * **The reason is three bits, 2-4, in the flags byte.** Bits 0 and 1 carry
 * contact and refused, and 5-7 carry the movement bucket, so the record stays
 * 16 bytes.
 *
 * **The numbering matters, because a reader that masks only two bits still
 * gets an answer.** The values are chosen so that a short read degrades to the
 * nearest true thing rather than to the most damaging available claim. MOVING
 * is 5 for exactly this reason: a two-bit reader computes `5 & 3 == 1` and says
 * NO_PULSE -- worn, nothing found worth reporting. That is not what happened, a
 * moving hand being emphatically periodic, but it is true in every respect such
 * a reader can act on and it points at no component. The alternatives are all
 * worse:
 *
 *   4 -> NOT_WORN    claims the ring was off the finger. Sends the reader to
 *                    the contact gate and the optics -- a wrong component, and
 *                    a wrong claim about the wearer.
 *   5 -> NO_PULSE    worn, no believable rate. True as far as it goes. Chosen.
 *   6 -> WEAK_PULSE  worn, pulse too weak. Sends the reader to the fit and the
 *                    optics -- a different wrong component.
 *   7 -> TOO_SLOW    files movement as a bradycardia finding *about the
 *                    wearer*. The worst available, and the exact misfile the
 *                    ordering of the chain in main.c exists to prevent.
 *
 * ⚠️ **This makes a short read less harmful. It does not make it correct.** A
 * two-bit reader still reports a walking wearer's windows under the wrong
 * heading and still cannot see the movement bucket.
 */
enum flash_refusal {
    FLASH_REFUSAL_NOT_WORN = 0,   /* no contact: nothing in front of the LEDs */
    FLASH_REFUSAL_NO_PULSE = 1,   /* worn, no periodicity found at all */
    FLASH_REFUSAL_WEAK_PULSE = 2, /* worn, pulse found, too weak to believe */
    /* Worn, a pulse found and believable, and slower than VITALS_BPM_MIN -- so
     * the firmware will not put a number on it. The one refusal that is a
     * finding about the wearer rather than about the measurement: this is what
     * bradycardia below the band looks like from here, and the record carries
     * the confidence that justifies saying so. Never a rate; see the band-edge
     * note in vitals.c for what happens when it is.
     */
    FLASH_REFUSAL_TOO_SLOW = 3,
    /* **4 is reserved and must stay unused.** It is the one value in this range
     * a two-bit reader turns into NOT_WORN -- a claim that the ring was off the
     * finger. Any reason numbered 4 inherits that alias.
     *
     * It is also the value MOVING once held, so a decoder reading an old log
     * should treat 4 as MOVING.
     */
    FLASH_REFUSAL_MOVING_LEGACY = 4,
    /* Worn, and the hand was moving too much for the window to mean anything.
     *
     * A refusal about the *measurement*, not the wearer: an autocorrelator
     * cannot tell a pulse from any other periodicity, and hand movement lives
     * in 0.7-1.5Hz -- 42-90bpm, straight through the middle of the band being
     * measured. Movement is judged from the accelerometer over the same window
     * the optics filled, so this is the one refusal whose evidence comes from a
     * different part; see WINDOW_MOVE_MILLI_G in main.c for the threshold.
     *
     * Deliberately its own reason rather than folded into WEAK_PULSE. The two
     * ask for opposite things from whoever reads the log: a weak pulse says the
     * optics or the fit need work, and a moving one says nothing is wrong with
     * either and the window simply cannot be used.
     */
    FLASH_REFUSAL_MOVING = 5,
};

/* What a reader that masks only two bits makes of a reason. The numbering above
 * is chosen so this is never NOT_WORN for a window the ring was worn for.
 * Pinned by the host tests so it cannot quietly stop being true.
 */
#define FLASH_REFUSAL_TRUNCATED(why) ((enum flash_refusal)((unsigned int)(why) & 0x03u))

/* How much the hand was moving while the window was measured, as three bits.
 *
 * **This exists so the movement gate can be argued about from the log alone.**
 * `WINDOW_MOVE_MILLI_G` (main.c) is a threshold on a continuous quantity, and
 * on one measured day it refused 361 of 462 windows -- 78% of everything the
 * device recorded. Without the figure in the record, a refusal at 101 mg and
 * one at 900 mg are the same record, and whether the threshold is wrong is
 * unanswerable from a log that does not carry the number.
 *
 * **It costs no bytes.** The flags byte spends bit 0 on contact, bit 1 on
 * refused and bits 2-4 on the reason; bits 5-7 were the last spare ones. The
 * record stays 16 bytes, the wire format does not move, and the run-length
 * collapse is untouched -- it matches on the reason and contact bits only, so a
 * run keeps the bucket of the refusal that opened it, exactly as it already
 * keeps that refusal's timestamp and the rest of its evidence.
 *
 * **Bucket 0 is `unknown`, and that is what makes this cost no erase.** Records
 * written before this existed have bits 5-7 clear, and a scheme that numbered
 * the first real bucket 0 would decode every one of them as "definitely still"
 * -- a false claim about the entire log, and exactly the misread
 * RECORD_FORMAT_VERSION exists to prevent by erasing. Reserving 0 makes those
 * records say the true thing instead, so the format version does not move and a
 * kept log survives the upgrade.
 *
 * The boundaries sit against the three populations measured on hardware (see
 * WINDOW_MOVE_MILLI_G in main.c), with three of the seven inside 50 mg of the
 * gate, because that is where the decision is:
 *
 *   0  unknown      the field was not recorded
 *   1  < 50 mg      hand deliberately still (36-45), or no IMU evidence at all
 *   2  50-99 mg     under the gate, upper half
 *   3  100-149 mg   over the gate by less than half of it
 *   4  150-249 mg   `stationary, hands in use` (122-217) straddles 3 and 4
 *   5  250-499 mg
 *   6  500-999 mg   `walking` (632-938)
 *   7  >= 1000 mg
 *
 * **Carried on every record, not only on refusals.** A gate's false-refusal
 * rate cannot be measured from the windows it refused, and bits 5-7 are free
 * either way.
 *
 * ⚠️ **The bucket records the *mean* per-drain movement; the gate judges the
 * *median*.** That is deliberate. The boundaries above are calibrated against
 * means, so a stored bucket stays comparable across every record ever written,
 * while the accepted / refused split *within* each bucket is the readout of
 * what judging on the median buys. A reader analysing this field is looking at
 * the same quantity throughout.
 */
#define FLASH_MOVEMENT_UNKNOWN 0u
#define FLASH_MOVEMENT_BUCKETS 8u

/* Encodes milli-g into the table above.
 *
 * Never returns FLASH_MOVEMENT_UNKNOWN: a measured zero is bucket 1, because
 * zero is what a still ring reads *and* what a ring whose IMU never came up
 * reads, and main.c deliberately does not distinguish the two -- both mean "no
 * evidence of movement". Unknown is reserved for records that predate the
 * field, which is a different statement and the only one this cannot measure.
 */
uint8_t flash_movement_bucket(uint16_t milli_g);

/* The bucket's range as a printable string -- "<50", "500-999", "unrecorded".
 * Bounds-checked, because a record read back off flash is not trusted input: a
 * torn write or a stale page can put any of the eight values in this field.
 */
const char *flash_movement_label(uint8_t bucket);

/* One measured window as main.c sees it, believed or not.
 *
 * This is also byte-for-byte what the flash record holds and what the vitals
 * characteristic notifies (ble.c), which is deliberate: one layout, described
 * once. The replay path freezes it onto the wire, so it is meant to be widened
 * rarely and carefully.
 */
struct flash_sample {
    uint32_t timestamp_ms;    /* k_uptime_get() at the reading */
    uint16_t bpm;             /* 0 if not determined */
    uint16_t spo2_tenths;     /* 976 = 97.6%, 0 if not determined */
    uint16_t confidence;      /* pulse periodicity, 0-1000 */
    uint16_t perfusion_milli; /* pulse AC as parts-per-thousand of DC */
    uint16_t steps;           /* steps since the previous record, not a total */
    bool contact;             /* was the ring against skin */
    /* Mean per-drain movement over the window, in milli-g, as
     * window_movement_milli_g() measured it. Narrowed to a three-bit bucket on
     * the way into the record -- see flash_movement_bucket() above. This is the
     * one field where the sample and the record are not the same number; it is
     * narrowed rather than dropped because the bucket boundaries are the
     * decision and the finer precision has no consumer.
     */
    uint16_t movement_milli_g;
};

/* One stored record: what a `struct flash_sample` becomes on flash, and
 * byte-for-byte what goes on the air (ble.c). Public because the replay path
 * forwards these verbatim and the console dump decodes them -- both need the
 * layout, and a second definition of it somewhere else is a format that drifts.
 *
 * 16 bytes, naturally aligned, no padding. Fields are little-endian on the wire;
 * the packing is explicit in ble.c rather than a memcpy of this struct, because
 * on the air the layout is a protocol and not the compiler's choice.
 */
struct flash_record {
    uint32_t timestamp_ms;    /* 0  */
    uint16_t bpm;             /* 4  */
    uint16_t spo2_tenths;     /* 6  */
    uint16_t confidence;      /* 8  */
    uint16_t perfusion_milli; /* 10 */
    uint16_t steps;           /* 12 */
    uint8_t flags;            /* 14 */
    uint8_t repeat;           /* 15 */
};

#define FLASH_RECORD_SIZE 16u

/* Reading a record's flags byte. The bit positions stay private to
 * flash_store.c -- a caller that needs to know whether a record is a refusal
 * should ask rather than learn the layout, so the layout can change without
 * every caller changing with it.
 */
bool flash_record_refused(const struct flash_record *rec);
bool flash_record_contact(const struct flash_record *rec);
enum flash_refusal flash_record_reason(const struct flash_record *rec);

/* The movement bucket, 0-7, per the table above. Meaningful on every record,
 * refused or not; 0 means the record predates the field.
 */
uint8_t flash_record_movement(const struct flash_record *rec);

/* How many records the boot erase destroyed, or 0 when the log was kept across
 * the reset. Valid after flash_store_init() and constant for the rest of the
 * boot.
 *
 * The console says this at boot (`Boot erase: destroying N record(s)`) and the
 * console is unavailable on a worn ring, which is the whole reason it is also
 * a function: ble.c carries it to the phone on the control read, so a reset in
 * the field can report what it cost as well as that it happened.
 */
uint32_t flash_store_boot_destroyed(void);

/* Builds the stored form of a sample without storing it. The live BLE
 * notification uses this so that "the package is byte-for-byte the record"
 * holds by construction rather than by two code paths agreeing.
 */
void flash_record_from_sample(struct flash_record *rec, const struct flash_sample *sample);

/* Returns the current cell voltage in millivolts, or a negative errno when the
 * battery monitor is unavailable. Called once per flush so the dump can carry
 * a battery package alongside the vitals ones. May be NULL to skip it.
 */
typedef int (*flash_store_battery_mv_fn)(void);

/* Returns the free-running hardware step total, or a negative errno when the
 * IMU never came up. Called alongside the battery callback so the flush's
 * battery package can carry a step figure with it.
 *
 * A *total*, deliberately, and not the per-record delta `struct flash_sample`
 * carries. The two answer different questions and the battery package is the
 * wrong place for a delta: it is emitted on flushes, which are irregular and
 * far apart, so a "steps since the last one" here would be a number over an
 * interval nobody downstream can reconstruct. A running total read straight
 * from the counter is interval-free -- any two battery packages can be
 * differenced -- and it doubles as the check on the deltas, which are
 * accumulated by a completely separate path in main.c.
 *
 * The 24-bit counter fits an int with room to spare, so a negative return is
 * unambiguously an error.
 */
typedef int (*flash_store_steps_fn)(void);

/* Opens the partition and arms the append cursor. The battery and step
 * callbacks (either may be NULL) are held and invoked on every flush, boot
 * included. Returns 0, or a negative errno.
 *
 * `restore_generation` decides what happens to whatever survived the last power
 * cycle. Zero -- the historical behaviour -- dumps and erases it, and draws a
 * fresh generation. Non-zero asks to *keep* it under that generation, which the
 * caller must have persisted across the reset along with the clock anchor; see
 * flash_store_restore(). A restore that cannot be proven coherent falls back to
 * the erase, so passing a generation is always safe.
 *
 * On a successful restore `*newest_ts_ms` (may be NULL) receives the highest
 * timestamp in the kept log. **The caller must rebase uptime above it** before
 * the first new record, or new records land at timestamps earlier than old ones
 * and the log stops being ordered. On an erase it is set to 0.
 */
int flash_store_init(flash_store_battery_mv_fn battery_mv, flash_store_steps_fn steps,
                     uint32_t restore_generation, uint32_t *newest_ts_ms);

/* Rebuilds the log from the part instead of erasing it, under `generation`.
 * Called by flash_store_init(); exposed for the host tests.
 *
 * Returns 0 and sets the pointers if the surviving log is coherent, -ENOENT if
 * the partition is blank, or a negative errno if it cannot be trusted -- in
 * which case the caller erases as usual. `*newest_ts_ms` (may be NULL) receives
 * the highest timestamp found on success.
 */
int flash_store_restore(uint32_t generation, uint32_t *newest_ts_ms);

/* Appends one reading the firmware vouches for. If the partition is full this
 * first dumps and erases it (the "full" flush), then writes. Returns 0, or a
 * negative errno.
 */
int flash_store_append(const struct flash_sample *sample);

/* Appends the *fact* that a window produced nothing believable, with the
 * evidence attached. Not the same thing as storing an unbelieved reading --
 * bpm and confidence are recorded as measured and the record is flagged
 * refused, so nothing downstream can mistake it for a vital sign.
 *
 * Why store it at all: a refused window used to leave no trace, which made "not
 * worn", "worn but unreadable" and "the ring has stopped working" the same
 * observation. Anything reading the log has to tell those apart, and it cannot
 * if every kind of silence looks alike.
 *
 * **Consecutive refusals of the same kind are collapsed into one record** with a
 * repeat count. Note where the refusals actually come from: a ring that is off
 * the finger never gets this far, because the cycle bails at the contact probe
 * before it measures anything (§5A.4). So these are overwhelmingly a *worn*
 * ring whose pulse cannot be read -- weak perfusion, a bad position, cold hands
 * -- which is a stretch that can last hours and would otherwise cost five
 * records a cycle saying the same thing.
 *
 * The run is held in RAM until something ends it: a believed reading, a refusal
 * of a different kind, a flush, or the count saturating at 255. A run in
 * progress therefore does not survive a power cycle. That is the right trade --
 * the data is low-value, and buying durability would cost an erase cycle per
 * refusal on a partition rated for ~10k of them -- and the saturation bounds
 * how much can ever be in flight (255 refusals is under two hours).
 *
 * Returns 0, or a negative errno.
 */
int flash_store_append_refusal(const struct flash_sample *sample, enum flash_refusal why);

/* Walks stored records from byte offset `from` to the append cursor, handing
 * each to `cb`. Read-only: no erase, no cursor movement, nothing observable
 * afterwards. Returns the offset it stopped at, which is `from` plus whole
 * records and is what a caller resumes from next time.
 *
 * `cb` returns true to continue and **false to stop early**, which is what lets
 * the replay path yield when the radio runs out of buffers and pick the walk up
 * where it left off. A cb that always returns true walks to the cursor.
 *
 * The cursor is snapshotted on entry, so a record appended *during* the walk is
 * not visited. That is deliberate and it is a correctness requirement, not a
 * simplification: a 16-byte record is four flash words and is not written
 * atomically, so walking to a live cursor could read a half-written record.
 *
 * A run of refusals still being accumulated in RAM is likewise not visited --
 * it is not in flash yet. See flash_store_append_refusal().
 */
typedef bool (*flash_store_cb)(uint32_t offset, const struct flash_record *rec, void *user_data);

uint32_t flash_store_foreach(uint32_t from, flash_store_cb cb, void *user_data);

/* The append cursor: the sequence number the next record will be written at,
 * and therefore the offset a caller has read everything up to once a walk
 * returns it. Always a whole multiple of FLASH_RECORD_SIZE.
 *
 * A *sequence* number, not a position in the partition. The log is a ring, so
 * physical offsets repeat every lap; sequence numbers do not repeat within a
 * boot, and the generation below covers boots.
 */
uint32_t flash_store_cursor(void);

/* The oldest record still held. Rises when the phone acknowledges delivery and
 * whole pages can be reclaimed, and also when the ring laps the partition and
 * has to drop its oldest page to keep recording.
 *
 * A caller comparing this against the offset it last read is asking "did I miss
 * anything?" -- and if head has moved past it, the answer is yes.
 */
uint32_t flash_store_head(void);

/* How many bytes the log can hold before it laps and starts dropping its oldest
 * page. One page short of the partition, because a page is always the erased
 * gap the cursor is about to run into.
 *
 * Public so a caller can express a policy as a *fraction* of the buffer rather
 * than as a record count that silently means something different the next time
 * the partition is resized -- which has already happened once (the DTS grew
 * storage from 24KB to 232KB and every "1536 records" comment in the tree
 * became wrong at a stroke). Returns 0 before flash_store_init() succeeds.
 */
uint32_t flash_store_capacity(void);

/* The flash page size, in bytes, as the driver reports it -- and therefore
 * **the smallest amount of the log that acknowledging can actually free.**
 *
 * That is the reason this is public rather than an implementation detail. A
 * flush the phone acknowledges reclaims nothing at all unless it covers a whole
 * page, because flash cannot erase less than one; so a policy about when to ask
 * the phone to collect (main.c) is really a policy about pages, and expressing
 * it in any other unit means expressing it in a unit where most values are
 * wrong. Returns 0 before flash_store_init() succeeds.
 */
uint32_t flash_store_page_size(void);

/* Acknowledges delivery of everything below `upto`, allowing the ring to erase
 * the flash pages that are now entirely spoken for.
 *
 * **This is the only thing that frees space on purpose.** An unconditional
 * erase -- every page, on demand -- throws away whatever the client has not
 * received and spends an erase cycle on pages whether or not they hold
 * anything. Here a page goes only when every record in it has been confirmed
 * delivered, which is the minimum bar for a buffer whose whole job is to hold
 * data until someone collects it.
 *
 * Whole pages only: flash cannot erase less, so a page holding one
 * unacknowledged record stays put. Acknowledgements may repeat or arrive out of
 * order without harm; only the highest one counts.
 *
 * **This records the acknowledgement and does nothing else.** Both the
 * reclaiming and the erase happen in flash_store_service(), on the measurement
 * thread. This call is the module's one entry point that does not run on main,
 * and keeping it to a single store is what lets every pointer in the log have
 * exactly one writer -- see the ownership note at the top of flash_store.c. The
 * cost is that space comes back on the next measurement cycle rather than
 * instantly, against a buffer that is days deep.
 *
 * Returns 0, -EINVAL for an offset outside the live range (a client that has
 * lost track -- better refused than acted on), or -ENODEV.
 */
int flash_store_release(uint32_t upto);

/* Does the work an acknowledgement implies: moves the head over pages the phone
 * has confirmed, and erases some of them. Call it once per measurement cycle,
 * from the measurement thread and from nowhere else.
 *
 * **Nothing waits on this.** Space is reclaimed by the arithmetic, which is
 * instant; the erase only decides how long acknowledged vitals go on physically
 * existing on the part, and that deadline is days rather than seconds. So it
 * erases a few pages per call and leaves the rest, which keeps the cycle time
 * predictable -- a phone returning from four days away would otherwise
 * acknowledge the entire buffer and add ~4.8s to one cycle, and the gap between
 * bursts is the ring's whole contribution to alert latency.
 *
 * Falling behind is free and self-correcting: a page the scrub has not reached
 * by the time the cursor laps onto it is erased by the write path instead.
 *
 * Safe to call when there is nothing to do, which is the common case.
 */
void flash_store_service(void);

/* Which erase generation the log is in. Incremented every time the partition is
 * erased, which resets every offset in it to mean something different.
 *
 * A cursor without a generation is a bug waiting to happen: a client that says
 * "resume from byte 800" after a flush would silently receive unrelated
 * records. Pairing the two makes a stale request detectable instead.
 */
uint32_t flash_store_generation(void);

/* Logs the packages appended since the last call, plus a fresh battery package,
 * and changes nothing in flash: no erase, no append cursor movement. A dump,
 * not a flush -- the log keeps growing afterwards exactly as if this had not
 * been called, so it can be done as often as you like without costing the
 * eventual flush a single record.
 *
 * Only the new ones, so the output stays a constant few lines however long the
 * log gets; the summary line carries the running total, which is what shows the
 * history behind them is still there. Reprinting the whole log would outgrow
 * the 1KB RTT buffer within ~13 records and start dropping lines silently.
 *
 * This is a "prove it is alive" call, not part of the storage contract.
 * Returns 0, or a negative errno.
 */
int flash_store_dump(void);

#endif
