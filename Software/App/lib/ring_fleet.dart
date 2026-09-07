// Every ring this phone is talking to, at once.
//
// **What lives here rather than in RingLink, and why.** A session
// ([RingLink]) owns one ring's connection, replay cursor and readings, and
// knows nothing about how many others exist. Four things are properties of the
// *phone* instead, and duplicating any of them per ring would be wrong rather
// than merely wasteful:
//
//   - **The foreground service.** Android gives a process one persistent
//     notification, not one per peripheral. Two sessions each starting and
//     stopping it would race, and the loser would silently stop monitoring for
//     both -- the one failure this design cannot tolerate, because monitoring
//     that is dead and monitoring that is alive look identical from outside.
//   - **Scanning.** FlutterBluePlus has one scanner. Two concurrent scans is
//     not two scans.
//   - **The log.** With two rings the interesting question is nearly always
//     what happened *between* them, and two buffers to interleave by eye is
//     the wrong tool for that.
//   - **Focus.** The Live tiles and the charts show one ring; that is a
//     property of the screen, and deliberately *not* the same thing as which
//     rings are connected. Both rings stream whether or not you are looking.

import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart'
    show WidgetsBinding, WidgetsBindingObserver, AppLifecycleState;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'history_db.dart';
import 'monitor_service.dart';
import 'ring_link.dart';

const _deviceName = 'SenseRing';

// How often the service notification is re-labelled. It is the only
// always-visible surface this system has, and "last reading 3 min ago" is the
// one fact worth putting on it: an app that has quietly stopped collecting is
// otherwise indistinguishable from one that is working.
const _notificationRefresh = Duration(seconds: 60);

class RingFleet extends ChangeNotifier with WidgetsBindingObserver {
  RingFleet._();
  static final RingFleet instance = RingFleet._();

  final Map<String, RingLink> _sessions = {};
  final List<String> _log = [];
  List<Ring> _rings = const [];
  Map<String, int> _counts = const {};

  String? _focusId;
  String? _dbError;
  bool _started = false;
  bool _busy = false;
  bool _monitoring = false;
  Timer? _notificationTimer;
  String? _notificationText;
  StreamSubscription<List<ScanResult>>? _scanSub;
  bool _askedForNotifications = false;

  // ---- What the UI reads -------------------------------------------------

  List<String> get log => _log;
  List<Ring> get rings => _rings;
  Map<String, int> get counts => _counts;
  bool get busy => _busy;
  bool get monitoring => _monitoring;

  /// Set when the readings database could not be opened. Nothing works in that
  /// state and the UI says so rather than looking idle.
  String? get dbError => _dbError;
  int get connectedCount => _sessions.values.where((s) => s.connected).length;
  Iterable<RingLink> get sessions => _sessions.values;

  RingLink? session(String id) => _sessions[id];

  /// The ring the Live and Graphs tabs are showing.
  ///
  /// Falls back to any connected session, then to any session at all, so the
  /// screen is never blank while a ring is streaming into it.
  RingLink? get focus {
    final pinned = _focusId == null ? null : _sessions[_focusId];
    if (pinned != null) return pinned;
    for (final s in _sessions.values) {
      if (s.connected) return s;
    }
    return _sessions.values.isEmpty ? null : _sessions.values.first;
  }

  String? get focusId => focus?.ringId;

  String labelFor(String id) {
    for (final r in _rings) {
      if (r.id == id) return r.label;
    }
    return id;
  }

  void setFocus(String id) {
    _focusId = id;
    notifyListeners();
  }

  // ---- Startup -----------------------------------------------------------

  /// Brings up a session for every enabled ring. This is what the boot path
  /// runs, so it must work with no UI attached.
  Future<void> start() async {
    if (_started) return;
    _started = true;
    _log0('App started.');

    // Registered here rather than in the constructor: this is a lazily-created
    // singleton, and the binding it attaches to does not exist until
    // WidgetsFlutterBinding.ensureInitialized() has run. start() is called from
    // the app, so by definition it has.
    //
    // Never removed. The fleet lives for the process, so there is no dispose to
    // pair this with -- and an observer that outlives its owner is only a leak
    // if the owner can go away.
    WidgetsBinding.instance.addObserver(this);

    // **A database that will not open stops everything here, loudly.** It used
    // to be fatal in silence: init() swallowed the failure, every write became
    // a no-op, and the app went on collecting readings, storing none, and
    // telling the ring it could erase them. Refusing to arm a single session is
    // the only safe response -- a link with nowhere to put its readings is
    // worse than no link, because it destroys the ring's copy too.
    try {
      await HistoryDb.instance.init();
    } catch (e) {
      _dbError = '$e';
      _log0('FATAL: the readings database will not open ($e). '
          'No ring will be connected — reinstalling the app would discard the '
          'readings already stored, so this needs looking at first.');
      notifyListeners();
      return;
    }
    await _reloadRings();

    // Nothing known and nothing enabled: a ring bonded before the app kept a
    // list, or paired from the system Bluetooth settings. The OS bond list is
    // itself the record.
    if (_rings.isEmpty) {
      for (final id in await _bondedRingIds()) {
        await HistoryDb.instance.rememberRing(id);
      }
      await _reloadRings();
    }

    await _syncSessions();
    if (_sessions.isEmpty) {
      _log0('No ring paired yet — press Connect.');
    } else {
      await _startMonitoring();
    }
    notifyListeners();
  }

  Future<void> _reloadRings() async {
    _rings = await HistoryDb.instance.rings();
    _counts = await HistoryDb.instance.countsByRing();
  }

  /// Makes the live sessions match the enabled rows: starts what is missing,
  /// shuts down what is no longer wanted.
  Future<void> _syncSessions() async {
    final wanted = {for (final r in _rings.where((r) => r.enabled)) r.id};

    for (final id in _sessions.keys.toList()) {
      if (!wanted.contains(id)) {
        await _sessions.remove(id)!.shutdown();
        _log0('link: ${labelFor(id)} disconnected');
        // Its conditions are no longer being reported, so its alerts must not
        // stay up saying otherwise.
        unawaited(_refreshAlerts());
      }
    }
    for (final id in wanted) {
      if (_sessions.containsKey(id)) continue;
      final s = RingLink(id)
        ..onLog = _sessionLog
        ..onAlert = _onAlert
        // One listener per session, forwarded on. The UI listens to the fleet
        // and never to a session directly, so a ring appearing or going away
        // does not mean rewiring every widget that was watching.
        ..addListener(_onSessionChanged);
      _sessions[id] = s;
      await s.start();
    }
    _focusId ??= _sessions.keys.isEmpty ? null : _sessions.keys.first;
  }

  /// Turns a ring's alert into something the wearer actually sees.
  ///
  /// **The fleet owns this, not the session.** A notification belongs to the
  /// phone: two rings both off a finger is one thing to tell somebody, and one
  /// notification per ring per condition is how a tray becomes something people
  /// clear without reading. So the state is collected across every session and
  /// the notification describes the fleet.
  ///
  /// Alerts are also *withdrawn* when their condition clears, which matters
  /// more than raising them. A battery warning still sitting there an hour
  /// after the ring was charged teaches the wearer that these do not mean
  /// anything.
  void _onAlert(String ringId, int reason) {
    unawaited(_refreshAlerts());
  }

  Future<void> _refreshAlerts() async {
    // Wear. Only rings that have actually said which way they are count --
    // `wornNow` is null until the ring has reported, and "has not said" must
    // not be presented as "came off".
    final off = _sessions.values.where((s) => s.wornNow == false).toList();
    if (off.isEmpty) {
      await MonitorService.clearAlert(MonitorService.alertWear);
    } else {
      final who = off.map((s) => labelFor(s.ringId)).join(', ');
      await MonitorService.alert(
        MonitorService.alertWear,
        off.length == 1 ? 'Ring is not being worn' : 'Rings are not being worn',
        '$who reported no finger. No readings are being taken until it is back on.',
      );
    }

    // Battery, most serious first. The three mean different things: low is
    // "measuring less often", critical is "not measuring at all, and the buffer
    // is at risk", and flat is "we think it is already gone".
    //
    // **Flat is the phone's own conclusion, not the ring's.** A ring that has
    // actually run out said nothing on the way down -- it stops measuring at 5%
    // and browns out somewhere below that -- so there is no alert coming and
    // this is the only thing that will ever raise one. See RingLink.battery.
    final flat = _sessions.values.where((s) => s.battery?.flat ?? false).toList();
    final critical = _sessions.values
        .where((s) => s.powerState == RingPower.critical && !flat.contains(s))
        .toList();
    final low = _sessions.values
        .where((s) => s.powerState == RingPower.low && !flat.contains(s))
        .toList();

    if (flat.isNotEmpty) {
      final who = flat.map((s) => labelFor(s.ringId)).join(', ');
      await MonitorService.alert(
        MonitorService.alertBattery,
        flat.length == 1 ? 'Ring has run out of charge' : 'Rings have run out',
        // Hedged on purpose. The ring cannot confirm this and will not be able
        // to until it is charged, so stating it flatly would be claiming a
        // reading nobody took -- but a wearer who does nothing because the
        // wording was too careful is the worse outcome of the two.
        '$who has not been heard from for long enough to have emptied. '
            'Nothing is being recorded until it is charged.',
      );
    } else if (critical.isNotEmpty) {
      final who = critical.map((s) => labelFor(s.ringId)).join(', ');
      await MonitorService.alert(
        MonitorService.alertBattery,
        'Ring battery critical',
        '$who has stopped measuring to save what it has already recorded. '
            'Charge it now — anything not yet collected is at risk.',
      );
    } else if (low.isNotEmpty) {
      final who = low.map((s) => labelFor(s.ringId)).join(', ');
      await MonitorService.alert(
        MonitorService.alertBattery,
        'Ring battery low',
        '$who is measuring less often to stretch the charge.',
      );
    } else {
      await MonitorService.clearAlert(MonitorService.alertBattery);
    }
  }

  void _onSessionChanged() {
    _refreshNotification();
    notifyListeners();
  }

  // ---- The ring list -----------------------------------------------------

  /// Connects or disconnects one ring, leaving every other alone.
  Future<void> setEnabled(String id, bool enabled) async {
    await HistoryDb.instance.setRingEnabled(id, enabled);
    await _reloadRings();
    await _syncSessions();
    if (_sessions.isEmpty) {
      await _stopMonitoring();
    } else {
      await _startMonitoring();
    }
    notifyListeners();
  }

  Future<void> renameRing(String id, String label) async {
    await HistoryDb.instance.renameRing(id, label);
    await _reloadRings();
    notifyListeners();
  }

  /// Drops a ring and unbonds it. **Its readings stay** -- they were real
  /// measurements, and unpairing does not undo them.
  Future<void> forgetRing(String id) async {
    final s = _sessions.remove(id);
    if (s != null) {
      await s.shutdown(unbond: true);
      s.removeListener(_onSessionChanged);
      s.dispose();
    }
    await HistoryDb.instance.forgetRing(id);
    if (_focusId == id) _focusId = null;
    await _reloadRings();
    await _syncSessions();
    _log0('Ring forgotten.');
    if (_sessions.isEmpty) await _stopMonitoring();
    notifyListeners();
  }

  Future<void> refresh() async {
    await _reloadRings();
    notifyListeners();
  }

  // ---- Pairing a new ring ------------------------------------------------

  /// Scans for an unpaired ring and bonds it.
  ///
  /// Only rings this phone does not already know are offered. Without that
  /// filter the scan reliably finds the ring already connected -- they all
  /// advertise the same name -- and "Connect" would appear to do nothing.
  Future<void> connectNew() async {
    if (_busy) return;
    _busy = true;
    notifyListeners();
    // Through the same lock the sessions' watchdogs use: pairing a new ring
    // while an existing one is scanning to recover would cancel its scan.
    try {
      await ScanLock.run(_connectNewLocked);
    } finally {
      _busy = false;
      await _reloadRings();
      notifyListeners();
    }
  }

  Future<void> _connectNewLocked() async {
    try {
      if (!await _ensurePermissions()) {
        _log0('Permissions denied — cannot scan.');
        return;
      }
      if (await FlutterBluePlus.adapterState.first !=
          BluetoothAdapterState.on) {
        _log0('Bluetooth is off — turn it on and retry.');
        return;
      }

      final known = {for (final r in _rings) r.id};
      _log0('Scanning for a new "$_deviceName"...');

      final found = Completer<BluetoothDevice?>();
      await _scanSub?.cancel();
      _scanSub = FlutterBluePlus.onScanResults.listen((results) {
        for (final r in results) {
          if (!known.contains(r.device.remoteId.str) && !found.isCompleted) {
            found.complete(r.device);
          }
        }
      });

      await FlutterBluePlus.startScan(
        withNames: [_deviceName],
        timeout: const Duration(seconds: 15),
      );
      final device = await found.future
          .timeout(const Duration(seconds: 17), onTimeout: () => null);
      await FlutterBluePlus.stopScan();
      await _scanSub?.cancel();
      _scanSub = null;

      if (device == null) {
        _log0(known.isEmpty
            ? 'No ring found. Is it powered and in range?'
            : 'No *new* ring found — every ring in range is already paired.');
        return;
      }

      final id = device.remoteId.str;
      await HistoryDb.instance.rememberRing(id);
      await _reloadRings();
      await _syncSessions();
      await _startMonitoring();

      // Pair over a direct connect: auto-connect rides the system's background
      // scan and is deliberately slow to fire, and this is the one moment the
      // wearer is watching.
      await _sessions[id]?.onFound(device);
      _focusId = id;
    } catch (e) {
      _log0('Scan failed: $e');
    }
  }

  Future<List<String>> _bondedRingIds() async {
    try {
      return [
        for (final d in await FlutterBluePlus.bondedDevices)
          if (d.platformName == _deviceName) d.remoteId.str
      ];
    } catch (e) {
      _log0('Bonded-device list unavailable: $e');
      return const [];
    }
  }

  Future<bool> _ensurePermissions() async {
    final scan = await Permission.bluetoothScan.request();
    final connect = await Permission.bluetoothConnect.request();
    return scan.isGranted && connect.isGranted;
  }

  // ---- Logging -----------------------------------------------------------

  // Sessions prefix their lines with the ring's label, because with two rings
  // an unattributed "transfer short" is not a diagnosis.
  void _sessionLog(String ringId, String msg) =>
      _log0('[${labelFor(ringId)}] $msg');

  void _log0(String msg) {
    final t = DateTime.now();
    String two(int n) => n.toString().padLeft(2, '0');
    _log.insert(
        0, '${two(t.hour)}:${two(t.minute)}:${two(t.second)}  $msg');
    if (_log.length > 400) _log.removeLast();
    notifyListeners();
  }

  // ---- Background monitoring ---------------------------------------------
  // The foreground service is what keeps this process -- and therefore every
  // link -- alive once the app is off screen. Started while any ring is wanted,
  // and stopped only when none is, because "stopped monitoring" must never be
  // something the app decides quietly.

  Future<void> _startMonitoring() async {
    final text = _notificationLine();
    // A refused start is not proof the service is down: at boot the receiver
    // starts it natively and this call can land in the same moment, from a
    // process Android will not let start one. Ask before believing the answer.
    final ok =
        await MonitorService.start(text) || await MonitorService.isRunning();
    _notificationText = ok ? text : null;
    if (ok != _monitoring) {
      _monitoring = ok;
      notifyListeners();
    }
    if (ok) {
      _notificationTimer ??= Timer.periodic(_notificationRefresh, (_) => _tick());
      await _ensureNotificationsVisible();
    } else {
      _log0('background: foreground service not started (refused by Android '
          '-- open the app to restore background monitoring)');
    }
  }

  Future<void> _stopMonitoring() async {
    _notificationTimer?.cancel();
    _notificationTimer = null;
    _notificationText = null;
    await MonitorService.stop();
    if (_monitoring) {
      _monitoring = false;
      notifyListeners();
    }
  }

  // Asked for here rather than only alongside the Bluetooth permissions,
  // because an install that already has those never reaches that prompt -- and
  // then the service runs with its notification suppressed, which is the one
  // outcome this design cannot tolerate: monitoring that is alive but has no
  // way of saying so, and monitoring that is dead, look identical.
  Future<void> _ensureNotificationsVisible() async {
    if (_askedForNotifications || !_hasUi) return;
    if (await Permission.notification.isGranted) return;
    _askedForNotifications = true;
    final result = await Permission.notification.request();
    if (!result.isGranted) {
      _log0('background: notifications are blocked — monitoring runs, but '
          'nothing on screen will say so');
    }
    notifyListeners();
  }

  /// The app came back to the foreground: ask every connected ring for
  /// whatever it has been holding.
  ///
  /// **This is the only path that treats a person looking at their phone as a
  /// reason to collect.** Everything else is either the ring deciding it has
  /// waited long enough -- an excursion, a backlog over BACKLOG_NUDGE_HIGH_PCT,
  /// the ten-minute uncollected backstop -- or this app connecting and asking
  /// once. On a link that stays up for hours, none of them fires when the app
  /// is opened, so the screen showed readings as old as the last nudge.
  ///
  /// The ring has a matching backstop from 2026-08-24: a client that subscribes
  /// and does not ask within two seconds is offered the backlog unprompted
  /// (`OFFER_AFTER_SUBSCRIBE_MS`, `ble.c`). That covers a *cold* open, where the
  /// app connects and subscribes. It cannot cover this one -- a foregrounded app
  /// that never dropped its subscription produces no BLE event at all, so from
  /// the ring's side an app in a hand and an app in a pocket are identical.
  ///
  /// Sessions that are not connected drop out inside collectNow().
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.resumed) return;
    for (final s in _sessions.values) {
      unawaited(s.collectNow());
    }
  }

  bool get _hasUi =>
      WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;

  /// One line for however many rings there are.
  ///
  /// With one ring it reads exactly as it did before. With several it leads
  /// with the count, because "Connected · last reading 2 min ago" next to two
  /// paired rings does not say *which* — and a wear test where one ring quietly
  /// stopped an hour ago is precisely what this line exists to catch.
  String _notificationLine() {
    if (_sessions.isEmpty) return 'No ring connected';

    if (_sessions.length == 1) {
      final s = _sessions.values.first;
      final link = s.connected
          ? 'Connected'
          : s.autoConnect
              ? 'Waiting for the ring'
              : 'Not connected';
      final at = s.lastRecordAt;
      if (at == null) return '$link · no readings yet';
      return '$link · last reading '
          '${_ago(DateTime.now().millisecondsSinceEpoch - at)}';
    }

    final live = connectedCount;
    final parts = <String>[];
    for (final s in _sessions.values) {
      final at = s.lastRecordAt;
      parts.add('${labelFor(s.ringId)} '
          '${at == null ? 'no readings' : _ago(DateTime.now().millisecondsSinceEpoch - at)}');
    }
    return '$live/${_sessions.length} connected · ${parts.join(' · ')}';
  }

  static String _ago(int ms) {
    final minutes = ms ~/ 60000;
    if (minutes < 1) return 'just now';
    if (minutes < 60) return '$minutes min ago';
    final hours = minutes ~/ 60;
    if (hours < 24) return '${hours}h ago';
    return '${hours ~/ 24}d ago';
  }

  /// The once-a-minute heartbeat, and the only thing that moves while the rings
  /// are silent.
  ///
  /// **Everything else in this class is driven by a packet arriving, and a
  /// cell running flat is defined by packets not arriving.** So the battery
  /// estimate advances on the clock instead: without this tick a ring that went
  /// quiet at 12% would sit at 12% on the tile and raise nothing, no matter how
  /// many hours passed, which is the entire bug. A minute is far finer than the
  /// half hour a gauge point takes, so the number never jumps.
  void _tick() {
    _refreshNotification();
    unawaited(_refreshAlerts());
    notifyListeners();
  }

  void _refreshNotification() {
    if (!_monitoring) return;
    final text = _notificationLine();
    if (text == _notificationText) return; // most refreshes change nothing
    _notificationText = text;
    unawaited(MonitorService.update(text));
  }

  /// Stops background monitoring and every link. The only thing that may --
  /// a service that stops itself is a monitor that goes quiet without anyone
  /// deciding it should.
  Future<void> stopEverything() async {
    for (final s in _sessions.values) {
      await s.disconnect();
    }
    await _stopMonitoring();
    notifyListeners();
  }
}
