// Persistent history (SQLite).
//
// One flat table of readings, written from the ring's buffered log: each row is
// {at, bpm, spo2} plus the evidence behind it -- confidence, perfusion, step
// delta, refusal reason, movement bucket. The charts query one column each,
// newest last.
//
// **Nothing leaves this file.** The database is the whole of the app's storage;
// there is no upload path and no backend. A fork that wants one has a queue
// ready-made -- rows are never deleted and `id` only ever increases -- but
// building it is an explicit act, not a switch to flip.
//
// A second, tiny `prefs` table holds the things that have to outlive the
// process for the link to resume on its own: which ring to wait for, and how
// much of its buffer this phone has already collected.
//
// Note there is a *second* preference store on Android, in SharedPreferences
// (see MonitorChannel.kt): the one flag the boot receiver reads to decide
// whether to restart monitoring. It lives there because it has to be readable
// from native code before this database — or Dart at all — has opened.

import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/foundation.dart' show debugPrint;
import 'package:path/path.dart' as p;
import 'package:sqflite/sqflite.dart';

// How many recent points each chart holds in memory / loads on startup.
const kMaxPoints = 600;

// How far back the charts look. The database keeps everything -- rows are never
// deleted -- but a trend line over every reading ever
// taken is not a trend line: at twelve days the axis could only label whole
// days, and every label read the same. Three days is the span where a night is
// still visible as a night.
const kChartWindow = Duration(days: 3);

int get kChartWindowStart =>
    DateTime.now().subtract(kChartWindow).millisecondsSinceEpoch;

// **These three are legacy.** They described a phone that could only ever know
// one ring, and v4 moved all three into the `rings` table -- see `_addRings`.
// They survive only so the migration can read what the old install left behind.
//
// The cursor pair in particular was actively broken the moment a second ring
// existed. Two rings have different erase generations by construction (the
// value is random per reset), so with one shared slot they took turns
// invalidating each other: ring A stores its generation, ring B connects, sees
// a generation that is not its own, correctly concludes the cursor is stale,
// restarts from 0 and overwrites the slot -- and then A does the same on its
// next connect. Neither ring could ever hold a valid cursor, so both
// re-downloaded their entire buffer on every reconnect, forever.
const kPrefRingId = 'ring_id';
const kPrefHistCursor = 'hist_cursor';
const kPrefHistGeneration = 'hist_generation';

/// One calendar day's step total.
///
/// [steps] is null when the day has no step data at all, which is a different
/// statement from 0 and is displayed differently. See [HistoryDb.dailySteps].
class DailySteps {
  const DailySteps(this.day, this.steps);

  /// Local midnight starting the day.
  final DateTime day;
  final int? steps;

  bool get hasData => steps != null;
}

/// One ring this phone knows about.
class Ring {
  Ring({
    required this.id,
    required this.label,
    this.enabled = false,
    this.histCursor = 0,
    this.histGeneration = 0,
    this.lastSeen,
    this.battPct,
    this.battMv,
    this.battAt,
    this.battState = 0,
  });

  /// The BLE address. Stable across resets, unlike the erase generation, which
  /// is exactly why identity hangs off this and not off anything the firmware
  /// reports about its log.
  final String id;
  final String label;

  /// Should this ring be connected. **Any number of rows may be true**: the
  /// app holds a session per ring, so this is one switch per ring rather than
  /// a choice between them. It was briefly an at-most-one flag, which is why
  /// the setter is [HistoryDb.setRingEnabled] and not a picker.
  final bool enabled;

  /// How much of *this* ring's buffer this phone has collected, and the erase
  /// generation that offset is meaningful within. Meaningless apart.
  final int histCursor;
  final int histGeneration;

  final int? lastSeen;

  /// The last thing this ring said about its own cell, and when it said it.
  ///
  /// **Persisted because the interesting case is the one where the ring stops
  /// talking.** A cell that empties while the phone is away empties in silence:
  /// the ring stops at 5% (`RingPower.critical`), coasts to brownout and is
  /// gone, and by the time it can be connected to again it has been charged.
  /// The last figure it managed to send, with its timestamp, is the only
  /// evidence the phone will ever have of that -- and holding it in RAM meant
  /// losing it to any app restart, which on Android is routine. See
  /// `RingLink.battery` for what is built on top of it.
  ///
  /// `battAt` is wall-clock ms on *this phone*, not a ring timestamp. It is
  /// answering "how long has it been since we heard", which is a question about
  /// the link, so the ring's own uptime-based clock is the wrong one.
  final int? battPct;
  final int? battMv;
  final int? battAt;

  /// The ring's own power state at [battAt], as a `RingPower` index.
  ///
  /// Stored alongside the percentage rather than derived from it: the ring
  /// decides this, with hysteresis the phone does not model, and a ring that
  /// said "critical" before the app was killed is still critical afterwards.
  /// 0 when the ring has never said.
  final int battState;

  static Ring fromRow(Map<String, Object?> r) => Ring(
        id: r['id'] as String,
        label: r['label'] as String,
        enabled: (r['enabled'] as int) != 0,
        histCursor: r['hist_cursor'] as int,
        histGeneration: r['hist_generation'] as int,
        lastSeen: r['last_seen'] as int?,
        battPct: r['batt_pct'] as int?,
        battMv: r['batt_mv'] as int?,
        battAt: r['batt_at'] as int?,
        battState: (r['batt_state'] as int?) ?? 0,
      );
}

class HistoryDb {
  HistoryDb._();
  static final HistoryDb instance = HistoryDb._();
  Database? _db;

  // Bumped to 2 when `prefs` arrived, and to 3 when the ring's vitals packet
  // widened to carry the quality fields behind each reading. An install from
  // before either upgrades in place -- the readings are the point of the file,
  // not something to discard.
  //
  // The v3 columns are nullable on purpose: rows written before the widening
  // genuinely have no confidence attached, and a default would invent one.
  // "Unknown quality" and "known to be poor quality" must not become the same
  // row -- anything reading this table later would be reading a number this app
  // made up.
  // Bumped to 4 when a second ring arrived. Every earlier row was written by
  // the only ring the app could know, so the migration attributes them to it
  // rather than leaving them ownerless -- that is a fact on record, not a guess.
  // 5 added `device_id` and `upload_cursor` to `rings`. **Both are vestigial**:
  // they belonged to an upload path this app no longer has. The columns and
  // their migration stay exactly as they were, because removing a column from
  // SQLite means rebuilding the table, and rebuilding it to delete two unread
  // fields would put every existing install's readings through a copy for no
  // gain. Nothing reads or writes them.
  // 6 made a reading unique per (ring, time). See _dedupeReadings.
  // 7 added `step_delta`, which is **not** the same quantity as `steps` and is
  // why it needed its own column rather than filling in the existing one. See
  // the note on the two above [insertVitals].
  // 9 persists what the ring last said about its cell. The columns are nullable
  // for the same reason v3's were: a ring this app has never heard from about
  // its battery has no level, and defaulting one to 0 would invent a flat cell
  // for every existing install on first launch -- which is precisely the false
  // reading the feature exists to avoid producing.
  static const _version = 9;

  /// Non-null once [init] has failed, and the reason.
  ///
  /// **A database that would not open used to be indistinguishable from one
  /// that was working.** Every accessor here is written `_db?.something`, so a
  /// null handle made each of them a silent no-op: readings arrived, were
  /// "stored", the history cursor was "saved", and the ring was told it could
  /// erase -- with nothing written anywhere and no error on any surface. That
  /// is the same shape as every other fault this project has found, and on
  /// 2026-08-07 it cost 85 minutes of a wear test.
  ///
  /// Callers that write must check this. [ready] is the short way.
  Object? get lastError => _lastError;
  Object? _lastError;

  bool get ready => _db != null;

  Future<void> init() async {
    if (_db != null) return;
    try {
      await _open();
      _lastError = null;
    } catch (e, st) {
      // Loud, and kept. The UI surfaces it and the writers refuse to pretend.
      _lastError = e;
      debugPrint('[SenseRing] DATABASE FAILED TO OPEN: $e\n$st');
      rethrow;
    }
  }

  Future<void> _open() async {
    _db = await openDatabase(
      p.join(await getDatabasesPath(), 'sensering.db'),
      version: _version,
      onCreate: (db, _) async {
        await db.execute('''
          CREATE TABLE readings(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ring_id TEXT,
            at INTEGER NOT NULL,
            bpm INTEGER,
            spo2 REAL,
            mag INTEGER,
            steps INTEGER,
            step_delta INTEGER,
            confidence INTEGER,
            perfusion INTEGER,
            refusal INTEGER,
            movement INTEGER,
            windows INTEGER
          )''');
        await db.execute('CREATE INDEX idx_readings_at ON readings(at)');
        await db.execute('CREATE UNIQUE INDEX uq_readings_ring_at '
            'ON readings(ring_id, at, (mag IS NOT NULL))');
        await _createPrefs(db);
        await _createRings(db);
      },
      onUpgrade: (db, from, to) async {
        if (from < 2) await _createPrefs(db);
        if (from < 3) await _addQualityColumns(db);
        if (from < 4) await _addRings(db);
        if (from < 5) await _addUploadColumns(db);
        if (from < 6) await _dedupeReadings(db);
        if (from < 7) await _addStepDeltaColumn(db);
        if (from < 8) await _addRefusalColumns(db);
        if (from < 9) await _addBatteryColumns(db);
      },
    );
  }

  static Future<void> _createPrefs(Database db) =>
      db.execute('CREATE TABLE prefs(k TEXT PRIMARY KEY, v TEXT NOT NULL)');

  // **Each migration step builds the schema of its own version, never the
  // latest one.** This is not style. The v4 step originally called the same
  // helper `onCreate` uses, which by then already carried v5's two columns --
  // so on a v3 install the v4 step created them and the v5 step immediately
  // failed with `duplicate column name: device_id`. openDatabase threw,
  // `_db` stayed null, and because every writer here is `_db?.something`, the
  // app then ran for 85 minutes accepting readings and silently storing none of
  // them. Observed 2026-08-07; ~2800 readings were lost.
  //
  // So: `_createRingsV4` is frozen at the shape v4 produced and must never be
  // "tidied up" to match the current table. `_createRings` is the current
  // shape and is only ever used for a fresh install.
  static Future<void> _createRings(Database db) => db.execute('''
        CREATE TABLE rings(
          id TEXT PRIMARY KEY,
          label TEXT NOT NULL,
          enabled INTEGER NOT NULL DEFAULT 0,
          hist_cursor INTEGER NOT NULL DEFAULT 0,
          hist_generation INTEGER NOT NULL DEFAULT 0,
          last_seen INTEGER,
          added_at INTEGER NOT NULL,
          device_id TEXT,
          upload_cursor INTEGER NOT NULL DEFAULT 0,
          batt_pct INTEGER,
          batt_mv INTEGER,
          batt_at INTEGER,
          batt_state INTEGER NOT NULL DEFAULT 0
        )''');

  static Future<void> _createRingsV4(Database db) => db.execute('''
        CREATE TABLE rings(
          id TEXT PRIMARY KEY,
          label TEXT NOT NULL,
          enabled INTEGER NOT NULL DEFAULT 0,
          hist_cursor INTEGER NOT NULL DEFAULT 0,
          hist_generation INTEGER NOT NULL DEFAULT 0,
          last_seen INTEGER,
          added_at INTEGER NOT NULL
        )''');

  static Future<void> _addUploadColumns(Database db) async {
    await db.execute('ALTER TABLE rings ADD COLUMN device_id TEXT');
    await db.execute(
        'ALTER TABLE rings ADD COLUMN upload_cursor INTEGER NOT NULL DEFAULT 0');
  }

  // Two ALTERs rather than a rebuild: SQLite adds a nullable column in place
  // without touching the existing rows, which is the whole reason the columns
  // are nullable rather than defaulted.
  static Future<void> _addQualityColumns(Database db) async {
    await db.execute('ALTER TABLE readings ADD COLUMN confidence INTEGER');
    await db.execute('ALTER TABLE readings ADD COLUMN perfusion INTEGER');
  }

  // v6 -> v7. Nullable and undefaulted for the same reason the quality columns
  // are: rows written before this genuinely carry no step delta, and a 0 would
  // assert the wearer took no steps in those minutes. A daily total must be
  // able to say "not recorded" rather than "zero", because for every day
  // between the firmware's BLE_LIVE_STREAM going to 0 (2026-08-07) and this
  // migration, "not recorded" is the truth.
  static Future<void> _addStepDeltaColumn(Database db) =>
      db.execute('ALTER TABLE readings ADD COLUMN step_delta INTEGER');

  // v7 -> v8. What the ring refused a window for, how hard the hand was moving
  // while it measured it, and how many windows the row stands for.
  //
  // **These arrived on every record and were parsed and dropped**, which is why
  // reading a worn day meant reconstructing it from phone screenshots. The
  // refusal reason existed only inside a per-transfer tally that died with the
  // log buffer; the movement bucket was never read at all.
  //
  // `windows` is here because a row is not a window. The firmware collapses a
  // run of identical consecutive refusals into one record with a repeat count,
  // so a single row can stand for up to 256 measured windows, and a distribution
  // that counted rows would understate refusals by exactly however well the
  // collapsing worked.
  //
  // `movement` is the three-bit bucket from flags 5-7, 0 meaning "not recorded"
  // -- firmware older than 2026-08-21. Nullable and undefaulted like every
  // column before it, for the same reason: rows written before this migration
  // carry no such figure, and a 0 here would be indistinguishable from a genuine
  // "the ring did not record it".
  static Future<void> _addRefusalColumns(Database db) async {
    await db.execute('ALTER TABLE readings ADD COLUMN refusal INTEGER');
    await db.execute('ALTER TABLE readings ADD COLUMN movement INTEGER');
    await db.execute('ALTER TABLE readings ADD COLUMN windows INTEGER');
  }

  // v8 -> v9. Remembers the ring's last battery report across an app restart.
  //
  // Four ALTERs and no rebuild, the same in-place pattern as `_addUploadColumns`
  // -- and note that `_createRings` above was updated to match while
  // `_createRingsV4` was not. That asymmetry is the rule stated at length above
  // this pair, not an oversight: the frozen helper builds v4's shape forever and
  // the current one is only ever reached by a fresh install.
  //
  // `batt_state` takes a default because 0 is `RingPower.ok`, and "this app has
  // never been told otherwise" is genuinely the ok state -- unlike the three
  // nullable columns, where an invented value would be a reading nobody took.
  static Future<void> _addBatteryColumns(Database db) async {
    await db.execute('ALTER TABLE rings ADD COLUMN batt_pct INTEGER');
    await db.execute('ALTER TABLE rings ADD COLUMN batt_mv INTEGER');
    await db.execute('ALTER TABLE rings ADD COLUMN batt_at INTEGER');
    await db.execute(
        'ALTER TABLE rings ADD COLUMN batt_state INTEGER NOT NULL DEFAULT 0');
  }

  // v3 -> v4. Gives every reading an owner and moves the history cursor out of
  // the single global slot it could not survive a second ring in.
  static Future<void> _addRings(Database db) async {
    await _createRingsV4(db);
    await db.execute('ALTER TABLE readings ADD COLUMN ring_id TEXT');
    await db.execute(
        'CREATE INDEX idx_readings_ring_at ON readings(ring_id, at)');

    String? pref(List<Map<String, Object?>> rows) =>
        rows.isEmpty ? null : rows.first['v'] as String?;

    final id = pref(await db
        .query('prefs', where: 'k = ?', whereArgs: [kPrefRingId], limit: 1));
    if (id == null) {
      // Never bonded, so there is nothing to attribute and nothing to carry.
      return;
    }

    final cursor = int.tryParse(pref(await db.query('prefs',
                where: 'k = ?', whereArgs: [kPrefHistCursor], limit: 1)) ??
            '') ??
        0;
    final generation = int.tryParse(pref(await db.query('prefs',
                where: 'k = ?', whereArgs: [kPrefHistGeneration], limit: 1)) ??
            '') ??
        0;

    await db.insert('rings', {
      'id': id,
      'label': 'Ring 1',
      // The only ring the install knew, so it keeps the link it already had.
      'enabled': 1,
      'hist_cursor': cursor,
      'hist_generation': generation,
      'last_seen': DateTime.now().millisecondsSinceEpoch,
      'added_at': DateTime.now().millisecondsSinceEpoch,
    });

    // Every existing reading came from this ring -- it is the only one that
    // could have written them.
    await db.update('readings', {'ring_id': id}, where: 'ring_id IS NULL');

    for (final k in [kPrefRingId, kPrefHistCursor, kPrefHistGeneration]) {
      await db.delete('prefs', where: 'k = ?', whereArgs: [k]);
    }
  }

  // v5 -> v6. Makes a reading unique per (ring, time), and clears out the ones
  // that were not.
  //
  // **28% of this database was duplicate rows when it was measured on
  // 2026-08-07** -- 19,557 of 68,951. They come from the short-transfer guard
  // doing its job: a transfer that arrives incomplete is re-read *whole*,
  // because DATA frames carry no offset and there is no way to ask for only the
  // gap, so every recovery re-inserts the records that did arrive. That was
  // always the documented cost of the guard. What it did not have was a floor,
  // and over a week-long test the waste compounds.
  //
  // **The `(mag IS NOT NULL)` term is history, and it is kept deliberately.**
  // This app once also stored live motion packets, which share a timestamp with
  // the vitals packet *by design* -- the firmware stamps both from one
  // k_uptime_get() so the two line up (ARCHITECTURE.md 3A.9). A plain unique
  // index on (ring_id, at) therefore collided two rows that were not duplicates
  // at all, and deduplicating on it merged a heart rate with the movement
  // recorded alongside it. Measured on a 2026-08-07 database: 17,450 colliding
  // groups, of which 1,927 were exactly this.
  //
  // Nothing writes `mag` any more, so on a fresh install the term is always
  // false and the index behaves as a plain (ring_id, at). It stays because
  // changing an index means a migration, and because an install that upgraded
  // from a build that *did* store motion still has those rows in it -- dropping
  // the term would make them collide with the readings they were taken beside.
  //
  // Splitting on that expression leaves only real replays. Checked on the same
  // database: of 16,404 vitals groups with repeats, **zero disagreed on any
  // value** -- so MIN(id) loses nothing. It keeps the first arrival.
  static Future<void> _dedupeReadings(Database db) async {
    await db.execute('DELETE FROM readings WHERE id NOT IN ('
        ' SELECT MIN(id) FROM readings GROUP BY ring_id, at, (mag IS NOT NULL))');
    // After the delete, not before: the index cannot be built while the rows
    // that violate it are still there.
    await db.execute('CREATE UNIQUE INDEX IF NOT EXISTS uq_readings_ring_at '
        'ON readings(ring_id, at, (mag IS NOT NULL))');
  }

  // ---- rings ------------------------------------------------------------

  Future<List<Ring>> rings() async {
    final rows = await _db?.query('rings', orderBy: 'added_at');
    return (rows ?? []).map(Ring.fromRow).toList();
  }

  Future<Ring?> ring(String id) async {
    final rows =
        await _db?.query('rings', where: 'id = ?', whereArgs: [id], limit: 1);
    if (rows == null || rows.isEmpty) return null;
    return Ring.fromRow(rows.first);
  }

  /// Every ring that should currently be connected.
  Future<List<Ring>> enabledRings() async {
    final rows =
        await _db?.query('rings', where: 'enabled = 1', orderBy: 'added_at');
    return (rows ?? []).map(Ring.fromRow).toList();
  }

  /// Remembers a ring, without disturbing one it already knows.
  Future<void> rememberRing(String id, {String? label}) async {
    final db = _db;
    if (db == null) return;
    final existing = await ring(id);
    if (existing != null) {
      await db.update('rings',
          {'last_seen': DateTime.now().millisecondsSinceEpoch},
          where: 'id = ?', whereArgs: [id]);
      return;
    }
    final n = (await rings()).length;
    await db.insert('rings', {
      'id': id,
      'label': label ?? 'Ring ${n + 1}',
      // Enabled on arrival. A ring is only remembered because it was just
      // paired, and pairing a ring in order to leave it disconnected is not a
      // thing anybody does. Sessions are independent, so this takes nothing
      // away from a ring already connected.
      'enabled': 1,
      'hist_cursor': 0,
      'hist_generation': 0,
      'last_seen': DateTime.now().millisecondsSinceEpoch,
      'added_at': DateTime.now().millisecondsSinceEpoch,
    });
  }

  /// Records what the ring just said about its cell, and that it said it now.
  ///
  /// **The timestamp is the point, not the percentage.** Consumers project
  /// forward from it to decide whether a stored level still means anything, so
  /// this must be written on *every* report rather than only when the number
  /// changes -- at the ring's measured drain a percentage point takes half an
  /// hour, and a `batt_at` that only moved on a change would make the projection
  /// double-count that half hour as silence.
  ///
  /// Uses `_db?`, not [_write]: a battery figure that fails to persist costs a
  /// stale estimate after the next app restart, and nothing acknowledges
  /// anything to the ring on the strength of it. That is the opposite of
  /// [saveCursor], where a lost write means the ring erases records nobody has.
  Future<void> saveBattery(String id, int percent, int? millivolts,
          int state) async =>
      _db?.update(
          'rings',
          {
            'batt_pct': percent,
            'batt_mv': millivolts,
            'batt_at': DateTime.now().millisecondsSinceEpoch,
            'batt_state': state,
          },
          where: 'id = ?',
          whereArgs: [id]);

  /// Turns one ring's connection on or off, independently of the others.
  Future<void> setRingEnabled(String id, bool enabled) async => _db?.update(
      'rings', {'enabled': enabled ? 1 : 0},
      where: 'id = ?', whereArgs: [id]);

  Future<void> renameRing(String id, String label) async =>
      _db?.update('rings', {'label': label}, where: 'id = ?', whereArgs: [id]);

  Future<void> forgetRing(String id) async {
    await _db?.delete('rings', where: 'id = ?', whereArgs: [id]);
    // Its readings stay. They were real measurements of a real person, and the
    // ring being unpaired does not make them not have happened.
  }

  /// The pair is written together because it is only meaningful together.
  ///
  /// Uses [_write], so a database that is not open throws here rather than
  /// quietly doing nothing -- and the caller must not acknowledge to the ring
  /// if this did not land, or the ring erases records nobody has.
  Future<void> saveCursor(String id, int cursor, int generation) async =>
      _write.update(
          'rings',
          {
            'hist_cursor': cursor,
            'hist_generation': generation,
            'last_seen': DateTime.now().millisecondsSinceEpoch,
          },
          where: 'id = ?',
          whereArgs: [id]);

  Future<String?> getPref(String k) async {
    final rows =
        await _db?.query('prefs', where: 'k = ?', whereArgs: [k], limit: 1);
    if (rows == null || rows.isEmpty) return null;
    return rows.first['v'] as String;
  }

  Future<void> setPref(String k, String v) async {
    await _db?.insert('prefs', {'k': k, 'v': v},
        conflictAlgorithm: ConflictAlgorithm.replace);
  }

  Future<void> deletePref(String k) async {
    await _db?.delete('prefs', where: 'k = ?', whereArgs: [k]);
  }

  // `ringId` is required rather than optional on both writers. It would have
  // been a one-word change to default it to null, and every row written during
  // the next bug would have been ownerless without anything complaining.
  /// The one place a missing database is allowed to be noticed rather than
  /// shrugged off. Writers call it; a throw here is what stops the ring being
  /// told to erase records nothing kept.
  Database get _write {
    final db = _db;
    if (db == null) {
      throw StateError(
          'history database is not open${_lastError == null ? '' : ': $_lastError'}');
    }
    return db;
  }

  /// One record from the ring, live or replayed.
  ///
  /// **`stepDelta` is a delta, and the vestigial `steps` column was a running
  /// total. They must never be conflated.** A flash record carries the steps
  /// taken since the previous record; the ring's own counter carries a total
  /// since *its* boot. Summing a column of running totals is meaningless, and a
  /// reboot resets the total to zero while leaving the deltas correct, so only
  /// the delta can be aggregated over a day. Nothing writes `steps` now, and
  /// this is why it was never merged into the same column.
  ///
  /// `refusal` is the reason the ring would not vouch for the window, or null
  /// when it did; `movement` is the three-bit bucket from the flags byte, on
  /// every record whether refused or not; `windows` is how many measured windows
  /// the row stands for, which is 1 for a reading and up to 256 for a collapsed
  /// run of refusals. The three together are what makes a worn day
  /// re-analysable after the fact rather than a screenshot exercise -- see
  /// WINDOW_MOVE_MILLI_G in the firmware's main.c for the question they answer.
  ///
  /// The delta rides every replayed record at offset 12, so it arrives whether
  /// or not the ring is streaming -- which is the reason the daily totals are
  /// built from it and not from the ring's own counter.
  Future<void> insertVitals(String ringId, int at, int? bpm, double? spo2,
      {int? confidence,
      int? perfusion,
      int? stepDelta,
      int? refusal,
      int? movement,
      int? windows}) async {
    await _write.insert(
        'readings',
        {
          'ring_id': ringId,
          'at': at,
          'bpm': bpm,
          'spo2': spo2,
          'confidence': confidence,
          'perfusion': perfusion,
          'step_delta': stepDelta,
          'refusal': refusal,
          'movement': movement,
          'windows': windows,
        },
        // A re-read after a short transfer replays records already stored, and
        // since v6 that violates a unique index. `ignore` rather than `replace`:
        // the row already there arrived first and is identical, and replacing it
        // would churn a rowid that only ever increases.
        conflictAlgorithm: ConflictAlgorithm.ignore);
  }

  // Newest `kMaxPoints` rows for one column inside the chart window, returned
  // oldest-first for plotting.
  //
  // **Scoped to one ring, always.** A chart drawn across two is not a trend, it
  // is two people's data -- or, right now, a bench board fabricating vitals
  // interleaved with a ring on a finger. There is no caller that wants both and
  // no overload that offers it.
  Future<List<FlSpot>> _load(String ringId, String col) async {
    final db = _db;
    if (db == null) return [];
    final rows = await db.query('readings',
        columns: ['at', col],
        where: 'ring_id = ? AND $col IS NOT NULL AND at >= ?',
        whereArgs: [ringId, kChartWindowStart],
        orderBy: 'at DESC',
        limit: kMaxPoints);
    return rows.reversed
        .map((r) =>
            FlSpot((r['at'] as int).toDouble(), (r[col] as num).toDouble()))
        .toList();
  }

  Future<List<FlSpot>> loadHr(String ringId) => _load(ringId, 'bpm');

  /// Steps per calendar day for the last [days] days, oldest first, one entry
  /// per day including days with no data.
  ///
  /// A day with no rows at all and a day whose rows carry no step delta are
  /// both reported as a null [DailySteps.steps] rather than 0 — see
  /// [_addStepDeltaColumn]. Every day before 2026-08-10 is in that state, and a
  /// bar chart that drew them as zero would be claiming the wearer did not move
  /// rather than admitting the app was not recording.
  ///
  /// Bucketed in **local** time. `at` is stored as UTC milliseconds, so the
  /// 'localtime' modifier is what makes "a day" the wearer's day rather than
  /// one starting at whatever hour UTC midnight falls on where they live. The
  /// range is filtered on local midnight boundaries for the same reason.
  Future<List<DailySteps>> dailySteps(String ringId, {int days = 7}) async {
    final db = _db;
    if (db == null) return [];

    final now = DateTime.now();
    final startDay = DateTime(now.year, now.month, now.day)
        .subtract(Duration(days: days - 1));

    final rows = await db.rawQuery('''
      SELECT date(at / 1000, 'unixepoch', 'localtime') AS day,
             SUM(step_delta) AS total
        FROM readings
       WHERE ring_id = ? AND step_delta IS NOT NULL AND at >= ?
       GROUP BY day
    ''', [ringId, startDay.millisecondsSinceEpoch]);

    final byDay = {
      for (final r in rows) r['day'] as String: (r['total'] as num?)?.toInt(),
    };

    return List.generate(days, (i) {
      final d = startDay.add(Duration(days: i));
      // Same yyyy-MM-dd shape SQLite's date() produces, so the two agree.
      final key = '${d.year.toString().padLeft(4, '0')}-'
          '${d.month.toString().padLeft(2, '0')}-'
          '${d.day.toString().padLeft(2, '0')}';
      return DailySteps(d, byDay[key]);
    });
  }

  Future<int> total([String? ringId]) async {
    final db = _db;
    if (db == null) return 0;
    final rows = ringId == null
        ? await db.rawQuery('SELECT COUNT(*) FROM readings')
        : await db.rawQuery(
            'SELECT COUNT(*) FROM readings WHERE ring_id = ?', [ringId]);
    return Sqflite.firstIntValue(rows) ?? 0;
  }

  /// Per-ring row counts, for the ring list.
  Future<Map<String, int>> countsByRing() async {
    final db = _db;
    if (db == null) return {};
    final rows = await db
        .rawQuery('SELECT ring_id, COUNT(*) n FROM readings GROUP BY ring_id');
    return {
      for (final r in rows)
        if (r['ring_id'] != null) r['ring_id'] as String: r['n'] as int
    };
  }

  Future<void> clear([String? ringId]) async => ringId == null
      ? _db?.delete('readings')
      : _db?.delete('readings', where: 'ring_id = ?', whereArgs: [ringId]);
}
