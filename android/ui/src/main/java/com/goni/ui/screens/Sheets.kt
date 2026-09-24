package com.goni.ui.screens

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.goni.ui.components.ButtonKind
import com.goni.ui.components.ColorSwatch
import com.goni.ui.components.IconAction
import com.goni.ui.components.KindBadge
import com.goni.ui.components.ListRow
import com.goni.ui.components.MenuItem
import com.goni.ui.components.MenuList
import com.goni.ui.components.ModalDialog
import com.goni.ui.components.ModalSheet
import com.goni.ui.components.OniButton
import com.goni.ui.components.OniField
import com.goni.ui.components.OniIcon
import com.goni.ui.components.OniSwitch
import com.goni.ui.components.SectionLabel
import com.goni.ui.components.Segmented
import com.goni.ui.model.AnimationModel
import com.goni.ui.model.LibraryKind
import com.goni.ui.model.MaterialModel
import com.goni.ui.model.Sheet
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniIcons
import com.goni.ui.theme.OniShape

/** Desenha a folha/diálogo atual por cima de qualquer tela. */
@Composable
fun BoxScope.SheetHost(state: UiState, actions: UiActions) {
    val sheet = state.sheet
    val dismiss = { actions.dismissSheet() }
    val isDialog = sheet is Sheet.Rename || sheet is Sheet.Confirm || sheet is Sheet.TextInput
    ModalSheet(visible = sheet != null && !isDialog, onDismiss = dismiss, title = sheetTitle(sheet)) {
        when (sheet) {
            is Sheet.NewProject -> NewProjectSheetContent(state.examples, sheet.template) { name, template -> actions.createProject(name, template) }
            is Sheet.ProjectMenu -> ProjectMenuContent(sheet.project, actions, state)
            Sheet.AddEntity -> AddEntityContent(actions)
            is Sheet.EntityMenu -> EntityMenuContent(sheet, state, actions)
            is Sheet.Reparent -> ReparentContent(sheet, state, actions)
            Sheet.AddComponent -> AddComponentContent(state, actions)
            is Sheet.Picker -> PickerContent(sheet, state, actions)
            is Sheet.Color -> ColorContent(sheet, actions)
            is Sheet.AssetMenu -> AssetMenuContent(sheet, state, actions)
            Sheet.Scenes -> ScenesContent(state, actions)
            Sheet.Settings -> SettingsContent(state, actions)
            is Sheet.Animation -> AnimationContent(sheet.model, state, actions)
            is Sheet.Material -> MaterialContent(sheet.model, actions)
            else -> {}
        }
    }
    ModalDialog(visible = isDialog, onDismiss = dismiss, title = dialogTitle(sheet)) {
        when (sheet) {
            is Sheet.Rename -> TextDialog(sheet.initial, "", "Salvar", { sheet.onConfirm(it) }, dismiss)
            is Sheet.TextInput -> TextDialog("", sheet.hint, sheet.action, { sheet.onConfirm(it) }, dismiss)
            is Sheet.Confirm -> {
                Text(sheet.message, style = Oni.type.body, color = Oni.colors.textMuted)
                Spacer(Modifier.height(20.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    OniButton("Cancelar", onClick = dismiss, kind = ButtonKind.Ghost, compact = true)
                    Spacer(Modifier.width(8.dp))
                    OniButton(sheet.action, onClick = { dismiss(); sheet.onConfirm() }, kind = ButtonKind.Danger, compact = true)
                }
            }
            else -> {}
        }
    }
}

private fun sheetTitle(sheet: Sheet?): String? = when (sheet) {
    is Sheet.NewProject -> "Novo jogo"
    is Sheet.ProjectMenu -> sheet.project.name
    Sheet.AddEntity -> "Adicionar à cena"
    is Sheet.EntityMenu -> sheet.node.name
    is Sheet.Reparent -> "Mover \"${sheet.node.name}\" para…"
    Sheet.AddComponent -> "Adicionar componente"
    is Sheet.Picker -> when (sheet.target.kind) {
        "texture" -> "Escolher imagem"
        "audio" -> "Escolher som"
        "material" -> "Escolher material"
        else -> "Escolher script"
    }
    is Sheet.Color -> "Cor"
    is Sheet.AssetMenu -> sheet.item.name
    Sheet.Scenes -> "Cenas"
    Sheet.Settings -> "Ajustes do jogo"
    is Sheet.Animation -> sheet.model.name.removeSuffix(".anim.json")
    is Sheet.Material -> sheet.model.name.removeSuffix(".mat.json")
    else -> null
}

private fun dialogTitle(sheet: Sheet?): String = when (sheet) {
    is Sheet.Rename -> sheet.title
    is Sheet.TextInput -> sheet.title
    is Sheet.Confirm -> sheet.title
    else -> ""
}

@Composable
private fun ColumnScope.TextDialog(initial: String, hint: String, action: String, onConfirm: (String) -> Unit, dismiss: () -> Unit) {
    var text by remember(initial) { mutableStateOf(initial) }
    OniField(text, { text = it }, Modifier.fillMaxWidth(), placeholder = hint, onChange = { text = it })
    Spacer(Modifier.height(20.dp))
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        OniButton("Cancelar", onClick = dismiss, kind = ButtonKind.Ghost, compact = true)
        Spacer(Modifier.width(8.dp))
        OniButton(action, onClick = {
            val value = text.trim()
            if (value.isNotEmpty()) {
                dismiss()
                onConfirm(value)
            }
        }, compact = true)
    }
}

// --- adicionar objeto -----------------------------------------------------------------------

private data class Template(val id: String, val label: String, val hint: String, val kind: String, val icon: ImageVector)

private val templates = listOf(
    Template("sprite", "Sprite", "Imagem na cena", "sprite", OniIcons.Image),
    Template("character", "Personagem", "Anda e pula com script", "character", OniIcons.Person),
    Template("ground", "Chão", "Plataforma sólida", "empty", OniIcons.Layers),
    Template("physics", "Caixa física", "Cai e colide", "sprite", OniIcons.Cube),
    Template("text", "Texto", "Placar, avisos", "text", OniIcons.Text),
    Template("camera", "Câmera", "O que o jogador vê", "camera", OniIcons.Camera),
    Template("light", "Luz", "Ilumina sprites", "light", OniIcons.Sun),
    Template("particles", "Partículas", "Faíscas, fumaça", "particles", OniIcons.Sparkles),
    Template("audio", "Som", "Música ou efeito", "audio", OniIcons.Speaker),
    Template("empty", "Vazio", "Só posição e nome", "empty", OniIcons.Cube),
)

@Composable
private fun AddEntityContent(actions: UiActions) {
    val c = Oni.colors
    LazyVerticalGrid(
        columns = GridCells.Fixed(3),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        items(templates, key = { it.id }) { t ->
            Column(
                Modifier
                    .clip(OniShape.lg)
                    .background(c.s2)
                    .border(1.dp, c.line, OniShape.lg)
                    .clickable { actions.createEntity(t.id) }
                    .padding(12.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                KindBadge(t.kind, size = 44.dp, icon = t.icon)
                Spacer(Modifier.height(8.dp))
                Text(t.label, style = Oni.type.label, color = c.text, maxLines = 1)
                Text(t.hint, style = Oni.type.caption, color = c.textFaint, maxLines = 2, minLines = 2, textAlign = androidx.compose.ui.text.style.TextAlign.Center)
            }
        }
    }
}

// --- menus de entidade ------------------------------------------------------------------------

@Composable
private fun ColumnScope.EntityMenuContent(sheet: Sheet.EntityMenu, state: UiState, actions: UiActions) {
    val node = sheet.node
    MenuList(
        listOf(
            MenuItem("Editar propriedades", OniIcons.Sliders) {
                actions.select(node.id)
                actions.openTab(com.goni.ui.model.EditorTab.Properties)
            },
            MenuItem("Renomear", OniIcons.Edit) {
                state.sheet = Sheet.Rename("Renomear", node.name) { actions.renameEntity(node.id, it) }
            },
            MenuItem("Duplicar", OniIcons.Copy) { actions.duplicateEntity(node.id) },
            MenuItem("Mover para…", OniIcons.Tree) { state.sheet = Sheet.Reparent(node) },
            MenuItem("Excluir", OniIcons.Trash, danger = true) { actions.deleteEntity(node.id) },
        ),
    ) { state.sheet = null }
}

@Composable
private fun ReparentContent(sheet: Sheet.Reparent, state: UiState, actions: UiActions) {
    val node = sheet.node
    // Não pode virar filho de si mesmo nem de um descendente.
    val list = state.snapshot.hierarchy
    val start = list.indexOfFirst { it.id == node.id }
    val blocked = mutableSetOf(node.id)
    if (start >= 0) {
        for (i in start + 1 until list.size) {
            if (list[i].depth <= node.depth) break
            blocked += list[i].id
        }
    }
    LazyColumn(contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
        item {
            ListRow("Raiz da cena", leading = { KindBadge("empty", icon = OniIcons.Map) }, onClick = {
                actions.reparentEntity(node.id, 0L); actions.dismissSheet()
            })
        }
        items(list.filter { it.id !in blocked }, key = { it.id }) { target ->
            ListRow(
                target.name,
                modifier = Modifier.padding(start = (target.depth * 16).dp),
                leading = { KindBadge(target.kind) },
                onClick = { actions.reparentEntity(node.id, target.id); actions.dismissSheet() },
            )
        }
    }
}

// --- componentes ------------------------------------------------------------------------------

@Composable
private fun AddComponentContent(state: UiState, actions: UiActions) {
    val c = Oni.colors
    val id = state.snapshot.selection
    val groups = state.catalog.groupBy { it.category }
    LazyColumn(contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
        for ((category, items) in groups) {
            item(key = "h-$category") { SectionLabel(category, Modifier.padding(horizontal = 8.dp)) }
            items(items, key = { it.name }) { comp ->
                ListRow(
                    comp.label,
                    subtitle = comp.dependency.ifEmpty { null }?.let { "Traz junto: ${it.substringAfterLast("::")}" },
                    leading = {
                        Box(Modifier.size(36.dp).clip(OniShape.sm).background(c.s3), contentAlignment = Alignment.Center) {
                            OniIcon(OniIcons.Plus, tint = c.accent, size = 18.dp)
                        }
                    },
                    onClick = { actions.addComponent(id, comp.name); actions.dismissSheet() },
                )
            }
        }
        if (state.catalog.isEmpty()) {
            item { Text("Este objeto já tem todos os componentes disponíveis.", style = Oni.type.body, color = c.textMuted, modifier = Modifier.padding(16.dp)) }
        }
    }
}

@Composable
private fun PickerContent(sheet: Sheet.Picker, state: UiState, actions: UiActions) {
    val c = Oni.colors
    val t = sheet.target
    val choose = { value: String ->
        actions.setField(t.entity, t.component, t.path, value)
        actions.dismissSheet()
    }
    if (t.kind == "texture") {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(92.dp),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            item {
                Box(
                    Modifier.aspectRatio(1f).clip(OniShape.md).background(c.s2).border(1.dp, c.line, OniShape.md).clickable { choose("") },
                    contentAlignment = Alignment.Center,
                ) { Text("Nenhuma", style = Oni.type.caption, color = c.textMuted) }
            }
            items(sheet.options, key = { it.name }) { item ->
                Column(Modifier.clip(OniShape.md).clickable { choose(item.name) }) {
                    Box(Modifier.fillMaxWidth().aspectRatio(1f).clip(OniShape.md).background(c.s3), contentAlignment = Alignment.Center) {
                        if (item.thumbnail != null) {
                            Image(item.thumbnail, null, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize().padding(6.dp))
                        } else {
                            OniIcon(OniIcons.Image, tint = c.textFaint)
                        }
                    }
                    Text(item.name, style = Oni.type.caption, color = c.textMuted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 4.dp))
                }
            }
        }
        OniButton("Importar imagem", onClick = { actions.dismissSheet(); actions.importAsset(LibraryKind.Images) }, kind = ButtonKind.Ghost, icon = OniIcons.Download, modifier = Modifier.padding(horizontal = 16.dp))
        return
    }
    LazyColumn(Modifier.heightIn(max = 480.dp), contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
        item { ListRow("Nenhum", leading = { KindBadge("empty", icon = OniIcons.Close) }, onClick = { choose("") }) }
        items(sheet.options, key = { it.name }) { item ->
            ListRow(item.name, subtitle = item.subtitle.ifEmpty { null }, leading = { KindBadge(if (t.kind == "audio") "audio" else "script", icon = if (t.kind == "audio") OniIcons.Music else if (t.kind == "material") OniIcons.Droplet else OniIcons.Code) }, onClick = { choose(item.name) })
        }
    }
}

// --- cor ----------------------------------------------------------------------------------------

private val swatches = listOf(
    0xFFFFFFFF, 0xFFE6E9F0, 0xFF9AA3B2, 0xFF3A4152, 0xFF0B0D12,
    0xFFFF7470, 0xFFFFA86B, 0xFFF4BE5E, 0xFF4FD197, 0xFF55D0E8,
    0xFF7C9CFF, 0xFFB892FF, 0xFFFF8FC8, 0xFF8B5A3C, 0xFF2F7D4E,
)

@Composable
private fun ColorContent(sheet: Sheet.Color, actions: UiActions) {
    val c = Oni.colors
    val initial = parseHex(sheet.hex) ?: Color.White
    val hasAlpha = sheet.hex.removePrefix("#").length == 8
    var r by remember { mutableFloatStateOf(initial.red) }
    var g by remember { mutableFloatStateOf(initial.green) }
    var b by remember { mutableFloatStateOf(initial.blue) }
    var a by remember { mutableFloatStateOf(initial.alpha) }
    val current = Color(r, g, b, if (hasAlpha) a else 1f)
    fun hex(): String {
        val argb = current.toArgb()
        val rgb = String.format("#%06X", argb and 0xFFFFFF)
        return if (hasAlpha) rgb + String.format("%02X", (argb ushr 24) and 0xFF) else rgb
    }
    Column(Modifier.padding(horizontal = 20.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(64.dp).clip(OniShape.lg).background(current).border(1.dp, c.lineStrong, OniShape.lg))
            Spacer(Modifier.width(16.dp))
            Text(hex(), style = Oni.type.mono, color = c.text)
        }
        Spacer(Modifier.height(16.dp))
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (s in swatches) {
                val col = Color(s)
                ColorSwatch(col, size = 34.dp) { r = col.red; g = col.green; b = col.blue }
            }
        }
        Spacer(Modifier.height(12.dp))
        ChannelSlider("R", r, Color(0xFFFF7470)) { r = it }
        ChannelSlider("G", g, Color(0xFF4FD197)) { g = it }
        ChannelSlider("B", b, Color(0xFF7C9CFF)) { b = it }
        if (hasAlpha) ChannelSlider("A", a, c.textMuted) { a = it }
        Spacer(Modifier.height(12.dp))
        OniButton("Aplicar", onClick = {
            actions.setField(sheet.entity, sheet.component, sheet.path, hex())
            actions.dismissSheet()
        }, modifier = Modifier.fillMaxWidth(), icon = OniIcons.Check)
    }
}

@Composable
private fun ChannelSlider(label: String, value: Float, tint: Color, onChange: (Float) -> Unit) {
    val c = Oni.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = Oni.type.label, color = c.textMuted, modifier = Modifier.width(22.dp))
        Slider(
            value = value,
            onValueChange = onChange,
            modifier = Modifier.weight(1f),
            colors = SliderDefaults.colors(thumbColor = tint, activeTrackColor = tint, inactiveTrackColor = c.s3),
        )
        Text((value * 255).toInt().toString(), style = Oni.type.mono, color = c.text, modifier = Modifier.width(40.dp))
    }
}

// --- assets -------------------------------------------------------------------------------------

@Composable
private fun ColumnScope.AssetMenuContent(sheet: Sheet.AssetMenu, state: UiState, actions: UiActions) {
    val item = sheet.item
    val items = mutableListOf<MenuItem>()
    val hasSelection = state.snapshot.selection != 0L
    when (item.category) {
        "textures" -> if (hasSelection) items += MenuItem("Usar no objeto selecionado", OniIcons.Check) { actions.useAsset(item) }
        "scripts" -> {
            items += MenuItem("Editar", OniIcons.Code) { actions.openAsset(item) }
            if (hasSelection) items += MenuItem("Anexar ao objeto selecionado", OniIcons.Link) { actions.useAsset(item) }
        }
        "audio" -> items += MenuItem("Ouvir", OniIcons.Play) { actions.toggleAudio(item.name) }
        else -> items += MenuItem("Abrir", OniIcons.Edit) { actions.openAsset(item) }
    }
    items += MenuItem("Renomear", OniIcons.Edit) {
        state.sheet = Sheet.Rename("Renomear", item.name) { actions.renameAsset(item, it) }
    }
    items += MenuItem("Excluir", OniIcons.Trash, danger = true) {
        state.sheet = Sheet.Confirm("Excluir ${item.name}?", "O arquivo sai do projeto.", "Excluir") { actions.deleteAsset(item) }
    }
    MenuList(items) { state.sheet = null }
}

// --- cenas ---------------------------------------------------------------------------------------

@Composable
private fun ScenesContent(state: UiState, actions: UiActions) {
    val c = Oni.colors
    val current = state.snapshot.scenePath
    Column(Modifier.padding(horizontal = 12.dp)) {
        for (scene in state.scenes) {
            ListRow(
                scene.removeSuffix(".json"),
                subtitle = if (scene == current) "Aberta agora" else null,
                selected = scene == current,
                leading = { KindBadge("empty", icon = OniIcons.Map) },
                onClick = { actions.loadScene(scene); actions.dismissSheet() },
            )
        }
        if (state.scenes.isEmpty()) {
            Text("Nenhuma cena salva ainda.", style = Oni.type.body, color = c.textMuted, modifier = Modifier.padding(12.dp))
        }
        Spacer(Modifier.height(12.dp))
        Row(Modifier.padding(horizontal = 8.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            OniButton("Salvar", onClick = { actions.save(); actions.dismissSheet() }, icon = OniIcons.Save, modifier = Modifier.weight(1f), compact = true)
            OniButton("Salvar como…", onClick = {
                state.sheet = Sheet.TextInput("Salvar cena como", "nome da cena", "Salvar") { actions.saveSceneAs(it) }
            }, kind = ButtonKind.Tonal, modifier = Modifier.weight(1f), compact = true)
            OniButton("Nova", onClick = { actions.newScene(); actions.dismissSheet() }, kind = ButtonKind.Tonal, icon = OniIcons.Plus, modifier = Modifier.weight(1f), compact = true)
        }
    }
}

// --- ajustes -------------------------------------------------------------------------------------

@Composable
private fun SettingsContent(state: UiState, actions: UiActions) {
    val c = Oni.colors
    val s = state.settings
    Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 20.dp)) {
        SectionLabel("Jogo")
        OniField(s.projectName, { actions.setProjectName(it) }, Modifier.fillMaxWidth(), prefix = "Nome")
        Text(
            "Valem para todas as cenas, no editor e no jogo exportado.",
            style = Oni.type.caption,
            color = c.textFaint,
            modifier = Modifier.padding(top = 6.dp),
        )
        SubLabel("Cor de fundo")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Row(
                Modifier.weight(1f).horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                for (hex in backgroundSwatches) {
                    val col = parseHex(hex) ?: Color.Black
                    val on = hex.equals(s.background, ignoreCase = true)
                    Box(
                        Modifier
                            .size(36.dp)
                            .clip(OniShape.sm)
                            .background(col)
                            .border(if (on) 2.dp else 1.dp, if (on) c.accent else c.lineStrong, OniShape.sm)
                            .clickable { actions.setGame(hex, s.orientation, s.controls) },
                        contentAlignment = Alignment.Center,
                    ) {
                        if (on) OniIcon(OniIcons.Check, tint = if (col.luminance() > 0.5f) Color.Black else Color.White, size = 16.dp)
                    }
                }
            }
            Spacer(Modifier.width(10.dp))
            OniField(
                s.background.uppercase(),
                { v -> normalizeHex(v)?.let { actions.setGame(it, s.orientation, s.controls) } },
                Modifier.width(112.dp),
                minHeight = 40.dp,
            )
        }
        SubLabel("Tela")
        OptionTiles(
            listOf(
                Option("portrait", "Em pé", OniIcons.Portrait),
                Option("landscape", "Deitada", OniIcons.Landscape),
                Option("auto", "Livre", OniIcons.Auto),
            ),
            s.orientation,
        ) { actions.setGame(s.background, it, s.controls) }
        SubLabel("Controles de toque")
        OptionTiles(
            listOf(
                Option("platformer", "Botões", OniIcons.Gamepad, "◀ ▶ e pulo"),
                Option("tap", "Toque", OniIcons.Tap, "a tela toda"),
                Option("none", "Nenhum", OniIcons.Close, "só scripts"),
            ),
            s.controls,
        ) { actions.setGame(s.background, s.orientation, it) }
        SectionLabel("Grade do editor")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Mostrar grade", style = Oni.type.body, color = c.text, modifier = Modifier.weight(1f))
            OniSwitch(s.gridVisible) { actions.setGrid(it, s.gridCell) }
        }
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Tamanho da célula", style = Oni.type.body, color = c.text, modifier = Modifier.weight(1f))
            OniField(s.gridCell.toString(), { v -> v.toFloatOrNull()?.let { actions.setGrid(s.gridVisible, it) } }, Modifier.width(110.dp), numeric = true)
        }
        SectionLabel("Física")
        val hz = listOf(30, 60, 120)
        Segmented(hz.map { "$it Hz" }, hz.indexOf(s.physicsHz).coerceAtLeast(0), { actions.setPhysicsHz(hz[it]) }, Modifier.fillMaxWidth())
        SectionLabel("Camadas de colisão", trailing = {
            OniButton("Nova", onClick = { state.sheet = Sheet.TextInput("Nova camada de colisão", "ex.: Inimigos", "Criar") { actions.addCollisionLayer(it) } }, kind = ButtonKind.Ghost, compact = true, icon = OniIcons.Plus)
        })
        for (layer in s.collisionLayers) {
            ListRow(layer.name, subtitle = "bit ${java.lang.Long.numberOfTrailingZeros(layer.bit)}", onClick = {
                state.sheet = Sheet.Rename("Renomear camada", layer.name) { actions.renameCollisionLayer(layer.bit, it) }
            }, trailing = { OniIcon(OniIcons.Edit, tint = c.textFaint, size = 16.dp) })
        }
        SectionLabel("Grupos de atualização", trailing = {
            OniButton("Novo", onClick = { state.sheet = Sheet.TextInput("Novo grupo", "ex.: Fundo", "Criar") { actions.addTickLayer(it) } }, kind = ButtonKind.Ghost, compact = true, icon = OniIcons.Plus)
        })
        for (layer in s.layers) {
            Column(Modifier.fillMaxWidth().padding(vertical = 4.dp).clip(OniShape.md).background(c.s2).padding(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(layer.name, style = Oni.type.bodyStrong, color = c.text, modifier = Modifier.weight(1f))
                    Text("velocidade", style = Oni.type.caption, color = c.textMuted)
                    Spacer(Modifier.width(8.dp))
                    OniField(layer.timeScale.toString(), { v -> v.toFloatOrNull()?.let { actions.setTickLayer(layer.copy(timeScale = it)) } }, Modifier.width(80.dp), numeric = true, minHeight = 36.dp)
                }
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    com.goni.ui.components.OniChip("Lógica", layer.update, { actions.setTickLayer(layer.copy(update = !layer.update)) })
                    com.goni.ui.components.OniChip("Física", layer.physics, { actions.setTickLayer(layer.copy(physics = !layer.physics)) })
                    com.goni.ui.components.OniChip("Desenho", layer.render, { actions.setTickLayer(layer.copy(render = !layer.render)) })
                }
            }
        }
        SectionLabel("Sobre")
        ListRow("Exportar diagnóstico", subtitle = "Logs para relatar um problema", leading = { KindBadge("empty", icon = OniIcons.Bug) }, onClick = actions::exportDiagnostics)
        Text(
            listOf(s.version, s.backend).filter { it.isNotEmpty() }.joinToString(" · "),
            style = Oni.type.caption,
            color = c.textFaint,
            modifier = Modifier.padding(vertical = 12.dp),
        )
    }
}

private val backgroundSwatches = listOf(
    "#12141C", "#1B2A55", "#5CA3DB", "#8FD3F4", "#2E4A2A", "#3A1E2E", "#F2D7B6", "#FFFFFF",
)

/** "#abc", "abc123", "#AABBCC" → "#AABBCC"; null se não for cor. */
private fun normalizeHex(input: String): String? {
    val h = input.trim().removePrefix("#")
    val full = when (h.length) {
        3 -> h.map { "$it$it" }.joinToString("")
        6 -> h
        else -> return null
    }
    return if (full.all { it.isDigit() || it.lowercaseChar() in 'a'..'f' }) "#" + full.uppercase() else null
}

@Composable
private fun SubLabel(text: String) {
    Text(text, style = Oni.type.label, color = Oni.colors.textMuted, modifier = Modifier.padding(top = 14.dp, bottom = 8.dp))
}

private data class Option(val id: String, val label: String, val icon: ImageVector, val hint: String = "")

/** Escolha única em cartões com ícone — mais legível que um seletor de texto. */
@Composable
private fun OptionTiles(options: List<Option>, selected: String, onSelect: (String) -> Unit) {
    val c = Oni.colors
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        for (o in options) {
            val on = o.id == selected
            Column(
                Modifier
                    .weight(1f)
                    .clip(OniShape.md)
                    .background(if (on) c.accentSoft else c.s2)
                    .border(if (on) 2.dp else 1.dp, if (on) c.accent else c.line, OniShape.md)
                    .clickable { onSelect(o.id) }
                    .padding(vertical = 12.dp, horizontal = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                OniIcon(o.icon, tint = if (on) c.accent else c.textMuted, size = 22.dp)
                Spacer(Modifier.height(6.dp))
                Text(o.label, style = Oni.type.label, color = if (on) c.text else c.textMuted, maxLines = 1)
                if (o.hint.isNotEmpty()) {
                    Text(o.hint, style = Oni.type.caption, color = c.textFaint, maxLines = 1)
                }
            }
        }
    }
}

// --- animação e material ---------------------------------------------------------------------

@Composable
private fun AnimationContent(model: AnimationModel, state: UiState, actions: UiActions) {
    val c = Oni.colors
    val hasSelection = state.snapshot.selection != 0L
    Column(Modifier.padding(horizontal = 20.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Quadros por segundo", style = Oni.type.body, color = c.text, modifier = Modifier.weight(1f))
            OniField(model.fps.toInt().toString(), { v -> v.toFloatOrNull()?.let { actions.saveAnimation(model.copy(fps = it)) } }, Modifier.width(90.dp), numeric = true)
        }
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Repetir", style = Oni.type.body, color = c.text, modifier = Modifier.weight(1f))
            OniSwitch(model.loop) { actions.saveAnimation(model.copy(loop = it)) }
        }
        SectionLabel("Quadros (${model.frames.size})")
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            model.frames.forEachIndexed { i, frame ->
                val thumb = state.assets.firstOrNull { it.category == "textures" && it.name == frame }?.thumbnail
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Box(Modifier.size(72.dp).clip(OniShape.md).background(c.s3), contentAlignment = Alignment.Center) {
                        if (thumb != null) Image(thumb, null, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize().padding(4.dp))
                        else OniIcon(OniIcons.Image, tint = c.textFaint)
                    }
                    Text("${i + 1}", style = Oni.type.caption, color = c.textFaint)
                }
            }
            Box(
                Modifier.size(72.dp).clip(OniShape.md).border(1.dp, c.lineStrong, OniShape.md).clickable { actions.addAnimationFrame(model) },
                contentAlignment = Alignment.Center,
            ) { OniIcon(OniIcons.Plus, tint = c.accent) }
        }
        Spacer(Modifier.height(16.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            OniButton("Pré-visualizar", onClick = { actions.previewAnimation(model.name) }, kind = ButtonKind.Tonal, icon = OniIcons.Eye, enabled = hasSelection, modifier = Modifier.weight(1f), compact = true)
            OniButton("Usar no objeto", onClick = { actions.assignAnimation(model.name) }, icon = OniIcons.Link, enabled = hasSelection, modifier = Modifier.weight(1f), compact = true)
        }
        if (!hasSelection) {
            Text("Selecione um objeto para pré-visualizar ou aplicar.", style = Oni.type.caption, color = c.textFaint, modifier = Modifier.padding(top = 8.dp))
        }
    }
}

@Composable
private fun MaterialContent(model: MaterialModel, actions: UiActions) {
    val c = Oni.colors
    var lit by remember(model) { mutableStateOf(model.lit) }
    Column(Modifier.padding(horizontal = 20.dp)) {
        SectionLabel("Iluminação")
        Segmented(listOf("Recebe luz", "Sem luz"), if (lit) 0 else 1, { lit = it == 0 }, Modifier.fillMaxWidth())
        SectionLabel("Cor")
        Row(verticalAlignment = Alignment.CenterVertically) {
            ColorSwatch(Color(model.tint), size = 36.dp)
            Spacer(Modifier.width(10.dp))
            Text(String.format("#%08X", model.tint), style = Oni.type.mono, color = c.textMuted)
        }
        Spacer(Modifier.height(16.dp))
        OniButton("Salvar", onClick = { actions.saveMaterial(model.copy(lit = lit)); actions.dismissSheet() }, icon = OniIcons.Check, modifier = Modifier.fillMaxWidth())
    }
}
