package com.goni.app

import com.goni.ui.model.EditorSnapshot
import com.goni.ui.model.HierarchyNode
import org.json.JSONArray
import org.json.JSONObject

/** Resposta de uma operação do protocolo (docs/editor-protocol.md). */
class Reply(val ok: Boolean, val result: Any?, val error: String) {
    val obj: JSONObject? get() = result as? JSONObject
    val array: JSONArray get() = result as? JSONArray ?: JSONArray()
    val string: String get() = result as? String ?: ""
    val long: Long get() = (result as? Number)?.toLong() ?: 0L
}

/** Acesso tipado ao motor: uma sessão = um handle nativo. */
class Engine(val handle: Long) {
    private var lastKey = 0L

    fun call(op: String, vararg args: Pair<String, Any?>): Reply {
        val req = JSONObject().put("op", op)
        for ((k, v) in args) {
            req.put(k, v ?: JSONObject.NULL)
        }
        val text = NativeBridge.nativeCall(handle, req.toString())
        return try {
            val json = JSONObject(text)
            if (json.optBoolean("ok")) {
                Reply(true, json.opt("result").takeUnless { it == JSONObject.NULL }, "")
            } else {
                Reply(false, null, json.optString("error", "erro desconhecido"))
            }
        } catch (e: Exception) {
            Reply(false, null, "resposta inválida do motor")
        }
    }

    /** Estado novo, ou null se nada mudou desde a última leitura. */
    fun pollSnapshot(force: Boolean = false): EditorSnapshot? {
        if (force) lastKey = 0L
        val text = NativeBridge.nativeSnapshot(handle, lastKey) ?: return null
        val json = JSONObject(text)
        lastKey = json.optLong("key")
        return parseSnapshot(json)
    }

    private fun parseSnapshot(j: JSONObject): EditorSnapshot {
        val project = j.optJSONObject("project")
        val scene = j.optJSONObject("scene")
        val snap = j.optJSONObject("snap")
        val list = j.optJSONArray("hierarchy") ?: JSONArray()
        val nodes = ArrayList<HierarchyNode>(list.length())
        for (i in 0 until list.length()) {
            val n = list.getJSONObject(i)
            nodes += HierarchyNode(n.optLong("id"), n.optString("name"), n.optInt("depth"), n.optString("kind"))
        }
        return EditorSnapshot(
            projectName = project?.optString("name").orEmpty(),
            projectFolder = project?.optString("folder").orEmpty(),
            scenePath = scene?.optString("path").orEmpty(),
            dirty = scene?.optBoolean("dirty") ?: false,
            playing = j.optString("mode") == "play",
            paused = j.optBoolean("paused"),
            previewing = j.optBoolean("previewing"),
            selection = j.optLong("selection"),
            tool = j.optInt("tool"),
            snapTranslate = snap?.optBoolean("translate") ?: false,
            snapRotate = snap?.optBoolean("rotate") ?: false,
            canUndo = j.optBoolean("canUndo"),
            canRedo = j.optBoolean("canRedo"),
            hierarchy = nodes,
        )
    }
}

fun JSONArray.strings(): List<String> = (0 until length()).map { optString(it) }

fun JSONArray.objects(): List<JSONObject> = (0 until length()).mapNotNull { optJSONObject(it) }
