package com.goni.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import com.goni.ui.components.ToastHost
import com.goni.ui.model.Screen
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.ui.screens.EditorScreen
import com.goni.ui.screens.HomeScreen
import com.goni.ui.screens.ScriptEditorScreen
import com.goni.ui.screens.SheetHost
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniTheme

/**
 * Raiz da interface. `viewport` é a superfície do motor (no app, um
 * SurfaceView); nas prévias é um desenho estático.
 */
@Composable
fun GoniApp(state: UiState, actions: UiActions, viewport: @Composable () -> Unit) {
    OniTheme {
        Box(Modifier.fillMaxSize().background(Oni.colors.bg)) {
            when (val screen = state.screen) {
                Screen.Home -> HomeScreen(state, actions)
                Screen.Editor -> EditorScreen(state, actions, viewport)
                is Screen.Script -> ScriptEditorScreen(screen.name, state, actions)
            }
            SheetHost(state, actions)
            ToastHost(state.toast)
        }
    }
}

/** Viewport de mentira para prévias: grade + sprites do modelo Plataforma. */
@Composable
fun FakeViewport(selected: Boolean = true) {
    Canvas(Modifier.fillMaxSize().background(Color(0xFF14171E))) {
        val cell = 48f
        val minor = Color(0xFF1D212B)
        val major = Color(0xFF262B37)
        var x = (size.width / 2) % cell
        var i = 0
        while (x < size.width) {
            drawLine(if (i % 5 == 0) major else minor, Offset(x, 0f), Offset(x, size.height))
            x += cell; i++
        }
        var y = (size.height / 2) % cell
        i = 0
        while (y < size.height) {
            drawLine(if (i % 5 == 0) major else minor, Offset(0f, y), Offset(size.width, y))
            y += cell; i++
        }
        val cx = size.width / 2
        val cy = size.height / 2
        drawLine(Color(0x55FF7470), Offset(0f, cy), Offset(size.width, cy), strokeWidth = 2f)
        drawLine(Color(0x554FD197), Offset(cx, 0f), Offset(cx, size.height), strokeWidth = 2f)
        val ground = Color(0xFF5C6B80)
        drawRect(ground, Offset(cx - 7 * cell, cy + 1.5f * cell), Size(14 * cell, cell))
        drawRect(ground, Offset(cx + 2f * cell, cy - 0.4f * cell), Size(3 * cell, 0.4f * cell))
        val player = Offset(cx - 2.5f * cell, cy - 0.5f * cell)
        drawRect(Color(0xFF8AB0FF), player, Size(cell, cell))
        if (selected) {
            drawRect(Color(0xFF7C9CFF), player - Offset(3f, 3f), Size(cell + 6f, cell + 6f), style = Stroke(3f))
            val c = player + Offset(cell / 2, cell / 2)
            drawLine(Color(0xFFFF7470), c, c + Offset(cell * 1.6f, 0f), strokeWidth = 5f)
            drawLine(Color(0xFF4FD197), c, c - Offset(0f, cell * 1.6f), strokeWidth = 5f)
            drawRoundRect(Color(0xFFF4BE5E), c - Offset(9f, 9f), Size(18f, 18f), CornerRadius(3f))
        }
        drawRoundRect(Color(0x66B892FF), Offset(cx - 5 * cell, cy - 4 * cell), Size(10 * cell, 7.5f * cell), CornerRadius(8f), style = Stroke(2f))
    }
}

/** Ações sem efeito para prévias e testes de screenshot. */
object NoActions : UiActions
