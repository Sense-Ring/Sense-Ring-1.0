// The ring session: connection, protocol, decoding, storage.
//
// This used to live inside the home page's State, which tied the entire data
// path to a widget -- and therefore to the Activity. It became a singleton so
// monitoring could keep running with no UI at all: after the app is swiped
// away, and after a phone reboot, where the process is started by a broadcast
// receiver and no Activity is ever created. Since 2026-08-07 it is one
// *instance per ring*, owned by RingFleet, for the same reason it stopped being
// a widget -- the number of rings is not the connection's business. Nothing in
// this file touches BuildContext, and none of it assumes a screen exists.
//
// Scanning is a one-time affair. After the first bond the app remembers the
// ring's address and, from then on, hands the platform a standing auto-connect
// request instead: the phone reconnects on its own whenever the ring is in
// range -- at app launch, after the ring wanders off and comes back, after it
// resets, after Bluetooth is toggled -- with no scan, no tap, and no pairing
// prompt, because the bond lets the link re-encrypt silently. See
// `_armAutoConnect`.

import 'dart:async';

import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart' show WidgetsBinding, AppLifecycleState;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'history_db.dart';

// ---- Ring GATT contract (mirrors src/ble.c on the firmware) -----------------
const _deviceName = 'SenseRing';

// Custom "SenseRing Vitals" service, its package characteristic, and the
// control characteristic the ring takes commands on.
//
// The ring also serves a motion characteristic at `f1a00003-...`, carrying
// {u32 ts, i16 x, i16 y, i16 z, u32 steps}. This app does not subscribe to it:
// it is a live stream, the firmware compiles it out by default
// (`BLE_LIVE_STREAM`), and everything shown here comes from the buffered log
// instead -- which is the path that survives the phone being out of range. A
// fork that wants raw acceleration adds the UUID to [_labelFor] and a case to
// [_onData].
final _uuidVitals = Guid('f1a00002-9c1b-4d3e-a7b2-5e8c6d9f0a11'); // 16 bytes
final _uuidControl = Guid('f1a00004-9c1b-4d3e-a7b2-5e8c6d9f0a11');
final _uuidHistory = Guid('f1a00005-9c1b-4d3e-a7b2-5e8c6d9f0a11');
// {u32 uptime_ms, u8 percent, u16 mV, u32 steps} -- 11 bytes. Read for the cell
// level only; the step total it also carries is ignored, because the
// authoritative step figure is the per-record delta in the history stream.
final _uuidStatus = Guid('f1a00006-9c1b-4d3e-a7b2-5e8c6d9f0a11'); // 11 bytes

// Control opcodes. `{u8 opcode, payload...}` -- see ble.c.
const _opSetTime = 0x01;
const _opReplay = 0x02;
const _opAck = 0x03;

// History notification framing. Every notification on the history
// characteristic leads with one of these type bytes.
const _histHeader = 0x01; // {u8, u32 generation, u32 from, u32 end}
const _histData = 0x02; // {u8, record[1..3]}  -- 16 bytes each
const _histEnd = 0x03; // {u8, u32 next_offset}
const _histStale = 0x04; // {u8, u32 generation} -- our cursor is dead
const _histEvent = 0x05; // {u8, u32 cursor} -- something happened, collect now
const _histAlert = 0x06; // {u8, u8 reason} -- tell the wearer, now

// Alert reasons. Mirrors BLE_ALERT_* in the firmware's ble.h.
//
// EVENT and ALERT ask for different things and that is why they are separate
// types: EVENT means "collect the buffer", ALERT means "say something to a
// person". Nothing is collected in response to an alert, so it carries no
// cursor. The ring sends them on transitions only.
const _alertWorn = 0x01;
const _alertNotWorn = 0x02;
const _alertBatteryLow = 0x03;
const _alertBatteryCritical = 0x04;
const _alertBatteryOk = 0x05;

const _recordSize = 16;

// Mirrors CONFIG_BT_L2CAP_TX_MTU in the ring's prj.conf, and must stay equal to
// it: the ring packs `(mtu - 4) / 16` records into each history DATA frame
// (records_per_notification, ble.c), so 65 carries three and Android's default
// of 23 carries one. Raising it is a real lever but not a free one -- a longer
// packet is a longer radio on-time in every connection event -- so change the
// firmware first and this second, never this alone.
const _desiredMtu = 65;

// Record flag bits (byte 14 of a record). Bit 0 is contact, which nothing here
// reads yet -- the refusal reason in bits 2-4 already says whether the ring was
// worn, and duplicating that into the UI before there is somewhere to show it
// would be inventing a use for it.
const _flagRefused = 0x02;

// The refusal reason, bits 2-4. Mirrors `enum flash_refusal` in the firmware's
// flash_store.h; the order is the wire format and must not be rearranged.
//
// **Three bits, not two, since the firmware widened the field on 2026-08-19 to
// make room for MOVING.** This mask stayed at 0x03 until 2026-08-21, and
// flash_store.h names exactly what that costs: MOVING is 4, and two bits read
// it as 0 -- NOT_WORN. Every yield line logged between those dates therefore
// reported a moving hand as a ring that was not being worn, which is the one
// decode that is actively misleading rather than merely unknown. The 08-20 wear
// test was read as a collapsed contact gate on the strength of it.
const _flagReasonShift = 2;
const _flagReasonMask = 0x07;

// **MOVING moved from 4 to 5 on 2026-08-21, and 4 is now reserved.** The
// firmware renumbered it so that a reader which masks only two bits -- this file
// until today, and any client that has not caught up -- computes `5 & 3 == 1`
// and reports `no pulse`: worn, nothing believable. At 4 it computed 0 and
// reported `not worn`, a claim that the ring was off the wearer's finger, which
// is what sent the 08-20 wear test to the contact gate and the optics. See the
// numbering note in the firmware's flash_store.h.
//
// Both values are folded to one bucket below rather than reported separately.
// Logs written between 08-19 and 08-21 still carry 4, and the ring's kept log
// deliberately survives a reflash, so a transfer can contain both numberings of
// the same thing -- and `12 moving, 30 moving` on one line would be a worse
// answer than either.
const _flagReasonMovingLegacy = 4;
const _flagReasonMoving = 5;
const _refusalNames = [
  'not worn',
  'no pulse',
  'weak pulse',
  'too slow',
  'moving (pre-08-21)', // folded into `moving`; never counted under this name
  'moving',
  'reason 6',
  'reason 7',
];

// How hard the hand was moving while the window was measured: flags bits 5-7,
// on **every** record, refused or not. Mirrors the bucket table in the
// firmware's flash_store.h; the boundaries are the wire format.
//
// Bucket 0 is `unrecorded` rather than a range, which is what let the firmware
// add this without a record-format bump: records written before 2026-08-21 have
// these bits clear, and numbering the first real bucket 0 would have made every
// one of them claim the hand was still.
//
// **Why the app needs it.** WINDOW_MOVE_MILLI_G refuses a window at 100 mg, and
// across the 08-20/21 wear test that was 361 of 462 windows. Whether 100 is the
// right number is not answerable from a log that records only *that* a window
// was refused, and the ring had no way to say more until these three bits
// existed. They arrive on every record and were read nowhere until now.
// The control read's flags byte, and what it gained on 2026-08-21.
//
// Bit 1 does not mean "there was a reset". It means the ring could *read*
// RESETREAS, which matters because on the nRF52832 a **zero cause is the
// finding, not a gap**: that part latches no bit for power-on and none for
// brownout, so a supply that actually went away leaves the register clear. Zero
// with this bit set says the rail dropped. Zero without it says hwinfo failed,
// which is a firmware gap and says nothing about the boot.
const _controlFlagAnchored = 0x01;
const _controlFlagResetCause = 0x02;

// Bit 2, from firmware 2026-09-03. The previous run ended with the cell empty.
//
// **This is the ring's own account of the thing this app otherwise has to
// guess at.** RingLink.battery projects a level forward through the silence and
// concludes "flat" on the arithmetic alone, because nothing else can be known
// while the ring is off the air. This flag is what the ring says afterwards,
// once it has been charged and can talk again: a mark it wrote into NVS on the
// way into critical, read back on a boot whose RESETREAS was clear. It settles
// the estimate rather than replacing it -- by the time it arrives the ring is
// charged and the question has moved on to what was lost.
const _controlFlagWentFlat = 0x04;

// A control read that carries the flat report. Older firmware answers 27, and
// older still 21; each prefix keeps its meaning, so a short answer is a firmware
// that cannot tell us rather than one that says no.
const _controlLenWithFlat = 38;

// RESETREAS bit names, by bit position. Mirrors Zephyr's `RESET_*` in
// hwinfo.h and the causes[] table in the firmware's main.c, in that order.
//
// Reported raw and decoded here rather than named on the ring: a bit this
// firmware has no name for is still a bit worth carrying, and the phone is the
// side that can be updated without a reflash.
const _resetCauseNames = [
  'pin', // 0
  'software request', // 1
  'brownout', // 2
  'power-on', // 3
  'watchdog', // 4
  'debug', // 5
  'security', // 6
  'low-power wake', // 7
  'cpu lockup', // 8
  'parity', // 9
  'pll', // 10
  'clock', // 11
  'hardware', // 12
  'user', // 13
  'temperature', // 14
];

String _describeResetCause(int cause) {
  if (cause == 0) {
    // The one that matters. See the note above _controlFlagResetCause.
    return 'power-on or brownout (RESETREAS clear) -- '
        'if nobody powered it up, the supply went away';
  }
  final names = <String>[];
  for (var bit = 0; bit < 32; bit++) {
    if ((cause & (1 << bit)) == 0) continue;
    names.add(bit < _resetCauseNames.length
        ? _resetCauseNames[bit]
        : 'bit $bit (unnamed)');
  }
  return '${names.join(', ')} (0x${cause.toRadixString(16).padLeft(8, '0')})';
}

const _flagMoveShift = 5;
const _flagMoveMask = 0x07;
const _movementNames = [
  'unrecorded',
  '<50',
  '50-99',
  '100-149',
  '150-249',
  '250-499',
  '500-999',
  '>=1000',
];

/// What one history transfer contained, by outcome.
///
/// **This exists because the shipping data path had no human-readable output at
/// all.** `conf=` and `pi=` were logged only in the live-notification branch,
/// and with firmware `BLE_LIVE_STREAM` at 0 nothing is live: replayed records
/// were parsed, stored, and never surfaced anywhere a person could look. The
/// yield number the whole optics question turns on — accepted windows against
/// refusals, and *which* refusal — was sitting in SQLite with no way to read it
/// off a phone.
///
/// Summarised per transfer rather than logged per record, deliberately. A page
/// is 256 records; 256 lines would push everything else in this log off the top
/// and still leave the counting to whoever read them.
///
/// **Counts windows, not records.** A run of consecutive identical refusals is
/// collapsed by the firmware into one record carrying a `repeat` count, so a
/// record can stand for many measured windows. A yield that counted records
/// would understate refusals by exactly however well the collapsing worked.
class _TransferTally {
  int accepted = 0; // windows with a rate this app believed
  int implausible = 0; // parsed, rate rejected here as out of range
  // One slot per decodable reason, so a firmware that gains a sixth cannot
  // index off the end of this list before the names above are updated.
  // by reason, in windows
  final List<int> refused = List<int>.filled(_refusalNames.length, 0);
  // by movement bucket, in windows, across accepted and refused alike -- a
  // gate's false-refusal rate cannot be measured from the windows it refused.
  final List<int> movement = List<int>.filled(_movementNames.length, 0);
  int confSum = 0, confMin = 0, confMax = 0; // accepted windows only
  int piSum = 0;

  int get refusedTotal => refused.fold(0, (a, b) => a + b);
  int get total => accepted + implausible + refusedTotal;

  void reset() {
    accepted = 0;
    implausible = 0;
    refused.fillRange(0, refused.length, 0);
    movement.fillRange(0, movement.length, 0);
    confSum = confMin = confMax = 0;
    piSum = 0;
  }

  void addAccepted(int confidence, int perfusion) {
    if (accepted == 0) {
      confMin = confMax = confidence;
    } else {
      if (confidence < confMin) confMin = confidence;
      if (confidence > confMax) confMax = confidence;
    }
    accepted++;
    confSum += confidence;
    piSum += perfusion;
  }

  /// One line, or null when the transfer carried nothing worth summarising.
  ///
  /// The yield is the headline because it is the number blocker 8 asks for; the
  /// refusal breakdown is next because "not worn" and "weak pulse" are entirely
  /// different findings — one is the wearer, the other is the optics.
  String? describe() {
    if (total == 0) return null;

    final pct = (100 * accepted / total).toStringAsFixed(0);
    final parts = <String>['yield $pct% ($accepted of $total windows)'];

    if (accepted > 0) {
      parts.add('conf ${(confSum / accepted).round()} '
          'avg [$confMin-$confMax]');
      parts.add('PI ${(piSum / accepted / 10).toStringAsFixed(1)}%');
    }
    if (implausible > 0) parts.add('$implausible implausible');
    for (var i = 0; i < refused.length; i++) {
      if (refused[i] > 0) parts.add('${refused[i]} ${_refusalNames[i]}');
    }
    final move = describeMovement();
    if (move != null) parts.add(move);
    return parts.join(', ');
  }

  /// The movement histogram, as `move 12/40/8 mg-buckets …`, or null when the
  /// ring recorded none — firmware older than 2026-08-21.
  ///
  /// A second line's worth of information on the same line, and it is the one
  /// the movement threshold turns on: the refusal breakdown says *how many*
  /// windows were called moving, and only this says **how far over the line**
  /// they were. 100-149 is a window that missed by less than half the gate;
  /// 500-999 is a wearer who was walking. Those ask for opposite things.
  String? describeMovement() {
    final recorded = movement.skip(1).fold(0, (a, b) => a + b);
    if (recorded == 0) return null;

    final parts = <String>[];
    for (var i = 1; i < movement.length; i++) {
      if (movement[i] > 0) parts.add('${_movementNames[i]}:${movement[i]}');
    }
    return 'move ${parts.join(' ')} mg';
  }
}

// A standard service the ring also serves. Guid normalises 16-bit to the full
// 128-bit base, so this compares equal to the UUID Android discovers.
//
// The ring serves the standard Heart Rate Measurement characteristic (`2A37`)
// too, and this app deliberately ignores it: it notifies only while the
// firmware is streaming live, so on a default build it never fires, and when it
// does it reports the same beat the vitals record already carries -- by a route
// with no confidence, no perfusion and no refusal reason attached to it.
final _uuidBattery = Guid('2A19'); // Battery Level

// Plausibility band -- a backstop against a garbled packet, not a filter on the
// firmware's judgement. Anything outside these bounds is dropped: not shown,
// not charted, not stored, and under the buffer-and-flush model the phone is
// the only transport, so a drop here is permanent.
//
// This used to be a workaround: the firmware pushed rates it had not vouched
// for (spurious 220s), and _hrMax = 200 caught them. Since 2026-07-31 it vouches
// for them first -- vitals are stored and notified only above confidence 500 --
// so an over-tight bound here now discards *believed* readings instead. The
// bounds therefore match the firmware's own clamp (BPM_MIN/BPM_MAX, vitals.c)
// rather than second-guessing it: a genuine tachycardia at 205 bpm is a reading
// the ring stands behind, and dropping it silently is the worse failure.
//
// The low bound is deliberately *below* the firmware's floor of 40 and is
// currently unreachable -- estimate_bpm() clamps, so 35 bpm arrives as 40. That
// clamp, not this constant, is what blinds the device to bradycardia. When it
// is lowered, lower this with it; leaving it generous here costs nothing.
const _hrMin = 30; // bpm
const _hrMax = 220; // bpm, matches BPM_MAX in firmware vitals.c
const _spo2Min = 70.0; // %
const _spo2Max = 100.0; // %

/// What the ring last reported about its own battery.
///
/// Distinct from the percentage: the percentage is a noisy reading, this is the
/// ring's own decision about what it is doing as a result -- measuring
/// normally, measuring less often, or no longer measuring at all.
enum RingPower { ok, low, critical }

/// How fast the cell empties, in gauge points per hour.
///
/// **Measured on hardware, not modelled.** Two figures exist, both from bench
/// runs on a single ring -- treat them as this board's numbers, not the
/// design's:
///
///   - **1.95 %/h worn** -- the 2026-08-08/09 weekend, worn ~100% of its 39
///     hours, which makes that a worn draw of ~604µA.
///   - **~1.6 %/h idle** -- 27 gauge points across the ~17 hours of 08-19/20,
///     worn for one of them, on a table for the other sixteen. ~480µA.
///
/// **The worn figure is the one used, and the choice is about which way to be
/// wrong rather than which number is more typical.** The phone cannot know
/// whether the ring spent the silence on a finger, so it has to pick, and the
/// two failures are not symmetric. Estimating too fast shows a flat ring that
/// still had a tenth of its charge: the wearer charges something that did not
/// need it. Estimating too slow shows 40% for a ring that is dead on a bedside
/// table, which says the ring is still recording while nothing is being
/// recorded at all. That is the failure this whole file keeps being
/// rewritten to prevent, so the estimate leans away from it.
///
/// **It is deliberately a single rate, and the firmware is why that survives.**
/// Below 15% the ring measures less often and below 5% it stops measuring
/// altogether, so a physical model would have the drain falling off toward the
/// end. At the *currently measured* floor it does not meaningfully: ~480µA is
/// burned doing nothing, which is within a quarter of the worn draw, so
/// switching the optics off removes much less than a model would predict. If
/// the firmware's idle regression is ever fixed, this becomes wrong -- fast --
/// and both numbers above have to be re-measured before it is re-tuned.
const _drainPctPerHour = 1.95;

/// A battery figure to put in front of a person, and how much of it is inferred.
///
/// **The ring cannot ever report 0%.** It stops measuring at 5%, coasts to
/// brownout and goes off the air, and the next time a phone can reach it, it has
/// been charged. So the moment at which 0% is true is exactly the moment nothing
/// can transmit it, and a phone that only ever displays what it was told will
/// display the last live reading -- 40%, indefinitely, for a ring that is dead.
/// Everything below zero on that path is inference, and this type exists so the
/// inference is never mistaken for a reading.
@immutable
class BatteryEstimate {
  const BatteryEstimate({
    required this.percent,
    required this.reported,
    required this.age,
  });

  /// What to show: [reported], less whatever [_drainPctPerHour] says has
  /// drained since, floored at zero.
  final int percent;

  /// What the ring actually said, last time it said anything.
  final int reported;

  /// How long ago it said it.
  final Duration age;

  /// True once the projection has moved off the reported figure. At the drain
  /// rate above a single point takes about half an hour, so a connected ring --
  /// which reports every couple of minutes -- never reaches this, and the
  /// estimate suppresses itself without needing to be told the link is up.
  bool get estimated => percent != reported;

  /// The projection has reached empty. Not the same as the ring having said so:
  /// it never can.
  bool get flat => percent == 0;
}

bool _plausibleHr(int bpm) => bpm >= _hrMin && bpm <= _hrMax;
bool _plausibleSpo2(double s) => s >= _spo2Min && s <= _spo2Max;

/// Serialises everything that touches the BLE scanner.
///
/// **There is one scanner in the process and it is not reentrant.**
/// flutter_blue_plus does not refuse a second `startScan` -- it silently stops
/// the one already running (`if (_isScanning.latestValue == true) await
/// _stopScan()`), and the loser's result stream simply goes quiet. With one
/// ring that could never happen. With two sessions whose watchdogs escalate
/// independently it happens exactly when both rings are in trouble, and the
/// consequence is worse than a failed scan: the cancelled session times out and
/// logs "ring is not advertising -- nothing to connect to", which is the one
/// line that exists to tell a dead ring apart from a stuck platform. A
/// collision made it assert the first while the second was true.
///
/// Chaining rather than a boolean flag, so a caller waits its turn instead of
/// being turned away -- a watchdog that skipped its scan would just fail
/// quietly a different way.
class ScanLock {
  static Future<void> _tail = Future<void>.value();

  static Future<T> run<T>(Future<T> Function() body) {
    final completer = Completer<T>();
    _tail = _tail.then((_) async {
      try {
        completer.complete(await body());
      } catch (e, st) {
        completer.completeError(e, st);
      }
    });
    return completer.future;
  }
}

/// One ring's connection, replay state and readings.
///
/// **One instance per ring.** This was a singleton until 2026-08-07, on the
/// assumption that a phone talks to one ring -- and almost every field in it was
/// already per-ring, which is why the change is a constructor rather than a
/// rewrite. What had to leave are the four things that are properties of the
/// *phone* and not of a ring: the shared log, the foreground-service
/// notification, BLE scanning, and which ring the UI is looking at. Those live
/// in [RingFleet], which owns a map of these.
///
/// Nothing here reaches for a global. A session talks about itself, reports
/// through [onLog] and [notifyListeners], and does not know how many others
/// exist -- which is what makes running several of them uneventful.
class RingLink extends ChangeNotifier {
  RingLink(this.ringId);

  /// The BLE address this session is bound to, fixed for its lifetime. It was
  /// a mutable field when a singleton had to change which ring it meant; a
  /// session that could change identity underneath its own replay cursor is
  /// exactly the bug this class was refactored to make impossible.
  final String ringId;

  /// Where this session's log lines go. The fleet owns the buffer so one list
  /// reads in arrival order across every ring.
  void Function(String ringId, String line)? onLog;

  /// Raised when the ring reports a condition a person should know about.
  void Function(String ringId, int reason)? onAlert;

  /// Whether the ring last said it was on a finger. Null until it has said
  /// either way -- "not worn" and "has not told us yet" are different, and the
  /// UI must not present the second as the first.
  bool? get wornNow => _wornNow;
  bool? _wornNow;

  /// What the ring last said about its cell.
  RingPower get powerState => _powerState;
  RingPower _powerState = RingPower.ok;

  // ---- Observable state ------------------------------------------------------
  String get status => _status;
  String _status = 'Idle';

  bool get busy => _busy; // a scan/connect is in flight
  bool _busy = false;

  bool get connected => _wasConnected;
  bool get autoConnect => _autoConnect;

  // Latest readings (for the Live tiles).
  int? get bpm => _bpm; // bpm, newest reading by any path -- see _handleRecord
  double? get spo2 => _spo2; // percent
  int? _bpm;
  double? _spo2;

  // What the ring last said about its cell, and when it said it. Restored from
  // the ring's row on start, so it survives the app being killed -- which is
  // the case that matters, because a cell empties over hours and Android
  // reclaims a background process in far less than that.
  int? _battPct;
  int? _battMv;
  int? _battAt; // wall clock ms on this phone, not a ring timestamp

  /// The cell, projected forward to now. Null until the ring has ever reported.
  ///
  /// **This decays on its own and is meant to.** A connected ring reports every
  /// couple of minutes and a point takes half an hour to drain, so while the
  /// link is alive the projection never leaves the reported figure and no
  /// caller has to ask whether we are connected. Once the ring goes quiet the
  /// number falls, and it reaches 0 at about the time the cell actually does.
  ///
  /// **The one case it gets wrong is a ring charging out of contact**: it will
  /// read 0% for a ring sitting full on a charger the phone cannot see. That
  /// resolves on the next packet, and it is the acceptable half of the trade
  /// argued at [_drainPctPerHour] -- being wrong toward "go and check it".
  BatteryEstimate? get battery {
    final reported = _battPct;
    final at = _battAt;
    if (reported == null || at == null) return null;

    final elapsed = DateTime.now().millisecondsSinceEpoch - at;
    // A backwards clock -- a timezone change, an NTP correction -- must not
    // *raise* the estimate. Nothing about the cell recovers because the phone
    // changed its mind about what time it is.
    final age = Duration(milliseconds: elapsed < 0 ? 0 : elapsed);
    final drained = age.inMinutes * _drainPctPerHour / 60.0;
    // Floor, not round: half a point of doubt is spent downward, for the same
    // reason the rate itself was chosen from the faster of the two measurements.
    final projected = (reported - drained).floor();

    return BatteryEstimate(
      percent: projected < 0 ? 0 : projected,
      reported: reported,
      age: age,
    );
  }

  /// Takes a battery report from the ring and starts the clock on it.
  ///
  /// [millivolts] is null on the standard Battery Service path, which carries a
  /// single percent byte and nothing else -- the previous mV is kept rather than
  /// zeroed, because "the last voltage we know of" is true and 0mV is not.
  void _noteBattery(int percent, {int? millivolts}) {
    _battPct = percent;
    if (millivolts != null) _battMv = millivolts;
    _battAt = DateTime.now().millisecondsSinceEpoch;
    _persistBattery();
    notifyListeners();
  }

  // Written on every report, not only on a change: `batt_at` is what the
  // projection measures silence from, so letting it go stale would have the
  // estimate charge the ring for hours it was in fact talking to us.
  void _persistBattery() {
    final pct = _battPct;
    if (pct == null) return;
    unawaited(HistoryDb.instance
        .saveBattery(ringId, pct, _battMv, _powerState.index));
  }

  /// Steps per calendar day, oldest first, for the last [stepHistoryDays] days.
  ///
  /// Read back from the database rather than accumulated as records arrive, and
  /// that is deliberate: a short transfer is re-read in full (see the END case
  /// in [_onHistory]), so the same record can reach [_handleRecord] more than
  /// once. The unique index makes the *stored* row idempotent, but an in-memory
  /// running total has no such protection and would drift upward on every
  /// retry. Re-querying costs one grouped scan per transfer and cannot drift.
  List<DailySteps> get dailySteps => List.unmodifiable(_dailySteps);
  List<DailySteps> _dailySteps = const [];

  /// Today's step total, or null if nothing has been recorded today.
  int? get stepsToday => _dailySteps.isEmpty ? null : _dailySteps.last.steps;

  static const stepHistoryDays = 7;

  Future<void> _refreshSteps() async {
    _dailySteps = await HistoryDb.instance
        .dailySteps(ringId, days: stepHistoryDays);
    notifyListeners();
  }

  // When the displayed reading was *measured* -- not when it arrived.
  //
  // The tiles need this because the ring no longer streams. Since firmware
  // BLE_LIVE_STREAM went to 0 a reading reaches the phone when its flash page
  // fills, which is ~1h43m at the shipping cadence, so "68 bpm" on its own
  // invites the reader to believe it is the wearer's rate *now*. A number that
  // old, presented as current, is worse than no number: it is the same mistake
  // as an uncalibrated SpO2 percentage, and somebody can act on it.
  int? _bpmAt;
  int? get bpmAt => _bpmAt;

  /// How long ago the displayed reading was measured, or null if none yet.
  Duration? get bpmAge => _bpmAt == null
      ? null
      : Duration(
          milliseconds: DateTime.now().millisecondsSinceEpoch - _bpmAt!);

  List<FlSpot> get hrSeries => _hrSeries;
  final List<FlSpot> _hrSeries = [];

  int get stored => _stored;
  int _stored = 0;


  /// Wall-clock time a packet last arrived from the ring, in ms since epoch.
  /// Not a reading's own timestamp: this is about whether the link is alive.
  int? get lastRecordAt => _lastRecordAt;
  int? _lastRecordAt;

  /// Whether the foreground service is up. The UI shows this because a
  /// monitoring app that is silently not monitoring is the failure this whole
  /// item exists to prevent.

  // ---- Session internals -----------------------------------------------------
  // The ring's uptime-to-epoch anchor: real time of its last boot, in ms.
  //
  // The ring has no RTC, so its record timestamps are milliseconds since power
  // up (see wallclock.h). This phone sets the anchor on every connection and
  // reads it back; with it, a record's real time is `_ringEpochAtBoot + ts`.
  //
  // Null means the ring has not been anchored this session. Stamping with
  // arrival time is the fallback, and it is only defensible for live data:
  // under the buffer-and-flush model a packet can arrive hours after it was
  // measured, and arrival time would date a whole night to the moment of the
  // flush.
  int? _ringEpochAtBoot;

  // The control characteristic, held so a replay can be requested after the
  // history subscription is up rather than during discovery.
  BluetoothCharacteristic? _control;

  // Backfill progress for the transfer in flight, and what we have collected
  // across sessions. `_histGeneration` is the ring's erase generation that
  // `_histCursor` is an offset into.
  int _histCursor = 0;
  int _histGeneration = 0;
  int _histReceived = 0; // records actually received in the current transfer
  final _tally = _TransferTally(); // outcome counts for the current transfer
  int _histFrom = 0; // offset the current transfer started at (from the HEADER)

  // A transfer that arrives short is retried rather than acknowledged, and this
  // bounds the retrying. If the link is dropping frames faster than it delivers
  // them, retrying forever would hold the radio up and never converge; the ring
  // keeps the records either way, and the next nudge or reconnect tries again.
  int _histShortRetries = 0;
  static const int _histShortRetryMax = 2;

  // What the ring reported at the last control read.
  int _ringGeneration = 0;
  int _ringStored = 0; // bytes currently in the ring's buffer

  BluetoothDevice? _device;
  bool _autoConnect = false; // an auto-connect request is wanted / armed
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  final List<StreamSubscription<List<int>>> _valueSubs = [];
  bool _wasConnected = false;

  // When the current link came up, for the disconnect log below. Null while
  // nothing is connected.
  DateTime? _connectedAt;

  bool _started = false;

  Timer? _linkWatchdog;
  int _linkRetries = 0;

  // ---- Session lifecycle -----------------------------------------------------
  /// Loads this ring's stored readings and arms its connection.
  ///
  /// Called by [RingFleet] once per enabled ring. Idempotent.
  Future<void> start() async {
    if (_started) return;
    _started = true;
    await _restoreHistory();
    await _armAutoConnect(BluetoothDevice.fromId(ringId));
  }

  /// Puts this session down for good. The rows it wrote stay.
  Future<void> shutdown({bool unbond = false}) async {
    _started = false;
    await _teardownLink(unbond: unbond);
  }

  Future<void> _restoreHistory() async {
    await _restoreBattery();
    _hrSeries
      ..clear()
      ..addAll(await HistoryDb.instance.loadHr(ringId));
    _stored = await HistoryDb.instance.total(ringId);
    await _refreshSteps();
    notifyListeners();
    if (_stored > 0) _log0('Restored $_stored stored reading(s).');
  }

  /// Picks the cell back up where the last run of the app left it.
  ///
  /// **Without this the feature does not work at all.** The whole point is the
  /// stretch where the ring is unreachable, and Android reclaims a background
  /// process inside that stretch as a matter of course. An app that forgot the
  /// last reading on every restart would come back showing `--`, which is a
  /// different way of not telling anybody the ring is flat.
  Future<void> _restoreBattery() async {
    final row = await HistoryDb.instance.ring(ringId);
    if (row == null) return;
    _battPct = row.battPct;
    _battMv = row.battMv;
    _battAt = row.battAt;
    // Defensive against a row written by a build that knew more states than
    // this one does. An unknown state is not a reason to claim the cell is ok.
    final state = row.battState;
    _powerState = (state >= 0 && state < RingPower.values.length)
        ? RingPower.values[state]
        : RingPower.critical;

    final est = battery;
    if (est == null) return;
    _log0(est.estimated
        ? 'battery: ${est.reported}% reported ${_since(est.age)}, '
            'estimated ${est.percent}% now'
        : 'battery: ${est.percent}% (${_since(est.age)})');
  }

  static String _since(Duration age) {
    if (age.inMinutes < 60) return '${age.inMinutes} min ago';
    if (age.inHours < 48) return '${age.inHours}h ago';
    return '${age.inDays}d ago';
  }

  // ---- Logging ---------------------------------------------------------------
  // Lines go to the fleet's single buffer rather than one per session. With two
  // rings the interesting question is almost always what happened *between*
  // them -- which one dropped, which one is re-reading -- and two separate logs
  // to interleave by eye is the wrong tool for that.
  void _log0(String msg) {
    debugPrint('[SenseRing] $msg'); // also to Logcat / `flutter logs`
    onLog?.call(ringId, msg);
  }

  void _setStatus(String s) {
    _status = s;
    _log0(s);
    notifyListeners();
  }

  // ---- Auto-connect ----------------------------------------------------------
  // Hand the platform a standing connect request and return. This is the whole
  // auto-connect mechanism: on Android it is `connectGatt(autoConnect: true)`,
  // which the stack keeps alive across the ring going out of range, resetting,
  // or the adapter being toggled, reconnecting each time on its own. Nothing
  // here polls or re-scans, and it costs no scan power — the radio work is the
  // system's own background scan, shared with every other app.
  //
  // It is also why the bond matters: the ring's vitals CCCs are
  // encryption-gated, and only a stored bond lets the link come back up
  // encrypted without prompting the wearer.
  Future<void> _armAutoConnect(BluetoothDevice device) async {
    if (!await _ensurePermissions()) {
      _setStatus('Permissions denied — cannot connect.');
      return;
    }

    _watch(device);
    _autoConnect = true;
    notifyListeners();

    // Before the connect, not after: the standing request is what the service
    // exists to protect, and a process killed between the two would leave the
    // platform holding a gatt for a dead app.

    try {
      // `mtu: null` is required alongside autoConnect (flutter_blue_plus
      // asserts on the pair). **It is not free**, and the comment that used to
      // sit here saying so cost a day: the ring is built for a 65-byte MTU, not
      // the 23 this leaves us on, and at 23 every history DATA frame carries
      // one record instead of three. `_onConnected` asks for it explicitly
      // once the link is up; see the note there.
      await device.connect(
        license: License.nonprofit,
        autoConnect: true,
        mtu: null,
      );
    } catch (e) {
      _setStatus('Auto-connect failed: $e');
      return;
    }

    // connect() returns immediately here — it does not wait for the ring — so
    // the connection state listener is what reports the actual arrival, and
    // the watchdog is what notices that it never does.
    _startLinkWatchdog();

    if (device.isConnected) return;
    if (await FlutterBluePlus.adapterState.first != BluetoothAdapterState.on) {
      _setStatus('Bluetooth is off — will connect when it is back on.');
    } else {
      _setStatus('Waiting for the ring — will connect automatically.');
    }
  }

  // ---- The reconnect loop ----------------------------------------------------
  // `connectGatt(autoConnect: true)` is a standing request the platform is
  // supposed to honour on its own, and mostly it does. Observed on the S23
  // 2026-08-04: a request armed by a freshly started process sat for five
  // minutes with the ring powered and advertising, and an identical request
  // armed by hand connected in three seconds. Whatever the stack was holding
  // onto across the process restart, the arm itself was dead and nothing about
  // it looked wrong -- the app said "will connect automatically" the entire
  // time, which is the worst way for a monitor to fail.
  //
  // So the standing request is no longer trusted alone. While armed and not
  // connected, this re-arms on a timer, and every third attempt falls back to
  // the scan-and-direct-connect path that demonstrably works. The scan is the
  // expensive one, hence its being the exception rather than the rule.
  static const _retryAfter = Duration(seconds: 90);

  void _startLinkWatchdog() {
    _linkWatchdog?.cancel();
    if (!_autoConnect) return;
    _linkWatchdog = Timer.periodic(_retryAfter, (_) => unawaited(_retryLink()));
  }

  void _stopLinkWatchdog() {
    _linkWatchdog?.cancel();
    _linkWatchdog = null;
  }

  Future<void> _retryLink() async {
    if (!_autoConnect || _wasConnected) return;
    _linkRetries++;
    final byScan = _linkRetries % 3 == 0;
    _log0('link: no connection after ${_retryAfter.inSeconds}s '
        '(attempt $_linkRetries) — ${byScan ? 'scanning' : 're-arming'}');
    if (byScan) {
      await _recoverByScan();
    } else {
      await _reArm();
    }
  }

  // Tear the standing request down before making a new one. Without the
  // disconnect the platform keeps the old, dead handle and the new request
  // changes nothing -- which is the failure this exists to break out of.
  Future<void> _reArm() async {
    final device = _linkDevice();
    if (device == null) return;
    try {
      await device.disconnect();
      await device.connect(
        license: License.nonprofit,
        autoConnect: true,
        mtu: null,
      );
    } catch (e) {
      _log0('link: re-arm failed ($e)');
    }
  }

  // The escalation, and it doubles as the diagnosis this failure badly needed:
  // it distinguishes "the ring is not advertising" -- flat battery, crashed,
  // out of range -- from "the ring is right here and the platform is not
  // connecting to it", which previously looked identical from the app.
  Future<void> _recoverByScan() => ScanLock.run(_recoverByScanLocked);

  Future<void> _recoverByScanLocked() async {
    final id = ringId;
    final found = Completer<BluetoothDevice?>();
    try {
      await _scanSub?.cancel();
      _scanSub = FlutterBluePlus.onScanResults.listen((results) {
        for (final r in results) {
          if (r.device.remoteId.str == id && !found.isCompleted) {
            found.complete(r.device);
          }
        }
      });
      await FlutterBluePlus.startScan(
        withNames: [_deviceName],
        timeout: const Duration(seconds: 12),
      );
      final device = await found.future
          .timeout(const Duration(seconds: 14), onTimeout: () => null);
      await FlutterBluePlus.stopScan();
      await _scanSub?.cancel();
      _scanSub = null;

      if (device == null) {
        _log0('link: ring is not advertising — nothing to connect to');
        return;
      }
      _log0('link: ring is advertising but the standing request never fired '
          '— connecting directly');
      // Drop the standing request first: a direct connect alongside a live
      // autoConnect handle is two connections to one peripheral.
      try {
        await device.disconnect();
      } catch (_) {
        // Nothing was up; that is the normal case here.
      }
      _watch(device);
      await device.connect(
        license: License.nonprofit,
        timeout: const Duration(seconds: 20),
      );
    } catch (e) {
      _log0('link: recovery failed ($e)');
    }
  }

  BluetoothDevice? _linkDevice() {
    return _device ?? BluetoothDevice.fromId(ringId);
  }

  // ---- Connect flow ----------------------------------------------------------
  /// Re-arms this session after a manual disconnect.
  ///
  /// Discovery of a *new* ring is the fleet's job -- scanning is a process-wide
  /// resource and a session, by construction, already knows which ring it is.
  Future<void> connect() async {
    if (_busy) return;
    _busy = true;
    notifyListeners();
    try {
      if (!await _ensurePermissions()) {
        _setStatus('Permissions denied — cannot connect.');
        return;
      }
      if (await FlutterBluePlus.adapterState.first !=
          BluetoothAdapterState.on) {
        _setStatus('Bluetooth is off — turn it on and retry.');
        return;
      }
      await _armAutoConnect(BluetoothDevice.fromId(ringId));
    } catch (e) {
      _setStatus('Connect failed: $e');
    } finally {
      _busy = false;
      notifyListeners();
    }
  }

  // One listener per device, replacing any previous one. Both entry points --
  // the first-time scan and the auto-connect arm -- go through here, so every
  // reconnection lands in _onConnected and re-runs discovery.
  void _watch(BluetoothDevice device) {
    _device = device;
    _connSub?.cancel();
    _connSub = device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.connected) {
        _wasConnected = true;
        _connectedAt = DateTime.now();
        _stopLinkWatchdog();
        _linkRetries = 0;
        notifyListeners();
        _onConnected(device);
      } else if (state == BluetoothConnectionState.disconnected &&
          _wasConnected) {
        _wasConnected = false;
        _logDisconnect(device);
        notifyListeners();
        _onDisconnected();
      }
    });
  }

  // **Why the link actually dropped, from the phone's side.**
  //
  // The ring cannot answer this. It sees a terminate arrive and reports 0x13
  // REMOTE_USER_TERMINATED, which is true of every one of these and says only
  // "the phone hung up" -- three days of ring-side logs said exactly that and
  // no more. Nothing in this file disconnects a live link (every
  // `device.disconnect()` here is either guarded by `!_wasConnected` or comes
  // straight off a button), so the decision is being made below
  // flutter_blue_plus and nothing on either side was recording it.
  //
  // Android reports the HCI status, phrased from *its* point of view, so the
  // two logs read against each other and the pair is the diagnosis:
  //
  //   CONNECTION_TERMINATED_BY_LOCAL_HOST (0x16)  the phone chose to, and with
  //       no disconnect of ours in flight that means the platform did it --
  //       process frozen, GATT reclaimed, foreground service refused or gone.
  //       Cross-check against the "foreground service not started" line in
  //       RingFleet, which is the condition that would explain it.
  //   LINK_SUPERVISION_TIMEOUT (0x08)             nobody chose; the radio lost
  //       it. Range or interference, not software.
  //   ANDROID_SPECIFIC_ERROR (0x85)               the GATT_ERROR/133 catch-all.
  //   REMOTE_USER_TERMINATED_CONNECTION (0x13)    the phone believes the *ring*
  //       hung up. Both sides cannot be right, so this would mean the terminate
  //       comes from below either host and both logs are downstream of it.
  //
  // No decode table here on purpose: flutter_blue_plus already turns the status
  // into these names (hciStatusString, in its Android plugin) and a second copy
  // would only be a second thing to keep current.
  //
  // The duration is here because it is what separates a timer from a
  // coincidence, and it is the number this took longest to establish by hand:
  // measured lifetimes ran 9s, 26s, 42s, 4m and 1h41m, which is what ruled out
  // every periodic timer in this file. Nothing else records it -- the ring
  // logs a connect and a disconnect but its uptime clock restarts on every
  // reset, so the subtraction is not reliably doable after the fact.
  //
  // Deliberately only for links that were actually up. `connectionState` opens
  // with a cached `disconnected` and repeats it, so logging every one would put
  // a line on the log per `_watch()` call and drown the drops this exists to
  // find. Attempts that never connect are already covered by `_retryLink` and
  // `_recoverByScan`, which log their own outcomes.
  void _logDisconnect(BluetoothDevice device) {
    final reason = device.disconnectReason;
    final up = _connectedAt;
    _connectedAt = null;

    final ms = up == null ? null : DateTime.now().difference(up).inMilliseconds;
    final held = ms == null ? 'unknown' : '${(ms / 1000).toStringAsFixed(1)}s';

    _log0('link: dropped after $held — '
        '${reason?.description ?? 'no reason reported'} '
        '(code ${reason?.code ?? '?'}, ${reason?.platform.name ?? '?'})');
  }

  /// Pairs with a freshly discovered ring. Called by the fleet after a scan.
  Future<void> onFound(BluetoothDevice device) async {
    _setStatus('Found ${device.remoteId}. Connecting...');
    _watch(device);

    try {
      // A direct connect for the first, pairing connection: auto-connect is
      // deliberately slow to fire (it rides the system's background scan) and
      // this is the one moment the wearer is watching. Auto-connect takes over
      // from the first disconnect onwards, in _onDisconnected.
      // License.nonprofit: this is a personal / educational research app.
      await device.connect(
        license: License.nonprofit,
        timeout: const Duration(seconds: 20),
      );
    } catch (e) {
      _setStatus('Connect failed: $e');
    }
  }

  Future<void> _onConnected(BluetoothDevice device) async {
    try {
      _setStatus('Connected. Checking bond...');
      final bond = await device.bondState.first;
      if (bond != BluetoothBondState.bonded) {
        _setStatus('Bonding (accept the pairing prompt)...');
        await device.createBond();
      }

      // Remember it only now: an address we could not bond with is of no use
      // to auto-connect, since the encrypted characteristics would stay shut.
      // From here on every disconnect re-arms rather than ending the session.
      //
      // A session cannot change which ring it means, so there is no identity to
      // reassign here -- only a row to make sure exists. Connecting one ring
      // takes nothing away from another; that is the whole point of a session
      // per ring.
      assert(device.remoteId.str == ringId,
          'session for $ringId connected to ${device.remoteId.str}');
      await HistoryDb.instance.rememberRing(ringId);
      if (!_autoConnect) {
        _autoConnect = true;
        notifyListeners();
      }

      // **Ask for the MTU here, because `connect()` cannot.** The ring sizes
      // its history frames for a 65-byte ATT MTU: one type byte plus three
      // 16-byte records. At Android's default of 23 only *one* record fits, so
      // a 250-record page becomes 250 notifications instead of 84 -- triple the
      // frames, and with them triple the chance of losing one. That is blocker
      // 1a's rate, and it is why the shortfall is always exactly one record:
      // at this MTU a lost frame *is* one record.
      //
      // flutter_blue_plus forbids passing `mtu:` alongside `autoConnect`
      // (see _armAutoConnect), and the reason is not arbitrary: with
      // autoConnect its `connect()` returns before the link exists, so its own
      // request -- guarded by `isConnected`, bluetooth_device.dart:191 -- would
      // silently never fire. Nothing logged that. The MTU has to be asked for
      // once the connection actually exists, which is here.
      //
      // Never fatal. A refused or timed-out exchange leaves the link at 23,
      // which still works: more frames, more short transfers, more retries, but
      // no data loss, because the guard in `case _histEnd` is what protects
      // against that. Losing the session over an optimisation is the worse
      // outcome, so this cannot be allowed to escape into the catch below.
      //
      // Logged either way. This failure was invisible for a day precisely
      // because nothing ever printed the negotiated MTU.
      try {
        final mtu = await device.requestMtu(_desiredMtu);
        _log0('link: ATT MTU $mtu '
            '(${(mtu - 4) ~/ _recordSize} record(s) per history frame)');
      } catch (e) {
        _log0('link: MTU request failed ($e) -- staying at ${device.mtuNow}, '
            'expect short transfers');
      }

      _setStatus('Bonded. Discovering services...');

      final services = await device.discoverServices();

      // Set the clock before subscribing, so the first packet that arrives is
      // already datable. The ring measures from power-up and may have been
      // buffering for hours with no idea what time it is.
      for (final s in services) {
        for (final c in s.characteristics) {
          if (c.uuid == _uuidControl) {
            _control = c;
            await _syncClock(c);
          }
        }
      }

      int subscribed = 0;
      for (final s in services) {
        for (final c in s.characteristics) {
          final label = _labelFor(c.uuid);
          if (label == null || !c.properties.notify) continue;
          _valueSubs.add(
            c.onValueReceived.listen((v) => _onData(label, v)),
          );
          await c.setNotifyValue(true);
          subscribed++;
          _log0('Subscribed to $label');
        }
      }
      _setStatus(
          'Streaming ($subscribed characteristics). Waiting for data...');

      // Only now: the ring refuses a replay request until the history CCC is
      // set, because a transfer whose notifications go nowhere would never
      // deliver an END and the cursor would never advance.
      await _requestBackfill();
    } catch (e) {
      _setStatus('Setup failed: $e');
    }
  }

  // Tell the ring what time it is, then read back the anchor it derived.
  //
  // Done on every connection rather than once at pairing, which is what makes
  // the ring's crystal drift a non-problem: the error can only accumulate since
  // the last connection instead of since boot. It also re-anchors for free
  // after the ring resets, which zeroes its uptime and invalidates the old
  // anchor completely.
  //
  // Read back rather than computed here: the anchor is `epoch - uptime`, and
  // this side does not know the ring's uptime. Asking is one round trip and
  // removes a whole class of drift between what the phone believes and what the
  // ring actually stored.
  Future<void> _syncClock(BluetoothCharacteristic c) async {
    try {
      final now = DateTime.now().millisecondsSinceEpoch;
      final payload = ByteData(9)
        ..setUint8(0, _opSetTime)
        ..setUint64(1, now, Endian.little);
      // withoutResponse: false -- a silently dropped clock write would date
      // every reading of the session wrong, which is worth one ack.
      await c.write(payload.buffer.asUint8List(), withoutResponse: false);

      // One read carries the clock anchor *and* the log's generation and
      // cursor. They arrive together because they are only meaningful together
      // -- assembling them from separate reads would risk mixing two moments.
      final raw = await c.read();
      if (raw.length < 21) {
        _log0('clock: short control read (${raw.length}B)');
        return;
      }
      final b = ByteData.sublistView(Uint8List.fromList(raw));
      _ringGeneration = b.getUint32(13, Endian.little);
      _ringStored = b.getUint32(17, Endian.little);

      // Boot state first, and above the anchor check on purpose. Whether the
      // ring has a clock and why it last restarted are independent questions,
      // and the reset hunt must not be the thing that goes quiet when the clock
      // handshake is the thing that failed. Uptime is at a fixed offset and does
      // not depend on the anchor being valid.
      final uptime = b.getUint32(9, Endian.little);
      _reportBoot(
          DateTime.now().subtract(Duration(milliseconds: uptime)), raw, b);

      final anchored = (b.getUint8(0) & _controlFlagAnchored) != 0;
      if (!anchored) {
        _log0('clock: ring reports no anchor');
        return;
      }
      final anchor = b.getUint64(1, Endian.little);
      _ringEpochAtBoot = anchor;
      notifyListeners();
      // Two different instants, and they were printed as one until 2026-08-21.
      //
      // `uptime` is the ring's raw `k_uptime_get()` -- milliseconds since the
      // last boot, so `now - uptime` is when the ring actually started. `anchor`
      // is the ring's `epoch_at_boot`, computed against *virtual* uptime
      // (wallclock.h), which carries a base across a reset so a kept log stays
      // on one scale. The two agree only until the first reset the log survives;
      // after that the anchor sits however long the ring has ever run in the
      // past, and labelling it "boot at" made a working clock read as a broken
      // one. The 08-20 wear test printed "boot at 2026-08-20 18:54" at 10:20 the
      // next morning for exactly this reason.
      //
      // The anchor is still the right number for dating records -- a record's
      // wall time is anchor + its virtual timestamp -- so it is kept and named,
      // not dropped.
      final bootedAt = DateTime.now().subtract(Duration(milliseconds: uptime));
      _log0('clock: anchored, ring up ${(uptime / 1000).round()}s '
          '(booted $bootedAt), record epoch '
          '${DateTime.fromMillisecondsSinceEpoch(anchor)}');
    } catch (e) {
      // Not fatal: the session falls back to arrival-time stamping, which is
      // what it did before the ring had a clock at all.
      _log0('clock: sync failed ($e)');
    }
  }

  // The boot instant the last control read reported, so a new one can be
  // recognised. Null until the first read of this app session -- which is why
  // the first connection after the app starts reports the cause without calling
  // it a reset: the app genuinely does not know whether it missed one.
  DateTime? _lastBootAt;
  int _resetsSeen = 0;

  /// Says out loud when the ring restarted, and why.
  ///
  /// **Until 2026-08-21 nothing on this side compared one connection's uptime
  /// with the last**, so a reset was visible only as a `t=` running backwards in
  /// a status line nobody was diffing. Eight of them across the 08-20/21 wear
  /// test were found afterwards, by hand, from seven screenshots. That is the
  /// whole reason this method exists: the census should be a log line, not an
  /// archaeology exercise.
  ///
  /// The cause comes from control bytes [21..26], appended by the firmware on
  /// 2026-08-21. It is the only way to learn *why* while the ring is worn --
  /// `log_reset_cause()` writes to RTT, RTT means a debugger, and a debugger
  /// means the ring is not on a finger, which is the one condition under which
  /// these resets happen.
  void _reportBoot(DateTime bootedAt, List<int> raw, ByteData b) {
    // A boot instant derived from `now - uptime` carries the round trip's
    // jitter, so two reads of the same boot land a few hundred ms apart. Five
    // seconds is far below the shortest run ever observed -- 22 minutes -- and
    // far above the noise.
    final prev = _lastBootAt;
    final isNewBoot =
        prev == null || bootedAt.difference(prev).abs() > const Duration(seconds: 5);
    _lastBootAt = bootedAt;

    if (isNewBoot && prev != null) {
      _resetsSeen++;
      final ran = bootedAt.difference(prev);
      _log0('*** RING RESET #$_resetsSeen -- the previous boot ran '
          '${_short(ran)} (from $prev)');
    }
    if (!isNewBoot) return;

    // Older firmware answers 21 bytes and knows nothing about a cause. Silence
    // is the honest response to that, not a fabricated zero.
    if (raw.length < 27) {
      _log0('boot: this firmware does not report a reset cause '
          '(${raw.length}B control read)');
      return;
    }

    final flags = b.getUint8(0);
    final cause = b.getUint32(21, Endian.little);
    final destroyed = b.getUint16(25, Endian.little);

    if ((flags & _controlFlagResetCause) == 0) {
      _log0('boot: reset cause unavailable -- the ring could not read RESETREAS');
    } else {
      _log0('boot: reset cause ${_describeResetCause(cause)}');
    }
    // Zero is the good news and is worth saying: the log is meant to survive a
    // reset intact, and this line is the only evidence that it did. A day of
    // silent zeroes is that holding; the first non-zero is the day it stopped.
    _log0(destroyed == 0
        ? 'boot: the log survived -- no records destroyed'
        : '*** boot: the boot erase destroyed $destroyed record(s)');

    if (raw.length < _controlLenWithFlat) {
      // Not "the cell did not run out" -- this firmware has no opinion either
      // way, and saying nothing is the only honest option.
      return;
    }
    if ((flags & _controlFlagWentFlat) == 0) return;

    _wentFlat = true;
    final pct = b.getUint8(27);
    final mv = b.getUint16(28, Endian.little);
    final epoch = b.getUint64(30, Endian.little);
    // 0 means no phone had set the ring's clock during the run that died, so
    // there is a percentage but no time to attach it to. Undatable is not the
    // same as untrue, and the finding still goes on the log.
    _flatAt = epoch == 0
        ? null
        : DateTime.fromMillisecondsSinceEpoch(epoch, isUtc: true).toLocal();
    final when = _flatAt == null ? 'at an unknown time' : 'at $_flatAt';
    _log0('*** boot: THE CELL RAN OUT. Last reading $pct% (${mv}mV) $when; '
        'nothing was recorded between then and this boot.');
  }

  /// The ring reported that the previous run ended with the cell empty, and
  /// when it last knew anything, if it had a clock at the time.
  ///
  /// Distinct from `battery.flat`, which is this phone's projection through a
  /// silence and is what shows a wearer 0% *while it is happening*. This is the
  /// confirmation, and it can only ever arrive afterwards.
  bool get wentFlat => _wentFlat;
  DateTime? get flatAt => _flatAt;
  bool _wentFlat = false;
  DateTime? _flatAt;

  static String _short(Duration d) {
    if (d.inMinutes < 1) return '${d.inSeconds}s';
    if (d.inHours < 1) return '${d.inMinutes}m ${d.inSeconds % 60}s';
    return '${d.inHours}h ${d.inMinutes % 60}m';
  }

  /// Collect whatever the ring has been holding, right now.
  ///
  /// Called when the app comes back to the foreground (RingFleet installs the
  /// lifecycle observer). Everything else that starts a transfer is either the
  /// ring deciding it has waited long enough -- an excursion, a deep backlog,
  /// the ten-minute uncollected backstop -- or this app connecting. **None of
  /// those is "the person picked their phone up and looked", and that is the
  /// moment the readings are actually wanted.**
  ///
  /// ⚠️ **The control re-read is not optional, and skipping it makes this a
  /// silent no-op.** `_ringStored` is otherwise set exactly once per
  /// connection, by the control read at connect, and `_requestBackfill()`
  /// refuses to ask for anything once `_histCursor` has caught up with it. On a
  /// link that has been up for hours -- which is the whole case this method
  /// exists for -- that value is stale by every record measured since, so
  /// asking without refreshing it first logs "nothing buffered" and returns
  /// while the ring sits on a full buffer. This is the same trap the
  /// `_histEvent` handler documents: it takes the live cursor off the event
  /// frame for exactly this reason.
  ///
  /// Safe to call at any time and as often as you like. A cursor already at the
  /// ring's own leaves `_requestBackfill()` with nothing to ask for, a stale
  /// generation is answered with STALE and restarted, and a transfer already in
  /// flight is left alone by the ring.
  Future<void> collectNow() async {
    if (!_wasConnected) return;
    final c = _control;
    if (c == null) return;

    // Bytes 13..16 are the erase generation and 17..20 the live cursor -- the
    // same fields _syncClock() reads, and the same layout. Deliberately not
    // routed through _syncClock(): that also writes the clock, and re-anchoring
    // on every foreground would move the timestamps of a session that is
    // already running for no reason connected to collecting records.
    try {
      final raw = await c.read();
      if (raw.length >= 21) {
        final b = ByteData.sublistView(Uint8List.fromList(raw));
        _ringGeneration = b.getUint32(13, Endian.little);
        _ringStored = b.getUint32(17, Endian.little);
      }
    } catch (e) {
      // Not fatal, and not a reason to skip the request: the cursor we have may
      // still be behind the ring's, in which case the backfill below is exactly
      // right. Worth a line, because a read failing here on a link the app
      // believes is up is the interesting case.
      _log0('history: control re-read failed on foreground ($e)');
    }

    await _requestBackfill();
  }

  // Ask the ring for everything measured since we last collected.
  //
  // This is the primary data path, not a catch-up nicety: the ring buffers to
  // flash and the live notifications only exist while a connection happens to
  // be up. Anything measured while this phone was out of range, asleep, or
  // swiped away arrives here or not at all.
  //
  // The cursor is remembered across sessions and paired with the ring's erase
  // generation. If the ring has flushed since we last spoke, our offset refers
  // to records that no longer exist, so the only honest thing to do is start
  // from the beginning of the new generation -- the ring will say so anyway
  // (STALE), but checking here saves a round trip.
  Future<void> _requestBackfill() async {
    final c = _control;
    if (c == null) return;

    // **Per ring.** This pair used to live in two global preference slots, and
    // with a second ring that was not merely untidy: two rings never share an
    // erase generation, so each connect found the other's, correctly declared
    // the cursor stale, and restarted from 0 -- both rings re-downloading their
    // whole buffer on every reconnect, permanently, with the duplicate rows to
    // match. Reading it from the ring's own row is the fix.
    final row = await HistoryDb.instance.ring(ringId);
    _histCursor = row?.histCursor ?? 0;
    _histGeneration = row?.histGeneration ?? 0;

    if (_histGeneration != _ringGeneration) {
      _log0('history: generation $_histGeneration -> $_ringGeneration, '
          'restarting from 0');
      _histGeneration = _ringGeneration;
      _histCursor = 0;
    }
    if (_histCursor > _ringStored) {
      // The ring holds less than we think we have read. Same class of problem
      // as a stale generation, and the same answer.
      _log0('history: cursor $_histCursor past ring cursor $_ringStored, '
          'restarting from 0');
      _histCursor = 0;
    }
    if (_histCursor == _ringStored) {
      _log0('history: nothing buffered (cursor $_histCursor)');
      return;
    }

    try {
      final payload = ByteData(9)
        ..setUint8(0, _opReplay)
        ..setUint32(1, _ringGeneration, Endian.little)
        ..setUint32(5, _histCursor, Endian.little);
      await c.write(payload.buffer.asUint8List(), withoutResponse: false);
      _histReceived = 0;
      _setStatus('Collecting buffered readings...');
    } catch (e) {
      _log0('history: request failed ($e)');
    }
  }

  // Tell the ring it may reclaim the flash below our cursor.
  //
  // This is the only thing that frees space on the ring on purpose, so it is
  // sent only after the records are committed to this phone's database. Until
  // it arrives the ring keeps them, at the cost of lapping its buffer sooner --
  // which is the right way round: a full buffer drops the oldest records, an
  // early acknowledgement drops ones nobody has.
  Future<void> _ackHistory() async {
    final c = _control;
    if (c == null) return;
    try {
      final payload = ByteData(9)
        ..setUint8(0, _opAck)
        ..setUint32(1, _histGeneration, Endian.little)
        ..setUint32(5, _histCursor, Endian.little);
      await c.write(payload.buffer.asUint8List(), withoutResponse: false);
      _log0('history: acknowledged up to $_histCursor');
    } catch (e) {
      // Not fatal. The ring keeps the records and we acknowledge them after the
      // next transfer; nothing is lost by an acknowledgement that never lands.
      _log0('history: ack failed ($e)');
    }
  }

  Future<void> _saveHistoryCursor() async {
    await HistoryDb.instance.saveCursor(ringId, _histCursor, _histGeneration);
  }

  // Real time for a ring timestamp, or arrival time if the ring is not anchored.
  //
  // `ts` is uint32 milliseconds of ring uptime and wraps after ~49.7 days. The
  // ring reports its current uptime alongside the anchor so that wrap is
  // resolvable, but resolving it is deliberately not done here: the buffer only
  // ever spans hours, so a record from before a wrap cannot still be in it, and
  // code for a case that cannot arise is code that is never exercised. It is
  // written down in ARCHITECTURE.md instead.
  int _realTime(int ts, int arrivalMs) {
    final anchor = _ringEpochAtBoot;
    return anchor == null ? arrivalMs : anchor + ts;
  }

  void _onDisconnected() {
    for (final s in _valueSubs) {
      s.cancel();
    }
    _valueSubs.clear();

    // The anchor belongs to one connection. Keeping it across a disconnect
    // would survive a ring reset that zeroed the uptime it is an offset from,
    // and then quietly date everything wrong.
    _ringEpochAtBoot = null;

    final device = _device;
    if (!_autoConnect || device == null) {
      _setStatus('Disconnected.');
      return;
    }

    if (device.isAutoConnectEnabled) {
      // The standing request is still in place: the platform is holding the
      // gatt open and should reconnect by itself. Asking again here would leak
      // a second handle, so this branch only arms the watchdog -- "should"
      // turned out to be worth checking (see _retryLink).
      _startLinkWatchdog();
      _setStatus('Ring out of range — will reconnect automatically.');
      return;
    }

    // The connection we just lost was a direct one (the pairing connect), so
    // there is nothing standing. Arm auto-connect now, and this is the last
    // time a disconnect needs any action at all.
    _setStatus('Disconnected — arming auto-connect.');
    unawaited(_armAutoConnect(device));
  }

  // Stops auto-connect as well as dropping the link — otherwise the platform
  // would reconnect within seconds and the button would look broken. The ring
  // stays remembered, so Connect (or the next app launch) resumes; "Forget
  // ring" is what actually undoes the pairing.
  //
  // This is also the only thing that stops background monitoring. Nothing else
  // may: a service that stops itself is a monitor that goes quiet without
  // anyone deciding it should.
  Future<void> disconnect() async {
    // Read before awaiting: the state listener may clear it under us, and a
    // link that was never up has no "disconnected" of its own to report.
    final wasConnected = _wasConnected;
    _autoConnect = false;
    _stopLinkWatchdog();
    notifyListeners();
    await _device?.disconnect();
    if (!wasConnected) _setStatus('Auto-connect stopped.');
  }

  /// Puts this session's link down.
  ///
  /// `unbond` is the whole difference between switching a ring off and
  /// forgetting it: the teardown is identical, and removing the bond is the
  /// half that must not run when the ring is coming back.
  Future<void> _teardownLink({bool unbond = false}) async {
    _autoConnect = false;
    _stopLinkWatchdog();
    notifyListeners();
    // Stop watching first, so the teardown does not race a "Disconnected"
    // status over the one the caller sets at the end.
    _connSub?.cancel();
    _connSub = null;
    _wasConnected = false;
    _connectedAt = null;
    for (final s in _valueSubs) {
      s.cancel();
    }
    _valueSubs.clear();

    final device = _linkDevice();
    try {
      await device?.disconnect();
      if (unbond) await device?.removeBond();
    } catch (e) {
      _log0(unbond ? 'Removing the bond failed: $e' : 'Disconnect failed: $e');
    }
    _device = null;
    _control = null;
    notifyListeners();
  }

  // Which characteristics this app subscribes to. A UUID with no label here is
  // discovered and left alone, which is how the motion and Heart Rate
  // characteristics are declined -- see the notes on their UUIDs above.
  String? _labelFor(Guid uuid) {
    if (uuid == _uuidVitals) return 'vitals';
    if (uuid == _uuidHistory) return 'history';
    if (uuid == _uuidStatus) return 'status';
    if (uuid == _uuidBattery) return 'battery';
    return null;
  }

  // Append to a live series, drop anything that has aged out of the chart
  // window, and cap the length so the charts stay responsive.
  //
  // The window is applied here as well as in the query because a session can
  // outlive it: a phone left running for a week would otherwise keep every
  // point it had ever charted, and the axis would go back to being unreadable
  // without anyone changing anything.
  void _push(List<FlSpot> series, double x, double y) {
    series.add(FlSpot(x, y));
    final cutoff = kChartWindowStart.toDouble();
    series.removeWhere((p) => p.x < cutoff);
    if (series.length > kMaxPoints) series.removeAt(0);
  }

  // Every packet from the ring, of any kind, is evidence the link is alive.
  // That is what the notification reports, so it is recorded here rather than
  // only where readings are stored -- a refused window still proves the ring
  // is talking.
  void _sawPacket(int arrivalMs) {
    _lastRecordAt = arrivalMs;
  }

  // One 16-byte record, live or replayed, decoded and stored.
  //
  // Shared between the two paths on purpose: they carry the identical layout
  // (the firmware packs both through one encoder), so decoding them in two
  // places would be two chances to disagree about the same bytes.
  //
  // `live` controls only whether the tiles and charts move. A replayed record
  // is history -- it belongs in the database and on the chart by its own
  // timestamp, but it must not be presented as the wearer's current state.
  void _handleRecord(ByteData b, int off, int arrivalMs, {required bool live}) {
    final ts = b.getUint32(off, Endian.little);
    final bpm = b.getUint16(off + 4, Endian.little);
    final spo2 = b.getUint16(off + 6, Endian.little) / 10.0;
    final confidence = b.getUint16(off + 8, Endian.little); // 0-1000
    final perfusion =
        b.getUint16(off + 10, Endian.little); // parts per thousand
    // Steps since the *previous* record, not a running total -- see the note on
    // the two quantities above HistoryDb.insertVitals. These bytes have been
    // arriving since the record widened and were parsed nowhere until now, so
    // the app showed no steps at all once BLE_LIVE_STREAM went to 0 and the
    // motion packets that used to carry them stopped.
    final stepDelta = b.getUint16(off + 12, Endian.little);
    final flags = b.getUint8(off + 14);
    final refused = (flags & _flagRefused) != 0;
    // How many measured windows this record stands for. The firmware collapses
    // a run of identical consecutive refusals into one record and counts the
    // rest here, so `repeat` is 0 for an ordinary record and N for a run of
    // N+1 windows -- the same `+ 1` the ring's own RTT line applies.
    final windows = b.getUint8(off + 15) + 1;
    final at = _realTime(ts, arrivalMs);
    // Bits 5-7, on every record whether the window was believed or not. Counted
    // in windows rather than rows for the same reason the refusals are: one row
    // can stand for a whole collapsed run.
    final movement = (flags >> _flagMoveShift) & _flagMoveMask;
    _tally.movement[movement] += windows;

    // Whose reading this is. A record that arrives with no ring to attribute it
    // to is dropped rather than stored anonymously: an ownerless row is
    // indistinguishable from one belonging to the *other* ring, and this table
    // is the only record either ring leaves behind.
    // A refused window is the ring saying it looked and found nothing it would
    // vouch for. It is real data -- it is what distinguishes "not worn" from
    // "the ring has stopped" -- but it is not a reading, so it is stored with
    // no bpm or spo2 rather than charted as one.
    // The step delta rides every record including this one. The wearer walked
    // whether or not the optics could find a pulse -- the firmware makes the
    // same argument for sampling the IMU above its own confidence gate
    // (main.c, record_vitals) -- and a refusal is exactly when the ring is
    // most likely to be on a moving hand.
    if (refused) {
      var reason = (flags >> _flagReasonShift) & _flagReasonMask;
      if (reason == _flagReasonMovingLegacy) reason = _flagReasonMoving;
      _tally.refused[reason] += windows;
      // Stored, not only tallied. The tally is one line in a log buffer that
      // scrolls away; the row is what makes a worn day answerable a week later.
      unawaited(HistoryDb.instance.insertVitals(ringId, at, null, null,
          confidence: confidence,
          perfusion: perfusion,
          stepDelta: stepDelta,
          refusal: reason,
          movement: movement,
          windows: windows));
      return;
    }

    final bpmOk = _plausibleHr(bpm);
    final spo2Ok = _plausibleSpo2(spo2);
    if (!bpmOk && !spo2Ok) {
      _tally.implausible += windows;
      _log0('record dropped  t=$ts  bpm=$bpm  '
          'spo2=${spo2.toStringAsFixed(1)}%  (implausible)');
      // Dropped as a *reading*, still stored for its steps and its quality
      // figures. Returning outright here used to discard the delta with it,
      // which put a hole in the day's total for no better reason than the
      // pulse being unreadable in that window.
      unawaited(HistoryDb.instance.insertVitals(ringId, at, null, null,
          confidence: confidence,
          perfusion: perfusion,
          stepDelta: stepDelta,
          movement: movement,
          windows: windows));
      return;
    }

    // **The tiles follow the newest reading, whatever path it arrived by.**
    //
    // This used to be `if (live)`, and that was right while the ring streamed:
    // a replayed record is history, and a backfill of four days of buffer would
    // otherwise have marched the tile through every reading in it. What made it
    // wrong is firmware BLE_LIVE_STREAM going to 0 -- nothing is live any more,
    // so nothing ever set the tiles and the app sat showing whatever number was
    // last received before the change. Observed on 2026-08-06: the graph
    // correctly showed a replayed 68bpm while the tile still read 74 from
    // before the reflash.
    //
    // Comparing timestamps keeps what the `live` test was actually protecting.
    // A replay arrives oldest-first, so each record is newer than the last and
    // the tile ends on the most recent one -- which *is* the ring's latest
    // measurement. And a backfill that overlaps readings the phone already has
    // cannot drag the tile backwards.
    if (bpmOk) {
      if (_bpmAt == null || at >= _bpmAt!) {
        _bpm = bpm;
        _bpmAt = at;
      }
      _push(_hrSeries, at.toDouble(), bpm.toDouble());
    }
    if (spo2Ok && (_bpmAt == null || at >= _bpmAt!)) _spo2 = spo2;
    _tally.addAccepted(confidence, perfusion);
    _stored++;
    notifyListeners();
    // Quality travels with the reading rather than being recomputed later:
    // confidence cannot be reconstructed from a bpm after the fact, so a row
    // stored without it is a reading nobody can ever re-judge.
    // Movement rides the accepted rows too, and that is the point rather than
    // an afterthought: the question the buckets exist to answer is whether the
    // gate refuses windows it should have kept, and that cannot be asked of the
    // refusals alone. An accepted window at 50-99 mg is the control group.
    unawaited(HistoryDb.instance.insertVitals(
        ringId, at, bpmOk ? bpm : null, spo2Ok ? spo2 : null,
        confidence: confidence,
        perfusion: perfusion,
        stepDelta: stepDelta,
        movement: movement,
        windows: windows));
  }

  /// A condition on the ring changed that a person should be told about.
  ///
  /// The session decides nothing here beyond what it now believes; raising an
  /// actual notification is the fleet's job, because a notification is a
  /// property of the phone and there is more than one ring.
  void _onAlert(int reason) {
    switch (reason) {
      case _alertNotWorn:
        _wornNow = false;
        _log0('alert: ring came off');
        break;
      case _alertWorn:
        _wornNow = true;
        _log0('alert: ring back on');
        break;
      case _alertBatteryLow:
        _powerState = RingPower.low;
        // Persisted with the level, because the ring's own verdict is the half
        // of the record the phone cannot re-derive: the hysteresis behind it
        // lives in the firmware, and a state that did not survive an app
        // restart would silently downgrade a critical ring to ok.
        _persistBattery();
        _log0('alert: battery low — the ring is measuring less often');
        break;
      case _alertBatteryCritical:
        _powerState = RingPower.critical;
        _persistBattery();
        _log0('alert: battery critical — the ring has stopped measuring');
        break;
      case _alertBatteryOk:
        _powerState = RingPower.ok;
        _persistBattery();
        _log0('alert: battery recovered');
        break;
      default:
        // An unknown reason is a newer firmware talking to an older app. Say
        // so rather than dropping it -- the wire format note in ble.c promises
        // exactly this is survivable, and a silent drop would make a real
        // alert indistinguishable from one that was never sent.
        _log0('alert: unknown reason $reason (firmware is ahead of this app)');
        return;
    }
    onAlert?.call(ringId, reason);
    notifyListeners();
  }

  // The buffered log arriving in batches. See ble.c for the framing.
  void _onHistory(List<int> value, int arrivalMs) {
    if (value.isEmpty) return;
    final b = ByteData.sublistView(Uint8List.fromList(value));

    switch (value[0]) {
      case _histHeader:
        if (value.length < 13) return;
        final gen = b.getUint32(1, Endian.little);
        final from = b.getUint32(5, Endian.little);
        final end = b.getUint32(9, Endian.little);
        _histReceived = 0;
        _tally.reset();
        // A `from` above what we asked for means the ring's buffer lapped while
        // this phone was away and that stretch is gone for good. Worth saying
        // plainly: it is a hole in the wearer's history, not a hiccup.
        if (from > _histCursor) {
          _log0('history: WARNING ${(from - _histCursor) ~/ _recordSize} '
              'record(s) lost before we reconnected');
          _histCursor = from;
        }
        // The ring's cursor as of this request. Kept current here and on END
        // because _requestBackfill() measures "is there anything to collect"
        // against it, and a value left over from connect makes that question
        // unanswerable -- see the note on _histEvent below.
        _ringStored = end;
        // Kept so END can check that everything between the two arrived.
        _histFrom = _histCursor;
        _log0('history: transfer starting, gen $gen, bytes $from..$end '
            '(${(end - from) ~/ _recordSize} records)');
        break;

      case _histData:
        final n = (value.length - 1) ~/ _recordSize;
        for (var i = 0; i < n; i++) {
          _handleRecord(b, 1 + i * _recordSize, arrivalMs, live: false);
        }
        _histReceived += n;
        break;

      case _histEnd:
        if (value.length < 5) return;
        final endOffset = b.getUint32(1, Endian.little);

        // **Never acknowledge a transfer that arrived short.**
        //
        // Acknowledging is not a receipt, it is permission to erase: the ring
        // reclaims and erases every whole page below the offset we send. So an
        // acknowledgement of records that never arrived destroys them, and
        // nothing anywhere would report it.
        //
        // That is not hypothetical. On 2026-08-06 a 252-record transfer
        // delivered 234 -- exactly six DATA frames missing -- and this code
        // acknowledged all 252 because END said so. The ring reclaimed the page
        // and 18 readings ceased to exist. No error was logged on either side:
        // the ring's replay cursor had reached the end, so it believed it had
        // delivered them, and BLE notifications are unacknowledged by
        // definition, so it could not have known otherwise. Small transfers had
        // always reconciled exactly; it took an 84-frame burst to lose any.
        //
        // The loss is above the controller -- the link layer retransmits, so
        // the bytes reached the phone's radio -- which means **only this side
        // can detect it.** Comparing what we received against what the ring
        // said it sent is the whole defence.
        //
        // What it cannot do is acknowledge the part that *did* arrive: DATA
        // frames carry no offset, so a short transfer tells us that records are
        // missing but not which. Re-reading the whole range is the price of
        // that, and re-reading is always safe -- the ring still holds it all.
        final expected = (endOffset - _histFrom) ~/ _recordSize;
        if (_histReceived < expected) {
          _log0('history: WARNING transfer short -- got $_histReceived of '
              '$expected record(s), NOT acknowledging');
          if (_histShortRetries < _histShortRetryMax) {
            _histShortRetries++;
            _log0('history: re-reading from $_histFrom '
                '(attempt $_histShortRetries of $_histShortRetryMax)');
            unawaited(_requestBackfill());
          } else {
            // The ring keeps everything it has not been told to erase, so
            // stopping here costs nothing but time.
            _log0('history: still short after $_histShortRetries retries -- '
                'leaving it with the ring until the next nudge');
            _histShortRetries = 0;
          }
          break;
        }
        _histShortRetries = 0;
        _histCursor = endOffset;
        // A transfer delivers up to the ring's *live* cursor, so this can be
        // past the `end` the header quoted -- records appended while it ran.
        // Without this the next backfill would see _histCursor > _ringStored,
        // read it as a cursor from a dead generation, and restart from 0,
        // re-downloading the entire buffer.
        if (_histCursor > _ringStored) {
          _ringStored = _histCursor;
        }
        // Persist *before* acknowledging. If the app dies between the two, the
        // worst case is receiving these records twice -- harmless, and the
        // unique index on (ring_id, at) makes it invisible. Acknowledging first and then
        // failing to save would tell the ring to erase records this phone
        // cannot prove it kept, which is the failure that actually loses data.
        // **The acknowledgement is conditional on the save, and the catch is
        // the point.** `.then(...)` alone already ordered these correctly, but
        // a *failed* save still ran the ack, because nothing was watching for
        // one. On 2026-08-07 the database would not open, every write was a
        // silent no-op, and this line told the ring it could erase 85 minutes
        // of readings the phone had not kept. Acknowledging is permission to
        // destroy; it must never outrun the thing it is vouching for.
        unawaited(_saveHistoryCursor().then((_) => _ackHistory()).catchError((e) {
          _log0('history: NOT acknowledging — the cursor could not be saved '
              '($e). The ring keeps these records.');
        }));
        // The day totals are only meaningful once the transfer's rows are all
        // in, so they are recomputed here rather than per record.
        unawaited(_refreshSteps());
        _log0('history: transfer complete, $_histReceived records, '
            'cursor now $_histCursor');
        // The quality of what just arrived, which until now went nowhere a
        // person could read it. Logged after the completion line so the two
        // read as one event, and only when the transfer actually carried
        // records -- an empty collection has nothing to say about yield.
        final quality = _tally.describe();
        if (quality != null) _log0('history: $quality');
        _setStatus('Streaming. Collected $_histReceived buffered readings.');
        break;

      case _histAlert:
        if (value.length < 2) return;
        _onAlert(value[1]);
        break;

      case _histEvent:
        // The ring saw something worth looking at and wants the buffer read
        // now rather than at our next connection.
        //
        // Take the cursor off the event itself. `_ringStored` is otherwise set
        // exactly once, by the control read at connect, and _requestBackfill()
        // refuses to ask for anything when our cursor has caught up with it --
        // so every nudge after the first backfill was answered with
        // "history: nothing buffered" and no REPLAY was ever sent. The ring
        // would report the excursion, flush, notify, and wait for a request
        // that could not arrive; nothing was ever acknowledged again for the
        // life of the connection.
        //
        // The firmware puts the live cursor in this frame for exactly this
        // reason (HISTORY_TYPE_EVENT is {u8, u32 cursor}, ble.c) and it was
        // being parsed nowhere.
        if (value.length >= 5) {
          _ringStored = b.getUint32(1, Endian.little);
        }
        _log0('history: ring reported an event, collecting '
            '(ring holds $_ringStored bytes)');
        unawaited(_requestBackfill());
        break;

      case _histStale:
        if (value.length < 5) return;
        // Our offset belonged to an erase generation that no longer exists.
        // Reset and try again from the start of the new one.
        _histGeneration = b.getUint32(1, Endian.little);
        _ringGeneration = _histGeneration;
        _histCursor = 0;
        unawaited(_saveHistoryCursor()
            .catchError((e) => _log0('history: cursor reset not saved ($e)')));
        _log0('history: cursor stale, ring is on generation $_histGeneration');
        unawaited(_requestBackfill());
        break;
    }
  }

  // ---- Packet parsing (all little-endian, matching the firmware) -------------
  void _onData(String label, List<int> value) {
    final b = ByteData.sublistView(Uint8List.fromList(value));
    final now = DateTime.now().millisecondsSinceEpoch;
    _sawPacket(now);
    switch (label) {
      // {u32 ts, u16 bpm, u16 spo2_tenths, u16 confidence, u16 perfusion_milli,
      //  u16 steps, u8 flags, u8 repeat} -- byte-for-byte the ring's flash
      // record (firmware flash_store.c). It was 8 bytes up to the widening;
      // the trailing fields are the evidence behind the reading, which the
      // ring used to compute and throw away.
      case 'vitals':
        if (value.length < _recordSize) return;
        _handleRecord(b, 0, now, live: true);
        _log0('vitals  t=${b.getUint32(0, Endian.little)}  '
            'bpm=${b.getUint16(4, Endian.little)}  '
            'conf=${(b.getUint16(8, Endian.little) / 10).toStringAsFixed(0)}%  '
            'pi=${(b.getUint16(10, Endian.little) / 10).toStringAsFixed(1)}%');
        break;

      case 'history':
        _onHistory(value, now);
        break;

      case 'battery': // Battery Level: single percent byte
        if (value.isEmpty) return;
        _noteBattery(value[0]);
        _log0('battery ${value[0]}%');
        break;

      case 'status': // {u32 ts, u8 percent, u16 mV, u32 steps}
        if (value.length < 11) return;
        final ts = b.getUint32(0, Endian.little);
        final pct = b.getUint8(4);
        final mv = b.getUint16(5, Endian.little);
        _noteBattery(pct, millivolts: mv);
        notifyListeners();
        // The packet's trailing u32 is the ring's free-running step total since
        // its last boot, and it is **read past, not stored**. Daily totals come
        // from the per-record `stepDelta` in the history stream; that figure is
        // bucketed by the wearer's calendar day and survives the ring
        // rebooting, and a since-boot counter is neither. Putting two different
        // quantities under one word on screen is how a step count stops meaning
        // anything.
        _log0('status  t=$ts  battery=$pct% (${mv}mV)');
        break;
    }
  }

  /// Clears the active ring's history, not everyone's. Wiping the ring you are
  /// looking at should not take the other one's readings with it.
  Future<void> clearHistory() async {
    await HistoryDb.instance.clear(ringId);
    _hrSeries.clear();
    _stored = 0;
    // Back to seven empty days rather than a stale total for rows that no
    // longer exist.
    await _refreshSteps();
    notifyListeners();
    _log0('Cleared stored history.');
  }

  // ---- Permissions -----------------------------------------------------------
  // Split into a check and a request, because this now runs with no Activity
  // attached -- at boot, and after the app has been swiped away. A permission
  // *request* in that state cannot show a dialog and only ever resolves to
  // denied, which would turn a reconnect into a permanent "Permissions denied"
  // for a user who granted them months ago. So: check always, ask only when
  // there is a screen to ask on.
  Future<bool> _ensurePermissions() async {
    if (await Permission.bluetoothScan.isGranted &&
        await Permission.bluetoothConnect.isGranted) {
      return true;
    }
    if (!_hasUi) {
      _log0('permissions: not granted and no UI to ask on — '
          'open the app once to restore monitoring');
      return false;
    }
    return requestPermissions();
  }

  bool get _hasUi =>
      WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;

  /// The interactive half. Also asks for notifications, because on Android 13+
  /// a denied POST_NOTIFICATIONS makes the foreground service invisible -- the
  /// service still runs, but the one always-on indicator that monitoring is
  /// alive is gone, and silence is exactly what must not look like health.
  Future<bool> requestPermissions() async {
    // On Android 12+ scan/connect are the relevant ones; on older Android the
    // plugin maps these to legacy Bluetooth + location. Requesting all of them
    // is harmless and covers both.
    final results = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
      Permission.notification,
    ].request();
    // Scan + connect are the ones we truly need; a denied location permission
    // (unneeded under neverForLocation on Android 12+) should not block us, and
    // neither should a denied notification.
    return (results[Permission.bluetoothScan]?.isGranted ?? false) &&
        (results[Permission.bluetoothConnect]?.isGranted ?? false);
  }
}
