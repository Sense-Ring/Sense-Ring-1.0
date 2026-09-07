// SenseRing connectivity app.
//
// Holds a connection to the "SenseRing" peripheral, subscribes to the
// notifying characteristics it exposes, collects the ring's buffered log,
// persists every reading to a local SQLite database, and plots heart rate and
// daily steps over time. History survives app restarts (loaded back from the
// DB).
//
// Everything on screen comes from the buffered log, never from a live stream.
// That is a deliberate limit rather than a missing feature: the ring's live
// characteristics are compiled out by default and carry nothing while the phone
// is out of range, so a tile fed from one would be blank on a working system
// and there would be no way to tell that apart from a broken one.
//
// The connection itself lives in RingLink, not in this file and not in a
// widget: it has to keep running when the app is off screen, which is the
// whole point of the foreground service (monitor_service.dart,
// android/.../RingService.kt). This file is the screen onto it.

import 'dart:async';
import 'dart:math';

import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import 'history_db.dart' show DailySteps;
import 'ring_fleet.dart';
import 'ring_link.dart';
import 'ring_sheet.dart';

void main() {
  final binding = WidgetsFlutterBinding.ensureInitialized();

  // The link comes up first and does not wait for a screen. This process is
  // not always started by a person opening the app -- BootReceiver starts it
  // after the phone restarts, with no Activity and no window -- and monitoring
  // that only ran when someone was looking would defeat the point.
  unawaited(RingFleet.instance.start());

  _UiGate(binding).arm();
}

// Runs the widget tree once there is somewhere to draw it.
//
// The engine outlives the Activity (SenseRingApplication.kt caches it), and can
// also be started with no Activity at all. `runApp` in that state has no view
// to attach to and would lay the UI out at zero size, so the tree is held back
// until a real window appears -- on the first launch that is immediate, and on
// the boot path it is whenever the user first opens the app.
class _UiGate with WidgetsBindingObserver {
  _UiGate(this._binding);
  final WidgetsBinding _binding;
  bool _running = false;
  Timer? _poll;

  void arm() {
    _binding.addObserver(this);
    // A blank app is the one failure here a user would read as "it's broken",
    // so the two event signals get a short-lived backstop over the window in
    // which an ordinary launch attaches its window. It is not left running:
    // after that, opening the app always comes with a lifecycle change.
    _poll = Timer.periodic(const Duration(milliseconds: 100), (t) {
      if (t.tick > 100) t.cancel(); // ~10s
      _tryRun();
    });
    _tryRun();
  }

  // Two independent signals, so a missed one does not leave a blank app: the
  // window gaining a size, and the app becoming resumed.
  @override
  void didChangeMetrics() => _tryRun();

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) => _tryRun();

  void _tryRun() {
    if (_running) return;
    final view = _binding.platformDispatcher.implicitView;
    if (view == null || view.physicalSize.isEmpty) return;
    _running = true;
    _poll?.cancel();
    _binding.removeObserver(this);
    runApp(const SenseRingApp());
  }
}

// ---- Chart data conditioning ------------------------------------------------
// Two rules, and everything here follows from them: **never draw a reading that
// was not taken, and never draw one at a time it was not taken.**
//
// This used to resample onto a fixed 48-bucket grid, mean each bucket, linearly
// interpolate the empty ones and smooth the result. It broke both rules. The
// interpolation was the serious one: a stretch with no readings -- the ring off
// the finger, out of range, or flat -- was drawn as a confident line between the
// readings either side of it, indistinguishable from measured data. On a monitor
// whose whole purpose is to notice when something is wrong, a chart that fills
// its own silence is the same failure as a notification that says everything is
// fine. The grid broke the second rule too: a handful of readings taken seconds
// apart were drawn minutes apart, because a bucket was positioned by where it
// sat on the grid rather than by when its readings actually happened. Both are
// worst exactly when there is least data, which is when a viewer can least
// afford to be misled.
//
// What replaces it still aggregates -- a day of readings every three seconds is
// more points than a phone screen has pixels, and refusing to summarise is its
// own kind of unreadable -- but it aggregates without either lie:
//
//   * an interval with no readings produces no point, and the line *breaks*
//     there rather than spanning it;
//   * an interval with readings produces one point at the **mean time of those
//     readings**, not at the middle of the interval it happens to fall in. Five
//     readings ten seconds apart land where they were taken, together;
//   * the tooltip says how many readings are behind a point, so a summary can
//     never be mistaken for a single measurement.
//
// With the ring worn continuously this is an ordinary trend line. With five
// bursts in a day it looks like five bursts in a day, which is the point.

// What counts as the ring having stopped reporting.
//
// A measurement cycle is ~41s in this build and ~121s as designed (the PAUSE_MS
// question), so five minutes is comfortably more than either. The threshold
// therefore does not depend on which of those is true -- and if the cadence
// ever changes, it fails towards drawing *fewer* gaps rather than inventing
// data across real ones.
const _gapAfter = Duration(minutes: 5);

// Roughly how many points to draw across the whole span. The intervals are
// derived from this rather than fixed, so a chart of ten minutes summarises
// over seconds and a chart of three days over hours.
const _targetPoints = 60;

class _Series {
  const _Series(this.points, this.plot, this.counts);

  /// One point per interval that had readings, at the mean time of those
  /// readings. Used for the axis bounds and the dots -- it has no null spots.
  final List<FlSpot> points;

  /// [points] with a null spot at every break, which is how fl_chart draws a
  /// gap rather than a segment across it.
  final List<FlSpot> plot;

  /// How many readings are behind each point, keyed by its x.
  final Map<double, int> counts;
}

_Series _buildSeries(List<FlSpot> raw) {
  // Sorted, because the two paths that fill these series do not arrive in time
  // order: a backfill delivers records older than the live ones already
  // charted.
  final sorted = List<FlSpot>.from(raw)..sort((a, b) => a.x.compareTo(b.x));
  if (sorted.length < 2) {
    return _Series(sorted, sorted, {for (final p in sorted) p.x: 1});
  }

  final minX = sorted.first.x;
  final span = sorted.last.x - minX;
  final width = span <= 0 ? 1.0 : span / _targetPoints;
  final gap = _gapAfter.inMilliseconds;

  final points = <FlSpot>[];
  final plot = <FlSpot>[];
  final counts = <double, int>{};

  int intervalOf(FlSpot p) => ((p.x - minX) / width).floor();

  var i = 0;
  int? previous;
  while (i < sorted.length) {
    final interval = intervalOf(sorted[i]);
    var sumX = 0.0, sumY = 0.0, n = 0;
    while (i < sorted.length && intervalOf(sorted[i]) == interval) {
      sumX += sorted[i].x;
      sumY += sorted[i].y;
      n++;
      i++;
    }
    final point = FlSpot(sumX / n, sumY / n);

    // Break on either signal: an interval that produced nothing, or two points
    // far enough apart in real time to mean the ring stopped reporting. Two
    // tests rather than one because a reading at the far edge of one interval
    // and another at the near edge of the next are adjacent by index and still
    // minutes apart.
    if (previous != null &&
        (interval - previous > 1 || point.x - points.last.x > gap)) {
      plot.add(FlSpot.nullSpot);
    }
    plot.add(point);
    points.add(point);
    counts[point.x] = n;
    previous = interval;
  }

  return _Series(points, plot, counts);
}

// Summary stats over the raw readings (not the resampled line).
({double min, double max, double avg, double last})? _stats(List<FlSpot> pts) {
  if (pts.isEmpty) return null;
  var mn = pts.first.y, mx = pts.first.y, sum = 0.0;
  for (final p in pts) {
    if (p.y < mn) mn = p.y;
    if (p.y > mx) mx = p.y;
    sum += p.y;
  }
  return (min: mn, max: mx, avg: sum / pts.length, last: pts.last.y);
}

// A "1/2/5 x 10^k" step near `raw`, so axis labels land on round numbers.
double _niceStep(double raw) {
  if (raw <= 0) return 1;
  final e = pow(10, (log(raw) / ln10).floor()).toDouble();
  final m = raw / e;
  if (m < 1.5) return e;
  if (m < 3) return 2 * e;
  if (m < 7) return 5 * e;
  return 10 * e;
}

// Round a data range out to nice bounds plus the step between gridlines.
({double min, double max, double step}) _niceRange(double lo, double hi) {
  if (hi <= lo) hi = lo + 1;
  final step = _niceStep((hi - lo) / 4);
  final niceMin = (lo / step).floorToDouble() * step;
  var niceMax = (hi / step).ceilToDouble() * step;
  if (niceMax == niceMin) niceMax += step;
  return (min: niceMin, max: niceMax, step: step);
}

// A clock-aligned time step (seconds table -> ms) giving ~4 ticks across span.
//
// The table runs past a day on purpose. It used to stop at 86400, and a series
// spanning twelve days therefore fell off the end and got a *daily* tick: 13
// labels crammed into the axis width, every one of them reading the same
// wall-clock time because daily ticks land on the same instant of the day.
// Saturating a step table is not a rare case, it is what happens whenever data
// outlives the range someone imagined.
double _niceTimeStep(double spanMs) {
  const optsSec = <double>[
    60,
    120,
    300,
    600,
    900,
    1800,
    3600,
    7200,
    14400,
    21600,
    43200,
    86400,
    172800,
    604800
  ];
  final targetSec = spanMs / 4 / 1000;
  for (final s in optsSec) {
    if (s >= targetSec) return s * 1000;
  }
  return optsSec.last * 1000;
}

const _monthNames = [
  'Jan',
  'Feb',
  'Mar',
  'Apr',
  'May',
  'Jun',
  'Jul',
  'Aug',
  'Sep',
  'Oct',
  'Nov',
  'Dec'
];

String _two(int n) => n.toString().padLeft(2, '0');
String _hhmm(DateTime t) => '${_two(t.hour)}:${_two(t.minute)}';
String _dayMonth(DateTime t) => '${t.day} ${_monthNames[t.month - 1]}';

class SenseRingApp extends StatelessWidget {
  const SenseRingApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'SenseRing',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
        useMaterial3: true,
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatelessWidget {
  const HomePage({super.key});

  RingFleet get _fleet => RingFleet.instance;

  /// The ring the tiles and charts are showing. Null only before the first
  /// pairing -- with several rings connected, one of them is always in focus.
  RingLink? get _link => RingFleet.instance.focus;

  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      length: 2,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('SenseRing'),
          backgroundColor: Theme.of(context).colorScheme.inversePrimary,
          actions: [
            ListenableBuilder(
              listenable: _fleet,
              builder: (_, __) => IconButton(
                icon: const Icon(Icons.radio_button_checked),
                tooltip: 'Rings',
                onPressed: () => showModalBottomSheet<void>(
                  context: context,
                  isScrollControlled: true,
                  showDragHandle: true,
                  builder: (_) => const RingSheet(),
                ),
              ),
            ),
          ],
          bottom: const TabBar(tabs: [
            Tab(text: 'Live', icon: Icon(Icons.sensors)),
            Tab(text: 'Graphs', icon: Icon(Icons.show_chart)),
          ]),
        ),
        body: ListenableBuilder(
          listenable: _fleet,
          builder: (context, __) => TabBarView(children: [
            _liveTab(context),
            _graphsTab(context),
          ]),
        ),
      ),
    );
  }

  // One scroll view for the whole tab, and the log as its own sliver rather
  // than an Expanded list. The Column-with-Expanded arrangement this replaces
  // overflows the moment the fixed content above it is taller than the screen
  // -- which it became the day the background card arrived, on a phone that is
  // not small. Nothing here is allowed to depend on fitting.
  Widget _liveTab(BuildContext context) {
    final link = _link;
    if (link == null) return const _NoRing();
    final connected = link.connected;
    return CustomScrollView(
      slivers: [
        SliverPadding(
          padding: const EdgeInsets.all(12),
          sliver: SliverToBoxAdapter(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(12),
                    child: Row(
                      children: [
                        Icon(
                            connected
                                ? Icons.bluetooth_connected
                                : link.autoConnect
                                    ? Icons.bluetooth_searching
                                    : Icons.bluetooth_disabled,
                            color: connected ? Colors.teal : Colors.grey),
                        const SizedBox(width: 10),
                        Expanded(child: Text(link.status)),
                      ],
                    ),
                  ),
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    // Connect is only live when nothing is pending: while
                    // auto-connect is armed the app is already trying, so the useful
                    // action is the one that calls it off.
                    Expanded(
                      child: FilledButton.icon(
                        onPressed:
                            (link.busy || connected || link.autoConnect)
                                ? null
                                : link.connect,
                        icon: const Icon(Icons.search),
                        label: Text(link.busy ? 'Working...' : 'Connect'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: (connected || link.autoConnect)
                            ? link.disconnect
                            : null,
                        icon: const Icon(Icons.link_off),
                        label: const Text('Disconnect'),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                const BackgroundCard(),
                const SizedBox(height: 12),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    // The unit carries the reading's age, because the ring
                    // delivers a page at a time rather than streaming: a bare
                    // "68" here would read as the wearer's rate now, when it
                    // may be an hour and a half old. See RingLink.bpmAge.
                    _tile(
                        'Heart rate',
                        link.bpm == null ? '--' : '${link.bpm}',
                        _ageLabel(link.bpmAge)),
                    _tile(
                        'SpO₂',
                        link.spo2 == null
                            ? '--'
                            : link.spo2!.toStringAsFixed(1),
                        '%'),
                    // Carries its age for the same reason heart rate does,
                    // and for a sharper one: the ring physically cannot report
                    // 0%. It stops measuring at 5%, coasts to brownout, and is
                    // charged before any phone reaches it again -- so a bare
                    // percentage here is the last number a *reachable* ring
                    // said, and on a dead one that number stays on screen
                    // forever. See RingLink.battery for the projection.
                    _tile(
                        'Battery',
                        link.battery == null
                            ? '--'
                            : '${link.battery!.percent}',
                        _batteryUnit(link.battery)),
                    // Today's total, summed from the per-record deltas in the
                    // database. This is the authoritative number: it is bucketed
                    // by the wearer's calendar day and survives the ring
                    // rebooting, which the running total beside it does not.
                    //
                    // What it cannot show is anything the ring has measured but
                    // not yet delivered. The deltas arrive with the buffered
                    // readings, so this fills in a page at a time like the heart
                    // rate above rather than ticking up as the wearer walks --
                    // and if a page is destroyed before it is collected, those
                    // steps never appear here at all. The 'today' unit is doing
                    // real work: it is what stops the number reading as a live
                    // pedometer.
                    _tile(
                        'Steps',
                        link.stepsToday == null
                            ? '--'
                            : _thousands(link.stepsToday!),
                        'today'),
                  ],
                ),
                const SizedBox(height: 12),
                const Align(
                  alignment: Alignment.centerLeft,
                  child: Text('Log',
                      style: TextStyle(fontWeight: FontWeight.bold)),
                ),
                const Divider(),
              ],
            ),
          ),
        ),
        SliverPadding(
          padding: const EdgeInsets.symmetric(horizontal: 12),
          sliver: SliverList.builder(
            itemCount: _fleet.log.length,
            itemBuilder: (_, i) => Text(
              _fleet.log[i],
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
            ),
          ),
        ),
        const SliverToBoxAdapter(child: SizedBox(height: 12)),
      ],
    );
  }

  Widget _graphsTab(BuildContext context) {
    final link = _link;
    if (link == null) return const _NoRing();
    final dark = Theme.of(context).brightness == Brightness.dark;
    // Categorical blue for heart rate: CVD-safe, and not a reserved status
    // colour.
    final hrColor = dark ? const Color(0xFF3987E5) : const Color(0xFF2A78D6);

    return ListView(
      padding: const EdgeInsets.all(12),
      children: [
        _chartCard(context, 'Heart rate', 'bpm', link.hrSeries, hrColor),
        const SizedBox(height: 12),
        _stepsCard(context, link),
        const SizedBox(height: 12),
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text('${link.stored} readings stored',
                style: const TextStyle(color: Colors.grey)),
            TextButton.icon(
              onPressed: link.stored == 0 ? null : link.clearHistory,
              icon: const Icon(Icons.delete_outline),
              label: const Text('Clear history'),
            ),
          ],
        ),
      ],
    );
  }

  // Steps by calendar day, one bar per day, seven days back.
  //
  // A bar chart rather than the line the card above uses, because this is seven
  // discrete totals and not a sampled signal: a line between Monday and Tuesday
  // would imply values in between, and there are none -- a day's step count
  // only exists at the end of the day.
  //
  // **A day with no data is drawn as absent, not as zero**, which is the whole
  // reason this is not a simple `steps ?? 0`. The wearer taking no steps and
  // the app never having recorded any are opposite facts, and a row of
  // zero-height bars would state the first while meaning the second -- a week
  // of not moving, drawn for a week the app simply never heard about.
  Widget _stepsCard(BuildContext context, RingLink link) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    // Purple, chosen against the blue above it rather than beside it.
    // Validated for colour-vision deficiency -- worst separation is deltaE 25
    // (deutan) in dark and 25 (protan) in light, well above the 8 floor.
    final stepColor = dark ? const Color(0xFF9A6DD7) : const Color(0xFF7E4EC4);
    final hint = Theme.of(context).hintColor;
    final days = link.dailySteps;
    final recorded = days.where((d) => d.hasData).toList();
    final total = recorded.fold<int>(0, (a, d) => a + d.steps!);
    final busiest =
        recorded.isEmpty ? 0 : recorded.map((d) => d.steps!).reduce(max);

    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 12, 16, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                    width: 10,
                    height: 10,
                    decoration: BoxDecoration(
                        color: stepColor, shape: BoxShape.circle)),
                const SizedBox(width: 8),
                const Text('Steps — last 7 days',
                    style: TextStyle(
                        fontWeight: FontWeight.bold, fontSize: 16)),
              ],
            ),
            Padding(
              padding: const EdgeInsets.only(left: 18, top: 2),
              child: Text(
                recorded.isEmpty
                    ? 'no days recorded'
                    : '${_thousands(total)} total · '
                        '${_thousands(total ~/ recorded.length)}/day avg · '
                        '${recorded.length} of ${days.length} days recorded',
                style: TextStyle(color: hint),
              ),
            ),
            const SizedBox(height: 14),
            if (days.isEmpty)
              SizedBox(
                height: 190,
                child: Center(
                  child: Text('Waiting for data — connect the ring.',
                      style: TextStyle(color: hint)),
                ),
              )
            else
              SizedBox(
                height: 190,
                child: _stepsChart(context, days, busiest, stepColor),
              ),
            if (recorded.isEmpty && days.isNotEmpty)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(
                  'No step counts stored for these days. The ring sends them '
                  'with every buffered reading; this build is the first that '
                  'keeps them, so the history starts filling from now.',
                  style: TextStyle(color: hint, fontSize: 12),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _stepsChart(BuildContext context, List<DailySteps> days, int busiest,
      Color color) {
    final labelStyle =
        TextStyle(color: Theme.of(context).hintColor, fontSize: 10);
    final scheme = Theme.of(context).colorScheme;
    final gridColor = Theme.of(context).dividerColor.withValues(alpha: 0.35);
    final today = DateTime.now();
    final maxY = _niceTop(busiest);

    bool isToday(DateTime d) =>
        d.year == today.year && d.month == today.month && d.day == today.day;

    return BarChart(
      BarChartData(
        alignment: BarChartAlignment.spaceAround,
        maxY: maxY,
        minY: 0,
        gridData: FlGridData(
          show: true,
          drawVerticalLine: false,
          horizontalInterval: maxY / 4,
          getDrawingHorizontalLine: (_) =>
              FlLine(color: gridColor, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        titlesData: FlTitlesData(
          rightTitles:
              const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          leftTitles:
              const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          // The value sits above its own bar instead of on a left axis. With
          // seven bars there is room to label every one, and a direct label
          // beats making the reader measure a bar against a distant scale.
          topTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: 20,
              interval: 1,
              getTitlesWidget: (v, meta) {
                final d = days[v.toInt().clamp(0, days.length - 1)];
                return SideTitleWidget(
                  meta: meta,
                  // An em dash for a day with nothing recorded, so the gap is
                  // stated rather than left for the reader to interpret.
                  child: Text(d.hasData ? _thousands(d.steps!) : '—',
                      style: labelStyle),
                );
              },
            ),
          ),
          bottomTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: 32,
              interval: 1,
              getTitlesWidget: (v, meta) {
                final d = days[v.toInt().clamp(0, days.length - 1)];
                final now = isToday(d.day);
                return SideTitleWidget(
                  meta: meta,
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(now ? 'today' : _weekday(d.day),
                          style: labelStyle.copyWith(
                              fontWeight:
                                  now ? FontWeight.bold : FontWeight.normal)),
                      Text('${d.day.day}', style: labelStyle),
                    ],
                  ),
                );
              },
            ),
          ),
        ),
        barTouchData: BarTouchData(
          enabled: true,
          touchTooltipData: BarTouchTooltipData(
            getTooltipColor: (_) =>
                scheme.inverseSurface.withValues(alpha: 0.92),
            getTooltipItem: (group, _, __, ___) {
              final d = days[group.x];
              return BarTooltipItem(
                d.hasData
                    ? '${_thousands(d.steps!)} steps\n${_dayMonth(d.day)}'
                    : 'not recorded\n${_dayMonth(d.day)}',
                TextStyle(
                  color: scheme.onInverseSurface,
                  fontWeight: FontWeight.bold,
                  fontSize: 12,
                ),
              );
            },
          ),
        ),
        barGroups: [
          for (var i = 0; i < days.length; i++)
            BarChartGroupData(
              x: i,
              barRods: [
                BarChartRodData(
                  // Absent days get a zero-height rod: nothing is drawn, and
                  // the em dash above the slot carries the meaning.
                  toY: (days[i].steps ?? 0).toDouble(),
                  color: color,
                  width: 18,
                  borderRadius: const BorderRadius.vertical(
                      top: Radius.circular(4)),
                ),
              ],
            ),
        ],
      ),
    );
  }

  // Rounded ceiling for the bar axis, so the gridlines land on whole numbers.
  double _niceTop(int v) {
    if (v <= 0) return 100;
    final mag = pow(10, (log(v) / ln10).floor()).toDouble();
    for (final m in [1.0, 1.5, 2.0, 2.5, 5.0, 7.5]) {
      if (v <= mag * m) return mag * m;
    }
    return mag * 10;
  }

  static const _weekdays = [
    'Mon',
    'Tue',
    'Wed',
    'Thu',
    'Fri',
    'Sat',
    'Sun'
  ];

  String _weekday(DateTime d) => _weekdays[d.weekday - 1];

  // 1234567 -> "1,234,567". Step counts get big enough over a week that an
  // unseparated run of digits is genuinely hard to compare bar to bar.
  String _thousands(int n) {
    final s = n.toString();
    final b = StringBuffer();
    for (var i = 0; i < s.length; i++) {
      if (i > 0 && (s.length - i) % 3 == 0) b.write(',');
      b.write(s[i]);
    }
    return b.toString();
  }

  Widget _chartCard(BuildContext context, String title, String unit,
      List<FlSpot> raw, Color color) {
    final s = _stats(raw);
    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 12, 16, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                    width: 10,
                    height: 10,
                    decoration:
                        BoxDecoration(color: color, shape: BoxShape.circle)),
                const SizedBox(width: 8),
                Text(title,
                    style: const TextStyle(
                        fontWeight: FontWeight.bold, fontSize: 16)),
              ],
            ),
            Padding(
              padding: const EdgeInsets.only(left: 18, top: 2),
              child: Text(
                s == null
                    ? '— $unit'
                    : 'avg ${s.avg.round()} · min ${s.min.round()} · '
                        'max ${s.max.round()} $unit',
                style: TextStyle(color: Theme.of(context).hintColor),
              ),
            ),
            const SizedBox(height: 14),
            SizedBox(
                height: 190, child: _chart(context, raw, unit, color, s?.avg)),
          ],
        ),
      ),
    );
  }

  Widget _chart(BuildContext context, List<FlSpot> raw, String unit,
      Color color, double? avg) {
    final built = _buildSeries(raw);
    final series = built.plot;
    final real = built.points;
    if (real.length < 2) {
      return const Center(
        child: Text('Waiting for data — connect the ring.',
            style: TextStyle(color: Colors.grey)),
      );
    }
    final gridColor = Theme.of(context).dividerColor.withValues(alpha: 0.35);
    final labelStyle =
        TextStyle(color: Theme.of(context).hintColor, fontSize: 10);
    final surface = Theme.of(context).cardColor;
    final scheme = Theme.of(context).colorScheme;
    final tooltipColor = scheme.inverseSurface.withValues(alpha: 0.92);
    final onTooltipColor = scheme.onInverseSurface;

    // From the real readings, not the plot list: that one carries null spots,
    // whose x and y are NaN.
    final ys = real.map((s) => s.y);
    final y = _niceRange(ys.reduce(min), ys.reduce(max));
    final minX = real.first.x, maxX = real.last.x;
    final span = max(maxX - minX, 1.0);
    final xStep = _niceTimeStep(span);
    // Past a day, the time alone stops identifying a tick -- two ticks a day
    // apart read identically. Beyond that the label carries the date too, on a
    // second line so it does not have to compete for width.
    final multiDay = span > const Duration(hours: 24).inMilliseconds;

    return LineChart(
      LineChartData(
        minX: minX,
        maxX: maxX,
        minY: y.min,
        maxY: y.max,
        clipData: const FlClipData.all(),
        gridData: FlGridData(
          show: true,
          drawVerticalLine: false,
          horizontalInterval: y.step,
          getDrawingHorizontalLine: (_) =>
              FlLine(color: gridColor, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        titlesData: FlTitlesData(
          topTitles:
              const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          rightTitles:
              const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          leftTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: 40,
              interval: y.step,
              getTitlesWidget: (v, meta) => SideTitleWidget(
                meta: meta,
                child: Text(v.toStringAsFixed(0), style: labelStyle),
              ),
            ),
          ),
          bottomTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: multiDay ? 34 : 22,
              interval: xStep, // clock-aligned, evenly spaced
              // The first and last ticks sit on the data's own extremes, which
              // are an arbitrary distance from their neighbours -- so they
              // collided with them and with the axis edge. The evenly spaced
              // ones in between are the ones that mean anything.
              minIncluded: false,
              maxIncluded: false,
              getTitlesWidget: (v, meta) {
                final t = DateTime.fromMillisecondsSinceEpoch(v.toInt());
                return SideTitleWidget(
                  meta: meta,
                  child: multiDay
                      ? Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            Text(_dayMonth(t), style: labelStyle),
                            Text(_hhmm(t), style: labelStyle),
                          ],
                        )
                      : Text(_hhmm(t), style: labelStyle),
                );
              },
            ),
          ),
        ),
        // Dotted average reference line.
        extraLinesData: avg == null
            ? const ExtraLinesData()
            : ExtraLinesData(horizontalLines: [
                HorizontalLine(
                  y: avg,
                  color: Theme.of(context).hintColor.withValues(alpha: 0.7),
                  strokeWidth: 1,
                  dashArray: const [4, 4],
                ),
              ]),
        // Tapping a point answers the two questions a point raises: what was
        // the reading, and when. Both are now literally true -- the point is a
        // reading, at its own timestamp -- which they were not while the chart
        // plotted bucket means at grid positions. The value is rounded because
        // a heart rate to two decimal places, which is what the default tooltip
        // showed, is false precision on a number the ring reports as whole bpm.
        lineTouchData: LineTouchData(
          enabled: true,
          touchTooltipData: LineTouchTooltipData(
            getTooltipColor: (_) => tooltipColor,
            getTooltipItems: (spots) => spots.map((spot) {
              final t = DateTime.fromMillisecondsSinceEpoch(spot.x.toInt());
              final n = built.counts[spot.x] ?? 1;
              // Says so when it is a summary. A point that averages eight
              // readings and one that is a single reading look identical on the
              // chart, and only one of them is a measurement.
              final value = n > 1
                  ? '${spot.y.round()} $unit · avg of $n'
                  : '${spot.y.round()} $unit';
              return LineTooltipItem(
                '$value\n${_dayMonth(t)} ${_hhmm(t)}',
                TextStyle(
                  color: onTooltipColor,
                  fontWeight: FontWeight.bold,
                  fontSize: 12,
                ),
              );
            }).toList(),
          ),
        ),
        // Straight segments between real readings, broken where the ring was
        // not reporting. Not curved: a spline between two readings bulges to
        // values neither of them measured, which is a small lie of the same
        // kind as the interpolation this replaced.
        lineBarsData: [
          LineChartBarData(
            spots: series,
            isCurved: false,
            color: color,
            barWidth: 2,
            // Every point gets a dot. A bare line invites the eye to read the
            // segments as data; the dots say where the readings actually are,
            // and an isolated burst shows up as a cluster rather than as a
            // spike out of nowhere.
            dotData: FlDotData(
              show: true,
              getDotPainter: (spot, pct, bar, i) => FlDotCirclePainter(
                radius: spot.x == maxX ? 4 : 2.5,
                color: color,
                strokeColor: surface,
                strokeWidth: spot.x == maxX ? 2 : 1,
              ),
            ),
            // No area fill. It reads as "this much of the time, at this level",
            // which is true of a continuous line and a lie under a broken one:
            // an isolated reading was drawn as a shaded column standing on the
            // axis, as though the interval around it had been measured.
            belowBarData: BarAreaData(show: false),
          ),
        ],
      ),
    );
  }

  /// 'bpm' while a reading is fresh, otherwise how old it is.
  ///
  /// Two minutes is one measurement cycle, so anything inside that is as
  /// current as this device can be and does not need qualifying. Past it, the
  /// age is the more important half of the reading -- the ring only delivers
  /// when a flash page fills, and a wearer looking at a stale number with
  /// nothing to say so has been misled rather than informed.
  String _ageLabel(Duration? age) {
    if (age == null) return 'bpm';
    if (age.inMinutes < 2) return 'bpm';
    if (age.inMinutes < 90) return '${age.inMinutes} min ago';
    return '${age.inHours} h ago';
  }

  /// '%' while the ring is talking, otherwise what the number now is.
  ///
  /// The unit slot is doing the same job it does for heart rate: it is the only
  /// place on the tile to say that a number is not a fresh reading. `est.` is
  /// the important word -- 12% here is the phone's arithmetic, not something
  /// the ring said -- and 'flat' rather than a bare 0% because 0 is the one
  /// value the ring can never have reported.
  String _batteryUnit(BatteryEstimate? est) {
    if (est == null) return '%';
    if (!est.estimated) return '%';
    if (est.flat) return 'flat, est.';
    return '% est.';
  }

  Widget _tile(String label, String value, String unit) {
    return SizedBox(
      width: 110,
      child: Card(
        child: Padding(
          padding: const EdgeInsets.all(10),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(label,
                  style: const TextStyle(fontSize: 12, color: Colors.grey)),
              const SizedBox(height: 4),
              Text(value,
                  style: const TextStyle(
                      fontSize: 18, fontWeight: FontWeight.bold)),
              Text(unit,
                  style: const TextStyle(fontSize: 11, color: Colors.grey)),
            ],
          ),
        ),
      ),
    );
  }
}

/// Whether the phone will actually let this app keep monitoring.
///
/// Two facts, both invisible otherwise, and both able to silently end
/// monitoring: the foreground service, and Android's battery optimisation. On
/// One UI in particular, "put unused apps to sleep" and the deep-sleeping list
/// will kill this service, and a wearer's family has no way to know that
/// happened. Surfacing it here is the only place it can be said before the
/// data stops rather than after.
class BackgroundCard extends StatefulWidget {
  const BackgroundCard({super.key});

  @override
  State<BackgroundCard> createState() => _BackgroundCardState();
}

class _BackgroundCardState extends State<BackgroundCard>
    with WidgetsBindingObserver {
  bool? _exempt; // exempt from battery optimisation
  bool? _visible; // allowed to post the monitoring notification

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _refresh();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  // The exemption is granted in a system dialog, so the answer only arrives
  // when we come back.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) _refresh();
  }

  Future<void> _refresh() async {
    final exempt = await Permission.ignoreBatteryOptimizations.isGranted;
    final visible = await Permission.notification.isGranted;
    if (!mounted) return;
    setState(() {
      _exempt = exempt;
      _visible = visible;
    });
  }

  // Notifications first: a blocked notification is the failure that hides
  // every other failure, including this app being asleep.
  Future<void> _request() async {
    if (!(_visible ?? true)) {
      await Permission.notification.request();
    } else {
      await Permission.ignoreBatteryOptimizations.request();
    }
    await _refresh();
  }

  // Listens to the link itself rather than relying on the parent rebuilding
  // it: this widget is const, so a parent rebuild does not reach it, and a
  // stale "monitoring is off" here would be a lie about the one thing the card
  // exists to report.
  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: RingFleet.instance,
        builder: (context, _) => _card(context),
      );

  Widget _card(BuildContext context) {
    final monitoring = RingFleet.instance.monitoring;
    // Don't cry wolf before the checks land.
    final exempt = _exempt ?? true;
    final visible = _visible ?? true;
    final ok = monitoring && exempt && visible;

    final String detail;
    if (!monitoring) {
      detail = 'Connect the ring to keep it reporting with the app closed.';
    } else if (!visible) {
      // The service is running; nothing on screen says so. That is worse than
      // it sounds -- it makes a dead service and a live one look the same.
      detail = 'Notifications are blocked, so nothing will show whether the '
          'ring is still reporting.';
    } else if (!exempt) {
      detail = 'Android may still put this app to sleep and stop the ring '
          'reporting.';
    } else {
      detail = 'The ring stays connected with the app closed.';
    }

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(
          children: [
            Icon(ok ? Icons.shield_outlined : Icons.warning_amber_rounded,
                color: ok ? Colors.teal : Colors.orange),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    monitoring
                        ? 'Background monitoring is on'
                        : 'Background monitoring is off',
                    style: const TextStyle(fontWeight: FontWeight.bold),
                  ),
                  Text(
                    detail,
                    style: TextStyle(
                        fontSize: 12, color: Theme.of(context).hintColor),
                  ),
                ],
              ),
            ),
            if (monitoring && !ok)
              TextButton(onPressed: _request, child: const Text('Fix')),
          ],
        ),
      ),
    );
  }
}

/// What the Live and Graphs tabs show before any ring is paired.
///
/// A first launch has no ring, no readings and nothing to plot. That is an
/// ordinary state rather than a failure, so it gets a sentence and a pointer to
/// the only thing worth doing next.
class _NoRing extends StatelessWidget {
  const _NoRing();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.radio_button_unchecked,
                size: 44, color: theme.colorScheme.onSurfaceVariant),
            const SizedBox(height: 12),
            Text('No ring paired',
                style: theme.textTheme.titleMedium, textAlign: TextAlign.center),
            const SizedBox(height: 6),
            Text(
              'Open Rings from the app bar, then Pair a new ring.',
              style: theme.textTheme.bodySmall
                  ?.copyWith(color: theme.colorScheme.onSurfaceVariant),
              textAlign: TextAlign.center,
            ),
          ],
        ),
      ),
    );
  }
}
