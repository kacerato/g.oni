package com.goni.app.export

import com.android.apksig.ApkSigner
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/**
 * Gera o APK independente de um jogo a partir do APK do próprio G.ONI:
 * copia o app, põe o jogo em `assets/game.goni`, troca o pacote e o nome
 * no manifesto binário e assina de novo. Ao abrir, o app encontra o jogo e
 * vai direto para ele (MainActivity).
 *
 * Só usa java.util.zip e apksig, então roda igual no aparelho e nos testes
 * da JVM.
 */
object ApkExporter {
    const val GAME_ASSET = "assets/game.goni"
    private const val MANIFEST = "AndroidManifest.xml"

    class Signer(val privateKey: PrivateKey, val certificates: List<X509Certificate>)

    /**
     * @param template APK instalado do G.ONI (applicationInfo.sourceDir).
     * @param templatePackage applicationId do template ("com.goni.runtime").
     * @param templateLabel nome do app no manifesto do template ("G.ONI").
     */
    fun export(
        template: File,
        templatePackage: String,
        templateLabel: String,
        game: File,
        packageName: String,
        label: String,
        signer: Signer,
        out: File,
    ) {
        require(isValidPackage(packageName)) { "pacote inválido: $packageName" }
        val unsigned = File(out.parentFile, out.name + ".unsigned")
        try {
            ZipFile(template).use { zip ->
                ZipOutputStream(unsigned.outputStream().buffered()).use { zos ->
                    for (entry in zip.entries()) {
                        val name = entry.name
                        if (name == GAME_ASSET || isOldSignature(name)) continue
                        var data = zip.getInputStream(entry).use { it.readBytes() }
                        if (name == MANIFEST) {
                            data = rewriteManifest(data, templatePackage, packageName, templateLabel, label)
                        }
                        zos.putNextEntry(copyEntry(name, entry.method, data))
                        zos.write(data)
                        zos.closeEntry()
                    }
                    val gameData = game.readBytes()
                    zos.putNextEntry(copyEntry(GAME_ASSET, ZipEntry.STORED, gameData))
                    zos.write(gameData)
                    zos.closeEntry()
                }
            }
            val config = ApkSigner.SignerConfig.Builder("goni", signer.privateKey, signer.certificates).build()
            ApkSigner.Builder(listOf(config))
                .setInputApk(unsigned)
                .setOutputApk(out)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .build()
                .sign()
        } finally {
            unsigned.delete()
        }
    }

    /** Pacote estável para um jogo: "com.goni.game.<nome>_<hash>". */
    fun packageFor(gameName: String): String {
        val slug = gameName.lowercase()
            .map { c ->
                when (c) {
                    in 'a'..'z', in '0'..'9' -> c
                    'á', 'à', 'â', 'ã', 'ä' -> 'a'
                    'é', 'ê', 'ë', 'è' -> 'e'
                    'í', 'î', 'ï', 'ì' -> 'i'
                    'ó', 'ô', 'õ', 'ö', 'ò' -> 'o'
                    'ú', 'û', 'ü', 'ù' -> 'u'
                    'ç' -> 'c'
                    else -> '_'
                }
            }
            .joinToString("")
            .split('_').filter { it.isNotEmpty() }.joinToString("_")
            .take(24)
            .ifEmpty { "jogo" }
        val safe = if (slug.first().isDigit()) "j$slug" else slug
        val hash = (gameName.hashCode().toLong() and 0xFFFFFFFFL).toString(36)
        return "com.goni.game.${safe}_$hash"
    }

    fun isValidPackage(name: String): Boolean =
        Regex("[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)+").matches(name)

    private fun isOldSignature(name: String): Boolean {
        if (!name.startsWith("META-INF/")) return false
        val upper = name.uppercase()
        return upper == "META-INF/MANIFEST.MF" || upper.endsWith(".SF") ||
            upper.endsWith(".RSA") || upper.endsWith(".DSA") || upper.endsWith(".EC")
    }

    private fun copyEntry(name: String, method: Int, data: ByteArray): ZipEntry {
        val e = ZipEntry(name)
        if (method == ZipEntry.STORED) {
            e.method = ZipEntry.STORED
            e.size = data.size.toLong()
            e.compressedSize = data.size.toLong()
            e.crc = CRC32().apply { update(data) }.value
        } else {
            e.method = ZipEntry.DEFLATED
        }
        return e
    }

    // --- manifesto binário (AXML) --------------------------------------------------------

    private const val RES_STRING_POOL_TYPE = 0x0001
    private const val UTF8_FLAG = 1 shl 8

    /**
     * Troca strings no pool do manifesto binário: o pacote (e tudo que começa
     * com "pacote." — authorities de providers, permissões internas) e o nome
     * do app. Os nós do XML referenciam strings por índice, então só o pool
     * é reescrito; o resto do arquivo segue igual.
     */
    fun rewriteManifest(
        axml: ByteArray,
        oldPackage: String,
        newPackage: String,
        oldLabel: String,
        newLabel: String,
    ): ByteArray {
        val input = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN)
        val fileHeaderSize = input.getShort(2).toInt() and 0xFFFF
        val poolStart = fileHeaderSize
        require(input.getShort(poolStart).toInt() and 0xFFFF == RES_STRING_POOL_TYPE) {
            "manifesto sem pool de strings"
        }
        val poolHeaderSize = input.getShort(poolStart + 2).toInt() and 0xFFFF
        val poolSize = input.getInt(poolStart + 4)
        val stringCount = input.getInt(poolStart + 8)
        val styleCount = input.getInt(poolStart + 12)
        val flags = input.getInt(poolStart + 16)
        val stringsStart = input.getInt(poolStart + 20)
        val stylesStart = input.getInt(poolStart + 24)
        val utf8 = flags and UTF8_FLAG != 0

        val offsetsAt = poolStart + poolHeaderSize
        val strings = (0 until stringCount).map { i ->
            val at = poolStart + stringsStart + input.getInt(offsetsAt + i * 4)
            if (utf8) readUtf8(axml, at) else readUtf16(input, at)
        }
        val replaced = strings.map { s ->
            when {
                s == oldPackage -> newPackage
                s.startsWith("$oldPackage.") -> newPackage + s.substring(oldPackage.length)
                s == oldLabel -> newLabel
                else -> s
            }
        }

        // Dados novos das strings + offsets.
        val data = ByteArrayOutputStream()
        val offsets = IntArray(stringCount)
        for ((i, s) in replaced.withIndex()) {
            offsets[i] = data.size()
            if (utf8) writeUtf8(data, s) else writeUtf16(data, s)
        }
        while (data.size() % 4 != 0) data.write(0)
        val stringData = data.toByteArray()

        // Estilos (raros no manifesto) copiados como estão.
        val styleOffsetsBytes = styleCount * 4
        val stylesData = if (styleCount > 0) {
            axml.copyOfRange(poolStart + stylesStart, poolStart + poolSize)
        } else {
            ByteArray(0)
        }

        val newStringsStart = poolHeaderSize + stringCount * 4 + styleOffsetsBytes
        val newStylesStart = if (styleCount > 0) newStringsStart + stringData.size else 0
        val newPoolSize = newStringsStart + stringData.size + stylesData.size

        val pool = ByteBuffer.allocate(newPoolSize).order(ByteOrder.LITTLE_ENDIAN)
        pool.put(axml, poolStart, poolHeaderSize)
        pool.putInt(4, newPoolSize)
        pool.putInt(20, newStringsStart)
        pool.putInt(24, newStylesStart)
        pool.position(poolHeaderSize)
        for (o in offsets) pool.putInt(o)
        pool.put(axml, offsetsAt + stringCount * 4, styleOffsetsBytes)
        pool.put(stringData)
        pool.put(stylesData)

        val rest = axml.copyOfRange(poolStart + poolSize, axml.size)
        val result = ByteBuffer.allocate(fileHeaderSize + newPoolSize + rest.size).order(ByteOrder.LITTLE_ENDIAN)
        result.put(axml, 0, fileHeaderSize)
        result.putInt(4, fileHeaderSize + newPoolSize + rest.size)
        result.position(fileHeaderSize)
        result.put(pool.array())
        result.put(rest)
        return result.array()
    }

    /** Todas as strings do pool (para diagnóstico e testes). */
    fun manifestStrings(axml: ByteArray): List<String> {
        val input = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN)
        val poolStart = input.getShort(2).toInt() and 0xFFFF
        val poolHeaderSize = input.getShort(poolStart + 2).toInt() and 0xFFFF
        val stringCount = input.getInt(poolStart + 8)
        val utf8 = input.getInt(poolStart + 16) and UTF8_FLAG != 0
        val stringsStart = input.getInt(poolStart + 20)
        return (0 until stringCount).map { i ->
            val at = poolStart + stringsStart + input.getInt(poolStart + poolHeaderSize + i * 4)
            if (utf8) readUtf8(axml, at) else readUtf16(input, at)
        }
    }

    private fun readUtf16(buf: ByteBuffer, at: Int): String {
        var pos = at
        var len = buf.getShort(pos).toInt() and 0xFFFF
        pos += 2
        if (len and 0x8000 != 0) {
            len = ((len and 0x7FFF) shl 16) or (buf.getShort(pos).toInt() and 0xFFFF)
            pos += 2
        }
        val chars = CharArray(len) { i -> buf.getChar(pos + i * 2) }
        return String(chars)
    }

    private fun writeUtf16(out: ByteArrayOutputStream, s: String) {
        val len = s.length
        if (len > 0x7FFF) {
            writeShort(out, 0x8000 or (len ushr 16))
            writeShort(out, len and 0xFFFF)
        } else {
            writeShort(out, len)
        }
        for (c in s) writeShort(out, c.code)
        writeShort(out, 0)
    }

    private fun readUtf8(data: ByteArray, at: Int): String {
        var pos = at
        // Tamanho em caracteres (ignorado) e depois em bytes, 1 ou 2 bytes cada.
        pos += if (data[pos].toInt() and 0x80 != 0) 2 else 1
        var len = data[pos].toInt() and 0xFF
        if (len and 0x80 != 0) {
            len = ((len and 0x7F) shl 8) or (data[pos + 1].toInt() and 0xFF)
            pos += 2
        } else {
            pos += 1
        }
        return String(data, pos, len, Charsets.UTF_8)
    }

    private fun writeUtf8(out: ByteArrayOutputStream, s: String) {
        val bytes = s.toByteArray(Charsets.UTF_8)
        writeLen8(out, s.length)
        writeLen8(out, bytes.size)
        out.write(bytes)
        out.write(0)
    }

    private fun writeLen8(out: ByteArrayOutputStream, len: Int) {
        if (len > 0x7F) {
            out.write(0x80 or ((len ushr 8) and 0x7F))
            out.write(len and 0xFF)
        } else {
            out.write(len)
        }
    }

    private fun writeShort(out: ByteArrayOutputStream, v: Int) {
        out.write(v and 0xFF)
        out.write((v ushr 8) and 0xFF)
    }
}
