// Smoke test: the app builds and, with no ring paired, says so.
//
// Deliberately asserts the empty state rather than the Live tiles. A widget
// test has no Bluetooth adapter and no database, so RingFleet finds no ring and
// both tabs render `_NoRing` -- which is the correct behaviour, and the only
// screen this test can reach without mocking the platform. The previous version
// looked for a 'Connect' button that only exists once a ring is known, and had
// been failing since focus moved into the ring sheet.
import 'package:flutter_test/flutter_test.dart';

import 'package:sense_ring_app/main.dart';

void main() {
  testWidgets('renders the no-ring empty state', (WidgetTester tester) async {
    await tester.pumpWidget(const SenseRingApp());
    expect(find.text('No ring paired'), findsOneWidget);
    expect(find.text('Live'), findsOneWidget);
    expect(find.text('Graphs'), findsOneWidget);
  });
}
