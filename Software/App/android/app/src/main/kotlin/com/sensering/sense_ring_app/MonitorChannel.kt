package com.sensering.sense_ring_app

import android.content.Context
import android.util.Log
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

/**
 * Dart's handle on [RingService].
 *
 * Also owns the one preference that has to live outside SQLite: whether the
 * user wants monitoring at all. [BootReceiver] reads it before Dart -- or the
 * database -- has had a chance to open, so SharedPreferences is the only store
 * that can answer in time.
 */
object MonitorChannel {

    private const val TAG = "SenseRingMonitor"
    private const val NAME = "sensering/monitor"
    private const val PREFS = "sensering"
    private const val KEY_ENABLED = "monitor_enabled"

    // Held so the channel is not collected while the engine lives.
    private var channel: MethodChannel? = null

    fun attach(context: Context, engine: FlutterEngine) {
        val app = context.applicationContext
        val c = MethodChannel(engine.dartExecutor.binaryMessenger, NAME)
        c.setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> result.success(start(app, call.argument("text") ?: ""))
                "stop" -> {
                    stop(app)
                    result.success(null)
                }
                "update" -> {
                    update(app, call.argument("text") ?: "")
                    result.success(null)
                }
                "alert" -> {
                    RingService.alert(
                        app,
                        call.argument<Int>("id") ?: RingService.ALERT_ID_WEAR,
                        call.argument<String>("title") ?: "SenseRing",
                        call.argument<String>("text") ?: ""
                    )
                    result.success(null)
                }
                "clearAlert" -> {
                    RingService.clearAlert(
                        app, call.argument<Int>("id") ?: RingService.ALERT_ID_WEAR
                    )
                    result.success(null)
                }
                "isRunning" -> result.success(RingService.running)
                else -> result.notImplemented()
            }
        }
        channel = c
    }

    fun isEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENABLED, false)

    private fun setEnabled(context: Context, on: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_ENABLED, on)
            .apply()
    }

    /**
     * Returns false when Android refuses the start, which from API 31 it does
     * for any foreground service started while the app is in the background.
     * That is a real state, not an error to swallow: the link keeps working for
     * as long as the process happens to survive, but the guarantee is gone and
     * Dart says so in its log.
     */
    private fun start(context: Context, text: String): Boolean {
        // Set first: if the start is refused now, a reboot should still bring
        // monitoring back, because the user's intent has not changed.
        setEnabled(context, true)
        if (RingService.running) {
            update(context, text)
            return true
        }
        return try {
            RingService.launch(context, text)
            true
        } catch (e: Exception) {
            Log.w(TAG, "foreground service start refused: $e")
            false
        }
    }

    private fun stop(context: Context) {
        setEnabled(context, false)
        RingService.stop(context)
    }

    private fun update(context: Context, text: String) {
        if (!RingService.running) return
        try {
            RingService.relabel(context, text)
        } catch (e: Exception) {
            // A failed re-label is cosmetic; the service is still up.
            Log.w(TAG, "notification update failed: $e")
        }
    }
}
