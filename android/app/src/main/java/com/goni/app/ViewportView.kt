package com.goni.app

import android.annotation.SuppressLint
import android.content.Context
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Superfície do motor + gestos do editor.
 *
 * Em edição: toque seleciona; arrastar uma alça do gizmo transforma;
 * arrastar começando em cima do objeto selecionado (ferramenta Mover) o
 * move; qualquer outro arrasto move a câmera; pinça dá zoom.
 * Em jogo: os eventos vão crus para o input do jogo (multitoque).
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class ViewportView(
    context: Context,
    private val handle: Long,
    private val host: Host,
) : SurfaceView(context), SurfaceHolder.Callback {

    interface Host {
        val playing: Boolean
        val tool: Int
        val selection: Long
        fun onSurfaceReady(ready: Boolean)
        fun onTapped(hit: Long)
        fun onEditGestureEnded()
    }

    private enum class Drag { None, Gizmo, Entity, Pan }

    private var drag = Drag.None
    private var dragPointer = -1

    private val scale = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(d: ScaleGestureDetector): Boolean {
            NativeBridge.nativeZoom(handle, d.scaleFactor, d.focusX, d.focusY)
            return true
        }
    })

    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onSingleTapUp(e: MotionEvent): Boolean {
            if (drag == Drag.Gizmo) return true
            host.onTapped(NativeBridge.nativeTap(handle, e.x, e.y))
            return true
        }

        override fun onScroll(e1: MotionEvent?, e2: MotionEvent, dx: Float, dy: Float): Boolean {
            if (scale.isInProgress || e2.pointerCount > 1) {
                NativeBridge.nativePan(handle, dx, dy)
                return true
            }
            when (drag) {
                Drag.Entity -> NativeBridge.nativeDragEntity(handle, host.selection, dx, dy)
                Drag.Gizmo -> {}
                else -> NativeBridge.nativePan(handle, dx, dy)
            }
            return true
        }
    })

    init {
        holder.addCallback(this)
        setOnTouchListener { _, event ->
            if (host.playing) {
                dispatchGame(event)
            } else {
                handleEdit(event)
            }
            true
        }
    }

    private fun handleEdit(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                dragPointer = event.getPointerId(0)
                drag = Drag.Pan
                val sel = host.selection
                if (sel != 0L && host.tool != 0) {
                    if (NativeBridge.nativeGizmoBegin(handle, event.x, event.y)) {
                        drag = Drag.Gizmo
                    } else if (host.tool == 1 && NativeBridge.nativePick(handle, event.x, event.y) == sel) {
                        drag = Drag.Entity
                    }
                }
            }
            MotionEvent.ACTION_MOVE -> if (drag == Drag.Gizmo) {
                val i = event.findPointerIndex(dragPointer)
                if (i >= 0) NativeBridge.nativeGizmoDrag(handle, event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_POINTER_UP -> if (drag == Drag.Gizmo && event.getPointerId(event.actionIndex) == dragPointer) {
                endDrag()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> endDrag()
        }
        // Durante o arrasto do gizmo, um segundo dedo não vira pinça.
        if (drag != Drag.Gizmo) {
            scale.onTouchEvent(event)
            gestures.onTouchEvent(event)
        }
    }

    private fun endDrag() {
        if (drag == Drag.Gizmo) NativeBridge.nativeGizmoEnd(handle)
        if (drag == Drag.Gizmo || drag == Drag.Entity) host.onEditGestureEnded()
        drag = Drag.None
        dragPointer = -1
    }

    private fun dispatchGame(event: MotionEvent) {
        fun send(phase: Int, i: Int) =
            NativeBridge.nativeGameTouch(handle, phase, event.getPointerId(i), event.getX(i), event.getY(i), event.getPressure(i))
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> send(0, event.actionIndex)
            MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) send(1, i)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> send(2, event.actionIndex)
            MotionEvent.ACTION_CANCEL -> for (i in 0 until event.pointerCount) send(3, i)
        }
    }

    /** Solta todos os dedos do jogo (perda de foco, pausa). */
    fun cancelGameTouches() {
        NativeBridge.nativeGameTouch(handle, 3, 0, 0f, 0f, 0f)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativeBridge.nativeSurfaceCreated(handle, holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeBridge.nativeSurfaceChanged(handle, width, height)
        host.onSurfaceReady(width > 0 && height > 0)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        host.onSurfaceReady(false)
        NativeBridge.nativeSurfaceDestroyed(handle)
    }
}
