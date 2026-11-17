package com.droidspaces.app.util

import android.util.Log
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * Terminal sessions the backend keeper still owns after the app that opened
 * them went away. The backend publishes one JSON record per session, keyed by
 * a 32 character hex id, so the terminal screen can offer the same shell back
 * instead of starting a new one.
 */
object TerminalSessionManager {
    private const val TAG = "TerminalSessionManager"

    // The backend generates ids as 32 lowercase hex characters. Validate before
    // putting one into a root shell command or a socket name.
    private val SESSION_ID = Regex("^[0-9a-fA-F]{32}$")

    data class BackendSession(
        val id: String,
        val container: String,
        val user: String,
        val created: Long,
    )

    /**
     * Live sessions of one container, oldest first. Returns an empty list when
     * the installed backend predates session management, so the caller can fall
     * back to opening a new shell.
     */
    suspend fun listSessions(containerName: String? = null): List<BackendSession> =
        withContext(Dispatchers.IO) {
            try {
                val command = ContainerCommandBuilder.buildSessionListCommand(containerName)
                val result = Shell.cmd("$command 2>/dev/null").exec()
                if (!result.isSuccess) return@withContext emptyList()

                val root = JSONObject(result.out.joinToString(""))
                val sessions = root.optJSONArray("sessions") ?: return@withContext emptyList()
                (0 until sessions.length()).mapNotNull { index ->
                    val obj = sessions.optJSONObject(index) ?: return@mapNotNull null
                    val id = obj.optString("id")
                    if (!SESSION_ID.matches(id)) return@mapNotNull null
                    BackendSession(
                        id = id,
                        container = obj.optString("container"),
                        user = obj.optString("user").ifBlank { "root" },
                        created = obj.optLong("created"),
                    )
                }
            } catch (e: Exception) {
                Log.w(TAG, "Failed to list terminal sessions", e)
                emptyList()
            }
        }

    /**
     * Containers that still have a keeper session behind them, in one backend
     * call. The panel reads this because its own registry lives in the process:
     * after the app is killed the shells are still there, but nothing local
     * remembers them, and the card has to say so.
     */
    suspend fun containersWithSessions(): Set<String> =
        listSessions().map { it.container }.filter { it.isNotEmpty() }.toSet()

    /**
     * End one backend session. Fire and forget: closing a tab must not wait for
     * a root shell, and a session that is already gone is not an error.
     */
    fun killSessionAsync(sessionId: String) {
        if (!SESSION_ID.matches(sessionId)) return
        try {
            val command = ContainerCommandBuilder.buildSessionKillCommand(sessionId)
            Shell.cmd("$command 2>/dev/null").submit()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to kill terminal session $sessionId", e)
        }
    }
}
