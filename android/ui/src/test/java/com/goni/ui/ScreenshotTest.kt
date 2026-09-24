package com.goni.ui

import app.cash.paparazzi.DeviceConfig
import app.cash.paparazzi.Paparazzi
import com.goni.ui.components.ToastKind
import com.goni.ui.components.ToastMessage
import com.goni.ui.model.AssetItem
import com.goni.ui.model.CatalogItem
import com.goni.ui.model.CollisionLayer
import com.goni.ui.model.ComponentModel
import com.goni.ui.model.EditorSnapshot
import com.goni.ui.model.EditorTab
import com.goni.ui.model.FieldModel
import com.goni.ui.model.HierarchyNode
import com.goni.ui.model.HudModel
import com.goni.ui.model.InspectorModel
import com.goni.ui.model.LibraryKind
import com.goni.ui.model.ProjectCard
import com.goni.ui.model.Screen
import com.goni.ui.model.ScriptDiag
import com.goni.ui.model.SettingsModel
import com.goni.ui.model.Sheet
import com.goni.ui.model.TickLayer
import com.goni.ui.model.TransformModel
import com.goni.ui.model.UiState
import org.junit.Rule
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.goni.ui.components.drawExampleArt
import com.goni.ui.components.exampleBackground
import org.junit.Test

/**
 * Screenshots das telas principais com dados de exemplo. Rodar
 * `./gradlew :ui:recordPaparazziDebug` regrava as imagens em
 * ui/src/test/snapshots/images.
 */
class ScreenshotTest {
    @get:Rule
    val paparazzi = Paparazzi(deviceConfig = DeviceConfig.PIXEL_5, theme = "android:Theme.Material.NoActionBar")

    private val hierarchy = listOf(
        HierarchyNode(1, "Chão", 0, "sprite"),
        HierarchyNode(2, "Plataforma", 0, "sprite"),
        HierarchyNode(3, "Jogador", 0, "character"),
        HierarchyNode(6, "Espada", 1, "sprite"),
        HierarchyNode(4, "Câmera", 0, "camera"),
        HierarchyNode(5, "Sol", 0, "light"),
    )

    private fun editorState(tab: EditorTab? = null) = UiState().apply {
        screen = Screen.Editor
        snapshot = EditorSnapshot(
            projectName = "Plataforma",
            projectFolder = "Plataforma",
            scenePath = "main.json",
            dirty = true,
            selection = 3,
            tool = 1,
            canUndo = true,
            hierarchy = hierarchy,
        )
        this.tab = tab
        settings = SettingsModel(
            projectName = "Plataforma",
            layers = listOf(TickLayer("Default", 1f, true, true, true), TickLayer("Fundo", 0.5f, true, false, true)),
            collisionLayers = listOf(CollisionLayer("Padrão", 1), CollisionLayer("Jogador", 2), CollisionLayer("Inimigos", 4)),
            version = "0.8.0 (80)",
            backend = "OpenGL ES",
        )
        inspector = InspectorModel(
            id = 3,
            name = "Jogador",
            kind = "character",
            transform = TransformModel(x = -2f, y = 0f, rotation = 0f, scaleX = 1f, scaleY = 1f),
            components = listOf(
                ComponentModel(
                    "eng::editor::SpriteData", "Sprite", "Render", true,
                    listOf(
                        FieldModel("textureAsset", "string", "heroi.png", "texture"),
                        FieldModel("tintR,tintG,tintB", "color", "#8AB5F8", "color"),
                        FieldModel("opacity", "f32", "1", "number"),
                        FieldModel("flipX", "bool", "false", "bool"),
                    ),
                ),
                ComponentModel(
                    "eng::physics::Collider", "Colisor", "Física", true,
                    listOf(
                        FieldModel("shape", "eng::physics::ColliderShape", "Box", "enum", listOf("Sphere", "Box")),
                        FieldModel("halfExtents.x", "f32", "0.5", "number"),
                        FieldModel("halfExtents.y", "f32", "0.5", "number"),
                        FieldModel("halfExtents.z", "f32", "0.5", "number"),
                        FieldModel("mask", "u32", "3", "bitfield"),
                        FieldModel("isTrigger", "bool", "false", "bool"),
                    ),
                ),
                ComponentModel(
                    "eng::physics::RigidBody", "Corpo rígido", "Física", true,
                    listOf(
                        FieldModel("bodyType", "eng::physics::BodyType", "DynamicLite", "enum", listOf("Static", "Kinematic", "DynamicLite")),
                        FieldModel("mass", "f32", "1", "number"),
                    ),
                ),
                ComponentModel(
                    "eng::editor::NiScriptComponent", "Script", "Lógica", true,
                    listOf(
                        FieldModel("scriptAsset", "string", "jogador.nis", "script"),
                        FieldModel("source", "string", "…", "code"),
                    ),
                ),
            ),
        )
        assets = listOf(
            AssetItem("textures", "heroi.png"), AssetItem("textures", "chao.png"), AssetItem("textures", "moeda.png"),
            AssetItem("textures", "nuvem.png"), AssetItem("textures", "fundo.png"),
            AssetItem("scripts", "jogador.nis", "16 linhas"), AssetItem("scripts", "moeda.nis", "8 linhas"),
            AssetItem("audio", "pulo.wav", "0,4 s"),
        )
    }

    @Test
    fun home() {
        val state = UiState().apply {
            projects = listOf(
                ProjectCard("Plataforma", "Plataforma", "editado há 2 min"),
                ProjectCard("Nave", "Nave Espacial", "editado ontem"),
                ProjectCard("Teste", "Teste de física", "editado em 12/09"),
            )
        }
        paparazzi.snapshot { GoniApp(state, NoActions) {} }
    }

    @Test
    fun homeEmptyNewProject() {
        val state = UiState().apply { sheet = Sheet.NewProject() }
        paparazzi.snapshot { GoniApp(state, NoActions) {} }
    }

    @Test
    fun editorIdle() {
        val state = editorState().apply { toast = ToastMessage("Cena salva", ToastKind.Success) }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport() } }
    }

    @Test
    fun editorScene() {
        paparazzi.snapshot { GoniApp(editorState(EditorTab.Scene), NoActions) { FakeViewport() } }
    }

    @Test
    fun editorProperties() {
        val state = editorState(EditorTab.Properties).apply { panelTall = true }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport() } }
    }

    @Test
    fun editorLibrary() {
        val state = editorState(EditorTab.Library).apply { library = LibraryKind.Images }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport() } }
    }

    @Test
    fun addEntitySheet() {
        val state = editorState().apply { sheet = Sheet.AddEntity }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport() } }
    }

    @Test
    fun addComponentSheet() {
        val state = editorState().apply {
            sheet = Sheet.AddComponent
            catalog = listOf(
                CatalogItem("eng::editor::AudioSource", "Som", "Áudio", ""),
                CatalogItem("eng::physics::CharacterBody", "Personagem", "Física", "eng::physics::Collider"),
                CatalogItem("eng::animation::Animator", "Animador", "Lógica", ""),
                CatalogItem("eng::particles::ParticleEmitter", "Partículas", "FX", ""),
                CatalogItem("eng::render::Light2D", "Luz 2D", "FX", ""),
            )
        }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport() } }
    }

    @Test
    fun newProjectFromExample() {
        val state = UiState().apply { sheet = Sheet.NewProject("flappy") }
        paparazzi.snapshot { GoniApp(state, NoActions) {} }
    }

    private fun flappyState() = UiState().apply {
        screen = Screen.Editor
        snapshot = EditorSnapshot(
            projectName = "Voo",
            projectFolder = "Voo",
            scenePath = "main.json",
            selection = 12,
            tool = 1,
            controls = "tap",
            orientation = "portrait",
            hierarchy = listOf(
                HierarchyNode(10, "Câmera", 0, "camera"),
                HierarchyNode(11, "Chão", 0, "sprite"),
                HierarchyNode(12, "Pássaro", 0, "sprite"),
                HierarchyNode(13, "Cano", 0, "empty", template = true),
                HierarchyNode(14, "Cima", 1, "sprite"),
                HierarchyNode(15, "Baixo", 1, "sprite"),
                HierarchyNode(16, "Vão", 1, "empty"),
                HierarchyNode(17, "Gerador", 0, "script"),
                HierarchyNode(18, "Placar", 0, "text"),
                HierarchyNode(19, "Mensagem", 0, "text"),
            ),
        )
        settings = SettingsModel(projectName = "Voo", background = "#5CA3DB", orientation = "portrait", controls = "tap")
        inspector = InspectorModel(
            id = 18,
            name = "Placar",
            kind = "text",
            transform = TransformModel(),
            components = listOf(
                ComponentModel(
                    "eng::editor::TextData", "Texto", "Render", true,
                    listOf(
                        FieldModel("text", "string", "0", "text"),
                        FieldModel("size", "f32", "0.9", "number"),
                        FieldModel("colorR,colorG,colorB", "color", "#FFFFFF", "color"),
                        FieldModel("align", "eng::editor::TextAlign", "Center", "enum", listOf("Left", "Center", "Right")),
                        FieldModel("screenSpace", "bool", "true", "bool"),
                        FieldModel("screenX", "f32", "0.5", "number"),
                        FieldModel("screenY", "f32", "0.9", "number"),
                    ),
                ),
                ComponentModel("eng::scene::Template", "Molde", "Lógica", true, listOf(FieldModel("active", "bool", "false", "bool"))),
            ),
        )
    }

    @Test
    fun flappyScene() {
        val state = flappyState().apply { tab = EditorTab.Scene }
        paparazzi.snapshot { GoniApp(state, NoActions) { FlappyViewport() } }
    }

    @Test
    fun flappyTextProperties() {
        val state = flappyState().apply { tab = EditorTab.Properties; panelTall = true }
        paparazzi.snapshot { GoniApp(state, NoActions) { FlappyViewport() } }
    }

    @Test
    fun flappyPlayingTap() {
        val state = flappyState().apply {
            snapshot = snapshot.copy(playing = true)
            hud = HudModel(fps = 60, scripts = 4)
        }
        paparazzi.snapshot { GoniApp(state, NoActions) { FlappyViewport() } }
    }

    @Test
    fun gameSettingsSheet() {
        val state = flappyState().apply { sheet = Sheet.Settings }
        paparazzi.snapshot { GoniApp(state, NoActions) { FlappyViewport() } }
    }

    @Test
    fun settingsSheet() {
        val state = editorState().apply { sheet = Sheet.Settings }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport() } }
    }

    @Test
    fun playing() {
        val state = editorState().apply {
            snapshot = snapshot.copy(playing = true)
            hud = HudModel(fps = 60, scripts = 1)
        }
        paparazzi.snapshot { GoniApp(state, NoActions) { FakeViewport(selected = false) } }
    }

    @Test
    fun scriptEditor() {
        val state = editorState().apply {
            screen = Screen.Script("jogador.nis")
            scriptText = """
                # Jogador de plataforma.
                add &BL

                var velocidade = 4.0
                var forcaPulo = 7.5

                up update:
                    var me = self()
                    var vx = 0.0
                    if action_down("left"):
                        vx = vx - velocidade
                    stop
                    me.rigidbody.velocity.x = vx
                    if action_pressed("jump"):
                        me.rigidbody.velocity.y = forcaPulo
                    stop
                stop
            """.trimIndent()
            scriptDirty = true
            scriptCompiled = false
            scriptDiags = listOf(ScriptDiag(14, 9, "nome desconhecido: forcaPulo2"))
        }
        paparazzi.snapshot { GoniApp(state, NoActions) {} }
    }
}

/** Quadro do Voo para as prévias (o jogo de verdade vem do motor). */
@Composable
private fun FlappyViewport() {
    Canvas(Modifier.fillMaxSize().background(exampleBackground("flappy"))) { drawExampleArt("flappy") }
}
