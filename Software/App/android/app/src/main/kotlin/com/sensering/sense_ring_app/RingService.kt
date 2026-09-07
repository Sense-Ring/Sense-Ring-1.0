package com.sensering.sense_ring_app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * The foreground service that keeps this app's process -- and therefore its
 * BLE connection -- alive when nobody is looking at the phone.
 *
 * It holds no Bluetooth state of its own. Everything about the ring stays in
 * Dart (RingLink); this exists purely so Android does not kill the process the
 * link runs in. The type is `connectedDevice`, which is what Android 14+
 * requires a service like this to declare, and it is honest: the app is
 * attached to a device.
 *
 * The notification is not decoration. It is the only always-visible surface
 * this system has, so it carries how long ago the ring last reported: an app
 * that has quietly stopped collecting is otherwise indistinguishable from one
 * that is working. Dart re-labels it through [MonitorChannel].
 */
class RingService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopForegroundCompat()
            running = false
            stopSelf()
            return START_NOT_STICKY
        }

        // A null intent means Android restarted us after killing the process
        // (START_STICKY). There is no text to restore then, and no Dart state
        // either -- the engine is starting from scratch behind us and will
        // re-label this within a minute.
        lastText = intent?.getStringExtra(EXTRA_TEXT) ?: lastText

        val notification = buildNotification(this, lastText)
        if (!running) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(
                    NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
                )
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            running = true
        } else {
            notificationManager(this).notify(NOTIFICATION_ID, notification)
        }

        // Restart us if the process is killed for memory: the whole point is
        // that monitoring outlives things the user did not decide.
        return START_STICKY
    }

    /**
     * Swiping the app away must not stop monitoring -- that is the exact case
     * this service exists for. `android:stopWithTask="false"` in the manifest
     * is what actually keeps us alive; this override is here so the default
     * (stopSelf) can never creep back in.
     */
    override fun onTaskRemoved(rootIntent: Intent?) {
        Log.i(TAG, "task removed; monitoring continues")
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }

    companion object {
        private const val TAG = "SenseRingService"
        private const val CHANNEL_ID = "sensering_monitor"
        private const val ALERT_CHANNEL_ID = "sensering_alerts"
        const val NOTIFICATION_ID = 1001

        /** One id per alert kind, so a repeat replaces rather than stacks. */
        const val ALERT_ID_WEAR = 2001
        const val ALERT_ID_BATTERY = 2002

        const val ACTION_STOP = "com.sensering.sense_ring_app.STOP"
        const val EXTRA_TEXT = "text"

        private const val DEFAULT_TEXT = "Monitoring the ring"

        /** Read from Dart to show whether monitoring is actually up. */
        @Volatile
        var running: Boolean = false
            private set

        @Volatile
        private var lastText: String = DEFAULT_TEXT

        fun notificationManager(context: Context): NotificationManager =
            context.getSystemService(Context.NOTIFICATION_SERVICE)
                    as NotificationManager

        /**
         * Starts the service. Throws if Android refuses a background start
         * (API 31+); callers decide what to do about that.
         */
        fun launch(context: Context, text: String) {
            lastText = text
            val intent = Intent(context, RingService::class.java)
                .putExtra(EXTRA_TEXT, text)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            val intent = Intent(context, RingService::class.java)
                .setAction(ACTION_STOP)
            // Plain startService: a stop must never be the thing that trips the
            // "started a foreground service from the background" rule.
            try {
                context.startService(intent)
            } catch (e: IllegalStateException) {
                Log.w(TAG, "could not deliver stop: $e")
            }
        }

        /**
         * Re-labels the live notification without going near the service-start
         * rules. Posting to an existing notification id is always allowed,
         * whereas another startForegroundService() from the background would be
         * refused -- and a refused re-label would leave the notification saying
         * "last reading 2 min ago" hours after the ring went quiet, which is
         * worse than saying nothing.
         */
        fun relabel(context: Context, text: String) {
            lastText = text
            notificationManager(context)
                .notify(NOTIFICATION_ID, buildNotification(context, text))
        }

        fun buildNotification(context: Context, text: String): Notification {
            ensureChannel(context)

            val open = Intent(context, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                        Intent.FLAG_ACTIVITY_CLEAR_TOP
            }
            var pendingFlags = PendingIntent.FLAG_UPDATE_CURRENT
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                pendingFlags = pendingFlags or PendingIntent.FLAG_IMMUTABLE
            }
            val tap = PendingIntent.getActivity(context, 0, open, pendingFlags)

            val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                Notification.Builder(context, CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION")
                Notification.Builder(context)
                    .setPriority(Notification.PRIORITY_LOW)
            }

            return builder
                .setContentTitle("SenseRing")
                .setContentText(text)
                .setSmallIcon(R.drawable.ic_ring_monitor)
                .setContentIntent(tap)
                .setOngoing(true)
                .setShowWhen(false)
                .build()
        }

        /**
         * Raises a user-visible alert: the ring came off, or its cell is low.
         *
         * **A separate channel, and that is the point.** The monitoring
         * notification is IMPORTANCE_LOW and permanently present, which is
         * correct for something that must never make a sound and never
         * disappear -- and exactly wrong for an alert, which has to interrupt.
         * Posting an alert as a re-label of the ongoing one would put it in the
         * one place a user has already learned to ignore.
         *
         * One id per kind, so a second "battery low" replaces the first rather
         * than stacking. `ongoing = false`, so the user can dismiss it; the
         * condition being real is the ring's business, and being told twice
         * about something you have acknowledged is how people learn to swipe
         * without reading.
         */
        fun alert(context: Context, id: Int, title: String, text: String) {
            ensureAlertChannel(context)

            val open = Intent(context, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                        Intent.FLAG_ACTIVITY_CLEAR_TOP
            }
            var pendingFlags = PendingIntent.FLAG_UPDATE_CURRENT
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                pendingFlags = pendingFlags or PendingIntent.FLAG_IMMUTABLE
            }
            val tap = PendingIntent.getActivity(context, id, open, pendingFlags)

            val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                Notification.Builder(context, ALERT_CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION")
                Notification.Builder(context)
                    .setPriority(Notification.PRIORITY_HIGH)
            }

            notificationManager(context).notify(
                id,
                builder
                    .setContentTitle(title)
                    .setContentText(text)
                    .setStyle(Notification.BigTextStyle().bigText(text))
                    .setSmallIcon(R.drawable.ic_ring_monitor)
                    .setContentIntent(tap)
                    .setAutoCancel(true)
                    .setOnlyAlertOnce(true)
                    .build()
            )
        }

        /** Withdraws an alert whose condition has cleared. */
        fun clearAlert(context: Context, id: Int) {
            notificationManager(context).cancel(id)
        }

        private fun ensureAlertChannel(context: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val manager = notificationManager(context)
            if (manager.getNotificationChannel(ALERT_CHANNEL_ID) != null) return
            val channel = NotificationChannel(
                ALERT_CHANNEL_ID,
                "Ring alerts",
                // HIGH, unlike the monitoring channel: these are the ones that
                // are supposed to interrupt.
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = "The ring came off, or its battery is running out."
                setShowBadge(true)
            }
            manager.createNotificationChannel(channel)
        }

        private fun ensureChannel(context: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val manager = notificationManager(context)
            if (manager.getNotificationChannel(CHANNEL_ID) != null) return
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Ring monitoring",
                // LOW: permanently present, so it must never make a sound.
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Shown while the ring is being monitored."
                setShowBadge(false)
            }
            manager.createNotificationChannel(channel)
        }
    }
}
