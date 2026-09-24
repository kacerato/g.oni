# Auditoria FASE 7 — Android runtime (bloqueante, pré-implementação)

- **Base:** `35ac978` (phase6: opengles backend)
- **Data:** 2026-09-17
- **Método:** verificação de baseline (§XLI FASE A) + inventário do código
  real + auditoria de ferramentas/ambiente executada por comandos (nada
  presumido).

## 1. Baseline (FASE A — verificada)

| Item | Resultado |
|---|---|
| HEAD | `35ac978` (origin/main), working tree limpo |
| linux-debug | build limpo do ZERO, **0 warnings**, **18/18 suites** (ASan+UBSan) |
| linux-release | build limpo do ZERO, **0 warnings**, **18/18 suites** (LTO) |
| CI GitHub | success em `b36f350`, `24d3950`, `35ac978` (matrix debug+release) |
| Nota de sessão | máquina resetada no início desta sessão — clone local era
  stale em `8469f7c`; `git fetch` + checkout de `35ac978` restaurou o
  estado (histórico no origin intacto, nenhum rebase/amend) |

## 2. Ambiente Android disponível (auditado por comando)

| Recurso | Estado | Evidência |
|---|---|---|
| JDK | **21.0.12 instalado no sistema** (`/usr/bin/java`) | `java -version` |
| Android SDK / NDK / Gradle | **AUSENTES** | sem `/opt/android*`, `gradle` não está no PATH |
| Disco | **8.7 GB livres** (de 9.9 GB) | `df -h /home` |
| CPU/memória | 2 vCPU, 3 GB RAM | `nproc`, `free` |
| `/dev/kvm` | **AUSENTE** | `ls /dev/kvm` |
| Dispositivo físico | **AUSENTE** | nenhum adb/device |
| Rede p/ dl.google.com | OK | HTTP/2 200 em `repository2-1.xml` |

**Implicações:**

1. JDK 21 já disponível — AGP 8.5+ exige JDK 17+: **atendido sem download**.
2. SDK/NDK/Gradle **podem ser provisionados sem root** (cmdline-tools +
   `sdkmanager` em diretório do usuário — mesmo padrão do gfx-sysroot das
   fases anteriores). O volume cabe no disco (NDK r27 ~2.2 GB extraído +
   SDK ~0.4 GB + Gradle ~0.6 GB + caches ~1.5 GB ≈ 4.7 GB < 8.7 GB), com
   zips descartados após extração e `--no-daemon`/heap limitado (3 GB RAM).
3. **Emulador: UNAVAILABLE** (sem KVM, sem imagens de sistema — não serão
   baixadas: inviável em disco/cópia sem virtualização).
   `ANDROID_HARDWARE_TEST = UNAVAILABLE` documentado desde já (§XXX/§XLIII).
4. **Dispositivo físico: UNAVAILABLE.** Sem adb.
5. Validação local possível e planejada: **APK BUILD + APK INSPECTION**
   estáticos (unzip/manifest/ELF/símbolos JNI via llvm-readelf do NDK) +
   CI Android no GitHub Actions (SDK pré-instalado nos runners) — evidência
   BUILT/UNIT/INTEGRATION no Linux; hardware fica UNAVAILABLE, sem invenção
   (§XXXIX).

## 3. Auditoria do código (o que existe e o que falta)

### 3.1 Grafo e fronteiras (00-overview.md)

`android/`, `editor/` são **consumidores** de `engine/` — nunca o contrário
(regra já registrada). JNI é permitido EXCLUSIVAMENTE em `android/`
(§II.4 da missão) — hoje **não existe nenhum arquivo** com `jni.h`,
`android/` ou `ANativeWindow` no repositório (verificado por varredura).

### 3.2 RHI abstraction (FASE 4) — pronta para Android

- `NativeWindowKind::Android` já existe; `SurfaceDesc` carrega
  `NativeWindowHandle{const void*, kind}` — **opaco por construção**: a
  abstraction nunca interpreta o ponteiro (missão §II.6/§V).
- `BackendType::{Auto,Vulkan,OpenGLES}` + seleção com validação completa e
  motivos agregados (ADR-036, revisão L2) — atende §XIV.
- `RendererCapabilities` reporta backendName/apiVersion/device/vendor/
  renderer/presentation — atende o registro do §XIV.

### 3.3 Vulkan backend (FASE 5) — quase pronto; lacuna única

- Loader por `dlopen` com candidatos `libvulkan.so.1`/`libvulkan.so` —
  **Android já coberto** (a segunda forma é a do sistema Android).
- `surfaceKindExtension(Android)` já mapeia para `"VK_KHR_android_surface"`
  (literal, sem `VK_USE_PLATFORM_*` — decisão ADR-037).
- **Lacuna V1:** a criação de surface só implementa `Headless`
  (`VK_EXT_headless_surface`). Falta o branch `Android`:
  `vkCreateAndroidSurfaceKHR` com `VkAndroidSurfaceCreateInfoKHR{window}`.
  Exige: macro `VK_USE_PLATFORM_ANDROID_KHR` + `<android/native_window.h>`
  (NDK, **não é JNI**) + `PFN_vkCreateAndroidSurfaceKHR` na tabela do
  loader — tudo sob `#ifdef __ANDROID__` (zero impacto no Linux).
- **Lacuna V2:** nomes de extensão por plataforma são validados contra as
  extensões REPORTADAS da instance (§X já atendido — sem presunção).
- Device selection (§XI) = lógica FASE 5 intacta (toda GPU enumerada,
  requisitos, motivos de rejeição) — nada a mudar.

### 3.4 GLES backend (FASE 6) — lacunas específicas

- Loader `dlopen` `libEGL.so.1`/`libEGL.so` + `libGLESv2.so.2`/`libGLESv2`
  — **Android já coberto** nos candidatos.
- Versão de contexto 3.2→3.1→3.0 com detecção REAL (§XIII atendido).
- **Lacuna G1:** display é criado SEMPRE via
  `EGL_PLATFORM_SURFACELESS_MESA`; no Android o correto é
  `eglGetDisplay(EGL_DEFAULT_DISPLAY)` (função EGL 1.0 que a tabela do
  loader **não carrega hoje** — precisa ser adicionada).
- **Lacuna G2:** surface é SEMPRE pbuffer; no Android com janela o correto
  é `eglCreateWindowSurface` (também ausente da tabela) com config
  `EGL_WINDOW_BIT` (a config atual pede `EGL_PBUFFER_BIT`).
- **Lacuna G3:** device-only no Android depende de
  `EGL_KHR_surfaceless_context` (não garantido em drivers Android) — o
  runtime Android não usará esse caminho (renderer criado apenas com
  surface); falha honesta com erro preciso se ocorrer.
- `eglSwapBuffers`/context loss/resize-recria-pbuffer: presentes.

### 3.5 Logging (§XVIII)

`eng::log::LogSink` é interface virtual (`write(level, category, message)`)
— docstring já antecipa "logcat". Um `LogcatSink` em `android/runtime`
(usando `__android_log_print` — NDK, não JNI) integra o engine ao logcat
sem tocar em `eng::log`. Eventos do §XVIII mapeiam 1:1 para `ENG_INFO`
com categorias `rhi.android`/`android`.

### 3.6 CMake / build (§XXIII)

- `engine/CMakeLists.txt` adiciona módulos; testes de módulo guardados por
  `ENG_BUILD_TESTS`; `tests/` (integração) também guardado — o build
  Android **reusa** `engine/` via `add_subdirectory` com
  `ENG_BUILD_TESTS=OFF`, sem duplicar fontes (§XXIII atendido por design).
- `cmake/EngineDependencies.cmake`: nlohmann/json é dependência de BUILD
  (busca incondicional — ok p/ Android, header-only); Catch2 é pulso com
  `ENG_BUILD_TESTS=OFF` (verificado por código-fonte da guard).
- `cmake_minimum_required(3.28)` na raiz — o SDK cmdline-tools oferece
  `cmake;3.31.x` (verificar disponibilidade exata em `sdkmanager --list`;
  fallback documentado: CMake local via propriedade `cmake.dir`, não
  commitado).
- Avisos: `-Wall -Wextra -Wpedantic -Werror` (ADR-020) + `-fno-exceptions
  -fno-rtti` (ADR-004/005) — o código é GCC-limpo; compilação com o clang
  do NDK pode revelar avisos novos (corrigir código, nunca `-Wno-` injustificado
  — §XXXVI).

### 3.7 Platform module

`eng::platform` lê fatos do host (Linux: /proc, uname) — não é pré-requisito
do runtime Android (a surface vem pela JNI do app, não por `platform`);
sem mudanças nesta fase (a fronteira é a `NativeWindowHandle` opaca).

### 3.8 JNI (§II/§III/§V)

- **Nada existe hoje** — correto (fases anteriores proibiram JNI).
- A API mínima da missão (§II.5) será implementada 1:1 (nomes adaptados à
  arquitetura real: `GoniRuntime` Kotlin object com `external fun`):
  `nativeCreate/backend`, `nativeDestroy`, `nativeSurfaceCreated`,
  `nativeSurfaceChanged`, `nativeSurfaceDestroyed`, `nativeOnPause`,
  `nativeOnResume`, `nativeSetBackend`, `nativeRenderFrame`.
- Handles JNI: `nativeCreate` devolve `Long` (endereço do runtime como
  `intptr_t`); demais funções recebem esse `Long` — **fronteira estável**
  sem expor tipos C++ (§II.6: sem std::string/vector/ponteiros arbitrários
  além do handle único documentado).

### 3.9 Lifecycle/surface (§IV–§VIII)

Estados do runtime a implementar (`android/runtime`, C++ puro testável no
Linux): `NO_SURFACE`, `SURFACE_AVAILABLE`, `SURFACE_CHANGED` (pendente),
`SURFACE_DESTROYED` + flag `PAUSED`. Robustez exigida (§VI): transições
fora de ordem tratadas como no-op/estado, nunca crash. `ANativeWindow`
ownership: runtime faz `acquire` no `surfaceCreated` (via
`ANativeWindow_fromSurface` no TU JNI) e `release` APÓS destruir o renderer
no `surfaceDestroyed` — sem uso pós-release, sem double-release, sem
dangling (§V). Render thread (§VII): **decisão de design — render na thread
UI via Choreographer** (a mais simples correta: EGL context e Vulkan
single-threaded na thread que chama; sem threads nativas próprias;
ADR-039 registra a decisão).

### 3.10 CI (§XXX)

Runners ubuntu do GitHub têm Android SDK pré-instalado; workflow dedicado
`ci-android.yml` (checkout, JDK, sdkmanager NDK/platform, gradle
assembleDebug, inspeção de APK) — **não substitui** o CI Linux existente
(missão §XXX: "adicionar ... se viável" — viável).

## 4. Riscos e mitigações

| Risco | Mitigação |
|---|---|
| Disco apertado (8.7 GB) | zips removidos pós-extração; `--no-daemon`; sem imagens de emulador; limpeza de caches intermediários |
| 3 GB RAM / 2 vCPU | heap do Gradle limitado; build sequencial; `--no-daemon` |
| clang×gcc warnings novos | corrigir no código; proibido `-Wno-` sem justificativa (§XXXVI) |
| `cmake;3.31.x` indisponível no sdkmanager | usar CMake 3.31.6 local (pip) via `cmake.dir` em `local.properties` (não commitado) + documentação |
| Sem hardware | evidência honesta por estágio (§XXXIX); CI como fonte de build reproduzível |

## 5. Conclusão

Baseline verde; a arquitetura das fases 4–6 já antecipou Android (kinds,
loaders dlopen, extensões por nome); as lacunas são **pequenas e locais**
(V1/V2 no Vulkan, G1/G2 no GLES, sob `#ifdef __ANDROID__`) + a camada nova
`android/` (runtime C++ + JNI + Gradle). Sem bloqueadores. Emulador e
dispositivo ficam **UNAVAILABLE** e serão reportados como tais — o primeiro
frame REAL em hardware Android não pode ser reivindicado neste ambiente;
a validação máxima alcançável aqui é: runtime TESTADO no Linux com backends
reais (lavapipe/llvmpipe), APK BUILT + INSPECTED, CI BUILT.

**Prosseguir para `docs/phase7_design.md`.**
