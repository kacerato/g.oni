# Build Android

```text
android/ui   interface Compose (módulo biblioteca, sem motor)
android/app  MainActivity + EditorController + NativeBridge (Kotlin)
             └─ externalNativeBuild (CMake do NDK)
                libgoni.so = EditorJni.cpp + editor/ + engine/
APK: arm64-v8a + x86_64, minSdk 24, sem permissões
```

Versões: AGP 8.5.2 · Kotlin 1.9.24 · Compose BOM 2024.06 · Gradle 8.10.2 ·
NDK 27.0.12077973 · CMake 3.31.6 · compileSdk/targetSdk 34.

## Pré-requisitos

- JDK 17
- Android SDK com:

```bash
sdkmanager "platforms;android-34" "build-tools;34.0.0" \
           "ndk;27.0.12077973" "cmake;3.31.6"
echo "sdk.dir=/caminho/do/android-sdk" > android/local.properties
```

## APK

```bash
cd android
./gradlew assembleDebug
# app/build/outputs/apk/debug/app-debug.apk
```

Sem acesso aos tarballs do GitHub (proxy/rede restrita), aponte fontes
locais das dependências do CMake:

```bash
export GONI_CMAKE_ARGS="-DFETCHCONTENT_SOURCE_DIR_STB_IMAGE=/src/stb \
  -DFETCHCONTENT_SOURCE_DIR_VULKANHEADERS=/src/Vulkan-Headers \
  -DFETCHCONTENT_SOURCE_DIR_EGLREGISTRY=/src/EGL-Registry \
  -DFETCHCONTENT_SOURCE_DIR_OPENGLREGISTRY=/src/OpenGL-Registry"
```

## Screenshots da interface

O módulo `:ui` não depende do motor, então as telas renderizam na JVM com
Paparazzi, sem aparelho:

```bash
ANDROID_HOME=/caminho/do/android-sdk ./gradlew :ui:recordPaparazziDebug
# android/ui/src/test/snapshots/images/*.png
```

O Paparazzi procura a plataforma 34 pelo `ANDROID_HOME`, não pelo
`local.properties`.

## Tema e fontes

`ui/theme/Theme.kt` concentra cores (`OniColors`), tipografia (`OniType`)
e formas (`OniShape`). As fontes ficam em `ui/src/main/res/font`: Space
Grotesk (títulos), Inter (texto) e JetBrains Mono (código). São cortes
estáticos, só com latim, gerados das variáveis do Google Fonts; as
licenças OFL estão em `ui/licenses/`.

Os dados de exemplo ficam em `ui/src/test/java/com/goni/ui/ScreenshotTest.kt`.

## Contrato JNI

Cada `external fun` de `NativeBridge.kt` precisa de um símbolo
`Java_com_goni_app_NativeBridge_<nome>` dentro do `extern "C"` de
`EditorJni.cpp`. O teste `[jni]` da suíte do editor lê o arquivo Kotlin e
confere todos por `dlsym`; o CI Android também recusa símbolos com nome
C++ (manglados).
