package com.goni.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.goni.ui.components.MenuItem
import com.goni.ui.components.MenuList
import com.goni.ui.components.EmptyState
import com.goni.ui.components.IconAction
import com.goni.ui.components.OniButton
import com.goni.ui.components.OniIcon
import com.goni.ui.components.drawExampleArt
import com.goni.ui.components.exampleBackground
import com.goni.ui.model.ExampleProject
import com.goni.ui.model.ProjectCard
import com.goni.ui.model.Sheet
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniIcons
import com.goni.ui.theme.OniShape

@Composable
fun HomeScreen(state: UiState, actions: UiActions) {
    val c = Oni.colors
    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = 160.dp),
        modifier = Modifier.fillMaxSize().background(c.bg),
        contentPadding = PaddingValues(start = 20.dp, end = 20.dp, bottom = 32.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item(span = { GridItemSpan(maxLineSpan) }) {
            Column(Modifier.statusBarsPadding().padding(top = 12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    LogoMark(34)
                    Spacer(Modifier.width(10.dp))
                    Text("G.ONI", style = Oni.type.heading.copy(letterSpacing = 2.sp), color = c.text)
                    Spacer(Modifier.weight(1f))
                    IconAction(OniIcons.Download, onClick = actions::importProject)
                }
                Spacer(Modifier.height(26.dp))
                Text("Seus jogos", style = Oni.type.display, color = c.text)
                Spacer(Modifier.height(6.dp))
                Text(
                    "Crie, teste e exporte — tudo no celular.",
                    style = Oni.type.body,
                    color = c.textMuted,
                )
                Spacer(Modifier.height(22.dp))
                NewGameCard { state.sheet = Sheet.NewProject() }
                SectionOverline("EXEMPLOS", Modifier.padding(top = 26.dp, bottom = 10.dp))
            }
        }
        item(span = { GridItemSpan(maxLineSpan) }) {
            LazyRow(
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                contentPadding = PaddingValues(end = 4.dp),
            ) {
                items(state.examples, key = { it.id }) { example ->
                    ExampleCard(example) { state.sheet = Sheet.NewProject(example.id) }
                }
            }
        }
        item(span = { GridItemSpan(maxLineSpan) }) {
            SectionOverline("RECENTES", Modifier.padding(top = 14.dp))
        }
        if (state.projects.isEmpty()) {
            item(span = { GridItemSpan(maxLineSpan) }) {
                EmptyState(
                    icon = OniIcons.Gamepad,
                    title = "Nenhum jogo ainda",
                    message = "Abra um exemplo acima: o Voo já vem pronto para jogar com um toque.",
                )
            }
        }
        items(state.projects, key = { it.folder }) { project ->
            ProjectTile(
                project,
                onOpen = { actions.openProject(project.folder) },
                onMenu = { state.sheet = Sheet.ProjectMenu(project) },
            )
        }
    }
}

@Composable
private fun SectionOverline(text: String, modifier: Modifier = Modifier) {
    val c = Oni.colors
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(6.dp).clip(OniShape.pill).background(c.accent))
        Spacer(Modifier.width(8.dp))
        Text(text, style = Oni.type.overline, color = c.textFaint)
    }
}

@Composable
private fun ExampleCard(example: ExampleProject, onClick: () -> Unit) {
    val c = Oni.colors
    Column(
        Modifier
            .width(212.dp)
            .clip(OniShape.lg)
            .background(c.s1)
            .border(1.dp, c.line, OniShape.lg)
            .clickable(onClick = onClick),
    ) {
        Box(Modifier.fillMaxWidth().height(112.dp).clipToBounds().background(exampleBackground(example.id))) {
            Canvas(Modifier.fillMaxSize()) { drawExampleArt(example.id) }
        }
        Column(Modifier.padding(start = 12.dp, end = 12.dp, top = 10.dp, bottom = 12.dp)) {
            Text(example.title, style = Oni.type.heading, color = c.text, maxLines = 1)
            Spacer(Modifier.height(2.dp))
            Text(
                example.description,
                style = Oni.type.caption,
                color = c.textMuted,
                maxLines = 2,
                minLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
fun LogoMark(sizeDp: Int) {
    val c = Oni.colors
    Box(
        Modifier
            .size(sizeDp.dp)
            .clip(OniShape.sm)
            .background(Brush.linearGradient(c.brand)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            "G",
            color = c.onAccent,
            style = Oni.type.display.copy(fontSize = (sizeDp * 0.56f).sp, lineHeight = (sizeDp * 0.6f).sp),
        )
    }
}

@Composable
private fun NewGameCard(onClick: () -> Unit) {
    val c = Oni.colors
    Box(
        Modifier
            .fillMaxWidth()
            .height(136.dp)
            .clip(OniShape.lg)
            .background(Brush.linearGradient(listOf(Color(0xFF3A1A12), Color(0xFF2A1640), Color(0xFF16163A))))
            .border(1.dp, c.accent.copy(alpha = 0.35f), OniShape.lg)
            .clickable(onClick = onClick),
    ) {
        Canvas(Modifier.fillMaxSize()) {
            // Grade de pontos: a "folha em branco" do editor.
            val step = 22.dp.toPx()
            var y = step / 2
            while (y < size.height) {
                var x = size.width * 0.5f
                while (x < size.width) {
                    drawCircle(Color.White.copy(alpha = 0.08f), 1.6f, Offset(x, y))
                    x += step
                }
                y += step
            }
            drawPlatformScene(this, alpha = 0.95f)
        }
        Column(Modifier.padding(20.dp).align(Alignment.CenterStart)) {
            Box(
                Modifier.size(40.dp).clip(OniShape.md).background(Brush.linearGradient(c.brand)),
                contentAlignment = Alignment.Center,
            ) { OniIcon(OniIcons.Plus, tint = c.onAccent, size = 22.dp) }
            Spacer(Modifier.height(12.dp))
            Text("Novo jogo", style = Oni.type.title, color = Color.White)
            Text("Vazio ou a partir de um exemplo", style = Oni.type.caption, color = Color.White.copy(alpha = 0.7f))
        }
    }
}

/** Ilustração do exemplo Plataforma (capa e cartão "Novo jogo"). */
fun drawPlatformScene(scope: DrawScope, alpha: Float = 1f) = with(scope) {
    val w = size.width
    val h = size.height
    val ground = Color(0xFF6B7690).copy(alpha = 0.55f * alpha)
    drawRoundRect(ground, Offset(w * 0.6f, h * 0.76f), Size(w * 0.36f, h * 0.12f), CornerRadius(8f))
    drawRoundRect(ground, Offset(w * 0.78f, h * 0.44f), Size(w * 0.16f, h * 0.07f), CornerRadius(8f))
    drawRoundRect(Color(0xFFFF7A45).copy(alpha = alpha), Offset(w * 0.66f, h * 0.54f), Size(h * 0.2f, h * 0.2f), CornerRadius(10f))
    drawCircle(Color(0xFFF4BE5E).copy(alpha = 0.8f * alpha), radius = h * 0.06f, center = Offset(w * 0.88f, h * 0.22f))
}

@Composable
private fun ProjectTile(project: ProjectCard, onOpen: () -> Unit, onMenu: () -> Unit) {
    val c = Oni.colors
    Column(
        Modifier
            .clip(OniShape.lg)
            .background(c.s1)
            .border(1.dp, c.line, OniShape.lg)
            .clickable(onClick = onOpen),
    ) {
        Box(Modifier.fillMaxWidth().aspectRatio(1.35f).background(c.s2)) {
            if (project.thumbnail != null) {
                Image(project.thumbnail, null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
            } else {
                GeneratedCover(project.name)
            }
        }
        Row(Modifier.padding(start = 12.dp, top = 10.dp, bottom = 10.dp, end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(project.name, style = Oni.type.bodyStrong, color = c.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(project.edited, style = Oni.type.caption, color = c.textMuted, maxLines = 1)
            }
            IconAction(OniIcons.More, onClick = onMenu, size = 36.dp, iconSize = 18.dp)
        }
    }
}

/** Capa gerada a partir do nome (cor estável por projeto). */
@Composable
private fun GeneratedCover(name: String) {
    val palettes = listOf(
        listOf(Color(0xFF2B3C7A), Color(0xFF5B3F8C)),
        listOf(Color(0xFF1D5C57), Color(0xFF2C3F73)),
        listOf(Color(0xFF6B3A4E), Color(0xFF3C2F6B)),
        listOf(Color(0xFF6A5222), Color(0xFF6B3A36)),
        listOf(Color(0xFF214E6B), Color(0xFF1F6B55)),
    )
    val colors = palettes[(name.hashCode() and 0x7fffffff) % palettes.size]
    Box(Modifier.fillMaxSize().background(Brush.linearGradient(colors)), contentAlignment = Alignment.Center) {
        Canvas(Modifier.fillMaxSize()) { drawPlatformScene(this, alpha = 0.35f) }
        Text(
            name.trim().take(1).uppercase().ifEmpty { "?" },
            color = Color.White.copy(alpha = 0.92f),
            fontSize = 40.sp,
            fontWeight = FontWeight.Black,
        )
    }
}

@Composable
fun NewProjectSheetContent(
    examples: List<ExampleProject>,
    initialTemplate: String,
    onCreate: (String, String) -> Unit,
) {
    val c = Oni.colors
    var name by remember { mutableStateOf("") }
    var template by remember { mutableStateOf(initialTemplate) }
    val picked = examples.firstOrNull { it.id == template }
    Column(Modifier.padding(horizontal = 20.dp, vertical = 8.dp)) {
        Text("Nome", style = Oni.type.label, color = c.textMuted)
        Spacer(Modifier.height(6.dp))
        com.goni.ui.components.OniField(
            value = name,
            onCommit = { name = it },
            onChange = { name = it },
            placeholder = picked?.title ?: "Meu jogo",
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(18.dp))
        Text("Começar de", style = Oni.type.label, color = c.textMuted)
        Spacer(Modifier.height(8.dp))
        for (row in examples.chunked(2)) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                for (ex in row) {
                    TemplateCard(
                        id = ex.id,
                        title = ex.title,
                        selected = template == ex.id,
                        modifier = Modifier.weight(1f),
                        onClick = { template = ex.id },
                    )
                }
                if (row.size == 1) Spacer(Modifier.weight(1f))
            }
            Spacer(Modifier.height(10.dp))
        }
        Text(
            picked?.description.orEmpty(),
            style = Oni.type.caption,
            color = c.textMuted,
            minLines = 2,
        )
        Spacer(Modifier.height(16.dp))
        OniButton(
            "Criar jogo",
            onClick = { onCreate(name.trim().ifEmpty { picked?.title ?: "Meu jogo" }, template) },
            icon = OniIcons.Plus,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
private fun TemplateCard(
    id: String,
    title: String,
    selected: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    val c = Oni.colors
    Column(
        modifier
            .clip(OniShape.lg)
            .background(if (selected) c.accentSoft else c.s2)
            .border(if (selected) 2.dp else 1.dp, if (selected) c.accent else c.line, OniShape.lg)
            .clickable(onClick = onClick)
            .padding(8.dp),
    ) {
        Box(Modifier.fillMaxWidth().height(70.dp).clip(OniShape.md).background(exampleBackground(id))) {
            Canvas(Modifier.fillMaxSize()) { drawExampleArt(id) }
        }
        Row(Modifier.padding(start = 4.dp, top = 8.dp, bottom = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(title, style = Oni.type.bodyStrong, color = c.text, maxLines = 1, modifier = Modifier.weight(1f))
            if (selected) OniIcon(OniIcons.Check, tint = c.accent, size = 16.dp)
        }
    }
}

@Composable
fun androidx.compose.foundation.layout.ColumnScope.ProjectMenuContent(
    project: ProjectCard,
    actions: UiActions,
    state: UiState,
) {
    val items = listOf(
        MenuItem("Abrir no editor", OniIcons.Edit) { actions.openProject(project.folder) },
        MenuItem("Jogar", OniIcons.Play) { actions.playProject(project.folder) },
        MenuItem("Renomear", OniIcons.Edit) {
            state.sheet = Sheet.Rename("Renomear jogo", project.name) { actions.renameProject(project.folder, it) }
        },
        MenuItem("Exportar (.goni)", OniIcons.Share) { actions.exportProject(project.folder) },
        MenuItem("Excluir", OniIcons.Trash, danger = true) {
            state.sheet = Sheet.Confirm(
                "Excluir \"${project.name}\"?",
                "O jogo e todos os arquivos dele serão apagados deste aparelho.",
                "Excluir",
            ) { actions.deleteProject(project.folder) }
        },
    )
    MenuList(items) { state.sheet = null }
}
