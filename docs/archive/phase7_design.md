> **CORREÇÃO (auditoria final 4–10):** os arquivos são
> `LogcatSink.hpp`/`TriangleDemoShaders.hpp` (header-only) — não
> `LogcatSink.cpp`/`TriangleDemo.hpp`; ambas as activities estão
> `exported="true"` no manifest (LAUNCHER exige). Bugs C-1 (overflow JNI
> de string MUTF-8) e C-2 (vazamento de referência ANativeWindow por
> ciclo) corrigidos na remediação. Ver `docs/final_phase4_10_audit.md`.

# Design FASE 7 — Android runtime

- **Base:** `35ac978` + auditoria `docs/phase7_audit.md`
- **Escopo (§XXXVII):** SOMENTE o runtime Android. Nada de editor/física/
  áudio/render graph/material/etc.

## 1. Arquitetura

```text
Kotlin (GoniActivity + GoniRuntime)          ── UI, lifecycle, SurfaceView
        ↓ JNI (única fronteira — android/app/.../cpp/GoniJni.cpp)
AndroidRuntime (C++, android/runtime/)      ── estados, ownership, demo
        ↓
eng::rhi (Renderer/Frame — FASE 4, intocada)
      ↙                    ↘
eng::rhi::vulkan       eng::rhi::gles        ── + surface Android (#ifdef)
```

Regras herdadas e mantidas: `android/` é consumidor de `engine/`; `jni.h`
existe em EXATAMENTE um arquivo (`GoniJni.cpp`); backends recebem a surface
como `NativeWindowHandle` opaca; zero JNI em `eng::*` (§II.4).

## 2. Layout de arquivos

```text
android/
  runtime/                          # C++ (compila no Linux p/ testes E no APK)
    include/eng/android/AndroidRuntime.hpp
    src/AndroidRuntime.cpp          # state machine + demo + ownership
    src/LogcatSink.cpp              # __android_log_print (guard __ANDROID__)
    src/TriangleDemo.hpp           # GLSL/SPIR-V embutidos (cópias com
                                    #   proveniência de tests/shaders)
    tests/AndroidRuntimeTests.cpp   # Catch2 — roda NO LINUX (headless)
    CMakeLists.txt
  app/
    build.gradle.kts                # AGP, NDK, CMake, arm64-v8a
    src/main/AndroidManifest.xml    # mínimo, sem permissões
    src/main/java/com/goni/runtime/GoniActivity.kt
    src/main/java/com/goni/runtime/GoniRuntime.kt
    src/main/res/values/themes.xml
    src/main/cpp/CMakeLists.txt     # add_subdirectory(engine) — sem duplicar
    src/main/cpp/GoniJni.cpp        # ÚNICO arquivo com jni.h
  build.gradle.kts                  # raiz (plugins AGP false)
  settings.gradle.kts               # inclui :app; repo google()/mavenCentral()
  gradle.properties                 # heap limitado (3 GB RAM)
```

## 3. JNI (§II)

`GoniRuntime.kt` (object) com `System.loadLibrary("goni")`:

```kotlin
object GoniRuntime {
    init { System.loadLibrary("goni") }
    external fun nativeCreate(backend: String): Long
    external fun nativeDestroy(handle: Long)
    external fun nativeSurfaceCreated(handle: Long, surface: Surface)
    external fun nativeSurfaceChanged(handle: Long, width: Int, height: Int)
    external fun nativeSurfaceDestroyed(handle: Long)
    external fun nativeOnPause(handle: Long); external fun nativeOnResume(handle: Long)
    external fun nativeSetBackend(handle: Long, backend: String)
    external fun nativeRenderFrame(handle: Long): Boolean  // true = desenhou
}
```

- `GoniJni.cpp` exporta `Java_com_goni_runtime_GoniRuntime_nativeXxx` —
  nomes derivam do package `com.goni.runtime` (verificáveis por `nm`).
- Handle = `intptr_t` do `AndroidRuntime*` — a ÚNICA referência C++ que
  cruza a fronteira (§II.6). Strings entram como `jstring` e são copiadas
  para `char[]` no TU JNI — nenhum `std::string`/`std::vector` exposto.
- `nativeSurfaceCreated` converte `Surface` → `ANativeWindow*` via
  `ANativeWindow_fromSurface` (NDK) e entrega o ponteiro adquirido.

## 4. Estados e ownership (§IV/§V/§VI)

```text
enum class SurfaceState { NoSurface, Available, ChangedPending, Destroyed }
bool paused_ (independente da surface — §VI)
```

| Evento | Ação |
|---|---|
| `surfaceCreated(window)` | `ANativeWindow_acquire` (Android); cria `Renderer` (seleção de backend) + demo; `Available`; log Backend selected/API/vendor/renderer |
| `surfaceChanged(w,h)` | `ChangedPending` (aplica no próximo frame: `renderer->resize(w,h)`) |
| `surfaceDestroyed` | destrói `Renderer` (backend libera tudo — ADR-035) e demo; `ANativeWindow_release`; ponteiro zerado ANTES (sem dangling); `Destroyed` |
| `onPause/onResume` | apenas flag `paused_` (qualquer ordem de chegada é segura) |
| `renderFrame` | só se `Available/ChangedPending && !paused_`; aplica resize pendente; desenha triangle; retorna `false` caso contrário (sem trabalho gráfico — §VIII) |

Invariante ANativeWindow (§V): o runtime é o único owner entre created→
destroyed; o renderer NUNCA sobrevive à janela (destruído primeiro);
recriação de surface = novo `Renderer` (caminho já testado nas FASES 5/6).
`NO_SURFACE` jamais renderiza (§VIII).

## 5. Render thread (§VII) — decisão ADR-039

Render na **thread UI dirigida por Choreographer** (Kotlin):
- EGL: contexto `eglMakeCurrent` na thread que renderiza — a UI thread; o
  loop para em `onPause` (nenhum acesso concorrente);
- Vulkan: todas as chamadas na mesma thread (contrato single-threaded
  ADR-035 atendido);
- sem threads nativas próprias, sem trancos, sem condição de corrida —
  "a arquitetura mais simples que mantém lifecycle correto" (§VII).
- `nativeRenderFrame` é idempotente e seguro em qualquer estado.

## 6. Backend selection (§XIV/XV)

- Config por intent extra `"backend"` ∈ {`auto`,`vulkan`,`gles`} (default
  `auto`) — lida no `onCreate`, repassada em `nativeCreate`.
- A seleção/validação real é a da FASE 4 (ADR-036): Auto tenta
  Vulkan→GLES com validação completa e motivos; explícito sem fallback
  silencioso; `allowFallback=false`.
- Registro no log (§XVIII): requested/selected/reason/apiVersion/vendor/
  renderer/driver/presentation — tudo de `RendererCapabilities` (REAL).

## 7. Superfícies nos backends (lacunas V1/V2/G1/G2 da auditoria)

### Vulkan (`#ifdef __ANDROID__` em VulkanBackend.cpp)

`VK_USE_PLATFORM_ANDROID_KHR` (definido antes de vulkan.h apenas sob
`__ANDROID__`) + `PFN_vkCreateAndroidSurfaceKHR` na tabela; branch
`case NativeWindowKind::Android`: `VkAndroidSurfaceCreateInfoKHR{.window =
static_cast<ANativeWindow*>(handle)}` → `vkCreateAndroidSurfaceKHR`.
Extensão validada contra as reportadas (erro preciso se ausente — §X).
`surfaceKindExtension` já lista `VK_KHR_android_surface`.

### GLES (`#ifdef __ANDROID__` em GlesBackend.cpp)

- Display: `eglGetDisplay(EGL_DEFAULT_DISPLAY)` (nova `EglGetDisplayFn`
  na tabela do loader; usada só sob `__ANDROID__`).
- Config com surface: `EGL_WINDOW_BIT` (Linux headless mantém `EGL_PBUFFER_BIT`).
- Surface: `eglCreateWindowSurface(display, config, window, NULL)` (nova
  `EglCreateWindowSurfaceFn`).
- Versões/validações: fluxo 3.2→3.1→3.0 e checagens existentes intactas.

### Não-alvos

`eng::rhi` (abstraction) NÃO muda — kinds já existem. Nenhum header
Android em `engine/rhi/include/`. `android/native_window.h`/`android/log.h`
(NDK, não JNI) aparecem apenas em backends sob `__ANDROID__` e em
`android/`.

## 8. Demo (§XVI/XVII)

`TriangleDemo` (android/runtime): mesmo vertex data/formato das FASES
5/6 (pos+cor vec4, stride 32), shaders embutidos com proveniência
(`tests/shaders/triangle_{vk,gles}.*`): GLSL ES para o backend GLES e
SPIR-V para o Vulkan (cópia dos arrays já validados). Um único pipeline
criado após a seleção do backend; `renderFrame` =
begin→clear(azul-escuro)→viewport→pipeline→vbo→draw(3)→end→present.
`RendererConfig::applicationName = "goni.triangle"`.

## 9. CMake Android (§XXIII)

`android/app/src/main/cpp/CMakeLists.txt`:
`cmake_minimum_required(3.28)` → `project(goni CXX)` → MODULE_PATH para
`<repo>/cmake` → includes EngineOptions/Warnings/Sanitizers/Dependencies →
`ENG_BUILD_TESTS=OFF` → `add_subdirectory(<repo>/engine engine)` →
`add_library(goni SHARED GoniJni.cpp <runtime sources>)` linkando
`eng::rhi eng::rhi::vulkan eng::rhi::gles eng::log eng::core` + `-landroid
-llog` (só em Android). Fontes NUNCA duplicadas; o runtime também compila
no Linux (testes) via seu próprio CMakeLists com Catch2.

## 10. Gradle/AGP (§XXI/§XXII/§XXXI — versões fixadas)

| Peça | Versão |
|---|---|
| compileSdk/targetSdk | 34 |
| minSdk | 24 (Android 7.0 — primeiro com Vulkan no NDK) |
| AGP | 8.5.2 |
| Gradle | 8.10.2 |
| NDK | 27.0.12077973 (r27) |
| CMake | 3.31.x do SDK (fallback documentado: local via cmake.dir) |
| JDK | 21 (presente; AGP 8.5 exige ≥17) |

- Kotlin DSL (`build.gradle.kts`/`settings.gradle.kts`), wrapper gerado.
- Sem dependências além do kotlin-stdlib; **zero permissões** no Manifest;
  `exported=false`; `theme` baseado em `android:Theme.Material.NoTitleBar`
  (sem AppCompat); `hardwareAccelerated` default (SurfaceView nativo).
- Package: `com.goni.runtime`; Activity: `GoniActivity`.

## 11. Testes e evidência (§XXV/§XXXIX)

| Estágio | Onde | Como |
|---|---|---|
| UNIT/INTEGRATION (runtime) | Linux | `eng_android_runtime_tests`: state machine × backends reais (lavapipe/llvmpipe): create/destroy, surface created→destroy→recreate→render, pause/resume (com e sem surface), render em NO_SURFACE (no-op), resize, setBackend, primeiro frame com contadores reais |
| Linux legado | Linux | 18 suites preservadas (§XXXIV) |
| BUILT (APK) | local + CI | `./gradlew assembleDebug`; unzip/readelf/nm |
| APK INSPECTION | local + CI | ELF AArch64, `lib/arm64-v8a/libgoni.so`, símbolos `Java_com_goni_...`, Manifest |
| EMULATOR | — | **UNAVAILABLE** (sem KVM) — reportado, não simulado |
| DEVICE | — | **UNAVAILABLE** — reportado |

A regra de evidência (§XXXIX) governa os relatórios: `IMPLEMENTED ≠ BUILT ≠
TESTED ≠ VALIDATED`; primeiro frame REAL é reivindicado APENAS nas
plataformas onde roda (aqui: Linux com drivers reais — o mesmo binário de
runtime que o APK embute).

## 12. ADRs planejados

- ADR-039: Activity+JNI e render na UI thread (Choreographer)
- ADR-040: ownership de ANativeWindow/surface Android
- ADR-041: integração de build Android (Gradle/NDK/CMake reuso de targets)
- (surface Vulkan/GLES Android: seção dedicada em ADR-040)
