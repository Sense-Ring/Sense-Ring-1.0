# SenseRing app

Android companion app for the [SenseRing 1.0](../../README.md). It holds a
Bluetooth connection to the ring, collects the log the ring has been buffering
to its flash, stores every record in a local SQLite database, and charts it.

The ring is the measuring instrument and this app is its notebook. Nothing here
interprets a reading, and nothing here leaves the phone.

## How it works, in five lines

- The ring advertises as `SenseRing`. The first connection bonds; after that the
  phone holds a standing auto-connect request, so it reconnects on its own with
  no scan and no tap.
- On every connection the app sets the ring's clock, then asks for a replay of
  everything the ring has buffered since the last acknowledged cursor.
- Records arrive framed on the history characteristic, three to a notification
  at the negotiated MTU. Each is 16 bytes: rate, SpO₂, confidence, perfusion,
  step delta, refusal reason, movement bucket.
- They are written to SQLite **before** the cursor is acknowledged, so a crash
  costs a re-read rather than the records.
- A foreground service keeps the process alive with the app off screen, and a
  boot receiver restarts it after the phone reboots. Collection that only ran
  while somebody was looking would not collect much.

## What is on screen

| Tab | Contents |
|---|---|
| **Live** | Heart rate, SpO₂, battery and today's step total, each carrying the age of the reading behind it, over a scrolling protocol log |
| **Graphs** | Heart rate over the last three days, steps per calendar day over the last seven |

The ring sheet (top-right) manages more than one ring at a time: each gets its
own connection, cursor and rows, and the screen shows whichever one has focus.

**Every number comes from the buffered log, not from a live stream.** The ring
can notify live vitals, live motion and the standard Heart Rate characteristic,
and this app subscribes to none of them — the firmware compiles them out by
default (`BLE_LIVE_STREAM`), and a tile fed from one reads blank on a working
system, which is indistinguishable from a broken one. A reading therefore
reaches the phone when its flash page fills, up to ~1h43m after it was taken,
and **the unit beside every tile carries that age** rather than letting an old
number read as a current one.

Adding a live view back is a small change on both sides: set `BLE_LIVE_STREAM`
to 1 in the firmware's `main.c`, then add the characteristic's UUID to
`_labelFor` and a case to `_onData` in `lib/ring_link.dart`.

## Privacy

Readings go from the ring to this phone's database and no further. The app
declares **no `INTERNET` permission** and no network security config, so the
platform itself enforces that — it is a claim you can check from the manifest
without reading any Dart. There is no account, no telemetry and no backend.

The database is never pruned, so exporting or uploading is a supported thing to
build on top: rows carry a monotonic `id`, and nothing deletes them. It is
deliberately not built in. Adding it means adding the permission back.

## Building

```
flutter pub get
flutter run              # a connected Android device
flutter build apk        # release APK
```

Android only. There is no iOS target — the foreground service and boot receiver
that keep collection alive have no iOS equivalent, and shipping a build that
silently stopped collecting when backgrounded would be worse than shipping none.

Requires a real device: this is a BLE app, and the emulator has no radio.

## Permissions, and why each one

| Permission | Why |
|---|---|
| `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` | Find and connect to the ring. `neverForLocation` is set, so no location grant is needed on API 31+ |
| `ACCESS_FINE_LOCATION` | API 30 and below only, where a BLE scan legally requires it |
| `FOREGROUND_SERVICE` / `..._CONNECTED_DEVICE` | Keep the connection alive with the app off screen |
| `POST_NOTIFICATIONS` | The service notification is the only always-visible sign that collection is running |
| `RECEIVE_BOOT_COMPLETED` | Resume after a reboot instead of waiting for someone to open the app |
| `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` | Some vendor skins will otherwise sleep the process on their own schedule |

## Layout

```
lib/main.dart            the screen: tiles, charts, chart conditioning
lib/ring_link.dart       one ring's session -- connection, protocol, decoding
lib/ring_fleet.dart      every ring at once, and what belongs to the phone
lib/ring_sheet.dart      the ring picker
lib/history_db.dart      SQLite, its schema and its migrations
lib/monitor_service.dart the Dart side of the foreground service
android/.../*.kt         the service, boot receiver, and the cached engine
```

Each file opens with why it exists rather than what it does; the reasoning is in
the comments, deliberately, because most of it is about failures that are
invisible from the code alone.

## Protocol

The GATT contract this app speaks is defined by the firmware, and
`Software/Firmware/src/ARCHITECTURE.md` §5B is its documentation. The service is
`f1a00001-9c1b-4d3e-a7b2-5e8c6d9f0a11`; this app uses `…0002` (vitals), `…0004`
(control), `…0005` (history) and `…0006` (status), plus the standard Battery
Service. Constants that must match the firmware are marked as such where they
are declared — `_desiredMtu` and the refusal-reason bit field especially.

## Licence

This app is MIT — see [`LICENSE`](LICENSE), the same licence as the firmware.
The SenseRing 1.0 repository as a whole is GPLv3; MIT is compatible with that,
so a combined work still distributes under the GPL.
