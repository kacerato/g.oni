package com.goni.ui.screens

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.goni.ui.components.ButtonKind
import com.goni.ui.components.ColorSwatch
import com.goni.ui.components.EmptyState
import com.goni.ui.components.IconAction
import com.goni.ui.components.KindBadge
import com.goni.ui.components.ListRow
import com.goni.ui.components.OniButton
import com.goni.ui.components.OniChip
import com.goni.ui.components.OniField
import com.goni.ui.components.OniIcon
import com.goni.ui.components.OniSwitch
import com.goni.ui.components.Segmented
import com.goni.ui.model.AssetItem
import com.goni.ui.model.ComponentModel
import com.goni.ui.model.FieldModel
import com.goni.ui.model.InspectorModel
import com.goni.ui.model.LibraryKind
import com.goni.ui.model.PickTarget
import com.goni.ui.model.Sheet
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniIcons
import com.goni.ui.theme.OniShape

@Composable
private fun PanelHeader(title: String, subtitle: String? = null, trailing: @Composable () -> Unit = {}) {
    Row(
        Modifier.fillMaxWidth().padding(start = 18.dp, end = 10.dp, top = 2.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Oni.type.title, color = Oni.colors.text)
            if (subtitle != null) {
                Text(subtitle, style = Oni.type.caption, color = Oni.colors.textMuted)
            }
        }
        trailing()
    }
}

// --- Cena --------------------------------------------------------------------------------

@Composable
fun ScenePanel(state: UiState, actions: UiActions) {
    val snap = state.snapshot
    val c = Oni.colors
    PanelHeader("Cena", "${snap.sceneName} · ${snap.hierarchy.size} objetos") {
        OniButton("Cenas", onClick = actions::openScenes, kind = ButtonKind.Tonal, icon = OniIcons.Map, compact = true)
    }
    if (snap.hierarchy.isEmpty()) {
        EmptyState(
            OniIcons.Cube,
            "Cena vazia",
            "Toque em + para adicionar um sprite, personagem ou câmera.",
            action = { OniButton("Adicionar objeto", onClick = { state.sheet = Sheet.AddEntity }, icon = OniIcons.Plus, compact = true) },
        )
        return
    }
    LazyColumn(contentPadding = PaddingValues(start = 8.dp, end = 8.dp, bottom = 16.dp)) {
        items(snap.hierarchy, key = { it.id }) { node ->
            val selected = node.id == snap.selection
            Row(
                Modifier
                    .fillMaxWidth()
                    .height(48.dp)
                    .clip(OniShape.md)
                    .background(if (selected) c.accentSoft else Color.Transparent)
                    .clickable { actions.select(node.id) }
                    .padding(start = 8.dp + (node.depth * 18).dp, end = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (node.depth > 0) {
                    Box(Modifier.width(10.dp).height(1.dp).background(c.lineStrong))
                    Spacer(Modifier.width(6.dp))
                }
                KindBadge(node.kind, size = 28.dp)
                Spacer(Modifier.width(12.dp))
                Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        node.name,
                        style = if (selected) Oni.type.bodyStrong else Oni.type.body,
                        color = c.text,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false),
                    )
                    if (node.template) {
                        Spacer(Modifier.width(8.dp))
                        Text(
                            "MOLDE",
                            style = Oni.type.overline,
                            color = c.kindTemplate,
                            modifier = Modifier
                                .clip(OniShape.xs)
                                .background(c.kindTemplate.copy(alpha = 0.14f))
                                .padding(horizontal = 6.dp, vertical = 2.dp),
                        )
                    }
                }
                IconAction(OniIcons.More, onClick = { state.sheet = Sheet.EntityMenu(node) }, size = 40.dp, iconSize = 18.dp)
            }
        }
    }
}

// --- Propriedades -----------------------------------------------------------------------

@Composable
fun PropertiesPanel(state: UiState, actions: UiActions) {
    val model = state.inspector
    val selection = state.snapshot.selectedNode
    if (model == null || selection == null) {
        PanelHeader("Propriedades")
        EmptyState(
            OniIcons.Pointer,
            "Nada selecionado",
            "Toque num objeto na cena ou na lista da aba Cena para editar.",
        )
        return
    }
    Inspector(model, state, actions)
}

@Composable
private fun Inspector(model: InspectorModel, state: UiState, actions: UiActions) {
    val c = Oni.colors
    val collapsed = remember(model.id) { mutableStateMapOf<String, Boolean>() }
    LazyColumn(contentPadding = PaddingValues(start = 12.dp, end = 12.dp, bottom = 20.dp)) {
        item(key = "header") {
            Row(Modifier.fillMaxWidth().padding(start = 4.dp, bottom = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                KindBadge(model.kind, size = 40.dp)
                Spacer(Modifier.width(12.dp))
                OniField(
                    value = model.name,
                    onCommit = { actions.renameEntity(model.id, it) },
                    modifier = Modifier.weight(1f),
                    textStyle = Oni.type.heading,
                )
                val node = state.snapshot.selectedNode
                IconAction(OniIcons.More, onClick = { if (node != null) state.sheet = Sheet.EntityMenu(node) })
            }
        }
        item(key = "transform") {
            SectionCard("Transform", icon = OniIcons.Move) {
                val t = model.transform
                FieldLabel("Posição")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OniField(fmt(t.x), { v -> v.toFloatOrNull()?.let { actions.setTransform(model.id, t.copy(x = it)) } }, Modifier.weight(1f), prefix = "X", numeric = true)
                    OniField(fmt(t.y), { v -> v.toFloatOrNull()?.let { actions.setTransform(model.id, t.copy(y = it)) } }, Modifier.weight(1f), prefix = "Y", numeric = true)
                }
                Spacer(Modifier.height(10.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Column(Modifier.weight(1f)) {
                        FieldLabel("Rotação")
                        OniField(fmt(t.rotation), { v -> v.toFloatOrNull()?.let { actions.setTransform(model.id, t.copy(rotation = it)) } }, prefix = "°", numeric = true)
                    }
                    Column(Modifier.weight(2f)) {
                        FieldLabel("Escala")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            OniField(fmt(t.scaleX), { v -> v.toFloatOrNull()?.let { actions.setTransform(model.id, t.copy(scaleX = it)) } }, Modifier.weight(1f), prefix = "X", numeric = true)
                            OniField(fmt(t.scaleY), { v -> v.toFloatOrNull()?.let { actions.setTransform(model.id, t.copy(scaleY = it)) } }, Modifier.weight(1f), prefix = "Y", numeric = true)
                        }
                    }
                }
            }
        }
        items(model.components.filter { it.name != "eng::math::Transform" }, key = { it.name }) { comp ->
            val isCollapsed = collapsed[comp.name] == true
            SectionCard(
                comp.label,
                icon = componentIcon(comp.name),
                tag = comp.category,
                collapsed = isCollapsed,
                onToggle = { collapsed[comp.name] = !isCollapsed },
                trailing = {
                    if (comp.removable) {
                        IconAction(OniIcons.Trash, onClick = {
                            state.sheet = Sheet.Confirm(
                                "Remover ${comp.label}?",
                                "O componente sai deste objeto. Dá para desfazer.",
                                "Remover",
                            ) { actions.removeComponent(model.id, comp.name) }
                        }, size = 36.dp, iconSize = 16.dp)
                    }
                },
            ) {
                ComponentFields(model.id, comp, state, actions)
            }
        }
        item(key = "add") {
            OniButton(
                "Adicionar componente",
                onClick = actions::openCatalog,
                kind = ButtonKind.Tonal,
                icon = OniIcons.Plus,
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
            )
        }
    }
}

@Composable
private fun SectionCard(
    title: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    tag: String? = null,
    collapsed: Boolean = false,
    onToggle: (() -> Unit)? = null,
    trailing: @Composable () -> Unit = {},
    content: @Composable () -> Unit,
) {
    val c = Oni.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 10.dp)
            .clip(OniShape.lg)
            .background(c.s2)
            .border(1.dp, c.line, OniShape.lg),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .then(if (onToggle != null) Modifier.clickable(onClick = onToggle) else Modifier)
                .padding(start = 14.dp, end = 6.dp, top = 6.dp, bottom = 6.dp)
                .height(36.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OniIcon(icon, tint = c.accent, size = 17.dp)
            Spacer(Modifier.width(10.dp))
            Text(title, style = Oni.type.bodyStrong, color = c.text, modifier = Modifier.weight(1f))
            if (tag != null) {
                Text(
                    tag,
                    style = Oni.type.caption,
                    color = c.textFaint,
                    modifier = Modifier.clip(OniShape.pill).background(c.s3).padding(horizontal = 8.dp, vertical = 2.dp),
                )
            }
            trailing()
            if (onToggle != null) {
                OniIcon(if (collapsed) OniIcons.ChevronDown else OniIcons.ChevronUp, tint = c.textFaint, size = 18.dp, modifier = Modifier.padding(horizontal = 6.dp))
            }
        }
        if (!collapsed) {
            Column(Modifier.padding(start = 14.dp, end = 14.dp, bottom = 14.dp)) { content() }
        }
    }
}

@Composable
private fun FieldLabel(text: String) {
    Text(text, style = Oni.type.caption, color = Oni.colors.textMuted, modifier = Modifier.padding(bottom = 5.dp))
}

private fun fmt(v: Float): String {
    val r = Math.round(v * 1000f) / 1000f
    return if (r == r.toLong().toFloat()) r.toLong().toString() else r.toString()
}

private fun componentIcon(name: String) = when (name) {
    "eng::editor::SpriteData" -> OniIcons.Image
    "eng::physics::RigidBody" -> OniIcons.Cube
    "eng::physics::Collider" -> OniIcons.Square
    "eng::physics::CharacterBody" -> OniIcons.Person
    "eng::animation::Animator" -> OniIcons.Film
    "eng::particles::ParticleEmitter" -> OniIcons.Sparkles
    "eng::editor::NiScriptComponent" -> OniIcons.Code
    "eng::tick::CameraData" -> OniIcons.Camera
    "eng::editor::AudioSource" -> OniIcons.Speaker
    "eng::render::Light2D" -> OniIcons.Sun
    "eng::scene::LayerMember" -> OniIcons.Layers
    "eng::editor::TextData" -> OniIcons.Text
    "eng::scene::Template" -> OniIcons.Stamp
    else -> OniIcons.Sliders
}

/** Rótulos legíveis para os campos mais comuns (o resto vira "Camel case"). */
private val fieldNames = mapOf(
    "textureAsset" to "Imagem", "materialAsset" to "Material", "tint" to "Cor", "tintR,tintG,tintB" to "Cor",
    "opacity" to "Opacidade", "flipX" to "Espelhar horizontal", "flipY" to "Espelhar vertical", "sort" to "Ordem de desenho",
    "pixelsPerUnit" to "Pixels por unidade", "pivotX" to "Pivô X", "pivotY" to "Pivô Y", "mass" to "Massa",
    "velocity" to "Velocidade", "gravity" to "Gravidade", "useGravity" to "Usar gravidade", "linearDamping" to "Amortecimento",
    "bodyType" to "Tipo de corpo", "shape" to "Forma", "radius" to "Raio", "halfExtents" to "Meia extensão",
    "layer" to "Camada", "mask" to "Colide com", "isTrigger" to "Só detecta (gatilho)", "zoom" to "Zoom",
    "active" to "Ativa", "followName" to "Seguir objeto", "source" to "Código", "scriptAsset" to "Script",
    "lodOptOut" to "Sempre atualizar", "snapToGround" to "Grudar no chão", "posX" to "Deslocamento X", "posY" to "Deslocamento Y",
    "rotationDeg" to "Rotação", "deadzoneW" to "Zona morta L", "deadzoneH" to "Zona morta A", "smoothingTime" to "Suavização",
    "volume" to "Volume", "loop" to "Repetir", "playOnStart" to "Tocar ao iniciar", "clip" to "Som", "intensity" to "Intensidade",
    "text" to "Texto", "size" to "Tamanho", "colorR,colorG,colorB" to "Cor", "align" to "Alinhamento",
    "screenSpace" to "Fixo na tela", "screenX" to "Posição na tela X", "screenY" to "Posição na tela Y",
    "viewHeight" to "Altura visível",
    "speed" to "Velocidade", "playing" to "Tocando", "rate" to "Taxa", "lifetime" to "Duração", "maxParticles" to "Máx. partículas",
)

private fun prettyLabel(path: String): String {
    fieldNames[path]?.let { return it }
    val base = path.substringBefore('.')
    val tail = path.substringAfter('.', "")
    val head = fieldNames[base] ?: base.replace(Regex("([a-z])([A-Z])"), "$1 $2").replaceFirstChar { it.uppercase() }
    return if (tail.isEmpty()) head else "$head ${tail.uppercase()}"
}

private val enumNames = mapOf(
    "Static" to "Estático", "Kinematic" to "Cinemático", "DynamicLite" to "Dinâmico",
    "Sphere" to "Círculo", "Box" to "Caixa",
    "Left" to "Esquerda", "Center" to "Centro", "Right" to "Direita",
)

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ComponentFields(entity: Long, comp: ComponentModel, state: UiState, actions: UiActions) {
    // Vetores (a.x/a.y/a.z) viram uma linha só; z fica de fora no editor 2D.
    val fields = comp.fields.filterNot { f ->
        f.path.endsWith(".z") && comp.fields.any { it.path == f.path.removeSuffix(".z") + ".x" }
    }
    if (comp.name == "eng::scene::Template") {
        Text(
            "Molde: fica fora do jogo. Scripts criam cópias com spawn(\"${state.inspector?.name ?: "Nome"}\"), " +
                "como os canos do Voo.",
            style = Oni.type.caption,
            color = Oni.colors.textMuted,
        )
        return
    }
    val done = mutableSetOf<String>()
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        for (f in fields) {
            if (f.path in done) continue
            val base = f.path.substringBeforeLast('.', "")
            val sibling = if (f.path.endsWith(".x") && f.kind in setOf("number", "int")) fields.firstOrNull { it.path == "$base.y" } else null
            if (sibling != null) {
                done += f.path
                done += sibling.path
                Column {
                    FieldLabel(prettyLabel(base))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OniField(f.value, { actions.setField(entity, comp.name, f.path, it) }, Modifier.weight(1f), prefix = "X", numeric = true)
                        OniField(sibling.value, { actions.setField(entity, comp.name, sibling.path, it) }, Modifier.weight(1f), prefix = "Y", numeric = true)
                    }
                }
                continue
            }
            done += f.path
            FieldEditor(entity, comp, f, state, actions)
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun FieldEditor(entity: Long, comp: ComponentModel, f: FieldModel, state: UiState, actions: UiActions) {
    val c = Oni.colors
    val label = prettyLabel(f.path)
    when (f.kind) {
        "bool" -> Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = Oni.type.body, color = c.text, modifier = Modifier.weight(1f))
            OniSwitch(f.value == "true") { actions.setField(entity, comp.name, f.path, if (it) "true" else "false") }
        }
        "enum" -> Column {
            FieldLabel(label)
            if (f.options.size in 2..3) {
                Segmented(
                    f.options.map { enumNames[it] ?: it },
                    f.options.indexOf(f.value).coerceAtLeast(0),
                    { i -> actions.setField(entity, comp.name, f.path, f.options[i]) },
                    Modifier.fillMaxWidth(),
                )
            } else {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (o in f.options) OniChip(enumNames[o] ?: o, o == f.value, { actions.setField(entity, comp.name, f.path, o) })
                }
            }
        }
        "color" -> Column {
            FieldLabel(label)
            Row(
                Modifier.fillMaxWidth().clip(OniShape.sm).background(c.s3).clickable { actions.openColor(entity, comp.name, f.path, f.value) }.padding(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ColorSwatch(parseHex(f.value) ?: Color.White)
                Spacer(Modifier.width(10.dp))
                Text(f.value.uppercase(), style = Oni.type.mono, color = c.text, modifier = Modifier.weight(1f))
                OniIcon(OniIcons.ChevronRight, tint = c.textFaint, size = 18.dp)
            }
        }
        "texture", "audio", "material", "script" -> Column {
            FieldLabel(label)
            AssetRefRow(f, state) { actions.openPicker(PickTarget(entity, comp.name, f.path, f.kind)) }
        }
        "code" -> {
            val linked = comp.fields.firstOrNull { it.path == "scriptAsset" }?.value.orEmpty()
            if (linked.isEmpty()) {
                val lines = if (f.value.isEmpty()) 0 else f.value.count { it == '\n' } + 1
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OniIcon(OniIcons.Code, tint = c.textMuted, size = 16.dp)
                    Spacer(Modifier.width(8.dp))
                    Text("Código embutido · $lines linhas", style = Oni.type.caption, color = c.textMuted)
                }
            }
        }
        "bitfield" -> Column {
            FieldLabel(label)
            val bits = f.value.toLongOrNull() ?: 0L
            val layers = state.settings.collisionLayers.ifEmpty { listOf(com.goni.ui.model.CollisionLayer("Padrão", 1)) }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for (layer in layers) {
                    val on = bits and layer.bit != 0L
                    OniChip(layer.name, on, {
                        val next = if (on) bits and layer.bit.inv() else bits or layer.bit
                        actions.setField(entity, comp.name, f.path, (next and 0xFFFFFFFFL).toString())
                    })
                }
            }
        }
        "layer" -> Column {
            FieldLabel(label)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for (layer in state.settings.layers) {
                    OniChip(layer.name, layer.name == f.value, { actions.setField(entity, comp.name, f.path, layer.name) })
                }
            }
        }
        "number", "int" -> Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = Oni.type.body, color = c.text, modifier = Modifier.weight(1f))
            OniField(f.value, { actions.setField(entity, comp.name, f.path, it) }, Modifier.width(120.dp), numeric = true)
        }
        else -> Column {
            FieldLabel(label)
            OniField(f.value, { actions.setField(entity, comp.name, f.path, it) }, Modifier.fillMaxWidth())
        }
    }
}

@Composable
private fun AssetRefRow(f: FieldModel, state: UiState, onClick: () -> Unit) {
    val c = Oni.colors
    val thumb = if (f.kind == "texture") state.assets.firstOrNull { it.category == "textures" && it.name == f.value }?.thumbnail else null
    Row(
        Modifier.fillMaxWidth().clip(OniShape.sm).background(c.s3).clickable(onClick = onClick).padding(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(36.dp).clip(OniShape.xs).background(c.s4), contentAlignment = Alignment.Center) {
            if (thumb != null) {
                Image(thumb, null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
            } else {
                OniIcon(
                    when (f.kind) {
                        "audio" -> OniIcons.Music
                        "material" -> OniIcons.Droplet
                        "script" -> OniIcons.Code
                        else -> OniIcons.Image
                    },
                    tint = c.textMuted,
                    size = 18.dp,
                )
            }
        }
        Spacer(Modifier.width(10.dp))
        Text(
            f.value.ifEmpty { "Nenhum" },
            style = Oni.type.body,
            color = if (f.value.isEmpty()) c.textFaint else c.text,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        Text("Trocar", style = Oni.type.label, color = c.accent)
    }
}

fun parseHex(hex: String): Color? {
    val h = hex.removePrefix("#")
    val v = h.toLongOrNull(16) ?: return null
    return when (h.length) {
        6 -> Color(0xFF000000 or v)
        8 -> Color(((v and 0xFF) shl 24) or (v shr 8))
        else -> null
    }
}

// --- Biblioteca ---------------------------------------------------------------------------

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun LibraryPanel(state: UiState, actions: UiActions) {
    val c = Oni.colors
    val kind = state.library
    PanelHeader("Biblioteca") {
        val (label, icon) = when (kind) {
            LibraryKind.Images, LibraryKind.Sounds -> "Importar" to OniIcons.Download
            else -> "Novo" to OniIcons.Plus
        }
        OniButton(label, onClick = {
            if (kind == LibraryKind.Images || kind == LibraryKind.Sounds) {
                actions.importAsset(kind)
            } else {
                state.sheet = Sheet.TextInput(
                    when (kind) {
                        LibraryKind.Scripts -> "Novo script"
                        LibraryKind.Animations -> "Nova animação"
                        else -> "Novo material"
                    },
                    "nome",
                    "Criar",
                ) { actions.createAsset(kind, it) }
            }
        }, kind = ButtonKind.Tonal, icon = icon, compact = true)
    }
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 14.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        for (k in LibraryKind.values()) {
            OniChip(k.label, k == kind, { actions.showLibrary(k) }, icon = libraryIcon(k))
        }
    }
    Spacer(Modifier.height(8.dp))
    val items = state.assets.filter { it.category == kind.category }
    if (items.isEmpty()) {
        EmptyState(
            libraryIcon(kind),
            "Nada aqui ainda",
            when (kind) {
                LibraryKind.Images -> "Importe PNG ou JPG da galeria ou dos arquivos."
                LibraryKind.Sounds -> "Importe arquivos WAV."
                LibraryKind.Scripts -> "Scripts dão comportamento aos objetos."
                LibraryKind.Animations -> "Anime sprites quadro a quadro."
                LibraryKind.Materials -> "Materiais mudam como o sprite recebe luz."
            },
        )
        return
    }
    if (kind == LibraryKind.Images) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(92.dp),
            contentPadding = PaddingValues(start = 14.dp, end = 14.dp, bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            items(items, key = { it.name }) { item ->
                Column(
                    Modifier
                        .clip(OniShape.md)
                        .combinedClickable(onClick = { actions.useAsset(item) }, onLongClick = { state.sheet = Sheet.AssetMenu(item) }),
                ) {
                    Box(
                        Modifier.fillMaxWidth().aspectRatio(1f).clip(OniShape.md).background(c.s3).border(1.dp, c.line, OniShape.md),
                        contentAlignment = Alignment.Center,
                    ) {
                        if (item.thumbnail != null) {
                            Image(item.thumbnail, null, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize().padding(6.dp))
                        } else {
                            OniIcon(OniIcons.Image, tint = c.textFaint, size = 24.dp)
                        }
                    }
                    Text(item.name, style = Oni.type.caption, color = c.textMuted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 4.dp, start = 2.dp))
                }
            }
        }
        return
    }
    LazyColumn(contentPadding = PaddingValues(start = 8.dp, end = 8.dp, bottom = 16.dp)) {
        items(items, key = { it.name }) { item ->
            AssetRow(item, state, actions)
        }
    }
}

@Composable
private fun AssetRow(item: AssetItem, state: UiState, actions: UiActions) {
    val c = Oni.colors
    val kind = LibraryKind.values().first { it.category == item.category }
    ListRow(
        title = item.name,
        subtitle = item.subtitle.ifEmpty { null },
        leading = { KindBadge(libraryKindTag(kind), size = 36.dp, icon = libraryIcon(kind)) },
        onClick = { actions.openAsset(item) },
        trailing = {
            if (kind == LibraryKind.Sounds) {
                val playing = state.playingAudio == item.name
                IconAction(if (playing) OniIcons.Stop else OniIcons.Play, onClick = { actions.toggleAudio(item.name) }, selected = playing, size = 40.dp, iconSize = 16.dp)
            }
            if (kind == LibraryKind.Scripts && state.snapshot.selection != 0L) {
                OniButton("Anexar", onClick = { actions.useAsset(item) }, kind = ButtonKind.Ghost, compact = true, icon = OniIcons.Link)
            }
            IconAction(OniIcons.More, onClick = { state.sheet = Sheet.AssetMenu(item) }, size = 40.dp, iconSize = 18.dp)
        },
    )
}

private fun libraryKindTag(kind: LibraryKind) = when (kind) {
    LibraryKind.Images -> "sprite"
    LibraryKind.Sounds -> "audio"
    LibraryKind.Scripts -> "script"
    LibraryKind.Animations -> "particles"
    LibraryKind.Materials -> "light"
}

fun libraryIcon(kind: LibraryKind) = when (kind) {
    LibraryKind.Images -> OniIcons.Image
    LibraryKind.Sounds -> OniIcons.Music
    LibraryKind.Scripts -> OniIcons.Code
    LibraryKind.Animations -> OniIcons.Film
    LibraryKind.Materials -> OniIcons.Droplet
}
