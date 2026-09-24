package com.goni.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.goni.ui.theme.Oni
import com.goni.ui.theme.OniIcons
import com.goni.ui.theme.OniShape
import com.goni.ui.theme.kindColor

// --- ícones e botões -------------------------------------------------------------

@Composable
fun OniIcon(icon: ImageVector, tint: Color = Oni.colors.text, size: Dp = 20.dp, modifier: Modifier = Modifier) {
    androidx.compose.material3.Icon(icon, contentDescription = null, tint = tint, modifier = modifier.size(size))
}

enum class ButtonKind { Primary, Tonal, Ghost, Danger }

@Composable
fun IconAction(
    icon: ImageVector,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    kind: ButtonKind = ButtonKind.Ghost,
    enabled: Boolean = true,
    selected: Boolean = false,
    size: Dp = 44.dp,
    iconSize: Dp = 20.dp,
) {
    val c = Oni.colors
    val bg = when {
        selected -> c.accentSoft
        kind == ButtonKind.Primary -> c.accent
        kind == ButtonKind.Tonal -> c.s3
        kind == ButtonKind.Danger -> c.dangerSoft
        else -> Color.Transparent
    }
    val fg = when {
        !enabled -> c.textFaint
        selected -> c.accent
        kind == ButtonKind.Primary -> c.onAccent
        kind == ButtonKind.Danger -> c.danger
        else -> c.text
    }
    Box(
        modifier
            .size(size)
            .clip(OniShape.md)
            .background(if (enabled || bg == Color.Transparent) bg else bg.copy(alpha = 0.4f))
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        OniIcon(icon, tint = fg, size = iconSize)
    }
}

@Composable
fun OniButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    kind: ButtonKind = ButtonKind.Primary,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    compact: Boolean = false,
) {
    val c = Oni.colors
    val bg = when (kind) {
        ButtonKind.Primary -> c.accent
        ButtonKind.Tonal -> c.s3
        ButtonKind.Ghost -> Color.Transparent
        ButtonKind.Danger -> c.dangerSoft
    }
    val fg = when (kind) {
        ButtonKind.Primary -> c.onAccent
        ButtonKind.Danger -> c.danger
        ButtonKind.Ghost -> c.accent
        else -> c.text
    }
    Row(
        modifier
            .heightIn(min = if (compact) 38.dp else 48.dp)
            .clip(OniShape.md)
            .background(if (enabled || bg == Color.Transparent) bg else bg.copy(alpha = 0.35f))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = if (compact) 14.dp else 18.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        if (icon != null) {
            OniIcon(icon, tint = if (enabled) fg else c.textFaint, size = 18.dp)
            Spacer(Modifier.width(8.dp))
        }
        Text(text, style = Oni.type.bodyStrong, color = if (enabled) fg else c.textFaint, maxLines = 1)
    }
}

@Composable
fun OniChip(
    text: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
) {
    val c = Oni.colors
    Row(
        modifier
            .height(36.dp)
            .clip(OniShape.pill)
            .background(if (selected) c.accentSoft else c.s2)
            .border(1.dp, if (selected) c.accent.copy(alpha = 0.55f) else c.line, OniShape.pill)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            OniIcon(icon, tint = if (selected) c.accent else c.textMuted, size = 16.dp)
            Spacer(Modifier.width(6.dp))
        }
        Text(text, style = Oni.type.label, color = if (selected) c.text else c.textMuted, maxLines = 1)
    }
}

/** Controle segmentado: opções mutuamente exclusivas numa pílula. */
@Composable
fun Segmented(
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val c = Oni.colors
    Row(
        modifier
            .clip(OniShape.md)
            .background(c.s2)
            .padding(3.dp),
    ) {
        options.forEachIndexed { i, label ->
            val on = i == selected
            Box(
                Modifier
                    .weight(1f)
                    .height(36.dp)
                    .clip(OniShape.sm)
                    .background(if (on) c.s4 else Color.Transparent)
                    .clickable { onSelect(i) },
                contentAlignment = Alignment.Center,
            ) {
                Text(label, style = Oni.type.label, color = if (on) c.text else c.textMuted, maxLines = 1)
            }
        }
    }
}

// --- superfícies ------------------------------------------------------------------

@Composable
fun OniCard(
    modifier: Modifier = Modifier,
    padding: PaddingValues = PaddingValues(16.dp),
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val c = Oni.colors
    Column(
        modifier
            .clip(OniShape.lg)
            .background(c.s1)
            .border(1.dp, c.line, OniShape.lg)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(padding),
        content = content,
    )
}

@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier, trailing: @Composable RowScope.() -> Unit = {}) {
    Row(modifier.fillMaxWidth().padding(top = 8.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(text.uppercase(), style = Oni.type.overline, color = Oni.colors.textFaint, modifier = Modifier.weight(1f))
        trailing()
    }
}

/** Quadrado arredondado com o ícone do tipo de entidade/asset. */
@Composable
fun KindBadge(kind: String, size: Dp = 32.dp, icon: ImageVector = OniIcons.forKind(kind)) {
    val tint = kindColor(kind)
    Box(
        Modifier.size(size).clip(OniShape.sm).background(tint.copy(alpha = 0.16f)),
        contentAlignment = Alignment.Center,
    ) {
        OniIcon(icon, tint = tint, size = size * 0.55f)
    }
}

@Composable
fun ListRow(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable RowScope.() -> Unit)? = null,
    selected: Boolean = false,
    onClick: (() -> Unit)? = null,
) {
    val c = Oni.colors
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 52.dp)
            .clip(OniShape.md)
            .background(if (selected) c.accentSoft else Color.Transparent)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(12.dp))
        }
        Column(Modifier.weight(1f)) {
            Text(title, style = Oni.type.bodyStrong, color = c.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (subtitle != null) {
                Text(subtitle, style = Oni.type.caption, color = c.textMuted, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        if (trailing != null) {
            Row(verticalAlignment = Alignment.CenterVertically, content = trailing)
        }
    }
}

@Composable
fun EmptyState(
    icon: ImageVector,
    title: String,
    message: String,
    modifier: Modifier = Modifier,
    action: (@Composable () -> Unit)? = null,
) {
    val c = Oni.colors
    Column(
        modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 28.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(Modifier.size(56.dp).clip(OniShape.lg).background(c.s2), contentAlignment = Alignment.Center) {
            OniIcon(icon, tint = c.textMuted, size = 26.dp)
        }
        Spacer(Modifier.height(14.dp))
        Text(title, style = Oni.type.heading, color = c.text)
        Spacer(Modifier.height(4.dp))
        Text(message, style = Oni.type.caption, color = c.textMuted, textAlign = androidx.compose.ui.text.style.TextAlign.Center)
        if (action != null) {
            Spacer(Modifier.height(16.dp))
            action()
        }
    }
}

// --- entradas ---------------------------------------------------------------------

/**
 * Campo de texto que só confirma no "Concluído" ou ao perder o foco.
 * Enquanto está focado, atualizações vindas de fora não sobrescrevem o
 * que o usuário digita.
 */
@Composable
fun OniField(
    value: String,
    onCommit: (String) -> Unit,
    modifier: Modifier = Modifier,
    placeholder: String = "",
    prefix: String? = null,
    numeric: Boolean = false,
    mono: Boolean = numeric,
    singleLine: Boolean = true,
    minHeight: Dp = 44.dp,
    textStyle: TextStyle? = null,
    onChange: ((String) -> Unit)? = null,
) {
    val c = Oni.colors
    var focused by remember { mutableStateOf(false) }
    var text by remember { mutableStateOf(value) }
    LaunchedEffect(value) {
        if (!focused) text = value
    }
    val focus = LocalFocusManager.current
    val style = (textStyle ?: if (mono) Oni.type.mono else Oni.type.body).copy(color = c.text)
    BasicTextField(
        value = text,
        onValueChange = {
            text = it
            onChange?.invoke(it)
        },
        singleLine = singleLine,
        textStyle = style,
        cursorBrush = SolidColor(c.accent),
        keyboardOptions = KeyboardOptions(
            keyboardType = if (numeric) KeyboardType.Decimal else KeyboardType.Text,
            imeAction = if (singleLine) ImeAction.Done else ImeAction.Default,
        ),
        keyboardActions = KeyboardActions(onDone = {
            focus.clearFocus()
        }),
        modifier = modifier.onFocusChanged {
            if (focused && !it.isFocused && text != value) onCommit(text)
            focused = it.isFocused
        },
        decorationBox = { inner ->
            Row(
                Modifier
                    .heightIn(min = minHeight)
                    .clip(OniShape.sm)
                    .background(c.s3)
                    .border(1.dp, if (focused) c.accent else Color.Transparent, OniShape.sm)
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                verticalAlignment = if (singleLine) Alignment.CenterVertically else Alignment.Top,
            ) {
                if (prefix != null) {
                    Text(prefix, style = Oni.type.label, color = c.textFaint)
                    Spacer(Modifier.width(8.dp))
                }
                Box(Modifier.weight(1f)) {
                    if (text.isEmpty() && placeholder.isNotEmpty()) {
                        Text(placeholder, style = style.copy(color = c.textFaint), maxLines = 1)
                    }
                    inner()
                }
            }
        },
    )
}

@Composable
fun OniSwitch(checked: Boolean, onChange: (Boolean) -> Unit) {
    val c = Oni.colors
    Switch(
        checked = checked,
        onCheckedChange = onChange,
        colors = SwitchDefaults.colors(
            checkedThumbColor = c.onAccent,
            checkedTrackColor = c.accent,
            checkedBorderColor = c.accent,
            uncheckedThumbColor = c.textMuted,
            uncheckedTrackColor = c.s3,
            uncheckedBorderColor = c.lineStrong,
        ),
    )
}

@Composable
fun ColorSwatch(color: Color, size: Dp = 28.dp, onClick: (() -> Unit)? = null) {
    Box(
        Modifier
            .size(size)
            .clip(OniShape.sm)
            .background(color)
            .border(1.dp, Oni.colors.lineStrong, OniShape.sm)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier),
    )
}

// --- camadas modais ---------------------------------------------------------------

/**
 * Folha inferior modal desenhada na própria árvore Compose (sem janela
 * extra): scrim tocável + cartão que sobe da borda inferior.
 */
@Composable
fun BoxScope.ModalSheet(
    visible: Boolean,
    onDismiss: () -> Unit,
    title: String? = null,
    maxHeightFraction: Float = 0.88f,
    content: @Composable ColumnScope.() -> Unit,
) {
    val c = Oni.colors
    AnimatedVisibility(visible, enter = fadeIn(tween(160)), exit = fadeOut(tween(140)), modifier = Modifier.matchParentSize()) {
        Box(
            Modifier
                .fillMaxSize()
                .background(c.scrim)
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onDismiss),
        )
    }
    AnimatedVisibility(
        visible,
        enter = slideInVertically(tween(220)) { it },
        exit = slideOutVertically(tween(180)) { it },
        modifier = Modifier.align(Alignment.BottomCenter),
    ) {
        androidx.compose.foundation.layout.BoxWithConstraints {
            Column(
                Modifier
                    .fillMaxWidth()
                    .heightIn(max = maxHeight * maxHeightFraction)
                    .clip(OniShape.sheet)
                    .background(c.s1)
                    .border(1.dp, c.line, OniShape.sheet)
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
                    .navigationBarsPadding()
                    .padding(bottom = 12.dp),
            ) {
                Box(Modifier.fillMaxWidth().padding(top = 10.dp), contentAlignment = Alignment.Center) {
                    Box(Modifier.width(36.dp).height(4.dp).clip(OniShape.pill).background(c.lineStrong))
                }
                if (title != null) {
                    Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(title, style = Oni.type.title, color = c.text, modifier = Modifier.weight(1f))
                        IconAction(OniIcons.Close, onDismiss)
                    }
                }
                content()
            }
        }
    }
}

/** Diálogo central curto (confirmar, renomear). */
@Composable
fun BoxScope.ModalDialog(
    visible: Boolean,
    onDismiss: () -> Unit,
    title: String,
    content: @Composable ColumnScope.() -> Unit,
) {
    val c = Oni.colors
    AnimatedVisibility(visible, enter = fadeIn(tween(140)), exit = fadeOut(tween(120)), modifier = Modifier.matchParentSize()) {
        Box(
            Modifier
                .fillMaxSize()
                .background(c.scrim)
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onDismiss),
            contentAlignment = Alignment.Center,
        ) {
            Column(
                Modifier
                    .padding(24.dp)
                    .widthIn(max = 420.dp)
                    .fillMaxWidth()
                    .clip(OniShape.xl)
                    .background(c.s2)
                    .border(1.dp, c.line, OniShape.xl)
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
                    .padding(20.dp),
            ) {
                Text(title, style = Oni.type.title, color = c.text)
                Spacer(Modifier.height(14.dp))
                content()
            }
        }
    }
}

enum class ToastKind { Info, Success, Error }

data class ToastMessage(val text: String, val kind: ToastKind = ToastKind.Info, val id: Long = System.nanoTime())

@Composable
fun BoxScope.ToastHost(message: ToastMessage?, modifier: Modifier = Modifier) {
    val c = Oni.colors
    AnimatedVisibility(
        message != null,
        enter = slideInVertically(tween(200)) { -it } + fadeIn(),
        exit = slideOutVertically(tween(160)) { -it } + fadeOut(),
        modifier = modifier.align(Alignment.TopCenter),
    ) {
        val m = message ?: return@AnimatedVisibility
        val (tint, icon) = when (m.kind) {
            ToastKind.Success -> c.success to OniIcons.Check
            ToastKind.Error -> c.danger to OniIcons.Warning
            ToastKind.Info -> c.accent to OniIcons.Info
        }
        Row(
            Modifier
                .statusBarsPadding()
                .padding(start = 16.dp, end = 16.dp, top = 64.dp)
                .widthIn(max = 480.dp)
                .clip(OniShape.pill)
                .background(c.s3)
                .border(1.dp, c.lineStrong, OniShape.pill)
                .padding(horizontal = 16.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.size(22.dp).clip(CircleShape).background(tint.copy(alpha = 0.18f)), contentAlignment = Alignment.Center) {
                OniIcon(icon, tint = tint, size = 14.dp)
            }
            Spacer(Modifier.width(10.dp))
            Text(m.text, style = Oni.type.label, color = c.text, maxLines = 3)
        }
    }
}

/** Menu de ações em folha (substitui menus de contexto). */
data class MenuItem(val label: String, val icon: ImageVector, val danger: Boolean = false, val onClick: () -> Unit)

@Composable
fun ColumnScope.MenuList(items: List<MenuItem>, onDone: () -> Unit) {
    val c = Oni.colors
    Column(Modifier.padding(horizontal = 12.dp, vertical = 4.dp)) {
        for (item in items) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .height(52.dp)
                    .clip(OniShape.md)
                    .clickable { onDone(); item.onClick() }
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                OniIcon(item.icon, tint = if (item.danger) c.danger else c.textMuted, size = 20.dp)
                Spacer(Modifier.width(14.dp))
                Text(item.label, style = Oni.type.body, color = if (item.danger) c.danger else c.text)
            }
        }
    }
}
