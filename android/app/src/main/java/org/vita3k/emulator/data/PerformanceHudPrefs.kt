package org.vita3k.emulator.data

import android.content.Context
import org.vita3k.emulator.NativeLib

object PerformanceHudPrefs {
    private const val PREFS = "vitastation_performance_hud"
    private const val KEY_MASTER = "master"
    const val KEY_FPS = "fps"
    const val KEY_RAM = "ram"
    const val KEY_CPU = "cpu"
    const val KEY_GPU = "gpu"
    const val KEY_BATTERY = "battery"

    @JvmStatic
    fun isMasterEnabled(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (prefs.contains(KEY_MASTER)) {
            return prefs.getBoolean(KEY_MASTER, false)
        }

        val enabled = runCatching {
            NativeLib.getGlobalConfig().performanceOverlay
        }.getOrDefault(false)

        prefs.edit().putBoolean(KEY_MASTER, enabled).apply()
        return enabled
    }

    @JvmStatic
    fun setMasterEnabled(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_MASTER, enabled)
            .apply()
    }

    @JvmStatic
    fun get(context: Context, key: String): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(key, true)

    @JvmStatic
    fun set(context: Context, key: String, value: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(key, value)
            .apply()
    }
}
