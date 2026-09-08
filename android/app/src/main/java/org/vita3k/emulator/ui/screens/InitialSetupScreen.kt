package org.vita3k.emulator.ui.screens

import android.view.Gravity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.ArrowForward
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import org.vita3k.emulator.R
import org.vita3k.emulator.data.FirmwareInstallState
import org.vita3k.emulator.data.FrameGenerationManager
import org.vita3k.emulator.data.FirmwareLinks
import org.vita3k.emulator.ui.components.HtmlText

private val setupPanelColor = Color(0xE60B1020)
private val setupCardColor = Color(0xFF11182A)
private val setupTextColor = Color(0xFFF7FAFF)

@Composable
fun InitialSetupScreen(
    firmwareInstallState: FirmwareInstallState,
    preferredLanguageIndex: Int,
    onInstallFirmware: () -> Unit,
    dismissLabel: String,
    onSkip: () -> Unit,
    onFinish: () -> Unit
) {
    val systemBars = WindowInsets.systemBars.asPaddingValues()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var frameGenerationReady by remember { mutableStateOf(FrameGenerationManager.isReady(context)) }
    var frameGenerationImporting by remember { mutableStateOf(false) }
    var frameGenerationImportResult by remember { mutableStateOf<Int?>(null) }

    val losslessPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult

        scope.launch {
            frameGenerationImporting = true
            frameGenerationImportResult = FrameGenerationManager.importLosslessDll(context, uri)
            frameGenerationReady = FrameGenerationManager.isReady(context)
            frameGenerationImporting = false
        }
    }

    var page by rememberSaveable { mutableIntStateOf(0) }
    var firmwareLocaleIndex by rememberSaveable {
        mutableIntStateOf(FirmwareLinks.coerceLocaleIndex(preferredLanguageIndex))
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.verticalGradient(
                    listOf(
                        MaterialTheme.colorScheme.primary.copy(alpha = 0.16f),
                        MaterialTheme.colorScheme.background,
                        MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.38f)
                    )
                )
            )
    ) {
        BackgroundOrb(
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(start = 24.dp, top = 88.dp),
            size = 180.dp,
            color = MaterialTheme.colorScheme.primary.copy(alpha = 0.09f)
        )
        BackgroundOrb(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(end = 8.dp, bottom = 72.dp),
            size = 220.dp,
            color = MaterialTheme.colorScheme.tertiary.copy(alpha = 0.08f)
        )

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(
                    start = 16.dp,
                    end = 16.dp,
                    top = systemBars.calculateTopPadding() + 4.dp,
                    bottom = systemBars.calculateBottomPadding() + 12.dp
                )
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                if (page > 0) {
                    TextButton(onClick = { page-- }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.initial_setup_back))
                    }
                } else {
                    Spacer(modifier = Modifier.width(96.dp))
                }
                Spacer(modifier = Modifier.weight(1f))
                TextButton(onClick = onSkip) {
                    Text(dismissLabel)
                }
            }

            Spacer(modifier = Modifier.height(6.dp))

            Surface(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                shape = RoundedCornerShape(28.dp),
                color = setupPanelColor,
                contentColor = setupTextColor,
                tonalElevation = 6.dp,
                border = BorderStroke(
                    1.dp,
                    MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f)
                )
            ) {
                Crossfade(
                    targetState = page,
                    animationSpec = tween(durationMillis = 220),
                    label = "initial-setup-page"
                ) { currentPage ->
                    when (currentPage) {
                        0 -> WelcomePage()
                        else -> FirmwareSetupPage(
                            firmwareInstallState = firmwareInstallState,
                            firmwareLocaleIndex = firmwareLocaleIndex,
                            onFirmwareLocaleSelected = { firmwareLocaleIndex = it },
                            onInstallFirmware = onInstallFirmware,
                            frameGenerationReady = frameGenerationReady,
                            frameGenerationImporting = frameGenerationImporting,
                            frameGenerationImportResult = frameGenerationImportResult,
                            onImportLosslessDll = {
                                losslessPicker.launch(arrayOf("*/*"))
                            }
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            SetupPageIndicator(
                currentPage = page,
                totalPages = 2,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )

            Spacer(modifier = Modifier.height(8.dp))

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Spacer(modifier = Modifier.width(1.dp))
                if (page == 0) {
                    Button(onClick = { page = 1 }) {
                        Text(stringResource(R.string.initial_setup_next))
                        Spacer(modifier = Modifier.width(8.dp))
                        Icon(Icons.AutoMirrored.Filled.ArrowForward, contentDescription = null)
                    }
                } else {
                    Button(onClick = onFinish) {
                        Text(stringResource(R.string.initial_setup_open_library))
                    }
                }
            }
        }
    }
}

@Composable
private fun BackgroundOrb(
    modifier: Modifier = Modifier,
    size: androidx.compose.ui.unit.Dp,
    color: Color
) {
    Box(
        modifier = modifier
            .size(size)
            .clip(CircleShape)
            .background(color)
    )
}

@Composable
private fun WelcomePage() {
    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 10.dp)
                .heightIn(min = maxHeight - 20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Image(
                painter = painterResource(id = R.drawable.vitastation_icon),
                contentDescription = stringResource(R.string.apps_list_app_title),
                modifier = Modifier.size(82.dp).clip(RoundedCornerShape(22.dp))
            )
            Spacer(modifier = Modifier.height(5.dp))
            Text("VitaStation", style = MaterialTheme.typography.headlineLarge,
                fontWeight = FontWeight.Bold, color = setupTextColor, textAlign = TextAlign.Center)
            Text(stringResource(R.string.initial_setup_brand_tagline),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.SemiBold, textAlign = TextAlign.Center)
            Spacer(modifier = Modifier.height(10.dp))
            Text(stringResource(R.string.initial_setup_welcome_title),
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Bold, color = setupTextColor, textAlign = TextAlign.Center)
            Spacer(modifier = Modifier.height(8.dp))
            Text(stringResource(R.string.initial_setup_welcome_body),
                style = MaterialTheme.typography.bodyLarge,
                color = setupTextColor.copy(alpha = 0.88f), textAlign = TextAlign.Center)
            Spacer(modifier = Modifier.height(8.dp))
            Surface(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(20.dp),
                color = setupCardColor,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.38f))
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(stringResource(R.string.initial_setup_firmware_cta),
                        style = MaterialTheme.typography.bodyMedium, color = setupTextColor,
                        modifier = Modifier.weight(1f))
                    Spacer(modifier = Modifier.width(10.dp))
                    Icon(Icons.AutoMirrored.Filled.ArrowForward, contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary)
                }
            }
            Spacer(modifier = Modifier.height(7.dp))
            HtmlText(
                html = stringResource(R.string.initial_setup_info_html),
                modifier = Modifier.fillMaxWidth(),
                textStyle = MaterialTheme.typography.bodyMedium,
                textColor = setupTextColor.copy(alpha = 0.82f),
                gravity = Gravity.CENTER
            )
            Spacer(modifier = Modifier.height(6.dp))
            Surface(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(18.dp),
                color = MaterialTheme.colorScheme.error.copy(alpha = 0.13f),
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.error.copy(alpha = 0.34f))
            ) {
                Text(stringResource(R.string.initial_setup_piracy_notice),
                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 7.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                    fontWeight = FontWeight.Medium,
                    textAlign = TextAlign.Center)
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun FirmwareSetupPage(
    firmwareInstallState: FirmwareInstallState,
    firmwareLocaleIndex: Int,
    onFirmwareLocaleSelected: (Int) -> Unit,
    onInstallFirmware: () -> Unit,
    frameGenerationReady: Boolean,
    frameGenerationImporting: Boolean,
    frameGenerationImportResult: Int?,
    onImportLosslessDll: () -> Unit
) {
    val uriHandler = LocalUriHandler.current

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(5.dp)
    ) {
        Text(
            text = stringResource(R.string.initial_setup_firmware_title),
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.SemiBold,
            color = setupTextColor
        )
        Text(
            text = stringResource(R.string.initial_setup_firmware_body),
            style = MaterialTheme.typography.bodyMedium,
            color = setupTextColor
        )

        FirmwareCard(
            title = stringResource(R.string.initial_setup_preinstall_title),
            installed = firmwareInstallState.components.preinstalled,
            missingStatusText = stringResource(R.string.initial_setup_status_optional)
        ) {
            if (!firmwareInstallState.components.preinstalled) {
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    FilledTonalButton(onClick = { uriHandler.openUri(FirmwareLinks.PREINSTALL_URL) }) {
                        Text(stringResource(R.string.initial_setup_download))
                    }
                    Button(onClick = onInstallFirmware) {
                        Text(stringResource(R.string.initial_setup_install_pup))
                    }
                }
            }
        }

        FirmwareCard(
            title = stringResource(R.string.initial_setup_main_title),
            installed = firmwareInstallState.components.main
        ) {
            if (!firmwareInstallState.components.main) {
                FirmwareLanguagePicker(
                    selectedIndex = firmwareLocaleIndex,
                    onSelected = onFirmwareLocaleSelected
                )
                Spacer(modifier = Modifier.height(5.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    FilledTonalButton(onClick = {
                        uriHandler.openUri(FirmwareLinks.firmwareDownloadUrl(firmwareLocaleIndex))
                    }) {
                        Text(stringResource(R.string.initial_setup_download))
                    }
                    Button(onClick = onInstallFirmware) {
                        Text(stringResource(R.string.initial_setup_install_pup))
                    }
                }
            }
        }

        FirmwareCard(
            title = stringResource(R.string.initial_setup_font_title),
            installed = firmwareInstallState.components.fontPackage
        ) {
            if (!firmwareInstallState.components.fontPackage) {
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    FilledTonalButton(onClick = { uriHandler.openUri(FirmwareLinks.FONT_PACKAGE_URL) }) {
                        Text(stringResource(R.string.initial_setup_download))
                    }
                    Button(onClick = onInstallFirmware) {
                        Text(stringResource(R.string.initial_setup_install_pup))
                    }
                }
            }
        }

        FirmwareCard(
            title = stringResource(R.string.settings_gpu_framegen),
            installed = frameGenerationReady,
            missingStatusText = stringResource(R.string.initial_setup_status_optional),
            installedStatusText = stringResource(R.string.initial_setup_status_configured)
        ) {
            Text(
                text = stringResource(R.string.initial_setup_framegen_body),
                style = MaterialTheme.typography.bodySmall,
                color = setupTextColor.copy(alpha = 0.82f)
            )

            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(5.dp)
            ) {
                FilledTonalButton(
                    onClick = onImportLosslessDll,
                    enabled = !frameGenerationImporting
                ) {
                    Text(
                        if (frameGenerationReady) {
                            stringResource(R.string.settings_gpu_framegen_reimport_dll)
                        } else {
                            stringResource(R.string.settings_gpu_framegen_import_dll)
                        }
                    )
                }

                if (frameGenerationImporting) {
                    CircularProgressIndicator(modifier = Modifier.size(22.dp))
                }
            }

            val frameGenerationStatus = when {
                frameGenerationImporting ->
                    stringResource(R.string.settings_gpu_framegen_importing)
                frameGenerationImportResult == FrameGenerationManager.RESULT_OK ->
                    stringResource(R.string.settings_gpu_framegen_import_success)
                frameGenerationImportResult == FrameGenerationManager.RESULT_DLL_UNREADABLE ->
                    stringResource(R.string.settings_gpu_framegen_import_bad_dll)
                frameGenerationImportResult == FrameGenerationManager.RESULT_MISSING_SHADERS ->
                    stringResource(R.string.settings_gpu_framegen_import_missing)
                frameGenerationImportResult == FrameGenerationManager.RESULT_TRANSLATION_FAILED ->
                    stringResource(R.string.settings_gpu_framegen_import_translation_error)
                frameGenerationImportResult != null ->
                    stringResource(R.string.settings_gpu_framegen_import_error)
                frameGenerationReady ->
                    stringResource(R.string.settings_gpu_framegen_cache_ready)
                else ->
                    stringResource(R.string.settings_gpu_framegen_import_hint)
            }

            Text(
                text = frameGenerationStatus,
                style = MaterialTheme.typography.bodySmall,
                color = setupTextColor.copy(alpha = 0.70f)
            )
        }
    }
}

@Composable
private fun FirmwareLanguagePicker(
    selectedIndex: Int,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(
            text = stringResource(R.string.initial_setup_select_firmware_language),
            style = MaterialTheme.typography.labelMedium,
            color = setupTextColor
        )
        Box {
            OutlinedButton(onClick = { expanded = true }) {
                Text(stringResource(FirmwareLinks.locales[FirmwareLinks.coerceLocaleIndex(selectedIndex)].nameResId))
            }
            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                FirmwareLinks.locales.forEachIndexed { index, locale ->
                    DropdownMenuItem(
                        text = { Text(stringResource(locale.nameResId)) },
                        onClick = {
                            onSelected(index)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun FirmwareCard(
    title: String,
    installed: Boolean,
    missingStatusText: String? = null,
    installedStatusText: String? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    val resolvedMissingStatusText = missingStatusText ?: stringResource(R.string.initial_setup_status_missing)

    Surface(
        shape = RoundedCornerShape(22.dp),
        color = setupCardColor,
        contentColor = setupTextColor,
        border = BorderStroke(
            1.dp,
            MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f)
        )
    ) {
        Column(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                    color = setupTextColor,
                    modifier = Modifier.weight(1f)
                )
                Spacer(modifier = Modifier.width(8.dp))
                StatusBadge(
                    installed = installed,
                    missingStatusText = resolvedMissingStatusText,
                    installedStatusText = installedStatusText
                        ?: stringResource(R.string.initial_setup_status_installed)
                )
            }
            content()
        }
    }
}

@Composable
private fun StatusBadge(
    installed: Boolean,
    missingStatusText: String,
    installedStatusText: String
) {
    val color = if (installed) Color(0xFF1B8A5A) else MaterialTheme.colorScheme.tertiary

    Surface(
        shape = CircleShape,
        color = color.copy(alpha = 0.13f)
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 2.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(6.dp)
                    .clip(CircleShape)
                    .background(color)
            )
            Text(
                text = if (installed) installedStatusText else missingStatusText,
                style = MaterialTheme.typography.labelMedium,
                color = color,
                fontWeight = FontWeight.SemiBold
            )
        }
    }
}

@Composable
private fun SetupPageIndicator(
    currentPage: Int,
    totalPages: Int,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier,
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically
    ) {
        repeat(totalPages) { page ->
            val width by animateDpAsState(
                targetValue = if (page == currentPage) 22.dp else 8.dp,
                animationSpec = tween(durationMillis = 220),
                label = "initial-setup-indicator"
            )
            Box(
                modifier = Modifier
                    .padding(horizontal = 4.dp)
                    .height(8.dp)
                    .width(width)
                    .clip(CircleShape)
                    .background(
                        if (page == currentPage) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.25f)
                    )
            )
        }
    }
}
