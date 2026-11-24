package com.droidspaces.app.ui.terminal

import com.droidspaces.app.util.Constants
import com.termux.terminal.TerminalEmulator
import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient

/**
 * Creates a [TerminalSession] that opens a root shell inside a Droidspaces container.
 *
 * ── How TerminalSession passes args to execvp ────────────────────────────────
 *
 * termux.c JNI creates:  execvp(shellPath, argv)
 * where argv IS the Java args array - shellPath is NOT prepended as argv[0].
 *
 * Therefore args[0] MUST be the shell path (argv[0] convention), otherwise:
 *   args = ["-c", "cmd"]  →  execvp(sh, ["-c", "cmd"])
 *   shell sees argv[0]="-c" (its own name), treats "cmd" as a script FILE path
 *   → error: "c: cmd: No such file or directory"   ← exactly what we saw
 *
 * Correct:
 *   args = ["/system/bin/sh", "-c", "cmd"]  →  execvp(sh, ["sh", "-c", "cmd"])
 *   shell sees argv[0]="sh", argv[1]="-c", argv[2]="cmd"  ← works correctly
 *
 * ── Command shape (confirmed on device) ─────────────────────────────────────
 *
 *   sh -c "exec su -c 'droidspaces -n Name enter user'"
 *
 * The shell termux starts exec's su, and su exec's the final command, so one
 * process replaces the next instead of stacking. That is how the old
 *   sh -c "su -c '...'"  →  sh + su + droidspaces
 * becomes
 *   sh -c "exec su -c '...'"  →  sh(→su→droidspaces)  [one process]
 *
 * Name quoting decides whether su can exec directly. A name with no spaces is
 * passed as one bare token and su exec's it; a quoted name makes su route the
 * command through a shell of its own, adding a process and leaving one that
 * is not a segment of ours. Names are therefore quoted only when they contain
 * whitespace (see create()).
 *
 * execvp's first arg stays /system/bin/sh: termux only accepts a real shell
 * as its shellPath (using su directly failed with ENOENT), so sh is retained
 * and `exec` is what removes the layer.
 *
 * Mapped to TerminalSession:
 *   shell = "/system/bin/sh"
 *   args  = ["/system/bin/sh", "-c", "exec su -c '...'"]
 */
object DroidspacesTerminalSession {

    // Container names allow letters, digits, spaces, '_', '.', '-' (mirrors
    // ValidationUtils.validateContainerName), none are shell metacharacters, so
    // a validated name is safe inside the "..." payload. A username discovered
    // from the container's /etc/passwd is NOT validated elsewhere, so it is
    // checked here too.
    private val SAFE_NAME = Regex("^[a-zA-Z0-9_\\s.-]+$")
    private val SAFE_USER = Regex("^[A-Za-z0-9_.@:+-]+$")
    private val SAFE_SESSION_ID = Regex("^[0-9a-fA-F]{32}$")

    fun create(
        sessionClient: TerminalSessionClient,
        containerName: String?,
        containerUser: String? = null,
        sessionId: String? = null,
    ): TerminalSession {
        val safeName = containerName?.takeIf { SAFE_NAME.matches(it) }
        val safeSessionId = sessionId?.takeIf { SAFE_SESSION_ID.matches(it) }
        // No container, or a name carrying shell metacharacters: a bare root shell
        // rather than an injectable payload. A username with metacharacters falls
        // back to root the same way. safeName cannot hold " ' $ or `, the escape is
        // belt-and-suspenders.
        if (safeName == null) return newRootShell(sessionClient)

        val user = containerUser?.takeIf { SAFE_USER.matches(it) } ?: "root"
        // Name the keeper session after the tab the app already created, so
        // closing that tab can end exactly this shell. Use the absolute binary
        // path: the command runs through su -c, which may not inherit our
        // PATH, so do not rely on `droidspaces` being found by name.
        val sessionOpt = safeSessionId?.let { "--session-id $it " } ?: ""
        // Quote the name only when it actually needs it. Magisk/KernelSU `su -c`
        // execs a simple command directly, but forks a /system/bin/sh as soon as
        // the command string contains shell quoting. An unquoted single-token
        // name therefore avoids one extra shell process per terminal tab.
        val nameArg = if (safeName.any { it.isWhitespace() })
            "\"${safeName.replace("\"", "\\\"")}\"" else safeName
        val command = "${Constants.DROIDSPACES_BINARY_PATH} ${sessionOpt}-n $nameArg enter $user"
        return newSession(sessionClient, command)
    }

    /**
     * Attach to a session the backend keeper still owns. The keeper keeps the
     * container pty and replays its scrollback, so this command only relays the
     * app's terminal to it; it does not start a new shell.
     */
    fun createAttach(
        sessionClient: TerminalSessionClient,
        sessionId: String,
    ): TerminalSession {
        val safeId = sessionId.takeIf { SAFE_SESSION_ID.matches(it) }
        // Ids come from the backend's JSON, so this is only a guard against a
        // malformed record. Say why the tab is empty instead of opening a root
        // shell the user did not ask for.
        if (safeId == null)
            return newSession(sessionClient, "echo 'Invalid terminal session id.'")
        return newSession(sessionClient,
            "${Constants.DROIDSPACES_BINARY_PATH} session attach $safeId")
    }

    /** A bare root shell (no container): sh exec's su, so no shell is left behind. */
    private fun newRootShell(sessionClient: TerminalSessionClient): TerminalSession =
        newSession(sessionClient, "exec su", quote = false)

    /**
     * Build a session that exec's su, which exec's `command`.
     *
     * Both execs replace their process instead of stacking a new one: the shell
     * termux starts becomes su, and su becomes the final command. Verified at
     * the command line as both root and the app's untrusted UID. For an
     * unquoted command only one process remains instead of the old
     * `sh -c "su -c '...'"` pair; quoting (a name with spaces) is what makes su
     * add a shell of its own.
     *
     * args[0] MUST be the shell path: termux's JNI passes args straight to
     * execvp and does not prepend shellPath as argv[0]. It must stay sh - su
     * as argv[0] fails to exec (termux restricts the shell to a real shell).
     */
    private fun newSession(
        sessionClient: TerminalSessionClient,
        command: String,
        quote: Boolean = true,
    ): TerminalSession = TerminalSession(
        /* shell      = */ "/system/bin/sh",
        /* workingDir = */ "/sdcard",
        /* args       = */ arrayOf("/system/bin/sh", "-c",
            if (quote) "exec su -c '${command.replace("'", "'\\''")}'" else command),
        /* env        = */ buildEnv().toTypedArray(),
        /* rows       = */ TerminalEmulator.DEFAULT_TERMINAL_TRANSCRIPT_ROWS,
        /* client     = */ sessionClient,
    )

    private fun buildEnv(): List<String> {
        val systemPath = System.getenv("PATH") ?: "/sbin:/system/bin:/system/xbin"
        val env = mutableListOf(
            "TERM=xterm-256color",
            "COLORTERM=truecolor",
            "LANG=C.UTF-8",
            "HOME=/root",
            "PATH=${Constants.INSTALL_PATH}:$systemPath",
            "EXTERNAL_STORAGE=${System.getenv("EXTERNAL_STORAGE") ?: "/sdcard"}",
        )
        listOf(
            "ANDROID_ART_ROOT", "ANDROID_DATA", "ANDROID_I18N_ROOT",
            "ANDROID_ROOT", "ANDROID_RUNTIME_ROOT", "ANDROID_TZDATA_ROOT",
        ).forEach { key ->
            System.getenv(key)?.let { env.add("$key=$it") }
        }
        return env
    }
}
