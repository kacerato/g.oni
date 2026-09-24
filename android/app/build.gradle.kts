// App G.ONI: Activity + controlador (Kotlin) sobre a libgoni.so, que o CMake
// do NDK constrói a partir dos mesmos fontes do engine. A interface vive no
// módulo :ui.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.goni.app"
    compileSdk = 34
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.goni.runtime"
        minSdk = 24
        targetSdk = 34
        // applicationId mantido: o APK novo instala por cima do antigo.
        versionCode = 80
        versionName = "0.8.0"
        // Hash do commit (injetado pelo CI; vazio em builds locais).
        buildConfigField("String", "GONI_COMMIT",
            "\"${System.getenv("GONI_COMMIT") ?: ""}\"")
        // arm64 para aparelhos, x86_64 para o emulador.
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
        compose = true
    }
    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
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
            isMinifyEnabled = false
        }
    }
}

dependencies {
    implementation(project(":ui"))
    implementation("androidx.activity:activity-compose:1.9.0")
}
