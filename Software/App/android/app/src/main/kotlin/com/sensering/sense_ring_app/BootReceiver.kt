package com.sensering.sense_ring_app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Restarts monitoring after the phone reboots.
 *
 * Without this, a phone that restarts at 3am stops monitoring until somebody
 * opens the app -- and nobody opens the app, which is the entire premise of the
 * foreground service. The ring would keep buffering and lose nothing, but the
 * alert path would be down for as long as it took a person to notice, which is
 * exactly the failure the ring's buffer cannot compensate for.
 *
 * Starting the service here also starts the process, which runs
 * SenseRingApplication.onCreate, which runs Dart's main(), which brings the
 * link back up. No Activity is involved at any point.
 *
 * The start is allowed from inside this broadcast: a background foreground
 * service start would normally be refused on API 31+, but BOOT_COMPLETED is one
 * of the exempt windows, and `connectedDevice` is a type Android permits to be
 * started from boot. It is still wrapped, because a refusal here must not
 * crash the receiver -- the user opening the app once restores monitoring.
 */
class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return

        // Only if the user had monitoring on when the phone went down. Someone
        // who pressed Disconnect should not find it running again after a
        // reboot, having never asked for it back.
        if (!MonitorChannel.isEnabled(context)) return

        try {
            RingService.launch(context, "Reconnecting after restart")
        } catch (e: Exception) {
            Log.w("SenseRingBoot", "could not restart monitoring: $e")
        }
    }
}
