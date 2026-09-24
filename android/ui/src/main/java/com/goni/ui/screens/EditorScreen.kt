package com.goni.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.goni.ui.components.ButtonKind
import com.goni.ui.components.IconAction
import com.goni.ui.components.KindBadge
import com.goni.ui.components.OniIcon
import com.goni.ui.model.EditorTab
import com.goni.ui.model.Sheet
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniIcons
import com.goni.ui.theme.OniShape

/**
 * Editor: o viewport ocupa a tela toda e o resto flutua por cima. Em Play
 * o cromo de edição some e só ficam os controles do jogo.
 */
@Composable
fun EditorScreen(state: UiState, actions: UiActions, viewport: @Composable () -> Unit) {
    val snap = state.snapshot
    Box(Modifier.fillMaxSize().background(Oni.colors.bg)) {
        viewport()
        if (snap.playing) {
            PlayOverlay(state, actions)
        } else {
            EditChrome(state, actions)
        }
    }
}

@Composable
private fun EditChrome(state: UiState, actions: UiActions) {
    val snap = state.snapshot
    Box(Modifier.fillMaxSize()) {
        // Barra superior.
        Row(
            Modifier
                .align(Alignment.TopCenter)
                .fillMaxWidth()
                .background(Brush.verticalGradient(listOf(Oni.colors.bg.copy(alpha = 0.92f), Color.Transparent)))
                .statusBarsPadding()
                .padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconAction(OniIcons.Home, onClick = actions::goHome)
            Column(
                Modifier
                    .weight(1f)
                    .clip(OniShape.md)
                    .clickable(onClick = actions::openScenes)
                    .padding(horizontal = 8.dp, vertical = 4.dp),
            ) {
                Text(
                    snap.projectName.ifEmpty { "Sem projeto" },
                    style = Oni.type.heading,
                    color = Oni.colors.text,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OniIcon(OniIcons.Map, tint = Oni.colors.textMuted, size = 13.dp)
                    Spacer(Modifier.width(5.dp))
                    Text(snap.sceneName, style = Oni.type.caption, color = Oni.colors.textMuted, maxLines = 1)
                    if (snap.dirty) {
                        Spacer(Modifier.width(6.dp))
                        Box(Modifier.size(6.dp).clip(CircleShape).background(Oni.colors.warning))
                    }
                    OniIcon(OniIcons.ChevronDown, tint = Oni.colors.textFaint, size = 14.dp, modifier = Modifier.padding(start = 2.dp))
                }
            }
            IconAction(OniIcons.Undo, onClick = actions::undo, enabled = snap.canUndo)
            IconAction(OniIcons.Redo, onClick = actions::redo, enabled = snap.canRedo)
            IconAction(OniIcons.Settings, onClick = actions::openSettings)
            Spacer(Modifier.width(4.dp))
            PlayButton(onClick = actions::play)
        }

        // Trilho de ferramentas.
        ToolRail(state, actions, Modifier.align(Alignment.CenterStart).padding(start = 10.dp))

        // Painel + abas.
        BottomDock(state, actions, Modifier.align(Alignment.BottomCenter))
    }
}

@Composable
private fun PlayButton(onClick: () -> Unit) {
    val c = Oni.colors
    Row(
        Modifier
            .height(44.dp)
            .clip(OniShape.md)
            .background(c.accent)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OniIcon(OniIcons.Play, tint = c.onAccent, size = 16.dp)
        Spacer(Modifier.width(6.dp))
        Text("Jogar", style = Oni.type.bodyStrong, color = c.onAccent)
    }
}

@Composable
private fun ToolRail(state: UiState, actions: UiActions, modifier: Modifier) {
    val c = Oni.colors
    val snap = state.snapshot
    val hasSelection = snap.selection != 0L
    Column(
        modifier
            .clip(OniShape.lg)
            .background(c.s1.copy(alpha = 0.94f))
            .border(1.dp, c.line, OniShape.lg)
            .padding(4.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        val tools = listOf(OniIcons.Pointer, OniIcons.Move, OniIcons.Rotate, OniIcons.Scale)
        tools.forEachIndexed { i, icon ->
            IconAction(icon, onClick = { actions.setTool(i) }, selected = snap.tool == i, enabled = i == 0 || hasSelection || snap.tool == i)
        }
        Box(Modifier.padding(vertical = 4.dp, horizontal = 8.dp).width(28.dp).height(1.dp).background(c.line))
        IconAction(OniIcons.Magnet, onClick = actions::toggleSnap, selected = snap.snapTranslate)
        IconAction(OniIcons.Fit, onClick = actions::fitView)
    }
}

@Composable
private fun BottomDock(state: UiState, actions: UiActions, modifier: Modifier) {
    val c = Oni.colors
    val tab = state.tab
    BoxWithConstraints(modifier.fillMaxWidth()) {
        val panelHeight = if (state.panelTall) maxHeight * 0.78f else maxHeight * 0.46f
        Column(Modifier.fillMaxWidth().navigationBarsPadding().imePadding()) {
            // Atalho da seleção quando o painel está fechado.
            val selected = state.snapshot.selectedNode
            AnimatedVisibility(
                tab == null && selected != null,
                enter = fadeIn() + slideInVertically { it / 2 },
                exit = fadeOut() + slideOutVertically { it / 2 },
            ) {
                if (selected != null) {
                    Row(
                        Modifier
                            .padding(horizontal = 12.dp, vertical = 6.dp)
                            .clip(OniShape.pill)
                            .background(c.s2.copy(alpha = 0.96f))
                            .border(1.dp, c.line, OniShape.pill)
                            .clickable { actions.openTab(EditorTab.Properties) }
                            .padding(start = 6.dp, end = 14.dp, top = 6.dp, bottom = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        KindBadge(selected.kind, size = 28.dp)
                        Spacer(Modifier.width(10.dp))
                        Text(selected.name, style = Oni.type.bodyStrong, color = c.text, maxLines = 1, modifier = Modifier.widthIn(max = 180.dp))
                        Spacer(Modifier.width(10.dp))
                        Text("Editar", style = Oni.type.label, color = c.accent)
                    }
                }
            }
            AnimatedVisibility(
                tab != null,
                enter = slideInVertically(tween(200)) { it } + fadeIn(tween(120)),
                exit = slideOutVertically(tween(160)) { it } + fadeOut(tween(100)),
            ) {
                Column(
                    Modifier
                        .padding(horizontal = 8.dp)
                        .fillMaxWidth()
                        .height(panelHeight)
                        .clip(OniShape.lg)
                        .background(c.s1)
                        .border(1.dp, c.line, OniShape.lg)
                        .animateContentSize(),
                ) {
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .clickable(onClick = actions::togglePanelHeight)
                            .padding(top = 8.dp, bottom = 2.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Box(Modifier.width(36.dp).height(4.dp).clip(OniShape.pill).background(c.lineStrong))
                    }
                    when (tab) {
                        EditorTab.Scene -> ScenePanel(state, actions)
                        EditorTab.Properties -> PropertiesPanel(state, actions)
                        EditorTab.Library -> LibraryPanel(state, actions)
                        null -> {}
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            TabBar(state, actions)
        }
    }
}

@Composable
private fun TabBar(state: UiState, actions: UiActions) {
    val c = Oni.colors
    Row(
        Modifier
            .padding(start = 8.dp, end = 8.dp, bottom = 8.dp)
            .fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier
                .weight(1f)
                .clip(OniShape.lg)
                .background(c.s1.copy(alpha = 0.97f))
                .border(1.dp, c.line, OniShape.lg)
                .padding(4.dp),
        ) {
            TabItem(OniIcons.Tree, EditorTab.Scene.label, state.tab == EditorTab.Scene) { actions.openTab(toggle(state, EditorTab.Scene)) }
            TabItem(OniIcons.Sliders, EditorTab.Properties.label, state.tab == EditorTab.Properties) { actions.openTab(toggle(state, EditorTab.Properties)) }
            TabItem(OniIcons.Grid, EditorTab.Library.label, state.tab == EditorTab.Library) { actions.openTab(toggle(state, EditorTab.Library)) }
        }
        Spacer(Modifier.width(8.dp))
        Box(
            Modifier
                .size(58.dp)
                .clip(OniShape.lg)
                .background(c.accent)
                .clickable { state.sheet = Sheet.AddEntity },
            contentAlignment = Alignment.Center,
        ) {
            OniIcon(OniIcons.Plus, tint = c.onAccent, size = 26.dp)
        }
    }
}

private fun toggle(state: UiState, tab: EditorTab): EditorTab? = if (state.tab == tab) null else tab

@Composable
private fun androidx.compose.foundation.layout.RowScope.TabItem(icon: ImageVector, label: String, selected: Boolean, onClick: () -> Unit) {
    val c = Oni.colors
    Column(
        Modifier
            .weight(1f)
            .height(50.dp)
            .clip(OniShape.md)
            .background(if (selected) c.accentSoft else Color.Transparent)
            .clickable(onClick = onClick),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        OniIcon(icon, tint = if (selected) c.accent else c.textMuted, size = 19.dp)
        Spacer(Modifier.height(3.dp))
        Text(label, style = Oni.type.caption, color = if (selected) c.text else c.textMuted, maxLines = 1)
    }
}

// --- modo jogo --------------------------------------------------------------------------

@Composable
private fun PlayOverlay(state: UiState, actions: UiActions) {
    val c = Oni.colors
    val snap = state.snapshot
    Box(Modifier.fillMaxSize()) {
        Row(
            Modifier
                .align(Alignment.TopStart)
                .statusBarsPadding()
                .padding(10.dp)
                .clip(OniShape.pill)
                .background(c.s1.copy(alpha = 0.88f))
                .border(1.dp, c.line, OniShape.pill)
                .padding(4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconAction(OniIcons.Stop, onClick = actions::stop, kind = ButtonKind.Danger, size = 40.dp, iconSize = 16.dp)
            Spacer(Modifier.width(4.dp))
            IconAction(
                if (snap.paused) OniIcons.Play else OniIcons.Pause,
                onClick = { actions.setPaused(!snap.paused) },
                kind = ButtonKind.Tonal,
                size = 40.dp,
                iconSize = 16.dp,
            )
            Spacer(Modifier.width(10.dp))
            Text("${state.hud.fps} fps", style = Oni.type.mono, color = c.textMuted)
            Spacer(Modifier.width(12.dp))
        }
        if (state.hud.faults > 0 || state.hud.error.isNotEmpty()) {
            Row(
                Modifier
                    .align(Alignment.TopCenter)
                    .statusBarsPadding()
                    .padding(top = 66.dp, start = 16.dp, end = 16.dp)
                    .clip(OniShape.md)
                    .background(c.dangerSoft.copy(alpha = 0.95f))
                    .border(1.dp, c.danger.copy(alpha = 0.5f), OniShape.md)
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                OniIcon(OniIcons.Warning, tint = c.danger, size = 16.dp)
                Spacer(Modifier.width(8.dp))
                Text(
                    state.hud.error.ifEmpty { "${state.hud.faults} erro(s) nos scripts" },
                    style = Oni.type.caption,
                    color = c.text,
                    maxLines = 2,
                )
            }
        }
        when (snap.controls) {
            "platformer" -> TouchControls(Modifier.align(Alignment.BottomCenter))
            "tap" -> TapHint(Modifier.align(Alignment.BottomCenter))
            else -> {}
        }
        if (snap.paused) {
            Box(
                Modifier.align(Alignment.Center).clip(OniShape.lg).background(c.s1.copy(alpha = 0.9f)).padding(horizontal = 22.dp, vertical = 14.dp),
            ) {
                Text("Pausado", style = Oni.type.title, color = c.text)
            }
        }
    }
}

/**
 * Desenho dos controles padrão do jogo. Só visual: os toques atravessam
 * para o motor, que tem as mesmas zonas (esquerda/direita/pulo).
 */
@Composable
private fun TouchControls(modifier: Modifier) {
    BoxWithConstraints(modifier.fillMaxWidth().fillMaxHeight(0.4f)) {
        val w = maxWidth
        val h = maxHeight
        ControlGhost(OniIcons.ChevronLeft, Modifier.align(Alignment.BottomStart).offset(x = w * 0.1f - 34.dp, y = -(h * 0.5f) + 34.dp))
        ControlGhost(OniIcons.ChevronRight, Modifier.align(Alignment.BottomStart).offset(x = w * 0.3f - 34.dp, y = -(h * 0.5f) + 34.dp))
        ControlGhost(OniIcons.ArrowUp, Modifier.align(Alignment.BottomEnd).offset(x = -(w * 0.125f) + 34.dp, y = -(h * 0.5f) + 34.dp))
    }
}

/** Jogos de toque único: uma dica que some sozinha, sem cobrir o jogo. */
@Composable
private fun TapHint(modifier: Modifier) {
    var visible by remember { mutableStateOf(true) }
    LaunchedEffect(Unit) {
        kotlinx.coroutines.delay(2500)
        visible = false
    }
    androidx.compose.animation.AnimatedVisibility(
        visible = visible,
        modifier = modifier.navigationBarsPadding().padding(bottom = 48.dp),
        exit = androidx.compose.animation.fadeOut(),
    ) {
        Row(
            Modifier
                .clip(OniShape.pill)
                .background(Color.Black.copy(alpha = 0.45f))
                .border(1.dp, Color.White.copy(alpha = 0.2f), OniShape.pill)
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OniIcon(OniIcons.Tap, tint = Color.White, size = 18.dp)
            Spacer(Modifier.width(8.dp))
            Text("Toque na tela para jogar", style = Oni.type.label, color = Color.White)
        }
    }
}

@Composable
private fun ControlGhost(icon: ImageVector, modifier: Modifier) {
    Box(
        modifier
            .size(68.dp)
            .clip(CircleShape)
            .background(Color.White.copy(alpha = 0.08f))
            .border(1.5.dp, Color.White.copy(alpha = 0.22f), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        OniIcon(icon, tint = Color.White.copy(alpha = 0.7f), size = 28.dp)
    }
}
