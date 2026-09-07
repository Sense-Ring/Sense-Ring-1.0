/* BLE peripheral: advertises "SenseRing" and serves vitals over GATT.
 *
 * Two of the three services are Zephyr's own -- the Heart Rate Service (HRS,
 * 0x180D) and Battery Service (BAS, 0x180F) -- so a generic phone app reads the
 * pulse and charge with no custom code. The third is custom and carries four
 * characteristics: live vitals, live motion, a control channel the client
 * writes commands to, and the buffered log replayed on request.
 *
 * **The replay is the primary data path, not a catch-up feature.** The ring
 * measures into flash whether or not anything is connected; the live
 * notifications only exist while a phone happens to be in range and awake.
 * Everything measured while it was not arrives over the history characteristic
 * or not at all. See ARCHITECTURE.md 5B.6.
 *
 * Measuring never waits on a connection: main() calls the notify helpers every
 * cycle regardless, and they quietly do nothing when no client is subscribed.
 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/hrs.h>
#include <zephyr/settings/settings.h>
#include "ble.h"
#include "wallclock.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Custom "SenseRing Vitals" service and its single package characteristic.
 * Randomly-assigned 128-bit UUIDs -- they only have to be unique, and sharing a
 * base makes the pair obvious in a scanner.
 */
#define BT_UUID_VITALS_SVC_VAL \
    BT_UUID_128_ENCODE(0xf1a00001, 0x9c1b, 0x4d3e, 0xa7b2, 0x5e8c6d9f0a11)
#define BT_UUID_VITALS_PKT_VAL \
    BT_UUID_128_ENCODE(0xf1a00002, 0x9c1b, 0x4d3e, 0xa7b2, 0x5e8c6d9f0a11)
#define BT_UUID_MOTION_PKT_VAL \
    BT_UUID_128_ENCODE(0xf1a00003, 0x9c1b, 0x4d3e, 0xa7b2, 0x5e8c6d9f0a11)
#define BT_UUID_CONTROL_VAL \
    BT_UUID_128_ENCODE(0xf1a00004, 0x9c1b, 0x4d3e, 0xa7b2, 0x5e8c6d9f0a11)
#define BT_UUID_HISTORY_VAL \
    BT_UUID_128_ENCODE(0xf1a00005, 0x9c1b, 0x4d3e, 0xa7b2, 0x5e8c6d9f0a11)
/* Cell level and step total together, on the cadence power_check() runs at.
 * Separate from the standard Battery Service rather than an extension of it,
 * because BAS is spec-fixed at a single percent byte and has nowhere to put a
 * step count -- and separate from the motion package because that one is a
 * live-stream packet the shipping build compiles out. See ble_notify_status().
 */
#define BT_UUID_STATUS_VAL \
    BT_UUID_128_ENCODE(0xf1a00006, 0x9c1b, 0x4d3e, 0xa7b2, 0x5e8c6d9f0a11)

static struct bt_uuid_128 vitals_svc_uuid = BT_UUID_INIT_128(BT_UUID_VITALS_SVC_VAL);
static struct bt_uuid_128 vitals_pkt_uuid = BT_UUID_INIT_128(BT_UUID_VITALS_PKT_VAL);
static struct bt_uuid_128 motion_pkt_uuid = BT_UUID_INIT_128(BT_UUID_MOTION_PKT_VAL);
static struct bt_uuid_128 control_uuid = BT_UUID_INIT_128(BT_UUID_CONTROL_VAL);
static struct bt_uuid_128 history_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_VAL);
static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(BT_UUID_STATUS_VAL);

/* The control characteristic is the ring's one inbound channel, and it is
 * deliberately *one* channel rather than a characteristic per setting. Every
 * command is `{u8 opcode, payload...}`, so adding one later costs an opcode
 * rather than a UUID, an attribute table entry, and another thing for a client
 * to discover.
 *
 * Anything else a client needs to tell the ring belongs here too, for the same
 * reason: it is a command to the ring rather than a value to expose.
 */
#define CONTROL_OP_SET_TIME 0x01u
#define CONTROL_OP_REPLAY   0x02u
#define CONTROL_OP_ACK      0x03u

/* SET_TIME: opcode + uint64 little-endian Unix milliseconds. */
#define CONTROL_SET_TIME_LEN 9u

/* REPLAY: opcode + uint32 generation + uint32 byte offset to resume from.
 *
 * The generation is not optional padding. An offset only means anything within
 * one erase generation -- after a flush, byte 800 is a different record than it
 * was -- so a client that resumed on the offset alone would silently receive
 * records it had never seen in place of the ones it asked for. Sending both
 * lets the ring answer "that offset is stale" instead of answering wrongly.
 */
#define CONTROL_REPLAY_LEN 9u

/* ACK: opcode + uint32 generation + uint32 offset delivered up to.
 *
 * Same shape as REPLAY and for the same reason -- an offset without its
 * generation is a number that silently means something else after an erase.
 * This is the command that lets the ring reclaim flash, so acting on a stale
 * one would destroy records the phone never received.
 */
#define CONTROL_ACK_LEN 9u

/* What a read of the control characteristic returns: everything a client needs
 * to date the ring's records and to ask for the ones it has not seen.
 *
 *   [0]      flags, bit 0 = the anchor is valid, bit 1 = the reset cause was readable
 *   [1..8]   epoch_at_boot, uint64 little-endian ms
 *   [9..12]  the ring's uptime right now, uint32 little-endian ms
 *   [13..16] erase generation, uint32
 *   [17..20] append cursor -- bytes currently stored, uint32
 *   [21..24] RESETREAS as this boot found it, uint32
 *   [25..26] records the boot erase destroyed, uint16
 *
 * One read rather than several, because a client that has to assemble this from
 * three characteristics can be interrupted between them and act on a mixture of
 * two different moments. Generation and cursor especially have to be read
 * together: they are only meaningful as a pair.
 *
 * **The last two fields are appended, deliberately, and that is what makes the
 * change free.** A client written against the 21-byte layout reads the prefix
 * and is unaffected; one written against this reads six more bytes it knows the
 * meaning of. Nothing above [21] moved, so there is no version negotiation and
 * no flag day. Inserting them anywhere else would have cost both.
 *
 * ## Why the reset cause is on this read at all
 *
 * **Because a worn ring cannot be asked any other way, and the ring resets.**
 * `log_reset_cause()` (main.c) names the cause at boot and writes it to RTT,
 * and RTT means a debugger, and a debugger means the ring is not on a finger --
 * which is precisely the condition under which the fault appears. The 08-20/21
 * wear test recorded eight unrequested resets in sixteen hours and could say
 * nothing about any of them beyond that they happened.
 *
 * The clock handshake already runs on every connection and is already the place
 * the ring answers questions about its own boot, so this costs no round trip
 * and no extra wake.
 *
 * ⚠️ **A zero cause with the valid bit set is the informative case, not a
 * missing one.** nRF52832 RESETREAS latches RESETPIN, DOG, SREQ, LOCKUP and the
 * wake sources, and has no bit for power-on or for brownout -- so a supply that
 * actually went away leaves the register clear. See the long note above
 * log_reset_cause() in main.c. The flag bit exists to keep that answer apart
 * from "hwinfo could not tell us", which is a firmware gap and means nothing
 * about the boot.
 *
 * The uptime is there for one reason. Record timestamps are uint32 milliseconds
 * and wrap after ~49.7 days, so `epoch_at_boot + record.timestamp_ms` is
 * ambiguous across a wrap. Knowing the ring's uptime at read time resolves it:
 * a record timestamp far *above* the current uptime belongs to the previous
 * wrap. In practice the buffer only spans hours so this cannot arise, but a
 * client that assumes it cannot is a client that breaks on the one ring that
 * runs seven weeks without a reset.
 */
/* **Growing this is safe in exactly one direction, and the layout is chosen to
 * keep it that way: every field keeps its offset forever and new ones go on the
 * end.** An older client reading a newer ring finds everything it knows where
 * it expects it and ignores the tail; a newer client reading an older ring gets
 * a short buffer and must check the length before touching the fields it does
 * not find. Neither side has to know the other's version.
 */
#define CONTROL_READ_LEN 38u
#define CONTROL_FLAG_ANCHORED    BIT(0)
#define CONTROL_FLAG_RESET_CAUSE BIT(1)

/* The previous run ended with the cell empty. See the flat mark in main.c for
 * what has to be true for this to be set -- it is a conclusion drawn from two
 * pieces of evidence at boot, not something the ring observed happening.
 */
#define CONTROL_FLAG_WENT_FLAT   BIT(2)

/* History notification framing. Every notification on `…0005` starts with one
 * type byte, so a client never has to infer what it is holding from its length.
 *
 *   HEADER {u8, u32 generation, u32 from, u32 end}   -- a transfer is starting
 *   DATA   {u8, record[1..n]}                        -- n whole 16-byte records
 *   END    {u8, u32 next_offset}                     -- everything sent; this
 *                                                       is the cursor to resume
 *                                                       from and to acknowledge
 *   STALE  {u8, u32 generation}                      -- your offset belongs to
 *                                                       an erased generation;
 *                                                       start again from 0
 *
 * A one-byte tag per notification costs ~2% of the payload and buys a stream
 * that can be extended later without every existing client mis-parsing it.
 */
#define HISTORY_TYPE_HEADER 0x01u
#define HISTORY_TYPE_DATA   0x02u
#define HISTORY_TYPE_END    0x03u
#define HISTORY_TYPE_STALE  0x04u

/* EVENT {u8 0x05, u32 cursor} -- "something happened, collect now".
 *
 * The ring cannot push its buffer; the phone pulls it. So when the ring decides
 * the buffer has waited long enough it needs a way to say so, or it waits for
 * whenever the phone next polls. This is that nudge, and it is the replacement
 * for what the old flush-and-erase was really doing: getting records out of the
 * device promptly.
 */
#define HISTORY_TYPE_EVENT  0x05u

/* ALERT {u8 0x06, u8 reason} -- a condition changed that a person should know
 * about now.
 *
 * Distinct from EVENT because they ask for different things. EVENT means
 * "collect the buffer"; ALERT means "tell the wearer". Nothing is collected in
 * response to this one, so it carries no cursor.
 *
 * It rides the history characteristic rather than a new one, which is a
 * deliberate trade: a fourth notifying characteristic would need its own CCC,
 * its own subscription and its own re-subscribe on every reconnect, for two
 * bytes that are sent perhaps five times a day. The phone is already subscribed
 * here, and a client that predates this type ignores an unknown leading byte --
 * which is exactly what the framing note above promised it could.
 *
 * **Sent on transitions only.** Repeating "still off the finger" every cycle
 * would train the wearer to dismiss it, which is how an alerting system stops
 * being one.
 */
#define HISTORY_TYPE_ALERT  0x06u

#define ALERT_WORN             0x01u /* back on a finger */
#define ALERT_NOT_WORN         0x02u /* came off */
#define ALERT_BATTERY_LOW      0x03u /* measuring less often to stretch it */
#define ALERT_BATTERY_CRITICAL 0x04u /* measuring has stopped; buffer at risk */
#define ALERT_BATTERY_OK       0x05u /* charged back above the low mark */

/* Set by the client subscribing to each package characteristic. Notifying while
 * false is harmless but pointless, so they gate their sends.
 *
 * Two flags rather than one because the two streams are genuinely independent:
 * a client that only wants a heart rate should not be made to carry
 * acceleration, and one logging movement should not have to subscribe to
 * vitals to get it.
 */
static bool vitals_notify_enabled;
static bool motion_notify_enabled;

/* Defined with the connection-parameter policy below, which needs the replay
 * state and the work queue that this file only declares further down. Every
 * inbound event calls it, and the earliest of those is a subscription.
 */
static void conn_mark_active(void);

static void vitals_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    vitals_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Vitals notifications %s", vitals_notify_enabled ? "enabled" : "disabled");
    conn_mark_active();
}

static void motion_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    motion_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Motion notifications %s", motion_notify_enabled ? "enabled" : "disabled");
    conn_mark_active();
}

static bool status_notify_enabled;

/* What this boot was, for the control read to hand to the phone.
 *
 * Held here rather than fetched on demand because it cannot be fetched: main.c
 * clears RESETREAS immediately after reading it, so that the third boot in a
 * row cannot report the second one's cause as well as its own. After that clear
 * the only copy is the one main.c passes down.
 *
 * Zero and invalid until ble_set_boot_report() is called, which main() does
 * before ble_init(). A client that reads before then sees the flag clear and
 * knows not to believe the field, which is the same answer it gets on a
 * firmware too old to have one.
 */
static uint32_t boot_reset_cause;
static bool boot_reset_cause_valid;
static uint16_t boot_records_destroyed;

/* What the cell said on the way out, if the previous run ended with it empty.
 * Zero and clear otherwise, which is why the flag is what a client tests --
 * a genuine last reading of 0% is not something this firmware can produce.
 */
static bool boot_went_flat;
static uint8_t boot_flat_percent;
static uint16_t boot_flat_mv;
static uint64_t boot_flat_epoch_ms;

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    status_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Status notifications %s", status_notify_enabled ? "enabled" : "disabled");
    conn_mark_active();
}

/* The service is defined below, because BT_GATT_SERVICE_DEFINE needs the
 * callbacks. The replay helpers need the service back, to notify on one of its
 * attributes -- hence the forward declaration rather than an ordering that
 * cannot exist.
 */
extern const struct bt_gatt_service_static vitals_svc;

static bool history_notify_enabled;

/* Has this client asked for its backlog since it subscribed?
 *
 * Set by CONTROL_OP_REPLAY and cleared on every subscribe, so it answers "did
 * the app already do this for itself" rather than "has a transfer ever
 * happened". That is the question the offer below has to get right: the phone
 * requests a backfill as soon as it subscribes, and an offer sent to a client
 * that has already asked is a second transfer of records the first one is
 * carrying.
 */
static bool client_asked_since_subscribe;

/* Defined with the work queue further down, which is what it schedules on. */
static void offer_schedule(k_timeout_t delay);
static void offer_cancel(void);

/* How long to wait after a subscribe before offering the backlog.
 *
 * Long enough for a client that asks for itself to have asked -- one that
 * writes REPLAY as soon as it subscribes does so within tens of connection
 * events at the 30ms interval this link
 * opens on that is tens of connection events. Short enough that a client which
 * does *not* ask is not left waiting on the ten-minute backstop.
 *
 * This is a grace period, not a delay on the data: a phone that asks gets its
 * transfer immediately and this timer expires into a no-op.
 */
#define OFFER_AFTER_SUBSCRIBE_MS 2000

static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    history_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("History notifications %s", history_notify_enabled ? "enabled" : "disabled");
    conn_mark_active();

    /* **Subscribing is the one moment the ring can see that the app was
     * opened.** Everything else the phone does on the way in -- connecting,
     * exchanging MTU, setting the clock -- happens for reasons that have
     * nothing to do with the user; subscribing to history is the app saying it
     * is ready to receive records, which is as close to "opened" as this side
     * of the link gets.
     *
     * Until this existed the ring's answer to an app being opened was to say
     * nothing and wait: the log left on a page-fill, on the backlog crossing
     * BACKLOG_NUDGE_HIGH_PCT, or on the ten-minute uncollected backstop -- none of which is "the wearer just looked at their
     * phone". A client that asks for itself was fine; a client that did not sat
     * in front of a full buffer.
     */
    if (history_notify_enabled) {
        client_asked_since_subscribe = false;
        offer_schedule(K_MSEC(OFFER_AFTER_SUBSCRIBE_MS));
    } else {
        offer_cancel();
    }
}

/* The replay in progress, if any.
 *
 * A transfer is deliberately *not* run from the control write callback. That
 * callback executes on the Bluetooth RX thread, and walking up to 14592 records
 * from it would hold the stack's own thread for the length of the transfer.
 * The write records what was asked for and returns; a work item does the work.
 */
static struct {
    bool active;
    uint32_t cursor;     /* offset the next undelivered record starts at */
    uint32_t generation; /* the generation this transfer belongs to */
} s_replay;

/* The connection, ref-held while it exists. Needed because the negotiated MTU
 * is a property of the connection and the replay runs from a work item that has
 * no conn handle of its own.
 */
static struct bt_conn *s_conn;

/* Records per DATA notification at the MTU this firmware asks for.
 *
 * CONFIG_BT_L2CAP_TX_MTU is 65, so a notification carries 62 bytes; one type
 * byte leaves 61, which is three whole 16-byte records with 13 to spare. The
 * buffer is sized from this constant and the runtime capacity is clamped to it,
 * so a client that somehow negotiates a larger MTU cannot overrun the buffer --
 * it just does not get the benefit until this constant and the Kconfig move
 * together.
 */
#define HISTORY_MAX_RECORDS 3u

static void history_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(history_work, history_work_handler);

/* The transfer runs on its own work queue rather than the system one.
 *
 * There are only CONFIG_BT_BUF_ACL_TX_COUNT (3) TX buffers, so a bulk transfer
 * empties the pool almost immediately, and bt_gatt_notify() may *block* waiting
 * for one rather than returning -ENOMEM. On the system work queue that would
 * stall every other work item behind a radio that is busy -- including this
 * firmware's advertising restart. A private queue makes that impossible to
 * reason wrongly about: the only thing a stalled transfer can delay is itself.
 *
 * The priority is deliberately below the main thread's. Measuring must never
 * wait on a backfill: the ring's job is to take readings, and delivering old
 * ones is strictly less important than not missing new ones.
 */
#define HISTORY_STACK_SIZE 1024
#define HISTORY_THREAD_PRIO 5

static K_THREAD_STACK_DEFINE(history_stack, HISTORY_STACK_SIZE);
static struct k_work_q history_workq;

/* One place to schedule from, so the queue cannot be got wrong at a call site. */
static void history_schedule(k_timeout_t delay)
{
    (void)k_work_reschedule_for_queue(&history_workq, &history_work, delay);
}

/* Offers the backlog to a client that has just subscribed and has not asked.
 *
 * On the history queue rather than the system one for the same reason the
 * transfer is: it ends in a notification, and a notification can block on the
 * TX pool. Its own work item rather than history_work's, because that one is
 * the transfer state machine and rescheduling it means something quite
 * different.
 *
 * Every condition here is a reason *not* to send, and they are all checked at
 * expiry rather than at subscribe, because the two seconds in between are
 * exactly when the phone does the thing that makes the offer unnecessary.
 */
static void offer_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Gone, or unsubscribed while the grace period ran. */
    if (!history_notify_enabled || s_conn == NULL) {
        return;
    }

    /* The phone asked for itself, which is the common case and the good one.
     * A transfer already running is the same answer by a different route.
     */
    if (client_asked_since_subscribe || s_replay.active) {
        return;
    }

    /* Nothing to offer. `head` is what the phone has acknowledged and this ring
     * has finished reclaiming, so this is "records the phone does not have",
     * not "records that exist" -- an app opened twice in a minute gets one
     * offer and then silence, rather than a nudge towards an empty transfer.
     */
    if (flash_store_cursor() <= flash_store_head()) {
        return;
    }

    LOG_INF("Phone subscribed and did not ask -- offering %u byte(s) of backlog",
            flash_store_cursor() - flash_store_head());
    ble_notify_event();
}

static K_WORK_DELAYABLE_DEFINE(offer_work, offer_work_handler);

static void offer_schedule(k_timeout_t delay)
{
    (void)k_work_reschedule_for_queue(&history_workq, &offer_work, delay);
}

static void offer_cancel(void)
{
    (void)k_work_cancel_delayable(&offer_work);
}

/* ---- the security deadline ------------------------------------------------
 *
 * A central that connects and never pairs holds this ring's only connection
 * slot for as long as it likes. CONFIG_BT_MAX_CONN is 1 and advertising stops
 * while connected, so the wearer's own phone cannot get in behind it -- and the
 * ring has no display, no button and no UART to say why. Meanwhile the log goes
 * on filling with nothing acknowledging it, so write_record() eventually laps
 * and drops the oldest unacknowledged page: the history is lost by attrition
 * rather than by anything being asked for.
 *
 * None of that needs keys, or proximity beyond radio range, or any knowledge of
 * this firmware at all. So the link gets a deadline: reach an encrypted link
 * within SECURITY_DEADLINE_MS or be hung up on.
 *
 * The test is *encryption*, not pairing, and the difference is what keeps this
 * invisible to the phone that owns the ring. A bonded phone re-encrypts from
 * its stored LTK in a few connection events and never pairs at all; a new phone
 * inside the pairing window below pairs and lands in the same place. Both cancel
 * the deadline through security_changed(). What does not cancel it is a peer
 * that connects and then does nothing, which is the only thing this is for.
 *
 * **It does end anonymous access to the standard services.** A generic heart
 * rate app that connects, subscribes to 0x180D and never pairs now gets thirty
 * seconds. That is a real behaviour change and it is the intended one: on a
 * one-wearer, one-phone device an unpaired client has no business holding the
 * slot, and what it would be reading is the wearer's pulse.
 *
 * Thirty seconds because the inbound-first conversation a real phone has on
 * arrival -- discovery, the MTU exchange, then pairing or re-encryption -- is
 * the traffic slave latency punishes hardest, and connected() has just handed
 * it CONN_ACTIVE_HOLD_MS of fast-regime link to do it in. This has to outlast
 * that or it would cut off the very phone it exists to protect.
 */
#define SECURITY_DEADLINE_MS 30000

static void security_deadline_handler(struct k_work *work)
{
    struct bt_conn *conn = s_conn;

    ARG_UNUSED(work);

    /* Read once, because disconnected() clears it from the Bluetooth RX thread.
     * That callback cancels this item before it unrefs, so the two only race
     * over an already-expired timer -- the same ownership contract
     * history_work_handler() runs under, and the same NULL check.
     */
    if (conn == NULL) {
        return; /* peer left on its own; nothing to hang up on */
    }

    /* Encrypted after all. security_changed() and this timer can land in either
     * order, and the timer losing that race must not drop a good link.
     */
    if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
        return;
    }

    LOG_WRN("Peer held the connection for %u ms without encrypting -- disconnecting",
            SECURITY_DEADLINE_MS);
    (void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static K_WORK_DELAYABLE_DEFINE(security_work, security_deadline_handler);

static void security_deadline_arm(void)
{
    (void)k_work_reschedule_for_queue(&history_workq, &security_work,
                                      K_MSEC(SECURITY_DEADLINE_MS));
}

static void security_deadline_cancel(void)
{
    (void)k_work_cancel_delayable(&security_work);
}

/* ---- connection parameters ------------------------------------------------
 *
 * The radio was the largest avoidable draw on this device, and almost all of it
 * was spent listening for a phone that had nothing to say. The link came up at a
 * 30-50ms interval with zero slave latency, so the receiver woke 20-33 times a
 * second, all day. What it was carrying is two packets -- one vitals package and
 * one motion package, sharing a timestamp -- per measurement cycle. Under
 * buffer-and-flush the live notifications are not even the primary path; the
 * history replay is (5B.6), and that is a burst every few minutes at most.
 *
 * Slave latency is the lever for exactly this shape of traffic. At latency N the
 * peripheral may ignore up to N consecutive connection events *when it has
 * nothing to send*, and still transmits immediately when it does -- so nothing
 * this ring sends is delayed by it. What it does delay is the first inbound
 * packet of a burst: a control write from the phone waits until the next event
 * we actually listen on. Once a burst is under way the controller keeps the link
 * awake on the More Data bit, so the cost is paid once per burst, not per
 * round trip.
 *
 * Hence two regimes rather than one setting:
 *
 *   idle    -- nothing has happened for CONN_ACTIVE_HOLD_MS. Listen once every
 *              (1 + 30) * 50ms = 1.55s instead of every 50ms.
 *   active  -- a transfer, a nudge, or any command in either direction. Zero
 *              latency, because inbound acks and REPLAY commands are precisely
 *              what latency delays, and a flush is a conversation.
 *
 * One rule drives both: anything interesting marks the link active and pushes
 * the return to idle out. That is why CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS is
 * off in prj.conf -- the stack's own one-shot update at five seconds would be a
 * second owner of this policy, and the two would disagree the first time a phone
 * connected and immediately asked for its backlog.
 *
 * The numbers are Apple's Accessory Design Guidelines, which are the binding
 * constraint here because a central is free to refuse anything it dislikes and
 * iOS is the strictest common one: latency <= 30, interval_max * (latency + 1)
 * <= 2s, that product * 3 < supervision timeout, and the timeout itself <= 6s.
 * 50ms * 31 = 1.55s, 1.55s * 3 = 4.65s < 6s. Zephyr's own validity check
 * (timeout * 4 > (1 + latency) * interval_max) also passes: 2400 > 1240.
 */
#define CONN_INTERVAL_MIN   24  /* 30ms, in 1.25ms units */
#define CONN_INTERVAL_MAX   40  /* 50ms */
#define CONN_LATENCY_IDLE   10
#define CONN_LATENCY_ACTIVE 0
#define CONN_TIMEOUT        600 /* 6s, in 10ms units */

/* How long the link stays awake after the last thing worth being awake for.
 * Has to comfortably exceed one idle listen period (1.55s) plus whatever the
 * phone takes to answer, or the ring would drop to idle in the gap between
 * sending an END and receiving the ACK it invites -- and pay the wake-up
 * latency on the reply it was waiting for.
 *
 * 15s rather than the 5s this started at, because 5s was shorter than the
 * update procedure it was supposed to outlast. The procedure is not free: the
 * connect path times it exactly, since connected() arms this timer and the
 * expiry is what asks for idle. Three connections in one capture, request to
 * grant: 1.23s, 1.23s, 2.03s -- and those are the *quiet* link. The active
 * regime is by definition asked for on a busy one, where the same procedure ran
 * to roughly 8s. A hold timed at 5s therefore expired before its own grant
 * arrived, the ring asked to go back to idle before it had finished going
 * active, and the central ran the two updates back to back: every active grant
 * in that capture was followed by an idle grant 300-990ms later. The active
 * regime existed, but never for longer than a second and never over the traffic
 * it was built for.
 *
 * The hold is also no longer timed from the request -- see le_param_updated(),
 * which restarts it when the grant actually lands. This constant only has to
 * cover the conversation after that point.
 */
#define CONN_ACTIVE_HOLD_MS 15000

/* How long a request may stay in flight before this side stops waiting on it.
 * A central may answer a request, narrow it, or ignore it outright, and the
 * third case produces no callback at all -- so the pending flag needs a
 * deadline, or one ignored request would freeze the regime for the life of the
 * connection. Above the slowest procedure observed (~8s, busy link): too long
 * costs a late regime change, too short costs the duplicate request the flag
 * exists to prevent.
 */
#define CONN_PARAM_PENDING_MS 10000

/* Re-check interval while a request is in flight. The grant normally arrives
 * first and le_param_updated() drives everything from there; this is only what
 * paces the wait when it does not, so a handful of wakeups per stuck update.
 */
#define CONN_PARAM_POLL_MS 1000

static const struct bt_le_conn_param conn_param_idle = BT_LE_CONN_PARAM_INIT(
    CONN_INTERVAL_MIN, CONN_INTERVAL_MAX, CONN_LATENCY_IDLE, CONN_TIMEOUT);
static const struct bt_le_conn_param conn_param_active = BT_LE_CONN_PARAM_INIT(
    CONN_INTERVAL_MIN, CONN_INTERVAL_MAX, CONN_LATENCY_ACTIVE, CONN_TIMEOUT);

/* The regime the link is actually in -- what the central granted, not what this
 * side asked for. It used to be the latter, and that was half the bug: a
 * request records an intention, and the guard below turned that intention into
 * a belief that suppressed the very re-request that would have corrected it. A
 * ring that had asked for the active regime believed it was at 30ms while the
 * link sat at 930ms for the whole of a history transfer.
 *
 * Written by le_param_updated() on the Bluetooth RX thread and read by the work
 * handler; see the note on races above conn_param_work_handler().
 */
static bool s_conn_active;

/* Set by any thread, cleared only by the handler. A lost write either way costs
 * one extra pass through the handler, never a wrong regime.
 */
static bool s_conn_want_active;

/* A request is out and its grant has not been seen yet. Exists to stop a second
 * update being stacked on the first: the central executes them in the order
 * they arrive regardless of which one this side wants by then, which is exactly
 * how an active request and the idle request that overtook it ended up landing
 * back to back.
 */
static bool s_conn_param_pending;

/* Uptime the pending request went out, for the CONN_PARAM_PENDING_MS deadline. */
static int64_t s_conn_param_sent;

static void conn_param_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(conn_param_work, conn_param_work_handler);

/* True while a request is still worth waiting for. Clears the flag itself once
 * the deadline passes, so an ignored request costs one delay rather than the
 * connection.
 */
static bool conn_param_in_flight(void)
{
    if (!s_conn_param_pending) {
        return false;
    }

    if ((k_uptime_get() - s_conn_param_sent) >= CONN_PARAM_PENDING_MS) {
        LOG_WRN("Connection parameter update never landed -- treating it as ignored");
        s_conn_param_pending = false;
        return false;
    }

    return true;
}

static void conn_params_apply(bool active)
{
    int rc;

    if (s_conn == NULL || s_conn_active == active) {
        return;
    }

    rc = bt_conn_le_param_update(s_conn, active ? &conn_param_active : &conn_param_idle);
    if (rc != 0) {
        /* The central is entitled to refuse, and a refused update is a slower
         * ring rather than a broken one -- so this is a warning, and the state
         * stays where it was so the next pass tries again.
         */
        LOG_WRN("Connection parameter update to %s refused (%d)", active ? "active" : "idle", rc);
        return;
    }

    /* Sent, not granted. s_conn_active deliberately stays where it is until
     * le_param_updated() says otherwise -- that callback is the only thing on
     * this side that knows what the link is really doing.
     */
    s_conn_param_pending = true;
    s_conn_param_sent = k_uptime_get();
}

/* Called from anywhere traffic worth staying awake for happens. Cheap: it sets
 * a flag and pokes a work item, so it is safe to call on every packet.
 */
static void conn_mark_active(void)
{
    s_conn_want_active = true;
    (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work, K_NO_WAIT);
}

/* The one writer of the regime request.
 *
 * Two threads now touch this state: this handler, and le_param_updated() on the
 * Bluetooth RX thread. The writes are single bools on a Cortex-M and each side
 * only ever loses a race by one pass -- the handler may request a regime the
 * callback has just granted (a no-op), or skip one the callback is about to
 * make stale (re-requested on the next pass). A mutex taken inside a stack
 * callback would be a worse trade than the extra pass, so there is none; what
 * makes that safe is that every path out of here either reschedules or has left
 * the link where it wanted it.
 *
 * The request stays deferred to a work item for the original reason:
 * conn_mark_active() is called from the Bluetooth RX thread (GATT callbacks)
 * and from main (the event nudge), and bt_conn_le_param_update() can block
 * waiting for a TX buffer. Blocking the stack's own RX thread is the hazard
 * this file already refuses for the replay walk.
 */
static void conn_param_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Wait for the outstanding request rather than stacking another on top of
     * it. Overtaking is what produced the active-then-idle flapping: the
     * central runs both procedures in the order they arrived, so the second
     * request does not replace the first, it follows it.
     */
    if (conn_param_in_flight()) {
        (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work,
                                          K_MSEC(CONN_PARAM_POLL_MS));
        return;
    }

    /* Raise now. The clock on the drop is started by the grant, not here --
     * le_param_updated() re-arms this work item when the active regime actually
     * lands. The reschedule below is only a backstop for a request that is
     * never answered, so the link cannot be left awake by a lost grant.
     */
    if (s_conn_want_active) {
        s_conn_want_active = false;
        conn_params_apply(true);
        (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work,
                                          K_MSEC(CONN_ACTIVE_HOLD_MS + CONN_PARAM_PENDING_MS));
        return;
    }

    /* A transfer in flight is a conversation even when the ring is the one
     * doing the talking: the phone acknowledges, and it may ask for more. Going
     * idle underneath it would put 1.55s on every one of those replies.
     */
    if (s_replay.active) {
        (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work,
                                          K_MSEC(CONN_ACTIVE_HOLD_MS));
        return;
    }

    conn_params_apply(false);

    /* Re-check instead of falling silent. That call may have found the request
     * refused, or raced a grant -- and in either case the link is still active
     * with nothing scheduled to notice. This is the only thing standing between
     * one lost update and a link that stays awake for the rest of the
     * connection; once idle is genuinely in force, s_conn_active goes false and
     * the polling stops.
     */
    if (s_conn != NULL && s_conn_active) {
        (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work,
                                          K_MSEC(CONN_ACTIVE_HOLD_MS));
    }
}

/* Packs one record into `out` little-endian. The single encoder for the vitals
 * format: the live notification and the replay stream both come through here,
 * so the two cannot disagree about what a record looks like on the air.
 *
 * Explicit puts rather than a memcpy of the struct. Little-endian happens to be
 * this SoC's byte order and the struct happens to be unpadded, so a memcpy
 * would work today and would silently become the compiler's decision rather
 * than the protocol's.
 */
static void pack_record(uint8_t *out, const struct flash_record *rec)
{
    sys_put_le32(rec->timestamp_ms, &out[0]);
    sys_put_le16(rec->bpm, &out[4]);
    sys_put_le16(rec->spo2_tenths, &out[6]);
    sys_put_le16(rec->confidence, &out[8]);
    sys_put_le16(rec->perfusion_milli, &out[10]);
    sys_put_le16(rec->steps, &out[12]);
    out[14] = rec->flags;
    out[15] = rec->repeat;
}

/* How many whole records fit in one notification on the current connection.
 *
 * bt_gatt_get_mtu() reports the negotiated ATT MTU; three bytes of it are the
 * notification header. The floor of one record matters: a client that never
 * negotiated a larger MTU still gets its data, just slowly, rather than a
 * transfer that cannot start.
 */
static uint16_t records_per_notification(struct bt_conn *conn)
{
    uint16_t mtu = (conn != NULL) ? bt_gatt_get_mtu(conn) : 0U;
    uint16_t payload = (mtu > 3U) ? (uint16_t)(mtu - 3U) : 0U;
    uint16_t n;

    /* One type byte, then whole records. */
    if (payload < 1U + FLASH_RECORD_SIZE) {
        return 1U;
    }

    n = (uint16_t)((payload - 1U) / FLASH_RECORD_SIZE);
    return (n > HISTORY_MAX_RECORDS) ? (uint16_t)HISTORY_MAX_RECORDS : n;
}

static int history_notify(const uint8_t *data, uint16_t len)
{
    /* attrs[9]/[10] are the history declaration and value -- see the table. */
    return bt_gatt_notify(NULL, &vitals_svc.attrs[10], data, len);
}

/* Walk state for one work-item slice.
 *
 * `next` is the offset of the first record **not yet delivered**, and it only
 * advances when a notification has actually been handed to the stack. Records
 * packed into the buffer but not sent are simply re-walked next slice. That is
 * what makes running out of radio buffers cost nothing but a delay.
 */
struct replay_ctx {
    uint8_t buf[1 + HISTORY_MAX_RECORDS * FLASH_RECORD_SIZE];
    uint16_t capacity; /* records this connection's MTU allows */
    uint16_t filled;   /* records packed and not yet sent */
    uint32_t next;
    bool blocked; /* radio out of buffers: retry the same records shortly */
    bool failed;  /* something else went wrong: abandon the transfer */
};

/* Sends whatever is packed. Returns false to stop the walk. */
static bool replay_flush_buffer(struct replay_ctx *ctx)
{
    int rc;

    if (ctx->filled == 0U) {
        return true;
    }

    ctx->buf[0] = HISTORY_TYPE_DATA;
    rc = history_notify(ctx->buf, (uint16_t)(1U + ctx->filled * FLASH_RECORD_SIZE));

    if (rc == -ENOMEM || rc == -EAGAIN) {
        /* Out of TX buffers -- expected under load, not an error. Leave `next`
         * where it is so these records are packed again next slice.
         */
        ctx->blocked = true;
        return false;
    }
    if (rc != 0) {
        /* -ENOTCONN is the client having gone away mid-transfer, which is
         * ordinary. Anything else is worth a line.
         */
        if (rc != -ENOTCONN) {
            LOG_WRN("History notify failed (%d)", rc);
        }
        ctx->failed = true;
        return false;
    }

    ctx->next += (uint32_t)ctx->filled * FLASH_RECORD_SIZE;
    ctx->filled = 0;
    return true;
}

static bool replay_one(uint32_t offset, const struct flash_record *rec, void *user_data)
{
    struct replay_ctx *ctx = user_data;

    ARG_UNUSED(offset);

    pack_record(&ctx->buf[1U + ctx->filled * FLASH_RECORD_SIZE], rec);
    ctx->filled++;

    if (ctx->filled >= ctx->capacity) {
        return replay_flush_buffer(ctx);
    }
    return true;
}

/* Ends the transfer, telling the client where to resume. That offset is what the
 * phone acknowledges, and the acknowledgement is what lets the ring reclaim
 * flash -- so this one frame is the hinge the whole storage contract turns on.
 *
 * Returns false when the radio had no buffer for it. **The transfer is then not
 * over** and the caller has to come back: this used to be
 *
 *     (void)history_notify(pkt, sizeof(pkt));
 *
 * which discarded exactly the error the DATA path above takes such care to
 * retry. The consequence was specific and silent. A bulk transfer runs the TX
 * pool dry -- CONFIG_BT_CONN_TX_MAX is 3 -- and the last DATA notification takes
 * the slot immediately before this one asks for another, so END is the frame
 * most likely of any to be refused and was the only frame with no retry. When it
 * was dropped the ring still cleared s_replay.active and still logged "transfer
 * complete", while the phone sat waiting for an END that never came. The phone
 * acknowledges *only* on END, so no END meant no acknowledgement, meant no
 * flash ever reclaimed, and the console on both sides showed a completed
 * transfer.
 *
 * Blocking here is not an error and gets no line: it is the same expected
 * back-pressure replay_flush_buffer() treats as ordinary. A client that vanishes
 * mid-END is likewise ordinary, and ends the transfer because there is nothing
 * left to end it for.
 */
static bool replay_end(uint32_t next_offset)
{
    uint8_t pkt[5];
    int rc;

    pkt[0] = HISTORY_TYPE_END;
    sys_put_le32(next_offset, &pkt[1]);
    rc = history_notify(pkt, sizeof(pkt));

    if (rc == -ENOMEM || rc == -EAGAIN) {
        return false;
    }

    s_replay.active = false;

    if (rc != 0) {
        if (rc != -ENOTCONN) {
            LOG_WRN("History END failed (%d) -- phone cannot acknowledge this transfer", rc);
        }
        return true;
    }

    LOG_INF("History transfer complete, delivered up to byte %u", next_offset);
    return true;
}

void ble_notify_event(void)
{
    uint8_t pkt[5];
    int rc;

    if (!history_notify_enabled) {
        /* Nothing subscribed. The event is still in the buffer and the phone
         * will collect it whenever it next connects -- later than we would
         * like, which is the architecture's known ceiling, not a bug here.
         */
        return;
    }

    /* Before the notification, not after: this is the one message the ring
     * sends that exists purely to provoke a reply, so the REPLAY it invites
     * should not be the packet that pays the wake-up latency.
     */
    conn_mark_active();

    pkt[0] = HISTORY_TYPE_EVENT;
    sys_put_le32(flash_store_cursor(), &pkt[1]);

    /* Not retried, but no longer silent. The phone asks for a backfill on every
     * connection anyway -- a client requests one as soon as it subscribes --
     * so a lost nudge costs promptness rather than data -- and promptness is
     * the entire point of the nudge, which is why a lost one earns a line
     * instead of being dropped without trace.
     *
     * This is the frame most exposed to it: a burst sends two notifications per
     * package, so the TX pool is at its emptiest exactly when this is asked
     * for.
     */
    rc = history_notify(pkt, sizeof(pkt));
    if (rc != 0) {
        LOG_WRN("Event nudge not sent (%d) -- phone will collect on its next connection", rc);
    }
}

void ble_notify_alert(uint8_t reason)
{
    uint8_t pkt[2];
    int rc;

    if (!history_notify_enabled) {
        /* Nobody is listening. Unlike an EVENT, there is no buffer that will
         * carry this later -- an alert is about *now* and a missed one is
         * simply missed. Worth a line, because "the phone was not subscribed"
         * and "the ring never noticed" look identical from the app.
         */
        LOG_WRN("Alert %u not sent -- no subscriber", reason);
        return;
    }

    conn_mark_active();

    pkt[0] = HISTORY_TYPE_ALERT;
    pkt[1] = reason;

    rc = history_notify(pkt, sizeof(pkt));
    if (rc != 0) {
        LOG_WRN("Alert %u not sent (%d)", reason, rc);
    }
}

static void replay_abandon(const char *why)
{
    s_replay.active = false;
    LOG_INF("History transfer abandoned: %s", why);
}

static void history_work_handler(struct k_work *work)
{
    struct replay_ctx ctx = {0};
    uint32_t slice_start;

    ARG_UNUSED(work);

    if (!s_replay.active) {
        return;
    }
    if (!history_notify_enabled || s_conn == NULL) {
        replay_abandon("client gone");
        return;
    }

    /* A flush between slices erases everything this transfer was walking, so
     * every offset it holds now points at an unrelated record. Checked every
     * slice rather than only at the start, because a transfer takes seconds and
     * a boot erase can land in the middle of one.
     */
    if (flash_store_generation() != s_replay.generation) {
        uint8_t pkt[5];

        pkt[0] = HISTORY_TYPE_STALE;
        sys_put_le32(flash_store_generation(), &pkt[1]);
        (void)history_notify(pkt, sizeof(pkt));
        replay_abandon("log was flushed mid-transfer");
        return;
    }

    /* The live cursor, not one snapshotted when the transfer started: records
     * appended during the transfer are worth delivering in it, and the walk
     * itself already refuses to read past a partially-written record.
     */
    if (s_replay.cursor >= flash_store_cursor()) {
        if (!replay_end(s_replay.cursor)) {
            /* Same back-pressure, same answer as a blocked DATA frame below.
             * Re-entering the handler re-runs the checks above it, so a client
             * that goes away while END is queued still abandons rather than
             * retrying into a dead link.
             */
            history_schedule(K_MSEC(10));
        }
        return;
    }

    ctx.capacity = records_per_notification(s_conn);
    ctx.next = s_replay.cursor;
    slice_start = s_replay.cursor;

    (void)flash_store_foreach(s_replay.cursor, replay_one, &ctx);

    /* A partial buffer at the end of the walk is a short final notification --
     * unless the walk stopped *because* a send failed, in which case sending
     * again here would duplicate the records that did go out.
     */
    if (!ctx.blocked && !ctx.failed) {
        (void)replay_flush_buffer(&ctx);
    }
    s_replay.cursor = ctx.next;

    if (ctx.failed) {
        replay_abandon("notification failed");
        return;
    }
    if (ctx.blocked) {
        /* Let the controller drain. 10ms is a few connection events at the
         * intervals this ring negotiates: long enough to be worth yielding for,
         * short enough that a full buffer still moves in seconds.
         */
        history_schedule(K_MSEC(10));
        return;
    }
    if (s_replay.cursor >= flash_store_cursor()) {
        if (!replay_end(s_replay.cursor)) {
            history_schedule(K_MSEC(10));
        }
        return;
    }
    if (s_replay.cursor == slice_start) {
        /* No progress and nothing blocked us: the walk is not advancing, which
         * should be impossible. Stopping beats rescheduling forever.
         */
        replay_abandon("walk made no progress");
        return;
    }

    /* More to send and the radio is keeping up. Back through the work queue
     * rather than looping here, so nothing else waits on this transfer.
     */
    history_schedule(K_NO_WAIT);
}

static ssize_t control_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    const uint8_t *cmd = buf;

    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    /* Any command at all, valid or not, means the phone is talking to us and
     * more is probably coming. Marked before the command is even parsed, so a
     * rejected write still holds the link awake for the corrected one.
     */
    conn_mark_active();

    /* A long write arriving in pieces would hand us half a command and no way
     * to know it. Commands are short enough to fit any negotiated MTU whole, so
     * the honest answer is to refuse the offset rather than reassemble.
     */
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    switch (cmd[0]) {
    case CONTROL_OP_SET_TIME:
        if (len != CONTROL_SET_TIME_LEN) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        /* Little-endian, matching every other multi-byte field on this service.
         * sys_get_le64 rather than a cast: the payload is not aligned and the
         * layout is a protocol, not the compiler's choice.
         */
        wallclock_set(sys_get_le64(&cmd[1]));
        return len;

    case CONTROL_OP_REPLAY: {
        uint32_t generation;
        uint32_t from;
        uint8_t hdr[13];

        if (len != CONTROL_REPLAY_LEN) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        if (!history_notify_enabled) {
            /* Nowhere to send it. Refusing is better than starting a transfer
             * whose notifications go nowhere and whose END never arrives.
             */
            return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
        }

        generation = sys_get_le32(&cmd[1]);
        from = sys_get_le32(&cmd[5]);

        /* A stale generation is the common case after a flush, and answering it
         * with data would be answering a question the client did not ask: its
         * offset refers to records that no longer exist. Tell it, and let it
         * ask again from zero.
         */
        if (generation != flash_store_generation()) {
            uint8_t stale[5];

            stale[0] = HISTORY_TYPE_STALE;
            sys_put_le32(flash_store_generation(), &stale[1]);
            (void)history_notify(stale, sizeof(stale));
            return len;
        }
        /* An offset past the end, or one not on a record boundary, is a client
         * bug or a corrupted cursor. Both are better restarted than guessed at.
         */
        if (from > flash_store_cursor() || (from % FLASH_RECORD_SIZE) != 0U) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        /* An offset *below* the oldest surviving record is not an error. It is a
         * phone that was away while the buffer lapped, which is an ordinary
         * thing to happen and the exact case this whole path exists for. Clamp
         * forward and report where the data really starts -- the client sees a
         * higher `from` than it asked for and knows it lost that stretch.
         */
        if (from < flash_store_head()) {
            LOG_INF("Replay asked for byte %u, log starts at %u -- %u byte(s) already gone", from,
                    flash_store_head(), flash_store_head() - from);
            from = flash_store_head();
        }

        /* The phone is collecting under its own steam, so the subscribe-time
         * offer has nothing left to do. Set here rather than on a completed
         * transfer: what the offer exists to detect is a client that never
         * asks, and one that asked and then failed mid-transfer will ask again
         * itself -- it knows what it did not receive, and the ring does not.
         */
        client_asked_since_subscribe = true;

        s_replay.cursor = from;
        s_replay.generation = generation;
        s_replay.active = true;

        hdr[0] = HISTORY_TYPE_HEADER;
        sys_put_le32(generation, &hdr[1]);
        sys_put_le32(from, &hdr[5]);
        sys_put_le32(flash_store_cursor(), &hdr[9]);
        (void)history_notify(hdr, sizeof(hdr));

        /* The walk itself runs on the work queue, not here: this callback is on
         * the Bluetooth RX thread and a full buffer is 14592 records.
         */
        history_schedule(K_NO_WAIT);
        return len;
    }

    case CONTROL_OP_ACK: {
        uint32_t generation;
        uint32_t upto;
        int rc;

        if (len != CONTROL_ACK_LEN) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }

        generation = sys_get_le32(&cmd[1]);
        upto = sys_get_le32(&cmd[5]);

        /* A stale generation here is worse than a stale one on REPLAY: acting
         * on it would erase pages against an offset that means nothing.
         *
         * Logged, because refusing in silence is how this reads from the ring's
         * console as "the phone never acknowledged" when what actually happened
         * is "the phone acknowledged and was turned away". Those are opposite
         * problems -- one is a transport fault, one is a stale cursor the client
         * recovers from by replaying -- and they were indistinguishable here.
         */
        if (generation != flash_store_generation()) {
            LOG_WRN("Acknowledgement refused: generation %u, log is on %u", generation,
                    flash_store_generation());
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        rc = flash_store_release(upto);
        if (rc != 0) {
            LOG_WRN("Acknowledgement of byte %u refused (%d)", upto, rc);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        /* "still holds", not "now holds", and the difference matters when
         * reading a console back. This runs on the Bluetooth RX thread, which
         * records the acknowledgement and nothing else -- the head does not
         * move until the measurement thread's next flash_store_service() (see
         * the ownership note in flash_store.c). So the figure printed here is
         * always the *pre-reclaim* one, and on a page-sized transfer it reads
         * as though nothing was freed: 4160 bytes acknowledged, 4160 bytes
         * still held, and only on the next cycle does the head jump a page.
         * Saying "now holds" invited exactly that misreading.
         */
        LOG_INF("Phone acknowledged up to byte %u; log still holds %u byte(s) until the next cycle",
                upto, flash_store_cursor() - flash_store_head());
        return len;
    }

    default:
        LOG_WRN("Unknown control opcode 0x%02x", cmd[0]);
        /* A client sending an opcode this firmware does not have is a version
         * mismatch, and telling it so is what lets it fall back. Silently
         * accepting would let it believe a command took effect.
         */
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
}

static ssize_t control_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset)
{
    uint8_t out[CONTROL_READ_LEN];
    uint64_t anchor = 0;
    bool anchored = wallclock_anchor(&anchor);

    ARG_UNUSED(conn);

    /* A client reads this to find out what to ask for, so a REPLAY is usually
     * the very next thing on the link.
     */
    conn_mark_active();

    out[0] = (uint8_t)((anchored ? CONTROL_FLAG_ANCHORED : 0) |
                       (boot_reset_cause_valid ? CONTROL_FLAG_RESET_CAUSE : 0) |
                       (boot_went_flat ? CONTROL_FLAG_WENT_FLAT : 0));
    sys_put_le64(anchor, &out[1]);
    sys_put_le32((uint32_t)k_uptime_get(), &out[9]);
    sys_put_le32(flash_store_generation(), &out[13]);
    sys_put_le32(flash_store_cursor(), &out[17]);

    /* Sent on every read rather than once, and it does not expire. A reset is
     * the event that *ends* a connection, so the read that follows one is on a
     * fresh link with no memory of what came before; a phone that reconnects
     * three times inside a minute -- which is what the 08-21 capture shows a
     * ring doing while it is struggling -- gets the same answer each time and
     * can tell from the uptime beside it whether it is looking at a new boot.
     */
    sys_put_le32(boot_reset_cause, &out[21]);
    sys_put_le16(boot_records_destroyed, &out[25]);

    /* The flat report, on the same terms as the reset cause above: sent on every
     * read, never expiring, because the connection that would have carried it at
     * the time is precisely the one a flat cell prevented from existing.
     *
     * The epoch is 0 when no phone had set the ring's clock during the run that
     * died, which is a real and unremarkable case -- the percentage and the flag
     * still stand, and the phone knows when it last heard from the ring, which
     * brackets the event well enough to act on.
     */
    out[27] = boot_flat_percent;
    sys_put_le16(boot_flat_mv, &out[28]);
    sys_put_le64(boot_flat_epoch_ms, &out[30]);

    /* Truncated to uint32 exactly as a record's timestamp is, so the client is
     * comparing like with like when it resolves a wrap.
     */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, out, sizeof(out));
}

/* attrs[0] primary service,
 * [1] vitals characteristic declaration, [2] vitals value, [3] vitals CCC,
 * [4] motion characteristic declaration, [5] motion value, [6] motion CCC,
 * [7] control characteristic declaration, [8] control value,
 * [9] history characteristic declaration, [10] history value, [11] history CCC.
 *
 * The value attributes -- [2] and [5] -- are what bt_gatt_notify targets. Those
 * indices are positional and silently wrong if a declaration is inserted above
 * them, which is the one hazard of extending this table. The control
 * characteristic was therefore **appended**, not slotted in next to the service
 * declaration where it might read more naturally: putting it anywhere above
 * motion would have moved [5] and broken the motion stream in a way that
 * compiles cleanly and fails only on the air.
 *
 * Motion is a second characteristic rather than extra fields on the vitals
 * package, deliberately. That package is byte-for-byte the record flash_store.c
 * writes (see flash_store.c), so widening it would either desynchronise the two
 * formats or force a flash layout change for a value that is not stored. A
 * separate characteristic keeps "what the log holds" and "what the radio can
 * offer" free to differ.
 */
/* The CCC descriptors require an encrypted link to read or write, not the plain
 * READ|WRITE they used to. A notify-only characteristic carries no readable
 * value, so subscribing -- the CCC write -- is the only door to the stream, and
 * gating it behind encryption is what keeps a wearer's vitals from any scanner
 * in range. A central writing the CCC over an unencrypted link gets an
 * insufficient-encryption error, which prompts it to pair (and, being bondable,
 * bond) before the subscription takes. See CONFIG_BT_SMP in prj.conf.
 */
BT_GATT_SERVICE_DEFINE(vitals_svc,
    BT_GATT_PRIMARY_SERVICE(&vitals_svc_uuid),
    BT_GATT_CHARACTERISTIC(&vitals_pkt_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(vitals_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&motion_pkt_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(motion_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    /* Encrypted both ways, for the same reason the CCCs are. The read leaks
     * when the wearer's ring last booted, which is minor; the *write* sets the
     * clock every stored reading is dated by, and letting any scanner in range
     * move it would corrupt the record silently and at a distance.
     */
    BT_GATT_CHARACTERISTIC(&control_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           control_read, control_write, NULL),
    /* The buffered log, replayed on request. A separate characteristic from the
     * live vitals stream even though both carry the same records: the live one
     * is one record per notification and means "now", this one is batched and
     * framed and means "here is what you missed". Merging them would make a
     * client infer which it was holding from the payload length.
     */
    BT_GATT_CHARACTERISTIC(&history_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(history_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    /* Appended, and it has to stay appended. The notify helpers address their
     * characteristics by index into this table -- attrs[2] vitals, attrs[5]
     * motion, attrs[10] history -- so inserting anything above this point
     * renumbers them and every notification starts going out on the wrong
     * characteristic. Adding at the end is the one edit that cannot do that.
     */
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(status_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
);

/* Name goes in the scan response; the flags and the two 16-bit standard-service
 * UUIDs go in the advertising packet so a scanner filtering on them finds us.
 * The custom 128-bit UUID is left out -- it would crowd the 31-byte payload and
 * a client that wants it already knows to look.
 */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL,
                  BT_UUID_16_ENCODE(BT_UUID_HRS_VAL),
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

/* The GAP "slow" connectable preset: 1.0-1.2s between advertising events,
 * against the 100-150ms of BT_LE_ADV_CONN_FAST_2 this used to use.
 *
 * The ring advertises whenever it is not connected, which on a wearable means
 * essentially always -- so this interval is not a discovery-latency knob, it is
 * a continuous load. At 100-150ms it cost ~130uA, a fifth of the whole power
 * budget, spent looking eager to a phone that is usually not scanning. At 1s it
 * is ~20uA. See ARCHITECTURE.md 5.1.1.
 *
 * What it costs is time-to-discover: a scanning client now takes on the order
 * of a second to see the ring rather than a tenth of one. Nothing about the
 * connection itself is slower once established, and measuring never waits on a
 * connection in the first place.
 */
#define ADV_PARAMS                                                                                 \
    BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_SLOW_INT_MIN, BT_GAP_ADV_SLOW_INT_MAX, NULL)

static void start_advertising(void)
{
    int rc = bt_le_adv_start(ADV_PARAMS, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

    if (rc == -EALREADY) {
        return; /* already advertising -- nothing to do */
    }
    if (rc != 0) {
        LOG_ERR("Advertising failed to start (%d)", rc);
        return;
    }

    LOG_INF("Advertising as \"%s\"", DEVICE_NAME);
}

/* Restart from a work item rather than straight out of the disconnected
 * callback: it keeps the radio work off the connection-teardown context.
 */
static void adv_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    start_advertising();
}

static K_WORK_DEFINE(adv_work, adv_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err != 0U) {
        LOG_ERR("BLE connection failed (0x%02x)", err);
        return;
    }

    /* Whether we already hold keys for this peer is the difference between a
     * silent auto-reconnect and a pairing prompt on the phone, so it is the
     * first thing worth knowing from a log. bt_conn_get_dst() gives the
     * identity address: a central using a resolvable private address has
     * already been resolved against the stored IRK by the time we get here.
     */
    LOG_INF("BLE connected (%s peer)",
            bt_le_bond_exists(BT_ID_DEFAULT, bt_conn_get_dst(conn)) ? "bonded" : "new");

    /* Held for the replay path, which runs from a work item and needs the
     * connection to ask what MTU was negotiated. One connection at a time, so
     * one reference.
     */
    if (s_conn == NULL) {
        s_conn = bt_conn_ref(conn);
    }

    /* A new link arrives on whatever the central chose, which is invariably
     * fast and zero-latency -- so the active regime is already in force and
     * asking for it again would be an LL procedure that changes nothing. Record
     * it and arm the timer instead.
     *
     * Staying active for the first few seconds is not incidental. Service
     * discovery, the MTU exchange and pairing all happen now, all of them
     * inbound-first, and they are the traffic slave latency punishes hardest.
     * Each of them re-arms the timer through the callbacks above, so the link
     * settles to idle only once the phone has genuinely finished.
     */
    s_conn_active = true;
    s_conn_want_active = false;
    s_conn_param_pending = false;
    (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work,
                                      K_MSEC(CONN_ACTIVE_HOLD_MS));

    /* Armed here, cancelled by security_changed() or by the peer leaving. A
     * client that does neither is holding the ring's only slot for nothing.
     */
    security_deadline_arm();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (0x%02x)", reason);

    /* Any transfer in flight is over -- its notifications have nowhere to go
     * and its offsets belong to a client that will re-ask on reconnect.
     */
    if (s_replay.active) {
        s_replay.active = false;
        (void)k_work_cancel_delayable(&history_work);
        LOG_INF("History transfer abandoned: peer disconnected");
    }

    /* Before the unref below, not after: the deadline handler reads s_conn, so
     * cancelling first is what keeps it from reading a reference this callback
     * has already dropped.
     */
    security_deadline_cancel();

    if (s_conn != NULL) {
        bt_conn_unref(s_conn);
        s_conn = NULL;
    }

    /* Same reasoning for the subscribe-time offer: it belongs to a client that
     * is gone, and the next one arms its own when it subscribes. Left standing
     * it would fire into a dead link at best, and at worst offer the *next*
     * client's grace period away.
     */
    offer_cancel();
    client_asked_since_subscribe = false;

    /* The regime belongs to the connection, not to the ring. Cleared rather
     * than left standing so the next client is not judged against the last
     * one's state -- and the timer cancelled because there is nothing left for
     * it to put to sleep.
     */
    (void)k_work_cancel_delayable(&conn_param_work);
    s_conn_active = false;
    s_conn_want_active = false;
    s_conn_param_pending = false;

    /* One connection at a time; go back to advertising for the next client. */
    k_work_submit(&adv_work);
}

/* Encryption is what makes the subscription go through, so log where it lands:
 * RTT is the only window on this board, and "did the phone actually pair?" is
 * the first question when a client sees no notifications. Level 2 (encrypted,
 * unauthenticated) is the Just Works ceiling with no I/O to confirm a passkey.
 */
static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    ARG_UNUSED(conn);

    if (err != 0) {
        LOG_WRN("BLE security change failed (level %u, err %d)", level, err);
        return;
    }
    LOG_INF("BLE security level %u", level);

    /* What the deadline was waiting for. Level 2 is the Just Works ceiling and
     * the level every attribute on this service is gated at, so it is the bar
     * here too -- asking for more would hang up on the only kind of link this
     * hardware can form.
     */
    if (level >= BT_SECURITY_L2) {
        security_deadline_cancel();
    }
}

/* What the central actually granted, which is the only half of this that is
 * real. The ring asks; iOS and Android both narrow or refuse requests they
 * dislike, and a refusal is invisible from the request side. Logged with the
 * effective listen period worked out, because that -- not the latency number --
 * is the thing the power budget is made of, and "latency 30" on its own tells
 * you nothing without the interval beside it.
 */
static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout)
{
    ARG_UNUSED(conn);

    LOG_INF("Connection parameters: interval %u.%02u ms, latency %u (listens every %u ms), "
            "timeout %u ms",
            (interval * 5U) / 4U, ((interval * 5U) % 4U) * 25U, latency,
            ((1U + latency) * interval * 5U) / 4U, timeout * 10U);

    /* This is where the regime is decided, because this is the only place that
     * knows it. A central may narrow a request, refuse it, or move the
     * parameters on its own initiative with nothing having been asked at all --
     * so the granted latency is the state, and anything that is not the active
     * latency is idle by definition.
     */
    s_conn_param_pending = false;
    s_conn_active = (latency == CONN_LATENCY_ACTIVE);

    /* Start the hold clock here rather than at the request. Timed from the
     * request it routinely expired mid-procedure and asked for idle before the
     * active grant had landed -- see the CONN_ACTIVE_HOLD_MS note. Timed from
     * the grant, the link gets the full hold in the regime it was actually put
     * into.
     *
     * Only on the way up: landing idle is the resting state, and there is
     * nothing left for the timer to put to sleep. Traffic arriving in the
     * meantime re-arms it through conn_mark_active() regardless.
     */
    if (s_conn_active) {
        (void)k_work_reschedule_for_queue(&history_workq, &conn_param_work,
                                          K_MSEC(CONN_ACTIVE_HOLD_MS));
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
    .le_param_updated = le_param_updated,
};

/* Pairing outcome, which is not the same thing as the encryption result
 * security_changed() reports. A link can come up encrypted from a pairing that
 * distributed no keys, or whose keys never reached flash -- and that is exactly
 * the failure that makes a phone re-prompt on every reconnect instead of
 * connecting silently. `bonded` is the bit that says the keys were stored, so
 * it is worth its own line.
 */
static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    ARG_UNUSED(conn);
    LOG_INF("BLE pairing complete (%s)", bonded ? "bonded" : "no bond stored");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    ARG_UNUSED(conn);
    LOG_WRN("BLE pairing failed (err %d)", reason);
}

/* The phone unpairing is the one event that ends auto-reconnect, and it leaves
 * no other trace on this side -- the ring just stops being connected to and has
 * no way to say why.
 */
static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
    ARG_UNUSED(id);
    ARG_UNUSED(peer);
    LOG_INF("BLE bond deleted");
}

/* ---- who is allowed to pair -----------------------------------------------
 *
 * Before this, anyone was. The ring is bondable, advertises continuously, and
 * has no I/O to confirm a passkey with, so pairing was Just Works accepted from
 * whoever asked whenever they asked. Every attribute on the custom service is
 * guarded with *_ENCRYPT, which a Just Works link satisfies -- so the gate was
 * real and the lock on it opened for anyone in range: the whole buffered log
 * readable with REPLAY, erasable with ACK, and the clock every record is dated
 * by settable with SET_TIME.
 *
 * The usual answer -- pair only while the wearer holds a button down -- is not
 * available here. This board has no button and no charger-detect line either:
 * the PMIC interrupt is named in the DTS and nothing drives it, and battery.c
 * guesses at charging from a voltage rise precisely because nothing tells it.
 *
 * So the gate is the power cycle, which on a ring with no button is the one
 * physical act a wearer can perform and a stranger in range cannot:
 *
 *   - No bond at all             -> accept. The ring has to be pairable out of
 *                                   the box, and until a phone has claimed it
 *                                   there is nothing here to protect.
 *   - Otherwise, window open     -> accept.
 *   - Otherwise                  -> refuse.
 *
 * **This keeps the case the ALLOW_UNAUTH_OVERWRITE comment in prj.conf is
 * about.** A phone that was factory reset, or had the bond cleared in Settings,
 * opens a fresh Just Works pairing that the ring must be able to accept or the
 * wearer's ring is unpairable forever -- and it still can, after a power cycle,
 * without anyone opening the device to erase flash. What it no longer accepts
 * is that same pairing arriving unprompted from a stranger, which is what made
 * the overwrite worth worrying about in the first place. The option stays on;
 * this window is what makes it safe to leave on.
 *
 * ## Why a power-on boot and not any boot
 *
 * Because this ring resets on its own. The 08-20/21 wear test recorded eight
 * unrequested resets in sixteen hours, and a window opened by every boot would
 * be a window opened eight times a day by nobody.
 *
 * RESETREAS separates the two, and it is already read and already carried in
 * this file for the control read. A lockup, a watchdog bite or a soft reset
 * each latch a bit; a supply that went away latches nothing, because this part
 * has no bit for power-on or for brownout. So a cause of exactly zero is the
 * power cycle -- the wearer taking the ring off and putting it on the charger
 * -- and any named cause is the ring falling over, which opens nothing.
 *
 * !! **A brownout reads as zero too.** A cell that sags far enough to reset the
 * SoC on a worn finger opens a window the wearer did not ask for. That is the
 * residual risk, it is the same ambiguity log_reset_cause() documents at length
 * in main.c, and it is two minutes rather than permanent.
 *
 * An *unreadable* cause opens the window. hwinfo failing is a firmware gap that
 * says nothing about the boot, and the alternative -- a ring that can never be
 * re-paired because a register read failed -- is precisely the failure this
 * gate exists to avoid. Same degraded-not-broken posture every optional
 * subsystem in this tree takes.
 */
#define PAIRING_WINDOW_MS 120000

/* Uptime past which the window is shut. Zero means it never opened at all,
 * which is a different thing from having expired and is worth keeping apart.
 */
static int64_t s_pairing_window_until;

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
    ARG_UNUSED(info);
    (*(uint8_t *)user_data)++;
}

static uint8_t bond_count(void)
{
    uint8_t bonds = 0;

    bt_foreach_bond(BT_ID_DEFAULT, count_bond, &bonds);
    return bonds;
}

/* Called once from ble_init(), after the boot report has been handed down and
 * after settings_load() has restored whatever bonds survived.
 */
static void pairing_window_open(void)
{
    if (boot_reset_cause_valid && boot_reset_cause != 0U) {
        LOG_INF("Pairing window stays shut: this boot was a reset (RESETREAS 0x%08x), "
                "not a power-on",
                boot_reset_cause);
        return;
    }
    if (!boot_reset_cause_valid) {
        LOG_WRN("Reset cause unreadable -- opening the pairing window rather than "
                "leaving the ring unpairable");
    }

    s_pairing_window_until = k_uptime_get() + PAIRING_WINDOW_MS;
    LOG_INF("Pairing window open for %u second(s)", PAIRING_WINDOW_MS / 1000U);
}

static bool pairing_window_is_open(void)
{
    return s_pairing_window_until != 0 && k_uptime_get() < s_pairing_window_until;
}

static enum bt_security_err pairing_accept(struct bt_conn *conn,
                                           const struct bt_conn_pairing_feat *const feat)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(feat);

    if (bond_count() == 0U) {
        LOG_INF("Pairing accepted: the ring holds no bond yet");
        return BT_SECURITY_ERR_SUCCESS;
    }
    if (pairing_window_is_open()) {
        LOG_INF("Pairing accepted: inside the power-on window");
        return BT_SECURITY_ERR_SUCCESS;
    }

    /* Deliberately the same answer whether or not this peer's address is the
     * bonded one. Answering differently would tell a stranger which address is
     * worth spoofing, and spoofing it is exactly what ALLOW_UNAUTH_OVERWRITE
     * would then let them cash in.
     */
    LOG_WRN("Pairing refused: already bonded and the window is shut. "
            "Power-cycle the ring to pair a different phone.");
    return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

static struct bt_conn_auth_cb auth_callbacks = {
    .pairing_accept = pairing_accept,
};

static struct bt_conn_auth_info_cb auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
    .bond_deleted = bond_deleted,
};

int ble_init(void)
{
    int rc = bt_enable(NULL);

    if (rc != 0) {
        LOG_ERR("bt_enable failed (%d)", rc);
        return rc;
    }

    LOG_INF("Bluetooth initialised");

    /* Started before anything can connect, so a replay request never arrives
     * for a queue that does not exist yet.
     */
    k_work_queue_start(&history_workq, history_stack, K_THREAD_STACK_SIZEOF(history_stack),
                       HISTORY_THREAD_PRIO, NULL);
    k_thread_name_set(&history_workq.thread, "history");

    rc = bt_conn_auth_info_cb_register(&auth_info_callbacks);
    if (rc != 0) {
        LOG_WRN("Pairing callbacks not registered (%d)", rc);
    }

    /* The gate itself, and the one registration here whose failure is not
     * cosmetic: without it every pairing is accepted again, which is the state
     * this file spent its comments describing. Logged at ERR for that reason
     * even though ble_init() carries on -- the two ways this call can fail are
     * a second registration and a callback set this struct does not have, so a
     * failure here means something changed in the host, not in the field.
     */
    rc = bt_conn_auth_cb_register(&auth_callbacks);
    if (rc != 0) {
        LOG_ERR("Pairing gate NOT installed (%d) -- any central in range can pair", rc);
    }

    /* Restore persisted bonds before advertising, so a phone that bonded on a
     * previous power cycle re-encrypts silently instead of being asked to pair
     * again. bt_enable() does not do this itself when CONFIG_BT_SETTINGS is set;
     * the app owns the settings load.
     *
     * This is also what restores the identity address. Without it the ring
     * comes up on a fresh random static address every boot, and a phone holding
     * a standing "connect to this address when you see it" request -- which is
     * how both Android and iOS auto-reconnect -- would be waiting for a peer
     * that no longer exists.
     */
    if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
        rc = settings_load();
        if (rc != 0) {
            LOG_WRN("settings_load failed (%d); bonds may not persist", rc);
        }
    }

    /* "Will the phone reconnect on its own?" is answerable at boot, before any
     * connection: it will if a bond survived. Say so, because the alternative
     * -- a silently empty key store after a settings partition change or a
     * flash erase -- looks exactly like a working ring until a phone tries.
     */
    LOG_INF("%u bond(s) restored", bond_count());

    /* After settings_load(), so "no bond yet" in pairing_accept() means the key
     * store is genuinely empty rather than not read back yet.
     */
    pairing_window_open();

    start_advertising();
    return 0;
}

void ble_notify_vitals(const struct flash_sample *sample)
{
    uint8_t pkt[16];

    if (sample == NULL) {
        return;
    }

    /* Built into a record and then packed by the same two functions the replay
     * path uses, rather than assembled by hand here. That is the whole point:
     * a live notification and a replayed one are the same 16 bytes because they
     * come out of the same encoder, not because two pieces of code were written
     * to agree and are expected to stay that way.
     *
     * The two trailing bytes are the record's flags and repeat count. Only the
     * contact bit can be set here -- a refused window never reaches this
     * function -- so a live notification is always an accepted reading covering
     * one window. They are sent anyway rather than trimmed, because a format
     * that changes length depending on the value of a field is a format every
     * future parser gets wrong once.
     */
    struct flash_record rec;

    flash_record_from_sample(&rec, sample);
    pack_record(pkt, &rec);

    /* Standard Heart Rate service. Skip the "no pulse" windows: an HR client
     * expects a real beat, not a zero. SpO2 has no equivalent standard service,
     * which is half the reason the custom one below exists.
     */
    if (sample->bpm != 0U) {
        (void)bt_hrs_notify(sample->bpm);
    }

    if (vitals_notify_enabled) {
        int rc = bt_gatt_notify(NULL, &vitals_svc.attrs[2], pkt, sizeof(pkt));

        if (rc != 0 && rc != -ENOTCONN) {
            LOG_WRN("Vitals notify failed (%d)", rc);
        }
    }
}

void ble_notify_motion(uint32_t timestamp_ms, int16_t x, int16_t y, int16_t z, uint32_t steps)
{
    uint8_t pkt[14];

    /* Nothing is subscribed, so there is nothing to pack. Checked before the
     * work rather than after, unlike the vitals path -- there the packing is
     * shared with the Heart Rate service, here it would be pure waste.
     */
    if (!motion_notify_enabled) {
        return;
    }

    /* Little-endian on the wire, matching the vitals package. The axes are
     * signed milli-g, so a client reads them as int16 and needs no scale factor
     * -- the range setting stays a firmware detail rather than leaking into the
     * protocol as a divisor the client has to know.
     */
    sys_put_le32(timestamp_ms, &pkt[0]);
    sys_put_le16((uint16_t)x, &pkt[4]);
    sys_put_le16((uint16_t)y, &pkt[6]);
    sys_put_le16((uint16_t)z, &pkt[8]);

    /* Cumulative since boot, not steps-since-last-package. A client wanting a
     * rate differences two notifications, which stays correct across a dropped
     * one -- a delta on the wire would not. See imu.h.
     */
    sys_put_le32(steps, &pkt[10]);

    int rc = bt_gatt_notify(NULL, &vitals_svc.attrs[5], pkt, sizeof(pkt));

    if (rc != 0 && rc != -ENOTCONN) {
        LOG_WRN("Motion notify failed (%d)", rc);
    }
}

void ble_set_battery(uint8_t percent)
{
    (void)bt_bas_set_battery_level(percent);
}

/* Cell level and step total in one packet, sent on the battery's cadence.
 *
 * **This is the only live packet the shipping build sends.** BLE_LIVE_STREAM is
 * 0, so the vitals and motion notifications are compiled out and everything a
 * client learns about a *reading* arrives through the history replay. The
 * battery was already exempt from that -- power_check() calls ble_set_battery()
 * unconditionally, because a phone that cannot see the cell draining until the
 * next flush is a phone that cannot warn anyone -- and the step total is exempt
 * for the same reason and on the same cadence.
 *
 * A running total, not a delta. Same argument as the motion package made: a
 * client wanting a rate differences two notifications, which stays correct
 * across a dropped one where a delta would silently lose those steps. It is
 * also the same number flash_store's battery package logs to RTT, so a capture
 * and a phone can be checked against each other directly.
 *
 * The per-record deltas in the history stream remain the authoritative record;
 * this is a live view, and a client that has both should trust the records.
 */
void ble_set_boot_report(uint32_t reset_cause, bool cause_valid, uint32_t records_destroyed)
{
    boot_reset_cause = reset_cause;
    boot_reset_cause_valid = cause_valid;

    /* The count is a uint16 on the wire because the partition cannot hold more
     * than 14848 records, and saturating is a truer answer than wrapping if
     * that ever stops being true.
     */
    boot_records_destroyed =
        (records_destroyed > UINT16_MAX) ? UINT16_MAX : (uint16_t)records_destroyed;
}

void ble_set_flat_report(bool went_flat, uint8_t percent, uint16_t millivolts,
                         uint64_t epoch_ms)
{
    boot_went_flat = went_flat;
    boot_flat_percent = percent;
    boot_flat_mv = millivolts;
    boot_flat_epoch_ms = epoch_ms;
}

void ble_notify_status(uint32_t timestamp_ms, uint8_t percent, uint16_t millivolts,
                       uint32_t steps)
{
    uint8_t pkt[11];

    if (!status_notify_enabled) {
        return;
    }

    sys_put_le32(timestamp_ms, &pkt[0]);
    pkt[4] = percent;
    sys_put_le16(millivolts, &pkt[5]);
    sys_put_le32(steps, &pkt[7]);

    int rc = bt_gatt_notify(NULL, &vitals_svc.attrs[13], pkt, sizeof(pkt));

    if (rc != 0 && rc != -ENOTCONN) {
        LOG_WRN("Status notify failed (%d)", rc);
    }
}
