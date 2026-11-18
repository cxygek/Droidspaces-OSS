package com.droidspaces.app.util

/**
 * Language data class representing a supported language in the app.
 *
 * @param code Language code (e.g., "en", "zh-rCN")
 * @param displayName Display name in English (e.g., "English", "Chinese (Simplified)")
 * @param nativeName Native name of the language (e.g., "English", "简体中文")
 */
data class Language(
    val code: String,
    val displayName: String,
    val nativeName: String
)
