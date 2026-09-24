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
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.goni.ui.R

/**
 * Identidade do G.ONI: tinta azul-noite com uma brasa laranja como acento
 * (o "oni" da marca) e violeta como segunda cor. Superfícies sobem de tom
 * em degraus fixos (bg → s1 → s2 → s3 → s4) em vez de sombras, o que
 * mantém contraste legível por cima do viewport do jogo.
 */
@Immutable
data class OniColors(
    val bg: Color = Color(0xFF0A0B10),
    val s1: Color = Color(0xFF111319),
    val s2: Color = Color(0xFF181B23),
    val s3: Color = Color(0xFF21252F),
    val s4: Color = Color(0xFF2A2F3B),
    val line: Color = Color(0xFF272B36),
    val lineStrong: Color = Color(0xFF3A3F4D),
    val text: Color = Color(0xFFF2F1EE),
    val textMuted: Color = Color(0xFFA9ABB5),
    val textFaint: Color = Color(0xFF6E7180),
    val accent: Color = Color(0xFFFF7A45),
    val accentStrong: Color = Color(0xFFF2602A),
    val accentSoft: Color = Color(0xFF34190F),
    val onAccent: Color = Color(0xFF1A0A03),
    val violet: Color = Color(0xFF9B8CFF),
    val violetSoft: Color = Color(0xFF211C3D),
    val success: Color = Color(0xFF4FD197),
    val successSoft: Color = Color(0xFF12301F),
    val warning: Color = Color(0xFFF4BE5E),
    val warningSoft: Color = Color(0xFF33280F),
    val danger: Color = Color(0xFFFF5C7A),
    val dangerSoft: Color = Color(0xFF3A1420),
    val scrim: Color = Color(0x99050608),
    // Cor por tipo de entidade (ícones da cena e da biblioteca).
    val kindSprite: Color = Color(0xFF7FA6FF),
    val kindCamera: Color = Color(0xFF9B8CFF),
    val kindCharacter: Color = Color(0xFF4FD197),
    val kindLight: Color = Color(0xFFF4BE5E),
    val kindParticles: Color = Color(0xFFFF8FC8),
    val kindAudio: Color = Color(0xFF55D0E8),
    val kindScript: Color = Color(0xFFFF9A62),
    val kindText: Color = Color(0xFFE7D8B8),
    val kindTemplate: Color = Color(0xFFC6A2FF),
    val kindEmpty: Color = Color(0xFF8C90A0),
) {
    /** Gradiente da marca (logo, cartões de destaque). */
    val brand: List<Color> get() = listOf(accent, Color(0xFFE2487E), violet)
}

/** Space Grotesk nos títulos, Inter no texto, JetBrains Mono no código. */
object OniFonts {
    val display = FontFamily(
        Font(R.font.space_grotesk_semibold, FontWeight.SemiBold),
        Font(R.font.space_grotesk_bold, FontWeight.Bold),
    )
    val text = FontFamily(
        Font(R.font.inter_regular, FontWeight.Normal),
        Font(R.font.inter_medium, FontWeight.Medium),
        Font(R.font.inter_semibold, FontWeight.SemiBold),
    )
    val mono = FontFamily(
        Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
        Font(R.font.jetbrains_mono_semibold, FontWeight.SemiBold),
    )
}

@Immutable
data class OniType(
    val display: TextStyle = TextStyle(fontFamily = OniFonts.display, fontSize = 30.sp, lineHeight = 36.sp, fontWeight = FontWeight.Bold, letterSpacing = (-0.6).sp),
    val title: TextStyle = TextStyle(fontFamily = OniFonts.display, fontSize = 20.sp, lineHeight = 26.sp, fontWeight = FontWeight.Bold, letterSpacing = (-0.2).sp),
    val heading: TextStyle = TextStyle(fontFamily = OniFonts.display, fontSize = 16.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold),
    val body: TextStyle = TextStyle(fontFamily = OniFonts.text, fontSize = 15.sp, lineHeight = 21.sp, fontWeight = FontWeight.Normal),
    val bodyStrong: TextStyle = TextStyle(fontFamily = OniFonts.text, fontSize = 15.sp, lineHeight = 21.sp, fontWeight = FontWeight.SemiBold),
    val label: TextStyle = TextStyle(fontFamily = OniFonts.text, fontSize = 13.sp, lineHeight = 18.sp, fontWeight = FontWeight.Medium),
    val caption: TextStyle = TextStyle(fontFamily = OniFonts.text, fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.Normal),
    val overline: TextStyle = TextStyle(fontFamily = OniFonts.mono, fontSize = 11.sp, lineHeight = 14.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 1.2.sp),
    val mono: TextStyle = TextStyle(fontFamily = OniFonts.mono, fontSize = 14.sp, lineHeight = 20.sp),
    val code: TextStyle = TextStyle(fontFamily = OniFonts.mono, fontSize = 14.sp, lineHeight = 21.sp),
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
        secondary = c.violet,
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
        typography = Typography(
            bodyLarge = t.body,
            bodyMedium = t.body,
            bodySmall = t.caption,
            labelLarge = t.label,
            titleLarge = t.title,
            titleMedium = t.heading,
        ),
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
        "text" -> c.kindText
        "template" -> c.kindTemplate
        else -> c.kindEmpty
    }
}
