package com.goni.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Paleta do G.ONI: carvão azulado com um acento índigo. Superfícies sobem
 * de tom em degraus fixos (bg → s1 → s2 → s3) em vez de usar sombras, o
 * que mantém contraste legível sobre o viewport do jogo.
 */
@Immutable
data class OniColors(
    val bg: Color = Color(0xFF0B0D12),
    val s1: Color = Color(0xFF12151C),
    val s2: Color = Color(0xFF191D26),
    val s3: Color = Color(0xFF222733),
    val s4: Color = Color(0xFF2B3140),
    val line: Color = Color(0xFF2A303D),
    val lineStrong: Color = Color(0xFF3A4152),
    val text: Color = Color(0xFFECEFF5),
    val textMuted: Color = Color(0xFFA3ABBB),
    val textFaint: Color = Color(0xFF6C7488),
    val accent: Color = Color(0xFF7C9CFF),
    val accentStrong: Color = Color(0xFF5B7CF5),
    val accentSoft: Color = Color(0xFF1C2544),
    val onAccent: Color = Color(0xFF0A0E1C),
    val success: Color = Color(0xFF4FD197),
    val successSoft: Color = Color(0xFF12301F),
    val warning: Color = Color(0xFFF4BE5E),
    val warningSoft: Color = Color(0xFF33280F),
    val danger: Color = Color(0xFFFF7470),
    val dangerSoft: Color = Color(0xFF3A1616),
    val scrim: Color = Color(0x99050608),
    // Cor por tipo de entidade (ícones da cena e da biblioteca).
    val kindSprite: Color = Color(0xFF7C9CFF),
    val kindCamera: Color = Color(0xFFB892FF),
    val kindCharacter: Color = Color(0xFF4FD197),
    val kindLight: Color = Color(0xFFF4BE5E),
    val kindParticles: Color = Color(0xFFFF8FC8),
    val kindAudio: Color = Color(0xFF55D0E8),
    val kindScript: Color = Color(0xFFFFA86B),
    val kindEmpty: Color = Color(0xFF8E97AA),
)

@Immutable
data class OniType(
    val display: TextStyle = TextStyle(fontSize = 28.sp, lineHeight = 34.sp, fontWeight = FontWeight.Bold, letterSpacing = (-0.5).sp),
    val title: TextStyle = TextStyle(fontSize = 20.sp, lineHeight = 26.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.2).sp),
    val heading: TextStyle = TextStyle(fontSize = 16.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold),
    val body: TextStyle = TextStyle(fontSize = 15.sp, lineHeight = 21.sp, fontWeight = FontWeight.Normal),
    val bodyStrong: TextStyle = TextStyle(fontSize = 15.sp, lineHeight = 21.sp, fontWeight = FontWeight.Medium),
    val label: TextStyle = TextStyle(fontSize = 13.sp, lineHeight = 18.sp, fontWeight = FontWeight.Medium),
    val caption: TextStyle = TextStyle(fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.Normal),
    val overline: TextStyle = TextStyle(fontSize = 11.sp, lineHeight = 14.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.8.sp),
    val mono: TextStyle = TextStyle(fontSize = 14.sp, lineHeight = 20.sp, fontFamily = FontFamily.Monospace),
    val code: TextStyle = TextStyle(fontSize = 14.sp, lineHeight = 21.sp, fontFamily = FontFamily.Monospace),
)

object OniShape {
    val xs = RoundedCornerShape(6.dp)
    val sm = RoundedCornerShape(10.dp)
    val md = RoundedCornerShape(14.dp)
    val lg = RoundedCornerShape(20.dp)
    val xl = RoundedCornerShape(28.dp)
    val pill = RoundedCornerShape(percent = 50)
    val sheet = RoundedCornerShape(topStart = 24.dp, topEnd = 24.dp)
}

val LocalOniColors = staticCompositionLocalOf { OniColors() }
val LocalOniType = staticCompositionLocalOf { OniType() }

object Oni {
    val colors: OniColors
        @Composable get() = LocalOniColors.current
    val type: OniType
        @Composable get() = LocalOniType.current
}

@Composable
fun OniTheme(content: @Composable () -> Unit) {
    val c = OniColors()
    val t = OniType()
    val scheme = darkColorScheme(
        primary = c.accent,
        onPrimary = c.onAccent,
        primaryContainer = c.accentSoft,
        onPrimaryContainer = c.text,
        secondary = c.accent,
        background = c.bg,
        onBackground = c.text,
        surface = c.s1,
        onSurface = c.text,
        surfaceVariant = c.s2,
        onSurfaceVariant = c.textMuted,
        surfaceContainer = c.s2,
        surfaceContainerHigh = c.s3,
        surfaceContainerHighest = c.s4,
        outline = c.line,
        outlineVariant = c.line,
        error = c.danger,
        scrim = c.scrim,
    )
    MaterialTheme(
        colorScheme = scheme,
        typography = Typography(bodyLarge = t.body, bodyMedium = t.body, labelLarge = t.label),
        shapes = Shapes(small = OniShape.sm, medium = OniShape.md, large = OniShape.lg),
    ) {
        androidx.compose.runtime.CompositionLocalProvider(
            LocalOniColors provides c,
            LocalOniType provides t,
            content = content,
        )
    }
}

/** Cor do tipo de entidade vindo do protocolo ("sprite", "camera", …). */
@Composable
fun kindColor(kind: String): Color {
    val c = Oni.colors
    return when (kind) {
        "sprite" -> c.kindSprite
        "camera" -> c.kindCamera
        "character" -> c.kindCharacter
        "light" -> c.kindLight
        "particles" -> c.kindParticles
        "audio" -> c.kindAudio
        "script" -> c.kindScript
        else -> c.kindEmpty
    }
}
