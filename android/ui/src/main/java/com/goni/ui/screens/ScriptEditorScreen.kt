package com.goni.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.input.OffsetMapping
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.input.TransformedText
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.goni.ui.components.IconAction
import com.goni.ui.components.OniButton
import com.goni.ui.components.OniIcon
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniIcons
import com.goni.ui.theme.OniShape

private val keywords = setOf(
    "f", "stop", "up", "if", "else", "repeat", "repair", "timeout", "link", "to", "emit", "give",
    "var", "add", "and", "or", "not", "true", "false",
)
private val types = setOf("int", "float", "bool", "string", "vec2", "vec3", "color", "entity", "transform")
private val builtins = setOf(
    "self", "delta", "action_down", "action_pressed", "action_released", "spawn", "despawn", "find",
    "move", "move_and_slide", "teleport", "abs", "min", "max", "clamp", "sin", "cos", "sqrt", "floor",
    "ceil", "str", "len", "i", "fl", "comp", "camera",
)

private class NiHighlight(
    val keyword: Color,
    val type: Color,
    val builtin: Color,
    val string: Color,
    val number: Color,
    val comment: Color,
    val event: Color,
) : VisualTransformation {
    override fun filter(text: AnnotatedString): TransformedText =
        TransformedText(highlight(text.text), OffsetMapping.Identity)

    fun highlight(src: String): AnnotatedString = buildAnnotatedString {
        append(src)
        var i = 0
        var afterUp = false
        while (i < src.length) {
            val ch = src[i]
            when {
                ch == '#' -> {
                    val end = src.indexOf('\n', i).let { if (it < 0) src.length else it }
                    addStyle(SpanStyle(color = comment, fontStyle = FontStyle.Italic), i, end)
                    i = end
                }
                ch == '"' -> {
                    var end = i + 1
                    while (end < src.length && src[end] != '"' && src[end] != '\n') {
                        if (src[end] == '\\') end++
                        end++
                    }
                    end = (end + 1).coerceAtMost(src.length)
                    addStyle(SpanStyle(color = string), i, end)
                    i = end
                }
                ch == '&' -> {
                    var end = i + 1
                    while (end < src.length && src[end].isLetter()) end++
                    addStyle(SpanStyle(color = type), i, end)
                    i = end
                }
                ch.isDigit() -> {
                    var end = i
                    while (end < src.length && (src[end].isDigit() || src[end] == '.')) end++
                    addStyle(SpanStyle(color = number), i, end)
                    i = end
                }
                ch.isLetter() || ch == '_' -> {
                    var end = i
                    while (end < src.length && (src[end].isLetterOrDigit() || src[end] == '_')) end++
                    val word = src.substring(i, end)
                    val prevDot = i > 0 && src[i - 1] == '.'
                    val color = when {
                        prevDot -> null
                        afterUp -> event
                        word in keywords -> keyword
                        word in types -> type
                        word in builtins -> builtin
                        else -> null
                    }
                    if (color != null) addStyle(SpanStyle(color = color), i, end)
                    afterUp = word == "up" || word == "emit"
                    i = end
                }
                else -> {
                    if (!ch.isWhitespace()) afterUp = false
                    i++
                }
            }
        }
    }
}

private val snippets = listOf(
    "    " to "⇥",
    "up update:\n    \nstop" to "up update",
    "if :\n    \nstop" to "if",
    "stop" to "stop",
    "var " to "var",
    "self()" to "self()",
    "delta()" to "delta()",
    "action_down(\"right\")" to "action_down",
    "action_pressed(\"jump\")" to "action_pressed",
    "move(0.0, 0.0)" to "move",
    "me.rigidbody.velocity.x" to "velocidade",
    "#" to "#",
)

@Composable
fun ScriptEditorScreen(name: String, state: UiState, actions: UiActions) {
    val c = Oni.colors
    var value by remember(name) { mutableStateOf(TextFieldValue(state.scriptText)) }
    val highlight = remember {
        NiHighlight(
            keyword = Color(0xFFB892FF),
            type = Color(0xFF55D0E8),
            builtin = Color(0xFF7C9CFF),
            string = Color(0xFF4FD197),
            number = Color(0xFFFFA86B),
            comment = Color(0xFF6C7488),
            event = Color(0xFFF4BE5E),
        )
    }
    fun update(v: TextFieldValue) {
        val changed = v.text != value.text
        value = v
        if (changed) actions.editScriptText(v.text)
    }
    fun insert(snippet: String) {
        val sel = value.selection
        val text = value.text.replaceRange(sel.min, sel.max, snippet)
        // Cursor na primeira linha vazia do snippet (dentro do bloco).
        val hole = snippet.indexOf("\n    \n").let { if (it >= 0) it + 5 else snippet.length }
        update(TextFieldValue(text, TextRange(sel.min + hole)))
    }
    fun jumpTo(line: Int) {
        var offset = 0
        var l = 1
        while (l < line && offset < value.text.length) {
            val nl = value.text.indexOf('\n', offset)
            if (nl < 0) break
            offset = nl + 1
            l++
        }
        value = value.copy(selection = TextRange(offset))
    }

    val lines = value.text.count { it == '\n' } + 1
    val errorLines = state.scriptDiags.map { it.line }.toSet()
    Column(Modifier.fillMaxSize().background(Color(0xFF0A0C10)).imePadding()) {
        // Barra superior.
        Row(
            Modifier.fillMaxWidth().background(c.s1).statusBarsPadding().padding(horizontal = 6.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconAction(OniIcons.Back, onClick = actions::closeScript)
            Column(Modifier.weight(1f).padding(start = 4.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(name, style = Oni.type.heading, color = c.text, maxLines = 1)
                    if (state.scriptDirty) {
                        Spacer(Modifier.width(6.dp))
                        Box(Modifier.size(6.dp).clip(CircleShape).background(c.warning))
                    }
                }
                val status = when {
                    state.scriptCompiled == true -> "Compila sem erros" to c.success
                    state.scriptCompiled == false -> "${state.scriptDiags.size} erro(s)" to c.danger
                    else -> "NI-Script" to c.textMuted
                }
                Text(status.first, style = Oni.type.caption, color = status.second)
            }
            IconAction(OniIcons.Zap, onClick = actions::compileScript)
            if (state.snapshot.selection != 0L) {
                IconAction(OniIcons.Link, onClick = actions::attachScript)
            }
            Spacer(Modifier.width(4.dp))
            OniButton("Salvar", onClick = actions::saveScript, compact = true, icon = OniIcons.Save)
        }
        // Código.
        Box(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())) {
            Row(Modifier.padding(vertical = 12.dp)) {
                Text(
                    buildAnnotatedString {
                        for (n in 1..lines) {
                            if (n > 1) append('\n')
                            val color = if (n in errorLines) c.danger else c.textFaint
                            pushStyle(SpanStyle(color = color))
                            append(n.toString())
                            pop()
                        }
                    },
                    style = Oni.type.code,
                    textAlign = TextAlign.End,
                    modifier = Modifier.width(if (lines >= 100) 52.dp else 40.dp).padding(end = 10.dp),
                )
                Box(Modifier.weight(1f).horizontalScroll(rememberScrollState())) {
                    BasicTextField(
                        value = value,
                        onValueChange = ::update,
                        textStyle = Oni.type.code.copy(color = Color(0xFFE3E7EF)),
                        cursorBrush = SolidColor(c.accent),
                        visualTransformation = highlight,
                        modifier = Modifier.widthIn(min = 600.dp).padding(end = 24.dp),
                    )
                }
            }
        }
        // Erros.
        if (state.scriptDiags.isNotEmpty()) {
            Column(
                Modifier.fillMaxWidth().heightIn(max = 150.dp).background(c.dangerSoft).verticalScroll(rememberScrollState()).padding(vertical = 4.dp),
            ) {
                for (d in state.scriptDiags) {
                    Row(
                        Modifier.fillMaxWidth().clickable { jumpTo(d.line) }.padding(horizontal = 14.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.Top,
                    ) {
                        OniIcon(OniIcons.Warning, tint = c.danger, size = 16.dp)
                        Spacer(Modifier.width(10.dp))
                        Text("${d.line}:${d.col}", style = Oni.type.mono, color = c.danger)
                        Spacer(Modifier.width(10.dp))
                        Text(d.message, style = Oni.type.caption, color = c.text)
                    }
                }
            }
        }
        // Atalhos.
        Row(
            Modifier.fillMaxWidth().background(c.s1).horizontalScroll(rememberScrollState()).navigationBarsPadding().padding(horizontal = 8.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            for ((snippet, label) in snippets) {
                Box(
                    Modifier
                        .height(36.dp)
                        .clip(OniShape.sm)
                        .background(c.s3)
                        .border(1.dp, c.line, OniShape.sm)
                        .clickable { insert(snippet) }
                        .padding(horizontal = 12.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(label, style = Oni.type.mono, color = c.text)
                }
            }
        }
    }
}
