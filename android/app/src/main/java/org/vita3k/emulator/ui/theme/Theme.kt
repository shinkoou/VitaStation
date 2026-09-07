package org.vita3k.emulator.ui.theme

import android.app.Activity
import android.os.Build
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.WindowInsetsControllerCompat

private val AppBackgroundColor = Color(0xFF070A14)
private val AppTopBarColor = Color(0xFF0D1326)

private val AppColorScheme = darkColorScheme(
    primary = Color(0xFF59D8FF),
    onPrimary = Color(0xFF001F2A),
    primaryContainer = Color(0xFF123C54),
    onPrimaryContainer = Color(0xFFC7F2FF),
    secondary = Color(0xFF8B5CF6),
    onSecondary = Color(0xFF180D33),
    secondaryContainer = Color(0xFF2A1D4F),
    onSecondaryContainer = Color(0xFFE7DDFF),
    tertiary = Color(0xFFD946EF),
    onTertiary = Color(0xFF2D0034),
    background = AppBackgroundColor,
    onBackground = Color(0xFFF7FAFF),
    surface = AppTopBarColor,
    onSurface = Color(0xFFF7FAFF),
    surfaceVariant = Color(0xFF17213A),
    onSurfaceVariant = Color(0xFF95A3BC),
    surfaceContainerHigh = Color(0xFF11182A),
    surfaceContainerHighest = Color(0xFF17213A),
    outline = Color(0xFF24314B),
    outlineVariant = Color(0xFF1B2942),
    error = Color(0xFFFF6B7A),
    onError = Color(0xFF300008)
)

@Composable
@Suppress("DEPRECATION")
fun Vita3KTheme(
    content: @Composable () -> Unit
) {
    val view = LocalView.current
    if (!view.isInEditMode) {
        val transparentSystemBar = Color.Transparent.toArgb()
        SideEffect {
  val window = (view.context as? Activity)?.window ?: return@SideEffect
  window.statusBarColor = transparentSystemBar
  window.navigationBarColor = transparentSystemBar
  WindowInsetsControllerCompat(window, view).apply {
      isAppearanceLightStatusBars = false
      isAppearanceLightNavigationBars = false
  }
  if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      window.isStatusBarContrastEnforced = false
      window.isNavigationBarContrastEnforced = false
  }
        }
    }

    MaterialTheme(
        colorScheme = AppColorScheme,
        content = content
    )
}

const val SCRIM_ALPHA = 0.46f

@Composable
fun ApplyDialogDim() {
    val view = LocalView.current
    if (!view.isInEditMode) {
        DisposableEffect(Unit) {
  val provider = (view as? DialogWindowProvider)
      ?: (view.parent as? DialogWindowProvider)
  provider?.window?.setDimAmount(SCRIM_ALPHA)
  onDispose {}
        }
    }
}
