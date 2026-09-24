package com.goni.app

import android.content.Context
import android.os.Build
import java.io.File
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Registra exceções Kotlin não tratadas antes de o processo morrer. */
object CrashGuard {
    private const val FILE = "goni_kotlin_crash.log"

    fun install(context: Context) {
        val file = File(context.filesDir, FILE)
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching {
                file.writeText("thread=${thread.name}\n${error.stackTraceToString()}")
            }
            previous?.uncaughtException(thread, error)
        }
    }

    /** true uma única vez depois de um crash Kotlin (o arquivo é renomeado). */
    fun hadCrash(context: Context): Boolean {
        val file = File(context.filesDir, FILE)
        if (!file.exists()) return false
        file.renameTo(File(context.filesDir, "$FILE.prev"))
        return true
    }
}

/** Empacota os logs do app (motor + Kotlin) para o usuário enviar. */
object Diagnostics {
    fun writeZip(context: Context, out: OutputStream) {
        ZipOutputStream(out).use { zip ->
            val info = buildString {
                appendLine("app=${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE}) ${BuildConfig.GONI_COMMIT}")
                appendLine("device=${Build.MANUFACTURER} ${Build.MODEL}")
                appendLine("android=${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
                appendLine("abi=${Build.SUPPORTED_ABIS.joinToString()}")
            }
            zip.putNextEntry(ZipEntry("device.txt"))
            zip.write(info.toByteArray())
            zip.closeEntry()
            context.filesDir.listFiles()
                ?.filter { it.isFile && (it.name.endsWith(".log") || it.name.endsWith(".prev")) }
                ?.forEach { f ->
                    zip.putNextEntry(ZipEntry(f.name))
                    f.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
        }
    }
}
