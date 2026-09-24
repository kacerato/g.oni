package com.goni.app.export

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/**
 * Exporta um jogo usando o APK de debug do próprio app como modelo e
 * confere o resultado com as ferramentas do SDK (aapt2, apksigner,
 * zipalign) — as mesmas verificações que o instalador do Android faz.
 */
class ApkExporterTest {
    private val template = File("build/outputs/apk/debug/app-debug.apk")
    private val buildTools: File? = System.getenv("ANDROID_HOME")
        ?.let { File(it, "build-tools").listFiles()?.maxByOrNull { f -> f.name } }

    @Test
    fun packageNamesAreValidAndStable() {
        val a = ApkExporter.packageFor("Voo")
        assertEquals(a, ApkExporter.packageFor("Voo"))
        assertTrue(a, ApkExporter.isValidPackage(a))
        assertTrue(a.startsWith("com.goni.game.voo_"))
        for (name in listOf("Chuva de caixas!", "3 em linha", "Ação É Já", "", "////")) {
            val p = ApkExporter.packageFor(name)
            assertTrue("$name -> $p", ApkExporter.isValidPackage(p))
        }
        assertFalse(ApkExporter.packageFor("Voo") == ApkExporter.packageFor("Voo 2"))
    }

    @Test
    fun manifestRewriteKeepsEverythingElse() {
        assumeTrue("rode :app:assembleDebug antes", template.exists())
        val original = ZipFile(template).use { z -> z.getInputStream(z.getEntry("AndroidManifest.xml")).readBytes() }
        val before = ApkExporter.manifestStrings(original)
        assertTrue(before.contains("com.goni.runtime"))
        val rewritten = ApkExporter.rewriteManifest(original, "com.goni.runtime", "com.goni.game.voo_x1", "G.ONI", "Voo Ação")
        val after = ApkExporter.manifestStrings(rewritten)
        assertEquals(before.size, after.size)
        assertFalse(after.any { it == "com.goni.runtime" || it.startsWith("com.goni.runtime.") })
        assertTrue(after.contains("com.goni.game.voo_x1"))
        assertTrue(after.contains("Voo Ação"))
        // Nada além do pacote e do nome mudou.
        for ((b, a) in before.zip(after)) {
            if (b != a) assertTrue("$b -> $a", b == "G.ONI" || b.startsWith("com.goni.runtime"))
        }
    }

    @Test
    fun exportedApkInstallsAsItsOwnApp() {
        assumeTrue("rode :app:assembleDebug antes", template.exists())
        val tools = buildTools
        assumeTrue("defina ANDROID_HOME", tools != null)
        val dir = Files.createTempDirectory("goni-export").toFile()
        try {
            val game = File(dir, "voo.goni")
            ZipOutputStream(game.outputStream()).use { zos ->
                zos.putNextEntry(ZipEntry("Voo/project.goni.json"))
                zos.write("{\"name\":\"Voo\"}".toByteArray())
                zos.closeEntry()
            }
            val out = File(dir, "voo.apk")
            val pkg = ApkExporter.packageFor("Voo")
            ApkExporter.export(template, "com.goni.runtime", "G.ONI", game, pkg, "Voo", testSigner(dir), out)

            val badging = run(File(tools, "aapt2").path, "dump", "badging", out.path)
            assertTrue(badging, badging.contains("package: name='$pkg'"))
            assertTrue(badging, badging.contains("application-label:'Voo'"))
            assertTrue(badging, badging.contains("launchable-activity: name='com.goni.app.MainActivity'"))

            val xml = run(File(tools, "aapt2").path, "dump", "xmltree", "--file", "AndroidManifest.xml", out.path)
            assertFalse("sobrou o pacote do editor:\n$xml", xml.contains("com.goni.runtime"))

            val verify = run(File(tools, "apksigner").path, "verify", "--min-sdk-version", "24", "-v", out.path)
            // minSdk 24: v2/v3 bastam (o apksig nem gera v1 nesse caso).
            assertTrue(verify, verify.contains("Verified using v2 scheme (APK Signature Scheme v2): true"))
            assertTrue(verify, verify.contains("Verified using v3 scheme (APK Signature Scheme v3): true"))

            run(File(tools, "zipalign").path, "-c", "-p", "4", out.path)

            ZipFile(out).use { z ->
                val entry = z.getEntry(ApkExporter.GAME_ASSET)
                assertTrue(entry != null)
                assertEquals(game.length(), entry.size)
                // O mesmo código nativo do editor vai junto.
                assertTrue(z.entries().toList().any { it.name.endsWith("libgoni.so") })
            }
        } finally {
            dir.deleteRecursively()
        }
    }

    /**
     * Amostra instalável: GONI_SAMPLE_GAME=voo.goni GONI_SAMPLE_APK=Voo.apk
     * gera o APK de um jogo real (chave descartável de teste).
     */
    @Test
    fun sampleApkFromRealGame() {
        val game = System.getenv("GONI_SAMPLE_GAME")?.let(::File)
        val out = System.getenv("GONI_SAMPLE_APK")?.let(::File)
        assumeTrue(game != null && out != null && template.exists())
        val name = System.getenv("GONI_SAMPLE_NAME") ?: "Voo"
        val dir = Files.createTempDirectory("goni-sample").toFile()
        try {
            ApkExporter.export(template, "com.goni.runtime", "G.ONI", game!!, ApkExporter.packageFor(name), name, testSigner(dir), out!!)
        } finally {
            dir.deleteRecursively()
        }
    }

    private fun testSigner(dir: File): ApkExporter.Signer {
        val ks = File(dir, "test.p12")
        run(
            "keytool", "-genkeypair", "-keystore", ks.path, "-storetype", "PKCS12",
            "-storepass", "goni123", "-keypass", "goni123", "-alias", "t", "-keyalg", "RSA",
            "-keysize", "2048", "-validity", "3650", "-dname", "CN=G.ONI teste",
        )
        val store = KeyStore.getInstance("PKCS12")
        ks.inputStream().use { store.load(it, "goni123".toCharArray()) }
        val key = store.getKey("t", "goni123".toCharArray()) as PrivateKey
        val cert = store.getCertificate("t") as X509Certificate
        return ApkExporter.Signer(key, listOf(cert))
    }

    private fun run(vararg cmd: String): String {
        val p = ProcessBuilder(*cmd).redirectErrorStream(true).start()
        val text = p.inputStream.bufferedReader().readText()
        val code = p.waitFor()
        assertEquals("${cmd.joinToString(" ")}\n$text", 0, code)
        return text
    }
}
