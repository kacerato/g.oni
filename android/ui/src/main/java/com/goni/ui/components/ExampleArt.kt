package com.goni.ui.components

import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate

/** Fundo de cada exemplo do catálogo (mesmo tom do jogo que ele cria). */
fun exampleBackground(id: String): Brush = when (id) {
    "platformer" -> Brush.linearGradient(listOf(Color(0xFF1B2A55), Color(0xFF2C1F4F)))
    "flappy" -> Brush.verticalGradient(listOf(Color(0xFF3F8FD8), Color(0xFF7CC4F0)))
    "boxes" -> Brush.linearGradient(listOf(Color(0xFF3A1E2E), Color(0xFF4A2A1A)))
    else -> Brush.linearGradient(listOf(Color(0xFF191B23), Color(0xFF21252F)))
}

/** Ilustração do exemplo: dá para reconhecer o jogo sem ler o nome. */
fun DrawScope.drawExampleArt(id: String, alpha: Float = 1f) {
    when (id) {
        "platformer" -> drawPlatformArt(alpha)
        "flappy" -> drawFlappyArt(alpha)
        "boxes" -> drawBoxesArt(alpha)
        else -> drawEmptyArt(alpha)
    }
}

private fun DrawScope.drawPlatformArt(alpha: Float) {
    val w = size.width
    val h = size.height
    val ground = Color(0xFF6B7690).copy(alpha = 0.7f * alpha)
    drawRoundRect(ground, Offset(-8f, h * 0.8f), Size(w + 16f, h * 0.3f), CornerRadius(6f))
    drawRoundRect(ground, Offset(w * 0.56f, h * 0.5f), Size(w * 0.26f, h * 0.08f), CornerRadius(6f))
    drawRoundRect(ground, Offset(w * 0.14f, h * 0.36f), Size(w * 0.18f, h * 0.07f), CornerRadius(6f))
    val hero = Offset(w * 0.3f, h * 0.6f)
    drawRoundRect(Color(0xFFFF7A45).copy(alpha = alpha), hero, Size(h * 0.2f, h * 0.2f), CornerRadius(8f))
    drawCircle(Color(0xFF1A0A03).copy(alpha = alpha), h * 0.022f, hero + Offset(h * 0.13f, h * 0.07f))
    drawCircle(Color(0xFFF4BE5E).copy(alpha = 0.85f * alpha), h * 0.05f, Offset(w * 0.68f, h * 0.36f))
    drawCircle(Color(0xFFF4BE5E).copy(alpha = 0.85f * alpha), h * 0.05f, Offset(w * 0.8f, h * 0.36f))
}

private fun DrawScope.drawFlappyArt(alpha: Float) {
    val w = size.width
    val h = size.height
    val pipe = Color(0xFF3DBE6A).copy(alpha = alpha)
    val lip = Color(0xFF2E9E55).copy(alpha = alpha)
    fun pipes(x: Float, gapY: Float) {
        val pw = w * 0.14f
        val gap = h * 0.34f
        drawRect(pipe, Offset(x, -4f), Size(pw, gapY - gap / 2 + 4f))
        drawRect(lip, Offset(x - pw * 0.1f, gapY - gap / 2 - h * 0.06f), Size(pw * 1.2f, h * 0.06f))
        drawRect(pipe, Offset(x, gapY + gap / 2), Size(pw, h))
        drawRect(lip, Offset(x - pw * 0.1f, gapY + gap / 2), Size(pw * 1.2f, h * 0.06f))
    }
    drawCircle(Color.White.copy(alpha = 0.35f * alpha), h * 0.1f, Offset(w * 0.18f, h * 0.2f))
    drawCircle(Color.White.copy(alpha = 0.35f * alpha), h * 0.08f, Offset(w * 0.27f, h * 0.22f))
    pipes(w * 0.5f, h * 0.44f)
    pipes(w * 0.88f, h * 0.56f)
    drawRect(Color(0xFFD9B26A).copy(alpha = alpha), Offset(0f, h * 0.88f), Size(w, h * 0.12f))
    val bird = Offset(w * 0.25f, h * 0.5f)
    drawCircle(Color(0xFFFFD34D).copy(alpha = alpha), h * 0.085f, bird)
    drawCircle(Color.White.copy(alpha = alpha), h * 0.03f, bird + Offset(h * 0.035f, -h * 0.025f))
    drawCircle(Color.Black.copy(alpha = alpha), h * 0.013f, bird + Offset(h * 0.045f, -h * 0.025f))
    drawRoundRect(Color(0xFFFF7A45).copy(alpha = alpha), bird + Offset(h * 0.06f, 0f), Size(h * 0.07f, h * 0.035f), CornerRadius(4f))
}

private fun DrawScope.drawBoxesArt(alpha: Float) {
    val w = size.width
    val h = size.height
    val colors = listOf(Color(0xFFFF7A45), Color(0xFF9B8CFF), Color(0xFF4FD197), Color(0xFFF4BE5E), Color(0xFF55D0E8))
    drawRect(Color(0xFF6B5A55).copy(alpha = 0.8f * alpha), Offset(0f, h * 0.84f), Size(w, h * 0.16f))
    val s = h * 0.18f
    val stack = listOf(
        Triple(0.34f, 0.84f, 0f), Triple(0.5f, 0.84f, 0f), Triple(0.66f, 0.84f, 4f),
        Triple(0.42f, 0.84f - 0.18f, -6f), Triple(0.59f, 0.84f - 0.18f, 8f), Triple(0.5f, 0.84f - 0.36f, -3f),
    )
    stack.forEachIndexed { i, (x, y, rot) ->
        val tl = Offset(w * x - s / 2, h * y - s)
        rotate(rot, pivot = tl + Offset(s / 2, s / 2)) {
            drawRoundRect(colors[i % colors.size].copy(alpha = alpha), tl, Size(s, s), CornerRadius(5f))
        }
    }
    val falling = Offset(w * 0.78f, h * 0.12f)
    rotate(18f, pivot = falling + Offset(s / 2, s / 2)) {
        drawRoundRect(colors[4].copy(alpha = alpha), falling, Size(s, s), CornerRadius(5f))
    }
    drawLine(Color.White.copy(alpha = 0.3f * alpha), falling + Offset(s * 0.2f, -h * 0.02f), falling + Offset(s * 0.2f, -h * 0.12f), 3f)
    drawLine(Color.White.copy(alpha = 0.3f * alpha), falling + Offset(s * 0.7f, -h * 0.04f), falling + Offset(s * 0.7f, -h * 0.14f), 3f)
}

private fun DrawScope.drawEmptyArt(alpha: Float) {
    val w = size.width
    val h = size.height
    val line = Color(0xFF8C90A0).copy(alpha = 0.45f * alpha)
    val step = h * 0.2f
    var x = (w / 2) % step
    while (x < w) {
        drawLine(line.copy(alpha = 0.12f * alpha), Offset(x, 0f), Offset(x, h))
        x += step
    }
    var y = (h / 2) % step
    while (y < h) {
        drawLine(line.copy(alpha = 0.12f * alpha), Offset(0f, y), Offset(w, y))
        y += step
    }
    drawRoundRect(line, Offset(w * 0.5f - h * 0.28f, h * 0.24f), Size(h * 0.56f, h * 0.52f), CornerRadius(8f), style = Stroke(3f))
    drawLine(line, Offset(w * 0.5f, h * 0.4f), Offset(w * 0.5f, h * 0.6f), 3f)
    drawLine(line, Offset(w * 0.5f - h * 0.1f, h * 0.5f), Offset(w * 0.5f + h * 0.1f, h * 0.5f), 3f)
}
