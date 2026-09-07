package com.sensering.sense_ring_app

import android.app.Application
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterEngineCache
import io.flutter.embedding.engine.dart.DartExecutor

/**
 * Owns the one Flutter engine this app ever runs.
 *
 * A normal Flutter app hangs its engine off the Activity, so the Dart isolate
 * dies the moment the user swipes the app away -- taking the BLE connection,
 * the record decoder and the acknowledgement bookkeeping with it. That is the
 * failure this whole class exists to remove: the ring buffers for days, but a
 * ring nobody collects from eventually overwrites its own oldest readings.
 *
 * So the engine is created here, at process start, and cached. MainActivity
 * attaches to it and detaches again without destroying it; RingService keeps
 * the process alive so the isolate underneath keeps running. There is exactly
 * one isolate either way, which is deliberate: the alternative -- a background
 * isolate, or the connection rewritten natively -- means two copies of the
 * record decoder, and two copies of a decoder are two chances to disagree about
 * a wearer's heart rate.
 *
 * Creating the engine here also covers the boot path, where BootReceiver starts
 * the process with no Activity at all. Dart's main() runs regardless; the UI is
 * held back until there is a window to draw into (see _UiGate in main.dart).
 */
class SenseRingApplication : Application() {

    override fun onCreate() {
        super.onCreate()

        // Registers the app's plugins automatically, then runs main().
        val engine = FlutterEngine(this)
        engine.dartExecutor.executeDartEntrypoint(
            DartExecutor.DartEntrypoint.createDefault()
        )
        FlutterEngineCache.getInstance().put(ENGINE_ID, engine)

        // On the engine's messenger rather than the Activity's, because the
        // service has to be controllable when there is no Activity.
        MonitorChannel.attach(this, engine)
    }

    companion object {
        const val ENGINE_ID = "sensering_engine"
    }
}
