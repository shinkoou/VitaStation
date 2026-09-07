package org.vita3k.emulator.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib

object VitaStationMigrations {
    private const val PREFS = "vitastation_migrations"
    private const val VALIDATION_LAYER_SAFE_DEFAULT_V1 = "validation_layer_safe_default_v1"

    suspend fun apply(context: Context) = withContext(Dispatchers.IO) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (prefs.getBoolean(VALIDATION_LAYER_SAFE_DEFAULT_V1, false)) return@withContext

        runCatching {
            if (NativeLib.isInitialized()) {
                val config = NativeLib.getGlobalConfig()
                if (config.validationLayer) {
                    config.validationLayer = false
                    NativeLib.saveSettings(null, config)
                }
            }
        }

        prefs.edit()
            .putBoolean(VALIDATION_LAYER_SAFE_DEFAULT_V1, true)
            .commit()
    }
}
