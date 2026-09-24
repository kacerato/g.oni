// Módulo app do runtime G.ONI (FASE 7, missão §XXI/§XXII/§XXIII).
//
// Toda a parte C++ é construída pelo CMake do NDK (externalNativeBuild)
// reutilizando os targets do engine — nenhum fonte duplicado; o APK embute
// lib/arm64-v8a/libgoni.so com eng::rhi + backends Vulkan/GLES + runtime.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.goni.runtime"
    compileSdk = 34
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.goni.runtime"
        minSdk = 24
        targetSdk = 34
        // P4.7.0 Bloco 0 — fase/versionamento centralizados aqui; a UI lê
        // SEMPRE de BuildConfig (zero hardcode).
        versionCode = 70
        versionName = "0.7.0"
        // BuildConfig explícito (AGP 8 desliga por default) —
        // PHASE_LABEL/BUILD_*/GONI_COMMIT alimentam splash-caption e a linha
        // "Versão" da sheet de Configurações.
        buildConfigField("String", "PHASE_LABEL", "\"P4.7.0\"")
        buildConfigField("String", "BUILD_NAME", "\"0.7.0\"")
        buildConfigField("int", "BUILD_CODE", "70")
        // Hash de commit é OPCIONAL via CI (export GONI_COMMIT=<sha> antes
        // de ./gradlew); builds locais ficam com string vazia.
        buildConfigField("String", "GONI_COMMIT",
            "\"${System.getenv("GONI_COMMIT") ?: ""}\"")
        // Backend por argumento (missão §XV): intent extra "backend"
        // ∈ {auto, vulkan, gles}; default auto.
        //
        // P3.1: x86_64 ADICIONADO ao lado do arm64-v8a (nada removido).
        // O MESMO APK passa a ser executável no Android Emulator x86_64
        // (validação automatizada BUILD→INSTALL→LAUNCH→OBSERVE) E nos
        // dispositivos físicos arm64 (Realme C33). Não é uma "versão de
        // teste": é o artefato único com cobertura de ABI estendida.
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_BUILD_TYPE=Release",
                )
                // Ambientes sem acesso aos tarballs do FetchContent podem
                // apontar fontes locais (ex.: -DFETCHCONTENT_SOURCE_DIR_X=…).
                System.getenv("GONI_CMAKE_ARGS")?.split(' ')
                    ?.filter { it.isNotBlank() }
                    ?.let { arguments += it }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = false  // runtime nativo mínimo — sem R8
        }
    }
}

dependencies {
    // Intencionalmente VAZIO (missão §XXII): Activity pura + Kotlin stdlib
    // trazida pelo plugin. Nenhuma lib de terceiros.
}
