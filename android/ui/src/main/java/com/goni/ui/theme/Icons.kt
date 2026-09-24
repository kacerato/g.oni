package com.goni.ui.theme

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.unit.dp

/**
 * Ícones de traço (grade 24, traço 2, pontas arredondadas). Desenhados à
 * mão para o editor não depender de pacotes de ícones enormes.
 */
object OniIcons {
    private fun icon(name: String, vararg paths: String, fill: Boolean = false): ImageVector {
        val b = ImageVector.Builder(
            name = name,
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f,
        )
        for (d in paths) {
            b.addPath(
                pathData = addPathNodes(d),
                fill = if (fill) SolidColor(Color.Black) else null,
                stroke = if (fill) null else SolidColor(Color.Black),
                strokeLineWidth = if (fill) 0f else 2f,
                strokeLineCap = StrokeCap.Round,
                strokeLineJoin = StrokeJoin.Round,
            )
        }
        return b.build()
    }

    private fun circle(cx: Float, cy: Float, r: Float) =
        "M${cx - r} ${cy}a$r $r 0 1 0 ${2 * r} 0a$r $r 0 1 0 ${-2 * r} 0z"

    val Plus = icon("plus", "M5 12h14", "M12 5v14")
    val Close = icon("close", "M18 6 6 18", "m6 6 12 12")
    val Back = icon("back", "m12 19-7-7 7-7", "M19 12H5")
    val Check = icon("check", "M20 6 9 17l-5-5")
    val Play = icon("play", "M7 4.5v15a1 1 0 0 0 1.5.86l12.2-7.5a1 1 0 0 0 0-1.72L8.5 3.64A1 1 0 0 0 7 4.5z", fill = true)
    val Pause = icon("pause", "M7 4h3a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1z", "M14 4h3a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1h-3a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1z", fill = true)
    val Stop = icon("stop", "M7 5h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2z", fill = true)
    val Undo = icon("undo", "M9 14 4 9l5-5", "M4 9h10.5a5.5 5.5 0 0 1 0 11H11")
    val Redo = icon("redo", "m15 14 5-5-5-5", "M20 9H9.5a5.5 5.5 0 0 0 0 11H13")
    val More = icon("more", circle(12f, 5f, 1f), circle(12f, 12f, 1f), circle(12f, 19f, 1f))
    val Search = icon("search", circle(11f, 11f, 7f), "m20 20-4-4")
    val Settings = icon(
        "settings", "M20 7h-9", "M14 17H5", circle(17f, 17f, 3f), circle(7f, 7f, 3f),
    )
    val Folder = icon("folder", "M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z")
    val Image = icon(
        "image",
        "M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z",
        circle(9f, 9f, 2f),
        "m21 15-3.1-3.1a2 2 0 0 0-2.8 0L6 21",
    )
    val Music = icon("music", "M9 18V5l12-2v13", circle(6f, 18f, 3f), circle(18f, 16f, 3f))
    val Code = icon("code", "m16 18 6-6-6-6", "m8 6-6 6 6 6")
    val Film = icon(
        "film",
        "M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z",
        "M7 3v18", "M17 3v18", "M3 12h18", "M3 7.5h4", "M3 16.5h4", "M17 7.5h4", "M17 16.5h4",
    )
    val Droplet = icon("droplet", "M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15a7 7 0 0 0 7 7z")
    val Tree = icon("tree", "M21 12h-8", "M21 6H8", "M21 18h-8", "M3 6v4a2 2 0 0 0 2 2h3", "M3 10v6a2 2 0 0 0 2 2h3")
    val Sliders = icon(
        "sliders", "M4 21v-7", "M4 10V3", "M12 21v-9", "M12 8V3", "M20 21v-5", "M20 12V3",
        "M2 14h4", "M10 8h4", "M18 16h4",
    )
    val Grid = icon("grid", "M4 3h6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z", "M14 3h6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1h-6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z", "M14 13h6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1h-6a1 1 0 0 1-1-1v-6a1 1 0 0 1 1-1z", "M4 13h6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-6a1 1 0 0 1 1-1z")
    val Trash = icon("trash", "M3 6h18", "M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6", "M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2", "M10 11v6", "M14 11v6")
    val Edit = icon("edit", "M21.17 6.81a1 1 0 0 0-3.98-3.98L3.84 16.17a2 2 0 0 0-.5.83l-1.32 4.35a.5.5 0 0 0 .62.62l4.35-1.32a2 2 0 0 0 .83-.5z", "m15 5 4 4")
    val Copy = icon("copy", "M10 8h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H10a2 2 0 0 1-2-2V10a2 2 0 0 1 2-2z", "M4 16a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2")
    val Pointer = icon("pointer", "M4.04 4.66a.5.5 0 0 1 .62-.62l15.5 5.62a.5.5 0 0 1-.03.95l-6.3 1.83a1 1 0 0 0-.68.68l-1.83 6.3a.5.5 0 0 1-.95.03z")
    val Move = icon("move", "M12 2v20", "m15 19-3 3-3-3", "m19 9 3 3-3 3", "M2 12h20", "m5 9-3 3 3 3", "m9 5 3-3 3 3")
    val Rotate = icon("rotate", "M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8", "M21 3v5h-5")
    val Scale = icon("scale", "M15 3h6v6", "M9 21H3v-6", "M21 3l-7 7", "M3 21l7-7")
    val Magnet = icon("magnet", "m6 15-4-4 6.75-6.77a7.79 7.79 0 0 1 11 11L13 22l-4-4 6.39-6.36a2.14 2.14 0 0 0-3-3L6 15", "m5 8 4 4", "m12 15 4 4")
    val Fit = icon("fit", "M3 7V5a2 2 0 0 1 2-2h2", "M17 3h2a2 2 0 0 1 2 2v2", "M21 17v2a2 2 0 0 1-2 2h-2", "M7 21H5a2 2 0 0 1-2-2v-2", "M8 12h8", "M12 8v8")
    val Camera = icon("camera", "m16 13 5.22 3.48a.5.5 0 0 0 .78-.42V7.87a.5.5 0 0 0-.75-.43L16 10.5", "M4 6h10a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2z")
    val Person = icon("person", circle(12f, 7f, 4f), "M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2")
    val Sun = icon(
        "sun", circle(12f, 12f, 4f), "M12 2v2", "M12 20v2", "m4.93 4.93 1.41 1.41", "m17.66 17.66 1.41 1.41",
        "M2 12h2", "M20 12h2", "m6.34 17.66-1.41 1.41", "m19.07 4.93-1.41 1.41",
    )
    val Sparkles = icon(
        "sparkles",
        "M9.94 15.5A2 2 0 0 0 8.5 14.06l-6.14-1.58a.5.5 0 0 1 0-.96L8.5 9.94A2 2 0 0 0 9.94 8.5l1.58-6.14a.5.5 0 0 1 .96 0l1.58 6.14a2 2 0 0 0 1.44 1.44l6.14 1.58a.5.5 0 0 1 0 .96l-6.14 1.58a2 2 0 0 0-1.44 1.44l-1.58 6.14a.5.5 0 0 1-.96 0z",
        "M20 3v4", "M22 5h-4",
    )
    val Speaker = icon(
        "speaker",
        "M11 4.7a.7.7 0 0 0-1.2-.5L6.41 7.59A1.4 1.4 0 0 1 5.42 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.42a1.4 1.4 0 0 1 .99.41l3.39 3.39a.7.7 0 0 0 1.2-.5z",
        "M16 9a5 5 0 0 1 0 6", "M19.36 18.36a9 9 0 0 0 0-12.72",
    )
    val Cube = icon("cube", "M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z", "m3.3 7 8.7 5 8.7-5", "M12 22V12")
    val Square = icon("square", "M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z")
    val Download = icon("download", "M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4", "m7 10 5 5 5-5", "M12 15V3")
    val Upload = icon("upload", "M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4", "m17 8-5-5-5 5", "M12 3v12")
    val Share = icon("share", "M4 12v8a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-8", "m16 6-4-4-4 4", "M12 2v13")
    val Gamepad = icon(
        "gamepad", "M6 11h4", "M8 9v4", "M15 12h.01", "M18 10h.01",
        "M17.32 5H6.68a4 4 0 0 0-3.98 3.59C2.6 9.42 2 14.46 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.41-1.41A2 2 0 0 1 9.83 16h4.34a2 2 0 0 1 1.41.59L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.55-.6-6.58-.69-7.26A4 4 0 0 0 17.32 5z",
    )
    val ChevronRight = icon("chevron-right", "m9 18 6-6-6-6")
    val ChevronDown = icon("chevron-down", "m6 9 6 6 6-6")
    val ChevronUp = icon("chevron-up", "m18 15-6-6-6 6")
    val ChevronLeft = icon("chevron-left", "m15 18-6-6 6-6")
    val Warning = icon("warning", "m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3", "M12 9v4", "M12 17h.01")
    val Info = icon("info", circle(12f, 12f, 10f), "M12 16v-4", "M12 8h.01")
    val Save = icon("save", "M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z", "M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7", "M7 3v4a1 1 0 0 0 1 1h7")
    val Map = icon(
        "map",
        "M14.1 5.55a2 2 0 0 0 1.8 0l3.65-1.83A1 1 0 0 1 21 4.62v12.76a1 1 0 0 1-.55.9l-4.55 2.27a2 2 0 0 1-1.8 0l-4.2-2.1a2 2 0 0 0-1.8 0l-3.65 1.83A1 1 0 0 1 3 19.38V6.62a1 1 0 0 1 .55-.9l4.55-2.27a2 2 0 0 1 1.8 0z",
        "M15 5.76v15", "M9 3.24v15",
    )
    val Zap = icon("zap", "M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z")
    val Link = icon("link", "M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71", "M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71")
    val Eye = icon("eye", "M2.06 12.35a1 1 0 0 1 0-.7 10.75 10.75 0 0 1 19.88 0 1 1 0 0 1 0 .7 10.75 10.75 0 0 1-19.88 0", circle(12f, 12f, 3f))
    val Layers = icon("layers", "M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83z", "m2 12 9.17 4.18a2 2 0 0 0 1.66 0L22 12", "m2 17 9.17 4.18a2 2 0 0 0 1.66 0L22 17")
    val Hash = icon("hash", "M4 9h16", "M4 15h16", "M10 3 8 21", "M16 3l-2 18")
    val Bug = icon(
        "bug", "m8 2 1.88 1.88", "M14.12 3.88 16 2", "M9 7.13v-1a3 3 0 1 1 6 0v1",
        "M12 20a6 6 0 0 1-6-6v-3a4 4 0 0 1 4-4h4a4 4 0 0 1 4 4v3a6 6 0 0 1-6 6", "M12 20v-9",
        "M6.53 9C4.6 8.8 3 7.1 3 5", "M6 13H2", "M3 21c0-2.1 1.7-3.9 3.8-4", "M20.97 5c0 2.1-1.6 3.8-3.5 4",
        "M22 13h-4", "M17.2 17c2.1.1 3.8 1.9 3.8 4",
    )
    val Home = icon("home", "M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8", "M3 10a2 2 0 0 1 .71-1.53l7-6a2 2 0 0 1 2.58 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z")
    val Keyboard = icon("keyboard", "M4 5h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2z", "M6 9h.01", "M10 9h.01", "M14 9h.01", "M18 9h.01", "M8 13h.01", "M16 13h.01", "M7 16h10")
    val Tab = icon("tab", "M3 12h14", "m13 8 4 4-4 4", "M21 6v12")
    val ArrowUp = icon("arrow-up", "m5 12 7-7 7 7", "M12 19V5")

    /** Ícone da entidade pelo tipo informado pelo protocolo. */
    val Text = icon("text", "M5 7V5h14v2", "M12 5v14", "M9 19h6")
    val Stamp = icon("stamp", "M4 15h16v5H4z", "M9 15v-4a3 3 0 1 1 6 0v4")
    val Palette = icon("palette", "M12 3a9 9 0 1 0 0 18c1.1 0 1.6-.8 1.6-1.6 0-1-.9-1.4-.9-2.4s.8-1.6 1.8-1.6H17a4 4 0 0 0 4-4c0-4.7-4-8.4-9-8.4z", circle(7.5f, 11f, 1f), circle(10.5f, 7f, 1f), circle(15f, 7.5f, 1f))
    val Portrait = icon("portrait", "M7 3h10v18H7z", "M11 18h2")
    val Landscape = icon("landscape", "M3 7h18v10H3z", "M18 11v2")
    val Tap = icon("tap", circle(12f, 12f, 3f), circle(12f, 12f, 8f))
    val Auto = icon("auto", "M4 12a8 8 0 0 1 14-5.3", "M20 12a8 8 0 0 1-14 5.3", "M18 3v4h-4", "M6 21v-4h4")

    fun forKind(kind: String): ImageVector = when (kind) {
        "sprite" -> Image
        "camera" -> Camera
        "character" -> Person
        "light" -> Sun
        "particles" -> Sparkles
        "audio" -> Speaker
        "script" -> Code
        "text" -> Text
        "template" -> Stamp
        else -> Cube
    }
}
