package com.sensering.sense_ring_app

import android.content.Context
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterEngineCache

/**
 * A window onto the engine, not the owner of it.
 *
 * Returning the cached engine from [provideFlutterEngine] is what makes the
 * Dart isolate survive this Activity being destroyed -- which is what happens
 * when the app is swiped away, and is precisely when monitoring has to carry
 * on. Flutter treats a host-provided engine as not its to destroy; the explicit
 * [shouldDestroyEngineWithHost] says the same thing again, because getting it
 * wrong fails silently and looks like an Android battery-killer bug.
 */
class MainActivity : FlutterActivity() {

    override fun provideFlutterEngine(context: Context): FlutterEngine? =
        FlutterEngineCache.getInstance().get(SenseRingApplication.ENGINE_ID)

    override fun shouldDestroyEngineWithHost(): Boolean = false
}
