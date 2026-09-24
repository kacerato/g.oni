package com.goni.app

import android.net.Uri
import android.content.pm.ActivityInfo
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.Choreographer
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.goni.ui.GoniApp
import com.goni.ui.components.ToastKind
import com.goni.ui.model.UiState
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniTheme
import java.io.File

class MainActivity : ComponentActivity(), Choreographer.FrameCallback {

    private var handle = 0L
    private var controller: EditorController? = null
    private var viewport: ViewportView? = null
    private var lastFrame = 0L
    private var running = false

    private var onCreated: ((Uri) -> Unit)? = null
    private var onOpened: ((Uri, String) -> Unit)? = null

    private val createDoc = registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        uri?.let { onCreated?.invoke(it) }
        onCreated = null
    }

    private val createApk = registerForActivityResult(
        ActivityResultContracts.CreateDocument("application/vnd.android.package-archive"),
    ) { uri ->
        uri?.let { onCreated?.invoke(it) }
        onCreated = null
    }

    private val openDoc = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { onOpened?.invoke(it, displayName(it)) }
        onOpened = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        CrashGuard.install(this)

        val previousCrash = try {
            NativeBridge.nativeInit(filesDir.absolutePath)
        } catch (t: Throwable) {
            showFatal("Não foi possível carregar o motor do G.ONI.\n\n${t.javaClass.simpleName}: ${t.message}")
            return
        }
        val workspace = File(filesDir, "projects").apply { mkdirs() }
        handle = NativeBridge.nativeCreate(workspace.absolutePath)
        if (handle == 0L) {
            showFatal("O motor não iniciou (sem suporte gráfico?). Exporte o diagnóstico e envie para análise.")
            return
        }
        NativeBridge.nativeSetUiScale(handle, resources.displayMetrics.density)

        val state = UiState()
        val ctl = EditorController(this, Engine(handle), state, workspace)
        controller = ctl
        // APK exportado de um jogo: abre direto nele, sem o editor.
        if (!ctl.startStandalone()) {
            ctl.refreshProjects()
        }
        if (previousCrash || CrashGuard.hadCrash(this)) {
            ctl.toast("O app fechou inesperadamente da última vez. Ajustes › Exportar diagnóstico.", ToastKind.Error)
        }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (!ctl.back()) {
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                }
            }
        })

        setContent {
            GoniApp(state, ctl) {
                AndroidView(
                    factory = { ctx -> ViewportView(ctx, handle, ctl).also { viewport = it } },
                    modifier = Modifier.fillMaxSize(),
                    onRelease = { if (viewport === it) viewport = null },
                )
            }
        }
    }

    /**
     * No Play a tela segue o ajuste do jogo; no editor fica livre. O
     * manifesto declara configChanges, então girar não recria a Activity.
     */
    fun applyPlayOrientation(playing: Boolean, orientation: String) {
        val wanted = when {
            !playing -> ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            orientation == "portrait" -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            orientation == "landscape" -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            else -> ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
        if (requestedOrientation != wanted) requestedOrientation = wanted
    }

    private fun showFatal(message: String) {
        setContent {
            OniTheme {
                Column(
                    Modifier.fillMaxSize().background(Oni.colors.bg).padding(32.dp),
                    verticalArrangement = Arrangement.Center,
                ) {
                    Text("G.ONI", style = Oni.type.title, color = Oni.colors.accent)
                    Text(message, style = Oni.type.body, color = Oni.colors.text, modifier = Modifier.padding(top = 12.dp))
                }
            }
        }
    }

    fun createDocument(name: String, mime: String, then: (Uri) -> Unit) {
        onCreated = then
        if (mime == "application/vnd.android.package-archive") createApk.launch(name) else createDoc.launch(name)
    }

    fun openDocument(mimes: Array<String>, then: (Uri, String) -> Unit) {
        onOpened = then
        openDoc.launch(mimes)
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) return c.getString(0) ?: ""
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: ""
    }

    // --- loop de frames -----------------------------------------------------------------

    override fun doFrame(frameTimeNanos: Long) {
        if (!running) return
        val dt = if (lastFrame == 0L) 0f else ((frameTimeNanos - lastFrame) / 1e9f).coerceIn(0f, 0.1f)
        lastFrame = frameTimeNanos
        controller?.frame(dt)
        Choreographer.getInstance().postFrameCallback(this)
    }

    override fun onResume() {
        super.onResume()
        if (handle == 0L) return
        NativeBridge.nativeOnResume(handle)
        running = true
        lastFrame = 0L
        Choreographer.getInstance().postFrameCallback(this)
    }

    override fun onPause() {
        if (handle != 0L) {
            running = false
            Choreographer.getInstance().removeFrameCallback(this)
            viewport?.cancelGameTouches()
            controller?.autosave()
            NativeBridge.nativeOnPause(handle)
        }
        super.onPause()
    }

    override fun onDestroy() {
        if (handle != 0L) {
            NativeBridge.nativeDestroy(handle)
            handle = 0L
        }
        super.onDestroy()
    }
}
