package com.goni.ui.model

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.ImageBitmap
import com.goni.ui.components.ToastMessage

// --- dados vindos do motor (snapshot/call) ------------------------------------------

@Immutable
data class ProjectCard(
    val folder: String,
    val name: String,
    val edited: String,
    val thumbnail: ImageBitmap? = null,
)

@Immutable
data class HierarchyNode(val id: Long, val name: String, val depth: Int, val kind: String)

@Immutable
data class EditorSnapshot(
    val projectName: String = "",
    val projectFolder: String = "",
    val scenePath: String = "",
    val dirty: Boolean = false,
    val playing: Boolean = false,
    val paused: Boolean = false,
    val previewing: Boolean = false,
    val selection: Long = 0L,
    val tool: Int = 1,
    val snapTranslate: Boolean = false,
    val snapRotate: Boolean = false,
    val canUndo: Boolean = false,
    val canRedo: Boolean = false,
    val hierarchy: List<HierarchyNode> = emptyList(),
) {
    val selectedNode: HierarchyNode? get() = hierarchy.firstOrNull { it.id == selection }
    val sceneName: String get() = scenePath.removeSuffix(".json").ifEmpty { "main" }
}

@Immutable
data class FieldModel(
    val path: String,
    val type: String,
    val value: String,
    val kind: String,
    val options: List<String> = emptyList(),
)

@Immutable
data class ComponentModel(
    val name: String,
    val label: String,
    val category: String,
    val removable: Boolean,
    val fields: List<FieldModel>,
)

@Immutable
data class TransformModel(
    val x: Float = 0f,
    val y: Float = 0f,
    val rotation: Float = 0f,
    val scaleX: Float = 1f,
    val scaleY: Float = 1f,
)

@Immutable
data class InspectorModel(
    val id: Long,
    val name: String,
    val kind: String,
    val transform: TransformModel,
    val components: List<ComponentModel>,
)

@Immutable
data class CatalogItem(val name: String, val label: String, val category: String, val dependency: String)

@Immutable
data class AssetItem(
    val category: String,
    val name: String,
    val subtitle: String = "",
    val thumbnail: ImageBitmap? = null,
)

@Immutable
data class ScriptDiag(val line: Int, val col: Int, val message: String)

@Immutable
data class HudModel(
    val fps: Int = 0,
    val scripts: Int = 0,
    val faults: Int = 0,
    val error: String = "",
)

@Immutable
data class TickLayer(val name: String, val timeScale: Float, val update: Boolean, val physics: Boolean, val render: Boolean)

@Immutable
data class CollisionLayer(val name: String, val bit: Long)

@Immutable
data class SettingsModel(
    val projectName: String = "",
    val gridVisible: Boolean = true,
    val gridCell: Float = 1f,
    val physicsHz: Int = 60,
    val layers: List<TickLayer> = emptyList(),
    val collisionLayers: List<CollisionLayer> = emptyList(),
    val version: String = "",
    val backend: String = "",
)

@Immutable
data class AnimationModel(
    val name: String,
    val fps: Float = 12f,
    val loop: Boolean = true,
    val frames: List<String> = emptyList(),
    val json: String = "",
)

@Immutable
data class MaterialModel(val name: String, val lit: Boolean, val tint: Long)

// --- navegação e camadas ---------------------------------------------------------------

sealed interface Screen {
    data object Home : Screen
    data object Editor : Screen
    data class Script(val name: String) : Screen
}

enum class EditorTab(val label: String) { Scene("Cena"), Properties("Propriedades"), Library("Biblioteca") }

enum class LibraryKind(val category: String, val label: String) {
    Images("textures", "Imagens"),
    Sounds("audio", "Sons"),
    Scripts("scripts", "Scripts"),
    Animations("animations", "Animações"),
    Materials("materials", "Materiais"),
}

/** Seletor de asset para um campo do inspector. */
data class PickTarget(val entity: Long, val component: String, val path: String, val kind: String)

sealed interface Sheet {
    data object NewProject : Sheet
    data class ProjectMenu(val project: ProjectCard) : Sheet
    data object AddEntity : Sheet
    data class EntityMenu(val node: HierarchyNode) : Sheet
    data class Reparent(val node: HierarchyNode) : Sheet
    data object AddComponent : Sheet
    data class Picker(val target: PickTarget, val options: List<AssetItem>) : Sheet
    data class Color(val entity: Long, val component: String, val path: String, val hex: String) : Sheet
    data class AssetMenu(val item: AssetItem) : Sheet
    data object Scenes : Sheet
    data object Settings : Sheet
    data class Animation(val model: AnimationModel) : Sheet
    data class Material(val model: MaterialModel) : Sheet
    data class Rename(val title: String, val initial: String, val onConfirm: (String) -> Unit) : Sheet
    data class Confirm(val title: String, val message: String, val action: String, val onConfirm: () -> Unit) : Sheet
    data class TextInput(val title: String, val hint: String, val action: String, val onConfirm: (String) -> Unit) : Sheet
}

/**
 * Estado observável da UI inteira. O controlador do app escreve aqui a
 * partir dos snapshots do motor; as telas só leem.
 */
class UiState {
    var screen: Screen by mutableStateOf(Screen.Home)
    var projects: List<ProjectCard> by mutableStateOf(emptyList())
    var snapshot: EditorSnapshot by mutableStateOf(EditorSnapshot())
    var tab: EditorTab? by mutableStateOf(null)
    var panelTall: Boolean by mutableStateOf(false)
    var inspector: InspectorModel? by mutableStateOf(null)
    var catalog: List<CatalogItem> by mutableStateOf(emptyList())
    var library: LibraryKind by mutableStateOf(LibraryKind.Images)
    var assets: List<AssetItem> by mutableStateOf(emptyList())
    var scenes: List<String> by mutableStateOf(emptyList())
    var settings: SettingsModel by mutableStateOf(SettingsModel())
    var sheet: Sheet? by mutableStateOf(null)
    var toast: ToastMessage? by mutableStateOf(null)
    var hud: HudModel by mutableStateOf(HudModel())
    var playingAudio: String? by mutableStateOf(null)
    // editor de script
    var scriptText: String by mutableStateOf("")
    var scriptDiags: List<ScriptDiag> by mutableStateOf(emptyList())
    var scriptCompiled: Boolean? by mutableStateOf(null)
    var scriptDirty: Boolean by mutableStateOf(false)
    /** Modo player: roda o jogo sem nenhuma interface de edição. */
    var playerOnly: Boolean by mutableStateOf(false)
}

/** Ações da UI. Os padrões vazios deixam prévias e testes triviais. */
interface UiActions {
    // início
    fun openProject(folder: String) {}
    fun createProject(name: String, template: String) {}
    fun renameProject(folder: String, name: String) {}
    fun deleteProject(folder: String) {}
    fun exportProject(folder: String) {}
    fun importProject() {}
    fun playProject(folder: String) {}

    // editor
    fun goHome() {}
    fun play() {}
    fun stop() {}
    fun setPaused(paused: Boolean) {}
    fun undo() {}
    fun redo() {}
    fun save() {}
    fun setTool(tool: Int) {}
    fun toggleSnap() {}
    fun fitView() {}
    fun openTab(tab: EditorTab?) {}
    fun togglePanelHeight() {}

    // entidades
    fun select(id: Long) {}
    fun createEntity(template: String) {}
    fun renameEntity(id: Long, name: String) {}
    fun duplicateEntity(id: Long) {}
    fun deleteEntity(id: Long) {}
    fun reparentEntity(id: Long, parent: Long) {}
    fun setField(id: Long, component: String, path: String, value: String) {}
    fun setTransform(id: Long, transform: TransformModel) {}
    fun addComponent(id: Long, component: String) {}
    fun removeComponent(id: Long, component: String) {}
    fun openCatalog() {}
    fun openPicker(target: PickTarget) {}
    fun openColor(id: Long, component: String, path: String, hex: String) {}

    // biblioteca
    fun showLibrary(kind: LibraryKind) {}
    fun importAsset(kind: LibraryKind) {}
    fun useAsset(item: AssetItem) {}
    fun renameAsset(item: AssetItem, newName: String) {}
    fun deleteAsset(item: AssetItem) {}
    fun createAsset(kind: LibraryKind, name: String) {}
    fun openAsset(item: AssetItem) {}
    fun toggleAudio(name: String) {}

    // cenas e ajustes
    fun openScenes() {}
    fun newScene() {}
    fun loadScene(path: String) {}
    fun saveSceneAs(name: String) {}
    fun openSettings() {}
    fun setProjectName(name: String) {}
    fun setGrid(visible: Boolean, cell: Float) {}
    fun setPhysicsHz(hz: Int) {}
    fun addTickLayer(name: String) {}
    fun setTickLayer(layer: TickLayer) {}
    fun addCollisionLayer(name: String) {}
    fun renameCollisionLayer(bit: Long, name: String) {}
    fun exportDiagnostics() {}

    // script
    fun editScriptText(text: String) {}
    fun compileScript() {}
    fun saveScript() {}
    fun attachScript() {}
    fun closeScript() {}

    // animação e material
    fun saveAnimation(model: AnimationModel) {}
    fun addAnimationFrame(model: AnimationModel) {}
    fun previewAnimation(name: String) {}
    fun assignAnimation(name: String) {}
    fun saveMaterial(model: MaterialModel) {}

    fun dismissSheet() {}
}
