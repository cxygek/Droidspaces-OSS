package com.droidspaces.app.service

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import com.droidspaces.app.util.HostNetworkManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Puts the port forwards back after the phone boots.
 *
 * The rules live in the kernel, so a reboot removes them and nothing else would
 * re-add them: the containers come back because the daemon starts them, but the
 * rules only existed while the Network tab was open.
 *
 * A boot broadcast gets a minute, so the retries happen inside it rather than on
 * timers: several OEMs push plain alarms into the next maintenance window, which
 * on this device means days later. The alarm is a fallback for gateways that take
 * even longer than the passes cover.
 *
 * The LAN cable itself needs nothing here: the backend re-asserts the veth and
 * the lease from the container monitors.
 */
class NetworkBootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED -> {
                apply(context)
                schedule(context, SECOND_PASS_MS)
            }
            ACTION_APPLY -> apply(context)
        }
    }

    private fun apply(context: Context) {
        val pending = goAsync()
        val power = context.getSystemService(PowerManager::class.java)
        val wake = power?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "$TAG:apply")
        wake?.acquire((PASS_COUNT * PASS_GAP_MS + 20_000L))

        CoroutineScope(Dispatchers.IO).launch {
            try {
                repeat(PASS_COUNT) { pass ->
                    if (pass > 0) delay(PASS_GAP_MS)
                    // Autostarted containers come up with the daemon, a few
                    // seconds apart; a later pass finds the ones that were not
                    // there yet.
                    val changed = HostNetworkManager.applyAll()
                    val routed = HostNetworkManager.syncGatewayRoute(
                        HostNetworkManager.status(),
                        HostNetworkManager.runningContainers(null)
                    )
                    Log.i(
                        TAG,
                        "network: pass ${pass + 1}/$PASS_COUNT, rules " +
                            (if (changed) "applied" else "already in place") +
                            ", gateway route " + (if (routed) "updated" else "unchanged")
                    )
                }
            } catch (t: Throwable) {
                Log.w(TAG, "network: could not apply after boot", t)
            } finally {
                wake?.takeIf { it.isHeld }?.release()
                pending.finish()
            }
        }
    }

    private fun schedule(context: Context, afterMs: Long) {
        val alarms = context.getSystemService(AlarmManager::class.java) ?: return
        val intent = Intent(context, NetworkBootReceiver::class.java).setAction(ACTION_APPLY)
        val pending = PendingIntent.getBroadcast(
            context,
            afterMs.toInt(),
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        alarms.setAndAllowWhileIdle(
            AlarmManager.ELAPSED_REALTIME_WAKEUP,
            SystemClock.elapsedRealtime() + afterMs,
            pending
        )
    }

    private companion object {
        const val TAG = "NetworkBoot"
        const val ACTION_APPLY = "com.droidspaces.app.action.APPLY_NETWORK"

        /** 0 s, 12 s and 24 s: enough for a daemon that walks its autostart list. */
        const val PASS_COUNT = 3
        const val PASS_GAP_MS = 12_000L

        /** Fallback for anything slower; some OEMs defer it, and it costs nothing. */
        const val SECOND_PASS_MS = 120_000L
    }
}
