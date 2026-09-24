package com.goni.app

import android.view.Surface

/**
 * Ponte com o motor (libgoni.so). Espelha android/app/src/main/cpp/EditorJni.cpp.
 *
 * O estado do editor chega por [nativeSnapshot] e toda operação sai por
 * [nativeCall], ambos em JSON (ver docs/editor-protocol.md). O resto é o
 * ciclo de vida da surface e os gestos do viewport, que precisam ser
 * baratos a cada evento de toque.
 */
object NativeBridge {
    init {
        System.loadLibrary("goni")
    }

    /** Abre o log de diagnóstico. true = a execução anterior terminou em crash. */
    external fun nativeInit(diagnosticsDir: String): Boolean

    external fun nativeCreate(workspace: String): Long
    external fun nativeDestroy(handle: Long)

    /** null quando nada mudou desde [sinceKey]. */
    external fun nativeSnapshot(handle: Long, sinceKey: Long): String?
    external fun nativeCall(handle: Long, requestJson: String): String

    external fun nativeSurfaceCreated(handle: Long, surface: Surface)
    external fun nativeSurfaceChanged(handle: Long, width: Int, height: Int)
    external fun nativeSurfaceDestroyed(handle: Long)
    external fun nativeOnPause(handle: Long)
    external fun nativeOnResume(handle: Long)
    external fun nativeRenderFrame(handle: Long, dt: Float): Boolean
    external fun nativeSetUiScale(handle: Long, scale: Float)

    external fun nativeTap(handle: Long, x: Float, y: Float): Long
    external fun nativePan(handle: Long, dx: Float, dy: Float)
    external fun nativeZoom(handle: Long, factor: Float, focusX: Float, focusY: Float)
    external fun nativeGizmoBegin(handle: Long, x: Float, y: Float): Boolean
    external fun nativeGizmoDrag(handle: Long, x: Float, y: Float)
    external fun nativeGizmoEnd(handle: Long)
    external fun nativeDragEntity(handle: Long, entity: Long, dx: Float, dy: Float)
    external fun nativeGameTouch(
        handle: Long, phase: Int, pointer: Int, x: Float, y: Float, pressure: Float,
    )
}
