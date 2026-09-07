package org.vita3k.emulator.data

import android.content.Context
import org.vita3k.emulator.NativeLib

object PerformanceHudPrefs {
    private const val PREFS = "vitastation_performance_hud"
    const val KEY_FPS = "fps"
    const val KEY_RAM = "ram"
    const val KEY_CPU = "cpu"
    const val KEY_GPU = "gpu"
    const val KEY_BATTERY = "battery"

    @JvmStatic
    fun isMasterEnabled(context: Context): Boolean =
        runCatching { NativeLib.getGlobalConfig().performanceOverlay }.getOrDefault(false)

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
