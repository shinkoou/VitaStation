package org.vita3k.emulator.data

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import java.io.File
import java.security.MessageDigest

object FrameGenerationManager {
    private const val PREFS = "vitastation_frame_generation"
    private const val KEY_DLL_SHA256 = "dll_sha256"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_MULTIPLIER = "multiplier"

    const val RESULT_OK = 0
    const val RESULT_DLL_UNREADABLE = -1
    const val RESULT_MISSING_SHADERS = -2
    const val RESULT_TRANSLATION_FAILED = -3
    const val RESULT_WRITE_FAILED = -4
    const val RESULT_COPY_FAILED = -10

    const val MULTIPLIER_2X = 2

    fun shaderCacheDir(context: Context): File =
        File(context.filesDir, "lsfg/shaders")

    fun isReady(context: Context): Boolean = runCatching {
        NativeLib.areLsfgShadersReady(shaderCacheDir(context).absolutePath)
    }.getOrDefault(false)

    fun backendInfo(): String = runCatching {
        NativeLib.getLsfgBackendInfo()
    }.getOrDefault("LSFG backend unavailable")

    fun importedDllSha256(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY_DLL_SHA256, "")
            .orEmpty()

    fun isEnabled(context: Context): Boolean {
        if (!isReady(context)) return false
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENABLED, false)
    }

    fun multiplier(context: Context): Int =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getInt(KEY_MULTIPLIER, MULTIPLIER_2X)
            .coerceAtLeast(MULTIPLIER_2X)

    fun setEnabled(context: Context, requested: Boolean): Boolean {
        val effective = requested && isReady(context)
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_ENABLED, effective)
            .putInt(KEY_MULTIPLIER, MULTIPLIER_2X)
            .apply()
        syncNative(context)
        return effective
    }

    fun syncNative(context: Context): Boolean {
        val ready = isReady(context)
        val enabled = ready && context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENABLED, false)

        return runCatching {
            NativeLib.configureLsfgFrameGeneration(
                enabled,
                MULTIPLIER_2X,
                shaderCacheDir(context).absolutePath
            )
        }.getOrDefault(false)
    }

    suspend fun importLosslessDll(context: Context, uri: Uri): Int =
        withContext(Dispatchers.IO) {
            val importDir = File(context.cacheDir, "vitastation-lsfg-import")
            importDir.mkdirs()
            val temporaryDll = File(importDir, "Lossless.dll")

            try {
                val digest = MessageDigest.getInstance("SHA-256")
                val input = context.contentResolver.openInputStream(uri)
                    ?: return@withContext RESULT_COPY_FAILED

                input.use { source ->
                    temporaryDll.outputStream().use { target ->
                        val buffer = ByteArray(1024 * 128)
                        while (true) {
                            val count = source.read(buffer)
                            if (count <= 0) break
                            target.write(buffer, 0, count)
                            digest.update(buffer, 0, count)
                        }
                    }
                }

                temporaryDll.inputStream().use { stream ->
                    val mz = ByteArray(2)
                    if (stream.read(mz) != 2 ||
                        mz[0] != 'M'.code.toByte() ||
                        mz[1] != 'Z'.code.toByte()) {
                        return@withContext RESULT_DLL_UNREADABLE
                    }
                }

                val result = NativeLib.prepareLsfgShaders(
                    temporaryDll.absolutePath,
                    shaderCacheDir(context).absolutePath
                )

                if (result == RESULT_OK) {
                    val sha = digest.digest().joinToString("") { "%02x".format(it) }
                    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                        .edit()
                        .putString(KEY_DLL_SHA256, sha)
                        .putInt(KEY_MULTIPLIER, MULTIPLIER_2X)
                        .apply()
                    syncNative(context)
                }

                result
            } catch (_: Throwable) {
                RESULT_COPY_FAILED
            } finally {
                temporaryDll.delete()
            }
        }
}
