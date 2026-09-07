// The Dart side of the Android foreground service that keeps this app alive.
//
// Why it exists: without a foreground service the process is killed when the
// user swipes the app away, and Android freezes it on its own schedule anyway.
// Under buffer-and-flush that does not merely lose live data -- the ring has
// nowhere to flush to, so its buffer fills and the oldest readings are
// overwritten. The ring buffers ~98 hours and forgets nothing inside that, but
// a buffer nobody reads eventually starts losing its tail.
//
// This class only starts, stops and re-labels the service. The connection
// itself stays in Dart, in RingLink, running in the one isolate this app has
// (see SenseRingApplication.kt for how that isolate outlives the Activity).

import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

class MonitorService {
  static const _channel = MethodChannel('sensering/monitor');

  static bool get _supported => !kIsWeb && Platform.isAndroid;

  /// Starts the service, or re-labels it if it is already up.
  ///
  /// Returns false if Android refused the start. That is a real case rather
  /// than a bug: from Android 12 an app may not start a foreground service
  /// while it is in the background, so a start attempted from a reconnect that
  /// happened with the UI closed will be refused. The boot path does not go
  /// through here at all -- BootReceiver starts the service natively, inside
  /// the broadcast, which is one of the windows where a background start is
  /// still allowed.
  static Future<bool> start(String text) async {
    if (!_supported) return false;
    try {
      return await _channel.invokeMethod<bool>('start', {'text': text}) ??
          false;
    } on PlatformException catch (e) {
      debugPrint('[SenseRing] foreground service start failed: $e');
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  static Future<void> stop() async {
    if (!_supported) return;
    try {
      await _channel.invokeMethod<void>('stop');
    } on PlatformException catch (e) {
      debugPrint('[SenseRing] foreground service stop failed: $e');
    } on MissingPluginException {
      // Nothing to stop.
    }
  }

  /// Changes the notification text. Silently does nothing when the service is
  /// not running, which is the desired behaviour -- the caller is a periodic
  /// refresh that should not care.
  static Future<void> update(String text) async {
    if (!_supported) return;
    try {
      await _channel.invokeMethod<void>('update', {'text': text});
    } on PlatformException catch (e) {
      debugPrint('[SenseRing] notification update failed: $e');
    } on MissingPluginException {
      // Nothing to update.
    }
  }

  static Future<bool> isRunning() async {
    if (!_supported) return false;
    try {
      return await _channel.invokeMethod<bool>('isRunning') ?? false;
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  /// Notification ids, one per alert kind. Mirrors RingService.kt.
  ///
  /// One id per *kind* rather than per ring: a second alert of the same kind
  /// replaces the first. Two rings both off a finger is one thing to tell
  /// somebody, and a notification per ring per condition is how a tray becomes
  /// something people clear without reading.
  static const alertWear = 2001;
  static const alertBattery = 2002;

  /// Raises a user-visible alert. Separate channel from the monitoring
  /// notification, at high importance -- see RingService.alert().
  static Future<void> alert(int id, String title, String text) async {
    if (!_supported) return;
    try {
      await _channel.invokeMethod<void>(
          'alert', {'id': id, 'title': title, 'text': text});
    } on PlatformException catch (e) {
      debugPrint('[SenseRing] alert failed: $e');
    }
  }

  /// Withdraws an alert whose condition has cleared.
  static Future<void> clearAlert(int id) async {
    if (!_supported) return;
    try {
      await _channel.invokeMethod<void>('clearAlert', {'id': id});
    } on PlatformException catch (e) {
      debugPrint('[SenseRing] clearAlert failed: $e');
    }
  }
}
