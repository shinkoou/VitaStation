package org.vita3k.emulator.ui.screens.settings

import androidx.annotation.StringRes
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Language
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.PhotoCamera
import androidx.compose.material.icons.filled.Public
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SettingsSuggest
import androidx.compose.material.icons.filled.ShowChart
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.TouchApp
import androidx.compose.material.icons.filled.Tune
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import org.vita3k.emulator.R

internal enum class SettingsScope(@StringRes val labelRes: Int?) {
    Both(labelRes = null),
    Global(labelRes = R.string.settings_scope_global),
    PerApp(labelRes = R.string.settings_scope_per_app)
}

internal enum class SettingsCategory(
    @StringRes val labelRes: Int,
    val icon: ImageVector
) {
    Core(R.string.settings_tab_core, Icons.Default.Tune),
    Cpu(R.string.settings_tab_cpu, Icons.Default.Speed),
    Gpu(R.string.settings_tab_gpu, Icons.Default.GraphicEq),
    Performance(R.string.settings_tab_performance, Icons.Default.ShowChart),
    Audio(R.string.settings_tab_audio, Icons.AutoMirrored.Filled.VolumeUp),
    Camera(R.string.settings_tab_camera, Icons.Default.PhotoCamera),
    System(R.string.settings_tab_system, Icons.Default.Language),
    Controls(R.string.settings_tab_controls, Icons.Default.TouchApp),
    Interface(R.string.settings_tab_interface, Icons.Default.Settings),
    Emulator(R.string.settings_tab_emulator, Icons.Default.SettingsSuggest),
    Network(R.string.settings_tab_network, Icons.Default.Public),
    Debug(R.string.settings_tab_debug, Icons.Default.BugReport)
}

internal fun settingsCategories(isPerApp: Boolean): List<SettingsCategory> {
    val ordered = listOf(
        SettingsCategory.System,
        SettingsCategory.Gpu,
        SettingsCategory.Performance,
        SettingsCategory.Controls,
        SettingsCategory.Audio,
        SettingsCategory.Interface,
        SettingsCategory.Cpu,
        SettingsCategory.Core,
        SettingsCategory.Camera,
        SettingsCategory.Network,
        SettingsCategory.Emulator,
        SettingsCategory.Debug
    )
    return if (isPerApp) {
        ordered.filter {
            it != SettingsCategory.Camera &&
                it != SettingsCategory.Interface &&
                it != SettingsCategory.Performance
        }
    } else {
        ordered
    }
}

@Composable
internal fun SettingsCategory.label(): String = stringResource(labelRes)

internal data class SettingsHelpEntry(
    val title: String,
    val body: String,
    val scope: SettingsScope = SettingsScope.Both
)

internal data class SettingsSearchEntry(
    val category: SettingsCategory,
    val title: String,
    val summary: String,
    val keywords: String,
    val help: SettingsHelpEntry? = null
)
