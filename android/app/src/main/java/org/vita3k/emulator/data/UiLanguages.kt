package org.vita3k.emulator.data

import android.content.Context
import android.content.res.Configuration
import androidx.annotation.StringRes
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import java.util.Locale
import org.vita3k.emulator.R

data class UiLanguageOption(val tag: String, @StringRes val labelRes: Int)

object UiLanguages {
    private const val PREFS_NAME = "vitastation_frontend"
    private const val PREF_UI_LANGUAGE = "ui_language"

    val options = listOf(
        UiLanguageOption("", R.string.settings_language_system_default),
        UiLanguageOption("en", R.string.settings_language_english),
        UiLanguageOption("pt-BR", R.string.settings_language_portuguese_br)
    )

    @JvmStatic
    fun currentTag(context: Context): String =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_UI_LANGUAGE, "") ?: ""

    fun applyStored(context: Context) = apply(currentTag(context))

    fun applyAndPersist(context: Context, tag: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putString(PREF_UI_LANGUAGE, tag).apply()
        apply(tag)
    }

    fun apply(tag: String) {
        val locales = if (tag.isBlank()) LocaleListCompat.getEmptyLocaleList()
        else LocaleListCompat.forLanguageTags(tag)
        AppCompatDelegate.setApplicationLocales(locales)
    }

    @JvmStatic
    fun wrapContext(context: Context): Context {
        val tag = currentTag(context)
        if (tag.isBlank()) return context
        val config = Configuration(context.resources.configuration)
        config.setLocale(Locale.forLanguageTag(tag))
        return context.createConfigurationContext(config)
    }
}
