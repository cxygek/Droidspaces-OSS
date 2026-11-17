package com.droidspaces.app.util

import android.content.Context
import android.os.Build

/**
 * Single source of truth for mapping the device's primary ABI to the arch
 * strings Droidspaces uses (the bundled-binary suffix, which also matches the
 * arch field in rootfs.json), plus a human-readable name. Previously this
 * mapping was duplicated across BinaryInstaller and RootfsRepository.
 */
object DeviceArch {
    /** Every Android device is ARM: aarch64, or armhf on a 32-bit userspace. */
    private fun nativeSuffix(): String {
        val abi = Build.SUPPORTED_ABIS[0]
        return when {
            abi.contains("arm64") || abi.contains("aarch64") -> "aarch64"
            abi.contains("armeabi") || abi.contains("arm")   -> "armhf"
            else                                             -> "aarch64"
        }
    }

    /**
     * True on a 32-bit userspace whose kernel can run 64-bit binaries, so the
     * "use the 64-bit backend" switch is worth showing. A 32-bit ARM kernel calls
     * itself armv7l even on a v8 core; armv8l is the arm64 kernel's compat name and
     * aarch64 what mainline reports to compat tasks.
     */
    fun is32Bit(): Boolean = nativeSuffix() == "armhf" &&
        SystemInfoManager.architecture.let { it == "armv8l" || it == "aarch64" }

    /**
     * Binary-suffix / rootfs arch: aarch64 or armhf. Some budget devices run a
     * 32-bit OS on a 64-bit kernel, so the user can opt into the 64-bit binaries
     * and rootfs images; the kernel runs them fine.
     */
    fun suffix(context: Context): String {
        val native = nativeSuffix()
        if (!PreferencesManager.getInstance(context).treatAs64Bit) return native
        return if (native == "armhf") "aarch64" else native
    }

    /** Human-readable architecture name for display. */
    fun displayName(context: Context): String = when (val s = suffix(context)) {
        "aarch64" -> "ARM64 (aarch64)"
        "armhf"   -> "ARM (armhf)"
        else      -> s
    }
}
