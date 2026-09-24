package com.goni.app

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.goni.ui.components.ToastKind
import com.goni.ui.components.ToastMessage
import com.goni.ui.model.AnimationModel
import com.goni.ui.model.AssetItem
import com.goni.ui.model.CatalogItem
import com.goni.ui.model.CollisionLayer
import com.goni.ui.model.ComponentModel
import com.goni.ui.model.EditorSnapshot
import com.goni.ui.model.EditorTab
import com.goni.ui.model.ExampleProject
import com.goni.ui.model.FieldModel
import com.goni.ui.model.HudModel
import com.goni.ui.model.InspectorModel
import com.goni.ui.model.LibraryKind
import com.goni.ui.model.MaterialModel
import com.goni.ui.model.PickTarget
import com.goni.ui.model.ProjectCard
import com.goni.ui.model.Screen
import com.goni.ui.model.ScriptDiag
import com.goni.ui.model.SettingsModel
import com.goni.ui.model.Sheet
import com.goni.ui.model.TickLayer
import com.goni.ui.model.TransformModel
import com.goni.ui.model.UiActions
import com.goni.ui.model.UiState
import com.goni.app.export.ApkExporter
import com.goni.app.export.KeystoreSigner
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.DateFormat
import java.util.Date

/**
 * Liga a interface ao motor. Lê o estado por snapshot a cada frame e
 * traduz cada ação da UI numa operação do protocolo. Tudo roda na thread
 * de UI (o motor é single-threaded).
 */
class EditorController(
    private val activity: MainActivity,
    private val engine: Engine,
    val state: UiState,
    private val workspace: File,
) : UiActions, ViewportView.Host {

    private val main = Handler(Looper.getMainLooper())
    private val thumbs = HashMap<String, ImageBitmap?>()
    private val animClips = HashMap<String, String>()
    private var surfaceReady = false
    private var pendingFit = false
    private var frames = 0
    private var fpsTime = 0f
    private var lastSelection = 0L
    private var compileTask: Runnable? = null
    private var toastTask: Runnable? = null
    private var audioCheck = 0f

    // --- ViewportView.Host ------------------------------------------------------------

    override val playing: Boolean get() = state.snapshot.playing
    override val tool: Int get() = state.snapshot.tool
    override val selection: Long get() = state.snapshot.selection

    override fun onSurfaceReady(ready: Boolean) {
        surfaceReady = ready
    }

    override fun onTapped(hit: Long) {
        // nativeTap já atualizou a seleção no motor; o snapshot traz o resto.
    }

    override fun onEditGestureEnded() {
        refreshInspector()
    }

    // --- loop -------------------------------------------------------------------------

    fun frame(dt: Float) {
        if (surfaceReady) {
            NativeBridge.nativeRenderFrame(engine.handle, dt)
            if (pendingFit) {
                pendingFit = false
                engine.call("viewport.fit")
            }
        }
        engine.pollSnapshot()?.let(::applySnapshot)
        if (state.snapshot.playing) {
            frames++
            fpsTime += dt
            if (fpsTime >= 0.5f) {
                updateHud(frames / fpsTime)
                frames = 0
                fpsTime = 0f
            }
        }
        if (state.playingAudio != null) {
            audioCheck += dt
            if (audioCheck > 0.5f) {
                audioCheck = 0f
                if (engine.call("audio.playing").result != true) state.playingAudio = null
            }
        }
    }

    private fun applySnapshot(snap: EditorSnapshot) {
        val before = state.snapshot
        state.snapshot = snap
        if (before.playing != snap.playing || (snap.playing && before.orientation != snap.orientation)) {
            activity.applyPlayOrientation(snap.playing, snap.orientation)
        }
        if (before.playing && !snap.playing && state.playerOnly) {
            leavePlayer()
            return
        }
        if (snap.selection != lastSelection || state.tab == EditorTab.Properties) {
            lastSelection = snap.selection
            refreshInspector()
        }
    }

    private fun updateHud(fps: Float) {
        val r = engine.call("play.hud").obj ?: return
        val scripts = r.optJSONObject("scripts") ?: JSONObject()
        state.hud = HudModel(
            fps = fps.toInt(),
            scripts = scripts.optInt("compiled"),
            faults = scripts.optInt("faults"),
            error = scripts.optString("compileError").ifEmpty { scripts.optString("fault") },
        )
    }

    // --- mensagens --------------------------------------------------------------------

    fun toast(text: String, kind: ToastKind = ToastKind.Info) {
        state.toast = ToastMessage(text, kind)
        toastTask?.let(main::removeCallbacks)
        toastTask = Runnable { state.toast = null }.also { main.postDelayed(it, if (kind == ToastKind.Error) 4000 else 2200) }
    }

    private fun check(r: Reply, success: String? = null): Boolean {
        if (!r.ok) {
            toast(r.error, ToastKind.Error)
        } else if (success != null) {
            toast(success, ToastKind.Success)
        }
        return r.ok
    }

    // --- início -----------------------------------------------------------------------

    fun refreshProjects() {
        val folders = engine.call("project.list").array.strings()
        val cards = folders.map { folder ->
            val dir = File(workspace, folder)
            val name = runCatching {
                JSONObject(File(dir, "project.goni.json").readText()).optString("name", folder)
            }.getOrDefault(folder)
            val stamp = newestTimestamp(dir)
            Triple(stamp, folder, ProjectCard(folder, name.ifEmpty { folder }, editedLabel(stamp), projectThumb(dir)))
        }
        state.projects = cards.sortedByDescending { it.first }.map { it.third }
        val examples = engine.call("project.templates").array.objects().map {
            ExampleProject(it.optString("id"), it.optString("title"), it.optString("description"))
        }
        if (examples.isNotEmpty()) state.examples = examples
    }

    private fun newestTimestamp(dir: File): Long {
        var t = File(dir, "project.goni.json").lastModified()
        File(dir, "scenes").listFiles()?.forEach { t = maxOf(t, it.lastModified()) }
        return t
    }

    private fun editedLabel(t: Long): String {
        if (t <= 0L) return ""
        val diff = (System.currentTimeMillis() - t) / 1000
        return when {
            diff < 60 -> "editado agora"
            diff < 3600 -> "editado há ${diff / 60} min"
            diff < 86400 -> "editado há ${diff / 3600} h"
            diff < 2 * 86400 -> "editado ontem"
            else -> "editado em " + DateFormat.getDateInstance(DateFormat.SHORT).format(Date(t))
        }
    }

    private fun projectThumb(dir: File): ImageBitmap? {
        val textures = File(dir, "assets/textures").listFiles()
            ?.filter { it.isFile && it.extension.lowercase() in setOf("png", "jpg", "jpeg") }
            ?.sortedBy { it.name } ?: return null
        return textures.firstOrNull()?.let { thumb(it, 320) }
    }

    private fun thumb(file: File, size: Int): ImageBitmap? {
        val key = "${file.path}@${file.lastModified()}#$size"
        return thumbs.getOrPut(key) {
            runCatching {
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(file.path, bounds)
                var sample = 1
                while (bounds.outWidth / (sample * 2) >= size && bounds.outHeight / (sample * 2) >= size) sample *= 2
                val opts = BitmapFactory.Options().apply {
                    inSampleSize = sample
                    inPreferredConfig = Bitmap.Config.ARGB_8888
                }
                BitmapFactory.decodeFile(file.path, opts)?.asImageBitmap()
            }.getOrNull()
        }
    }

    override fun openProject(folder: String) {
        if (!check(engine.call("project.open", "folder" to folder))) return
        enterEditor()
    }

    private fun enterEditor() {
        state.sheet = null
        state.tab = null
        state.inspector = null
        state.screen = Screen.Editor
        state.snapshot = engine.pollSnapshot(force = true) ?: state.snapshot
        lastSelection = -1
        thumbs.clear()
        loadSettings()
        refreshAllAssets()
        pendingFit = true
    }

    override fun createProject(name: String, template: String) {
        val taken = engine.call("project.list").array.strings().toSet()
        var folder = name.replace('/', '-').trim().ifEmpty { "Meu jogo" }
        if (folder in taken) {
            var n = 2
            while ("$folder $n" in taken) n++
            folder = "$folder $n"
        }
        val r = engine.call("project.new", "name" to folder, "template" to template)
        if (!check(r)) return
        enterEditor()
        toast(if (template == "empty") "Jogo criado" else "Toque em Jogar para testar", ToastKind.Success)
    }

    override fun renameProject(folder: String, name: String) {
        if (!check(engine.call("project.open", "folder" to folder))) return
        if (check(engine.call("project.rename", "name" to name))) {
            engine.call("project.save")
        }
        refreshProjects()
    }

    override fun deleteProject(folder: String) {
        if (File(workspace, folder).deleteRecursively()) {
            toast("Jogo excluído", ToastKind.Success)
        } else {
            toast("Não foi possível excluir", ToastKind.Error)
        }
        refreshProjects()
    }

    override fun exportProject(folder: String) {
        val name = state.projects.firstOrNull { it.folder == folder }?.name ?: folder
        activity.createDocument("$name.goni", "application/zip") { uri ->
            if (!check(engine.call("project.open", "folder" to folder))) return@createDocument
            val tmp = File(workspace, ".export.zip")
            try {
                if (!check(engine.call("project.exportZip", "path" to tmp.name))) return@createDocument
                activity.contentResolver.openOutputStream(uri)?.use { out -> tmp.inputStream().use { it.copyTo(out) } }
                toast("\"$name\" exportado", ToastKind.Success)
            } catch (e: Exception) {
                toast("Falha ao exportar: ${e.message}", ToastKind.Error)
            } finally {
                tmp.delete()
            }
        }
    }

    /**
     * Gera um APK instalável só com o jogo: o próprio app como modelo, o
     * jogo dentro, pacote e nome próprios, assinado com a chave do aparelho.
     */
    override fun exportApk(folder: String) {
        val name = state.projects.firstOrNull { it.folder == folder }?.name ?: folder
        activity.createDocument("$name.apk", "application/vnd.android.package-archive") { uri ->
            if (!check(engine.call("project.open", "folder" to folder))) return@createDocument
            val game = File(activity.cacheDir, "export.goni")
            game.delete()
            // O motor escreve relativo ao workspace; o resto roda fora da thread de UI.
            val staged = File(workspace, ".export-apk.zip")
            if (!check(engine.call("project.exportZip", "path" to staged.name))) return@createDocument
            staged.renameTo(game)
            toast("Gerando o APK de \"$name\"…")
            val template = File(activity.applicationInfo.sourceDir)
            Thread {
                val out = File(activity.cacheDir, "export.apk")
                val result = runCatching {
                    ApkExporter.export(
                        template = template,
                        templatePackage = activity.packageName,
                        templateLabel = TEMPLATE_LABEL,
                        game = game,
                        packageName = ApkExporter.packageFor(name),
                        label = name,
                        signer = KeystoreSigner.signer(),
                        out = out,
                    )
                    activity.contentResolver.openOutputStream(uri)?.use { dst -> out.inputStream().use { it.copyTo(dst) } }
                        ?: error("não foi possível gravar o arquivo")
                }
                game.delete()
                out.delete()
                main.post {
                    result.fold(
                        onSuccess = { toast("APK pronto: abra o arquivo para instalar \"$name\"", ToastKind.Success) },
                        onFailure = { toast("Falha ao gerar o APK: ${it.message}", ToastKind.Error) },
                    )
                }
            }.start()
        }
    }

    /**
     * Modo jogo independente: o APK traz assets/game.goni. Importa o jogo
     * (sempre a versão do APK) e começa jogando. Voltar fecha o app.
     */
    fun startStandalone(): Boolean {
        val bytes = runCatching { activity.assets.open(GAME_ASSET).use { it.readBytes() } }.getOrNull()
            ?: return false
        state.standalone = true
        workspace.listFiles()?.forEach { if (it.isDirectory) it.deleteRecursively() }
        val tmp = File(workspace, ".game.goni")
        tmp.writeBytes(bytes)
        val r = engine.call("project.importZip", "path" to tmp.name, "name" to "Jogo")
        tmp.delete()
        if (!check(r)) return true
        playProject(r.string.ifEmpty { "Jogo" })
        return true
    }

    override fun importProject() {
        activity.openDocument(arrayOf("application/zip", "application/octet-stream", "*/*")) { uri, displayName ->
            val tmp = File(workspace, ".import.zip")
            try {
                activity.contentResolver.openInputStream(uri)?.use { input -> tmp.outputStream().use { input.copyTo(it) } }
                val suggested = displayName.removeSuffix(".zip").removeSuffix(".goni").ifEmpty { "Importado" }
                val r = engine.call("project.importZip", "path" to tmp.name, "name" to suggested)
                if (check(r, "Jogo importado")) refreshProjects()
            } catch (e: Exception) {
                toast("Falha ao importar: ${e.message}", ToastKind.Error)
            } finally {
                tmp.delete()
            }
        }
    }

    override fun playProject(folder: String) {
        if (!check(engine.call("project.open", "folder" to folder))) return
        enterEditor()
        state.playerOnly = true
        if (!check(engine.call("play.start"))) {
            state.playerOnly = false
        }
    }

    private fun leavePlayer() {
        if (state.standalone) {
            activity.finish()
            return
        }
        state.playerOnly = false
        goHome()
    }

    // --- editor -----------------------------------------------------------------------

    fun autosave() {
        if (state.screen == Screen.Home || state.snapshot.playing) return
        if (state.screen is Screen.Script && state.scriptDirty) saveScript(quiet = true)
        if (state.snapshot.dirty) engine.call("project.save")
    }

    override fun goHome() {
        if (state.snapshot.playing) engine.call("play.stop")
        autosave()
        engine.call("audio.stop")
        state.playingAudio = null
        state.playerOnly = false
        state.sheet = null
        state.tab = null
        state.screen = Screen.Home
        refreshProjects()
    }

    override fun play() {
        autosave()
        engine.call("anim.previewStop")
        state.hud = HudModel()
        check(engine.call("play.start"))
    }

    override fun stop() {
        check(engine.call("play.stop"))
    }

    override fun setPaused(paused: Boolean) {
        engine.call("play.pause", "paused" to paused)
    }

    override fun undo() {
        check(engine.call("history.undo"))
        refreshInspector()
    }

    override fun redo() {
        check(engine.call("history.redo"))
        refreshInspector()
    }

    override fun save() {
        check(engine.call("project.save"), "Salvo")
    }

    override fun setTool(tool: Int) {
        engine.call("tool.set", "tool" to tool)
    }

    override fun toggleSnap() {
        val on = !state.snapshot.snapTranslate
        engine.call("snap.set", "translate" to on, "rotate" to on)
        toast(if (on) "Encaixe na grade ligado" else "Encaixe desligado")
    }

    override fun fitView() {
        engine.call("viewport.fit")
    }

    override fun openTab(tab: EditorTab?) {
        state.tab = tab
        when (tab) {
            EditorTab.Properties -> refreshInspector()
            EditorTab.Library -> refreshLibrary()
            else -> {}
        }
    }

    override fun togglePanelHeight() {
        state.panelTall = !state.panelTall
    }

    // --- entidades --------------------------------------------------------------------

    override fun select(id: Long) {
        engine.call("entity.select", "id" to id)
    }

    override fun createEntity(template: String) {
        state.sheet = null
        if (check(engine.call("entity.create", "template" to template))) {
            refreshInspector()
            if (template == "character") refreshLibrary()
        }
    }

    override fun renameEntity(id: Long, name: String) {
        if (name.isNotBlank()) check(engine.call("entity.rename", "id" to id, "name" to name.trim()))
    }

    override fun duplicateEntity(id: Long) {
        check(engine.call("entity.duplicate", "id" to id), "Duplicado")
    }

    override fun deleteEntity(id: Long) {
        if (check(engine.call("entity.delete", "id" to id))) toast("Excluído — use desfazer para voltar")
    }

    override fun reparentEntity(id: Long, parent: Long) {
        check(engine.call("entity.reparent", "id" to id, "parent" to parent))
    }

    fun refreshInspector() {
        val id = state.snapshot.selection
        if (id == 0L) {
            state.inspector = null
            return
        }
        val r = engine.call("inspector", "id" to id).obj ?: run {
            state.inspector = null
            return
        }
        val t = engine.call("transform.get", "id" to id).obj
        val p = t?.optJSONArray("p")
        val rot = t?.optJSONArray("r")
        val s = t?.optJSONArray("s")
        val comps = r.optJSONArray("components")?.objects().orEmpty().map { c ->
            ComponentModel(
                name = c.optString("name"),
                label = c.optString("label"),
                category = c.optString("category"),
                removable = c.optBoolean("removable"),
                fields = c.optJSONArray("fields")?.objects().orEmpty().map { f ->
                    FieldModel(
                        f.optString("path"),
                        f.optString("type"),
                        f.optString("value"),
                        f.optString("kind"),
                        f.optJSONArray("options")?.strings().orEmpty(),
                    )
                },
            )
        }
        val kind = state.snapshot.hierarchy.firstOrNull { it.id == id }?.kind ?: "empty"
        state.inspector = InspectorModel(
            id = id,
            name = r.optString("name"),
            kind = kind,
            transform = TransformModel(
                x = p?.optDouble(0)?.toFloat() ?: 0f,
                y = p?.optDouble(1)?.toFloat() ?: 0f,
                rotation = rot?.optDouble(2)?.toFloat() ?: 0f,
                scaleX = s?.optDouble(0)?.toFloat() ?: 1f,
                scaleY = s?.optDouble(1)?.toFloat() ?: 1f,
            ),
            components = comps,
        )
    }

    override fun setField(id: Long, component: String, path: String, value: String) {
        // Quadro novo de animação vem do seletor de imagens.
        if (component == ANIM_FRAME) {
            addFrame(path, value)
            return
        }
        // Trocar o script liga o componente ao arquivo (a fonte vem junto).
        val r = if (component == "eng::editor::NiScriptComponent" && path == "scriptAsset" && value.isNotEmpty()) {
            engine.call("script.assign", "id" to id, "name" to value)
        } else {
            engine.call("component.set", "id" to id, "component" to component, "path" to path, "value" to value)
        }
        check(r)
        refreshInspector()
    }

    override fun setTransform(id: Long, transform: TransformModel) {
        val cur = engine.call("transform.get", "id" to id).obj ?: return
        val p = cur.getJSONArray("p")
        val r = cur.getJSONArray("r")
        val s = cur.getJSONArray("s")
        check(
            engine.call(
                "transform.set",
                "id" to id,
                "p" to JSONArray(listOf(transform.x, transform.y, p.optDouble(2))),
                "r" to JSONArray(listOf(r.optDouble(0), r.optDouble(1), transform.rotation)),
                "s" to JSONArray(listOf(transform.scaleX, transform.scaleY, s.optDouble(2))),
            ),
        )
        refreshInspector()
    }

    override fun addComponent(id: Long, component: String) {
        val r = engine.call("component.add", "id" to id, "name" to component)
        if (check(r)) {
            val added = r.array.strings()
            if (added.size > 1) toast("Adicionado com dependências: " + added.joinToString { it.substringAfterLast("::") })
        }
        refreshInspector()
    }

    override fun removeComponent(id: Long, component: String) {
        check(engine.call("component.remove", "id" to id, "name" to component))
        refreshInspector()
    }

    override fun openCatalog() {
        val id = state.snapshot.selection
        state.catalog = engine.call("component.catalog", "id" to id).array.objects().map {
            CatalogItem(it.optString("name"), it.optString("label"), it.optString("category"), it.optString("dependency"))
        }
        state.sheet = Sheet.AddComponent
    }

    override fun openPicker(target: PickTarget) {
        val options = when (target.kind) {
            "texture" -> assetsOf("textures")
            "audio" -> assetsOf("audio")
            "material" -> engine.call("material.list").array.objects().map { AssetItem("materials", it.optString("name"), it.optString("shader")) }
            "script" -> engine.call("script.list").array.strings().map { AssetItem("scripts", it) }
            else -> emptyList()
        }
        state.sheet = Sheet.Picker(target, options)
    }

    override fun openColor(id: Long, component: String, path: String, hex: String) {
        state.sheet = Sheet.Color(id, component, path, hex)
    }

    // --- biblioteca -------------------------------------------------------------------

    private fun projectDir() = File(workspace, state.snapshot.projectFolder)

    private fun assetsOf(category: String): List<AssetItem> {
        val list = engine.call("asset.list", "category" to category).array.objects()
        return list.map { a ->
            val name = a.optString("name")
            val file = File(projectDir(), "assets/$category/$name")
            when (category) {
                "textures" -> AssetItem(category, name, thumbnail = thumb(file, 192))
                "audio" -> AssetItem(category, name, "%.1f KB".format(file.length() / 1024f))
                else -> AssetItem(category, name)
            }
        }
    }

    private fun refreshAllAssets() {
        state.assets = LibraryKind.values().flatMap { itemsFor(it) }
    }

    fun refreshLibrary() {
        val kind = state.library
        state.assets = state.assets.filter { it.category != kind.category } + itemsFor(kind)
    }

    private fun itemsFor(kind: LibraryKind): List<AssetItem> = when (kind) {
        LibraryKind.Images, LibraryKind.Sounds -> assetsOf(kind.category)
        LibraryKind.Scripts -> engine.call("script.list").array.strings().map { name ->
            val lines = runCatching { File(projectDir(), "assets/scripts/$name").readLines().size }.getOrDefault(0)
            AssetItem("scripts", name, "$lines linhas")
        }
        LibraryKind.Animations -> engine.call("anim.list").array.objects().map {
            animClips[it.optString("name")] = it.optString("clip")
            AssetItem("animations", it.optString("name"), "${it.optInt("frames")} quadros")
        }
        LibraryKind.Materials -> engine.call("material.list").array.objects().map {
            AssetItem("materials", it.optString("name"), if (it.optString("shader") == "unlit") "Sem luz" else "Recebe luz")
        }
    }

    override fun showLibrary(kind: LibraryKind) {
        state.library = kind
        refreshLibrary()
    }

    override fun importAsset(kind: LibraryKind) {
        val mimes = if (kind == LibraryKind.Sounds) {
            arrayOf("audio/wav", "audio/x-wav", "audio/wave", "audio/vnd.wave")
        } else {
            arrayOf("image/png", "image/jpeg")
        }
        activity.openDocument(mimes) { uri, displayName ->
            val tmpDir = File(workspace, ".import_tmp").apply { mkdirs() }
            val dest = File(tmpDir, displayName.ifEmpty { "arquivo" })
            try {
                activity.contentResolver.openInputStream(uri)?.use { input -> dest.outputStream().use { input.copyTo(it) } }
                val r = engine.call(
                    "asset.import",
                    "temp" to ".import_tmp/${dest.name}",
                    "category" to kind.category,
                    "name" to dest.nameWithoutExtension,
                )
                if (check(r, "Importado")) {
                    state.library = kind
                    refreshLibrary()
                }
            } catch (e: Exception) {
                toast("Falha ao importar: ${e.message}", ToastKind.Error)
            } finally {
                dest.delete()
            }
        }
    }

    override fun useAsset(item: AssetItem) {
        val sel = state.snapshot.selection
        when (item.category) {
            "textures" -> {
                if (sel == 0L) {
                    // Sem seleção: a imagem vira um sprite novo na cena.
                    val r = engine.call("entity.create", "template" to "sprite", "name" to item.name.substringBeforeLast('.'))
                    if (!check(r)) return
                    check(engine.call("texture.apply", "id" to r.long, "name" to item.name), "Sprite criado")
                } else {
                    check(engine.call("texture.apply", "id" to sel, "name" to item.name), "Imagem aplicada")
                }
                refreshInspector()
            }
            "scripts" -> if (sel != 0L) {
                check(engine.call("script.assign", "id" to sel, "name" to item.name), "Script anexado")
                refreshInspector()
            } else {
                openAsset(item)
            }
            "audio" -> toggleAudio(item.name)
            else -> openAsset(item)
        }
    }

    override fun renameAsset(item: AssetItem, newName: String) {
        val ext = item.name.substringAfter('.', "")
        val target = if (ext.isNotEmpty() && !newName.contains('.')) "$newName.$ext" else newName
        check(engine.call("asset.rename", "category" to item.category, "name" to item.name, "newName" to target))
        refreshLibrary()
    }

    override fun deleteAsset(item: AssetItem) {
        check(engine.call("asset.delete", "category" to item.category, "name" to item.name))
        refreshLibrary()
    }

    override fun createAsset(kind: LibraryKind, name: String) {
        when (kind) {
            LibraryKind.Scripts -> {
                val r = engine.call("script.create", "name" to name)
                if (check(r)) {
                    refreshLibrary()
                    openAsset(AssetItem("scripts", r.string))
                }
            }
            LibraryKind.Animations -> {
                if (check(engine.call("anim.create", "name" to name))) {
                    refreshLibrary()
                    val file = if (name.endsWith(".anim.json")) name else "$name.anim.json"
                    openAsset(AssetItem("animations", file))
                }
            }
            LibraryKind.Materials -> {
                if (check(engine.call("material.create", "name" to name))) {
                    refreshLibrary()
                    val file = if (name.endsWith(".mat.json")) name else "$name.mat.json"
                    openAsset(AssetItem("materials", file))
                }
            }
            else -> {}
        }
    }

    override fun openAsset(item: AssetItem) {
        when (item.category) {
            "scripts" -> {
                val r = engine.call("script.read", "name" to item.name)
                if (!check(r)) return
                state.scriptText = r.string
                state.scriptDiags = emptyList()
                state.scriptCompiled = null
                state.scriptDirty = false
                state.sheet = null
                state.screen = Screen.Script(item.name)
                compileScript()
            }
            "animations" -> openAnimation(item.name)
            "materials" -> {
                val r = engine.call("material.read", "name" to item.name)
                if (!check(r)) return
                val j = JSONObject(r.string)
                val tint = j.optJSONArray("tint")
                fun ch(i: Int) = ((tint?.optDouble(i, 1.0) ?: 1.0) * 255).toInt().coerceIn(0, 255).toLong()
                val argb = (ch(3) shl 24) or (ch(0) shl 16) or (ch(1) shl 8) or ch(2)
                state.sheet = Sheet.Material(MaterialModel(item.name, j.optString("shader") != "unlit", argb))
            }
            else -> state.sheet = Sheet.AssetMenu(item)
        }
    }

    override fun toggleAudio(name: String) {
        if (state.playingAudio == name) {
            engine.call("audio.stop")
            state.playingAudio = null
        } else if (check(engine.call("audio.preview", "name" to name))) {
            state.playingAudio = name
        }
    }

    // --- cenas e ajustes ----------------------------------------------------------------

    override fun openScenes() {
        state.scenes = engine.call("scene.list").array.strings()
        state.sheet = Sheet.Scenes
    }

    override fun newScene() {
        autosave()
        check(engine.call("scene.new"))
    }

    override fun loadScene(path: String) {
        if (path == state.snapshot.scenePath) return
        autosave()
        check(engine.call("scene.load", "path" to path))
        refreshInspector()
    }

    override fun saveSceneAs(name: String) {
        val path = name.trim().replace('/', '-').removeSuffix(".json") + ".json"
        check(engine.call("scene.save", "path" to path), "Cena salva")
    }

    private fun loadSettings() {
        val s = engine.call("settings.get").obj ?: return
        val grid = s.optJSONObject("grid") ?: JSONObject()
        val game = s.optJSONObject("game")
        val dt = s.optDouble("physicsDt", 1.0 / 60.0)
        state.settings = SettingsModel(
            projectName = state.snapshot.projectName,
            gridVisible = grid.optBoolean("visible", true),
            gridCell = grid.optDouble("cell", 1.0).toFloat(),
            physicsHz = if (dt > 0) Math.round(1.0 / dt).toInt() else 60,
            layers = s.optJSONArray("layers")?.objects().orEmpty().map {
                TickLayer(it.optString("name"), it.optDouble("timeScale", 1.0).toFloat(), it.optBoolean("update"), it.optBoolean("physics"), it.optBoolean("render"))
            },
            collisionLayers = s.optJSONArray("collisionLayers")?.objects().orEmpty().map {
                CollisionLayer(it.optString("name"), it.optLong("bit"))
            },
            background = game?.optJSONArray("background")?.let { bg ->
                fun ch(i: Int) = (bg.optDouble(i, 0.0).coerceIn(0.0, 1.0) * 255.0 + 0.5).toInt()
                String.format("#%02X%02X%02X", ch(0), ch(1), ch(2))
            } ?: "#12141C",
            orientation = game?.optString("orientation")?.ifEmpty { null } ?: "auto",
            controls = game?.optString("controls")?.ifEmpty { null } ?: "platformer",
            version = "Versão ${BuildConfig.VERSION_NAME}" + BuildConfig.GONI_COMMIT.take(7).let { if (it.isEmpty()) "" else " · $it" },
            backend = engine.call("host.info").obj?.optString("backend").orEmpty(),
        )
    }

    override fun openSettings() {
        loadSettings()
        state.sheet = Sheet.Settings
    }

    override fun setProjectName(name: String) {
        if (check(engine.call("project.rename", "name" to name))) {
            engine.call("project.save")
            state.snapshot = engine.pollSnapshot(force = true) ?: state.snapshot
            loadSettings()
        }
    }

    override fun setGrid(visible: Boolean, cell: Float) {
        check(engine.call("settings.grid", "visible" to visible, "cell" to cell))
        loadSettings()
    }

    override fun setGame(background: String, orientation: String, controls: String) {
        val rgb = background.removePrefix("#").toLongOrNull(16) ?: return
        val channels = JSONArray()
            .put(((rgb shr 16) and 0xFF) / 255.0)
            .put(((rgb shr 8) and 0xFF) / 255.0)
            .put((rgb and 0xFF) / 255.0)
        val r = engine.call("settings.game", "background" to channels, "orientation" to orientation, "controls" to controls)
        if (check(r)) {
            engine.call("project.save")
            state.snapshot = engine.pollSnapshot(force = true) ?: state.snapshot
        }
        loadSettings()
    }

    override fun setPhysicsHz(hz: Int) {
        check(engine.call("settings.physicsDt", "dt" to 1.0 / hz))
        loadSettings()
    }

    override fun addTickLayer(name: String) {
        check(engine.call("layer.add", "name" to name))
        loadSettings()
        state.sheet = Sheet.Settings
    }

    override fun setTickLayer(layer: TickLayer) {
        check(
            engine.call(
                "layer.set", "name" to layer.name, "timeScale" to layer.timeScale,
                "update" to layer.update, "physics" to layer.physics, "render" to layer.render,
            ),
        )
        loadSettings()
    }

    override fun addCollisionLayer(name: String) {
        check(engine.call("collision.add", "name" to name))
        loadSettings()
        state.sheet = Sheet.Settings
    }

    override fun renameCollisionLayer(bit: Long, name: String) {
        check(engine.call("collision.rename", "bit" to bit, "name" to name))
        loadSettings()
        state.sheet = Sheet.Settings
    }

    override fun exportDiagnostics() {
        activity.createDocument("goni-diagnostico.zip", "application/zip") { uri ->
            try {
                activity.contentResolver.openOutputStream(uri)?.use { Diagnostics.writeZip(activity, it) }
                toast("Diagnóstico exportado", ToastKind.Success)
            } catch (e: Exception) {
                toast("Falha: ${e.message}", ToastKind.Error)
            }
        }
    }

    // --- script -------------------------------------------------------------------------

    override fun editScriptText(text: String) {
        state.scriptText = text
        state.scriptDirty = true
        compileTask?.let(main::removeCallbacks)
        compileTask = Runnable { compileScript() }.also { main.postDelayed(it, 700) }
    }

    override fun compileScript() {
        val r = engine.call("script.compile", "source" to state.scriptText).obj ?: return
        state.scriptCompiled = r.optBoolean("ok")
        state.scriptDiags = r.optJSONArray("diags")?.objects().orEmpty().map {
            ScriptDiag(it.optInt("line"), it.optInt("col"), it.optString("message"))
        }
    }

    override fun saveScript() = saveScript(quiet = false)

    private fun saveScript(quiet: Boolean) {
        val name = (state.screen as? Screen.Script)?.name ?: return
        val r = engine.call("script.write", "name" to name, "content" to state.scriptText)
        if (if (quiet) r.ok else check(r, "Script salvo")) state.scriptDirty = false
    }

    override fun attachScript() {
        val name = (state.screen as? Screen.Script)?.name ?: return
        saveScript(quiet = true)
        check(engine.call("script.assign", "id" to state.snapshot.selection, "name" to name), "Anexado a ${state.snapshot.selectedNode?.name ?: "objeto"}")
    }

    override fun closeScript() {
        if (state.scriptDirty) saveScript(quiet = true)
        compileTask?.let(main::removeCallbacks)
        state.screen = Screen.Editor
        refreshLibrary()
        refreshInspector()
    }

    // --- animação e material ---------------------------------------------------------------

    private fun openAnimation(name: String) {
        val r = engine.call("anim.read", "name" to name)
        if (!check(r)) return
        val j = runCatching { JSONObject(r.string) }.getOrNull() ?: return
        val frames = j.optJSONArray("frames")?.let { arr ->
            (0 until arr.length()).mapNotNull { arr.optJSONArray(it)?.optString(1) }
        }.orEmpty()
        state.sheet = Sheet.Animation(AnimationModel(name, j.optDouble("fps", 8.0).toFloat(), j.optBoolean("loop", true), frames, r.string))
    }

    override fun saveAnimation(model: AnimationModel) {
        check(engine.call("anim.setMeta", "name" to model.name, "loop" to model.loop, "fps" to model.fps))
        openAnimation(model.name)
    }

    override fun addAnimationFrame(model: AnimationModel) {
        state.sheet = Sheet.Picker(PickTarget(0L, ANIM_FRAME, model.name, "texture"), assetsOf("textures"))
    }

    private fun addFrame(anim: String, texture: String) {
        if (texture.isNotEmpty()) check(engine.call("anim.addFrame", "name" to anim, "texture" to texture))
        refreshLibrary()
        main.post { openAnimation(anim) }
    }

    override fun previewAnimation(name: String) {
        val clip = animClips[name] ?: name
        check(engine.call("anim.preview", "id" to state.snapshot.selection, "clip" to clip))
        state.sheet = null
    }

    override fun assignAnimation(name: String) {
        check(engine.call("anim.assign", "id" to state.snapshot.selection, "name" to name), "Animação aplicada")
        refreshInspector()
    }

    override fun saveMaterial(model: MaterialModel) {
        val r = engine.call("material.read", "name" to model.name)
        if (!check(r)) return
        val j = JSONObject(r.string)
        j.put("shader", if (model.lit) "lit" else "unlit")
        val t = model.tint
        j.put(
            "tint",
            JSONArray(listOf(((t shr 16) and 0xFF) / 255.0, ((t shr 8) and 0xFF) / 255.0, (t and 0xFF) / 255.0, ((t shr 24) and 0xFF) / 255.0)),
        )
        check(engine.call("material.write", "name" to model.name, "json" to j.toString(2)), "Material salvo")
        refreshLibrary()
    }

    override fun dismissSheet() {
        state.sheet = null
    }

    /** Volta um nível. false = nada a fazer (a Activity fecha). */
    fun back(): Boolean {
        when {
            state.sheet != null -> state.sheet = null
            state.screen is Screen.Script -> closeScript()
            state.snapshot.playing && state.screen == Screen.Editor -> stop()
            state.tab != null -> state.tab = null
            state.screen == Screen.Editor -> goHome()
            else -> return false
        }
        return true
    }

    private companion object {
        const val ANIM_FRAME = "__anim_frame__"
        const val GAME_ASSET = "game.goni"
        /** Nome do app no manifesto (trocado pelo do jogo no export). */
        const val TEMPLATE_LABEL = "G.ONI"
    }
}
