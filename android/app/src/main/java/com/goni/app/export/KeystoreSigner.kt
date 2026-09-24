package com.goni.app.export

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.Calendar
import javax.security.auth.x500.X500Principal

/**
 * Chave de assinatura dos jogos exportados, guardada no Android Keystore
 * deste aparelho (nunca sai dele). A mesma chave assina todo export, então
 * exportar de novo um jogo gera uma atualização que instala por cima.
 */
object KeystoreSigner {
    private const val ALIAS = "goni-export"
    private const val PROVIDER = "AndroidKeyStore"

    fun signer(): ApkExporter.Signer {
        val store = KeyStore.getInstance(PROVIDER).apply { load(null) }
        if (!store.containsAlias(ALIAS)) {
            val start = Calendar.getInstance()
            val end = Calendar.getInstance().apply { add(Calendar.YEAR, 30) }
            val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setKeySize(2048)
                .setCertificateSubject(X500Principal("CN=G.ONI, O=Jogos criados no G.ONI"))
                .setCertificateSerialNumber(BigInteger.ONE)
                .setCertificateNotBefore(start.time)
                .setCertificateNotAfter(end.time)
                .build()
            KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, PROVIDER).apply {
                initialize(spec)
                generateKeyPair()
            }
        }
        val key = store.getKey(ALIAS, null) as PrivateKey
        val cert = store.getCertificate(ALIAS) as X509Certificate
        return ApkExporter.Signer(key, listOf(cert))
    }
}
