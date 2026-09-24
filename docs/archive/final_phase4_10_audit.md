# Auditoria Final — FASES 4 a 10 (independente)

> Auditoria independente do repositório real, executada antes da FASE 11
> (NI-Script). Nada neste documento é baseado apenas em nomes de arquivos: a
> implementação e os testes de cada fase foram lidos, e os suites foram
> executados. Classificações: **[A]** confirmado correto · **[B]** drift de
> documentação · **[C]** bug real de código · **[D]** risco arquitetural ·
> **[E]** teste faltante · **[F]** extensão futura (NÃO é bug).

## 1. Baseline do repositório

| Item | Valor |
|---|---|
| Branch | `main` (única) |
| HEAD auditado | `f8314d8` — `feat(engine): add physics animation and particles` |
| Working tree | limpa (nenhuma alteração não commitada) |
| Remote | `origin = github.com/criandojogodenovo-sketch/g.oni.git`, em sync |

Cadeia de commits esperada e verificada:

| Fase | Commit | Assunto |
|---|---|---|
| 4 | `b36f350` | phase4: renderer abstraction (eng::rhi) |
| 5 | `24d3950` | phase5: vulkan backend |
| 6 | `35ac978` | phase6: opengles backend |
| 7 | `f1bd4ce` | fix(ci-android): bit executável do gradlew (+ cadeia `4daac0b`, `53730fa`, `24564a5`, `32dd431`, `004aec4`, `8e61837`, `41956a9`) |
| 8 | `9a144e4` | feat(editor): implement native mobile editor |
| 9 | `969ac2d` | feat(runtime): add input ui and audio systems |
| 10 | `f8314d8` | feat(engine): add physics animation and particles |

Nenhum rebase/amend/force-push no histórico. Nenhum commit de merge.

## 2. Metodologia e ambiente de validação

1. Leitura completa dos fontes de cada fase (implementação + testes + CMake).
2. Build local `linux-debug` (ASan+UBSan, `-Werror`) do zero: **96/96
   alvos, zero warnings** (GCC 14.2, CMake 3.31.6, Ninja).
3. `ctest --preset linux-debug` local: **24/26 suites verdes**; os 2 suites
   que exigem driver gráfico real (`android_runtime`, `editor`) falham
   **localmente** porque este sandbox não tem ICD Vulkan nem EGL — ver
   achado [C-17]. No CI (lavapipe + libegl1 + libgles2 instalados) os 26/26
   passam (evidência §3).
4. Consulta da API do GitHub Actions para o HEAD `f8314d8`.
5. Varredura de `TODO|FIXME|XXX|HACK|stub|placeholder` em todo o código das
   fases auditadas: **zero ocorrências** (exceto a palavra portuguesa
   "TODOS" em comentários).

## 3. Evidência de CI (HEAD f8314d8)

| Workflow | Status | Conteúdo verificado |
|---|---|---|
| CI Linux | **success** | matrix `linux-debug`+`linux-release`, ubuntu-24.04, lavapipe+EGL+GLES instalados, `ctest` completo (26/26) |
| CI Android | **success** | NDK 27.0.12077973, CMake 3.31.6, `gradlew assembleDebug`, **verifica existência do APK**, inspeciona `lib/arm64-v8a/libgoni.so` (readelf `NEEDED`, símbolos JNI via llvm-nm, badging aapt), upload do APK como artifact |

Os três commits de fix do CI Android (`8e61837`, `41956a9`, `f1bd4ce`) no
histórico da FASE 7 evidenciam que o pipeline Android executou de verdade e
foi depurado — não é uma claimed verde.

## 4. Auditoria por fase

### FASE 4 — `eng::rhi` (b36f350)

**[A] Confirmado correto.**

- `Types.hpp` é agnóstico de API: nenhum tipo/enum/handle de Vulkan/GLES/EGL
  aparece no módulo de interface; handles `Handle<Tag>` são opacos
  (`uint64_t id`, 0 = nulo) e nunca ponteiros.
- `RhiBackend.hpp` define o contrato completo: `probe()` sem efeitos,
  `initialize()` com validação total preenchendo capabilities CONSULTADAS,
  recursos (buffer/shader/pipeline + destroy), frame
  (`beginFrame → frameXxx → endFrame → present`) com `FrameAcquireStatus`
  como protocolo (OutOfDate/Minimized não são erro).
- `Renderer.cpp`: registry de fábricas thread-safe (mutex, `emplace`
  idempotente com `AlreadyExists`), ordem de preferência do `Auto`
  (Vulkan → OpenGL ES) com motivos agregados, seleção explícita SEM fallback
  silencioso, `allowFallback` explícito com `ENG_WARN`.
- **FakeBackend confinado a testes**: vive apenas em
  `engine/rhi/tests/FakeBackend.cpp`, compilado exclusivamente no alvo
  `eng_rhi_tests` — nunca na biblioteca (`engine/rhi/CMakeLists.txt`).
- `Frame` RAII move-only: dtor/move executam `endFrame` pendente como
  fail-safe logado; moved-from retorna erro, não UB; `Frame` vivo mantém o
  backend vivo via `shared_ptr<RendererState>`.
- Camadas: `eng_rhi` linka apenas `eng::core` (PUBLIC) + `eng::log`
  (PRIVATE). Zero dependência gráfica em compile/link.
- Testes: **16 casos / 410 asserções** (FakeBackend) — seleção/registry,
  validação de descritores, protocolo do frame exato, OutOfDate/Minimized,
  device-only, resize/surface, move-only defensivo, destruição libera tudo.

**[F] Extensões documentadas**: uniform/storage buffers, texturas, compute
(caps existem, operações não — ADR-035/036 coerentes).

### FASE 5 — backend Vulkan (24d3950)

**[A] Confirmado correto.**

- Loader REAL: `dlopen("libvulkan.so.1"/"libvulkan.so")`, `vkGetInstanceProcAddr`
  via dlsym, funções carregadas por tabela; **sem link** com libvulkan
  (`target_link_libraries ... ${CMAKE_DL_LIBS}`). Deliberadamente não
  `dlclose` (documentado, precedente volk).
- Instance/device: `vkEnumerateInstanceVersion` (1.1 mínimo), layers
  Khronos de validação descobertas por enumeração honesta
  (`ValidationState::{Enabled,Unavailable,...}` nunca mascarado),
  `vkEnumeratePhysicalDevices` com seleção por tipo, queue graphics+
  present, `VK_KHR_surface`/`VK_KHR_swapchain` exigidos quando há surface.
- Swapchain + sincronização: imageAvailable/renderFinished semaphores +
  fence por slot in-flight (espera/reset corretos em `VulkanFrame.cpp`),
  `vkAcquireNextImageKHR` com tratamento OutOfDate/suboptimal.
- Staging REAL: todo buffer é `DEVICE_LOCAL` + `TRANSFER_DST` com upload via
  staging `HOST_VISIBLE|HOST_COHERENT` + `vkCmdCopyBuffer`.
- Handles geracionais com detecção de stale/double-free
  (`VulkanResources.cpp` — erros `InvalidArgument` precisos, nunca UB).
- Modo device-only/headless sem surface: recursos funcionam, frames devolvem
  erro preciso (testado).
- Testes: **6 casos** — mapeamentos, validação SPIR-V (magic/tamanho),
  probe honesto + device-only, e 2 hardware-gated: triangle REAL submetido
  à GPU e registro/Auto no Renderer. Sem ICD → SKIP com motivo (não falha).
- Shaders de teste: SPIR-V bytes embutidos em headers
  (`tests/shaders/triangle_vk_*_spirv.hpp`) com verificação de magic.

**[D] Riscos**: `HandleTable` (índice+geração) duplicada em
`VulkanInternal.hpp` e `GlesInternal.hpp` (cópia, não compartilhada) —
correções precisam ser aplicadas duas vezes; `find()` const muta gerações
via `operator[]`/`const_cast` (crescimento de mapa com garbage handles,
não UB).

**[F]**: texturas/samplers, depth na abstraction (Vulkan tem, GLES não),
render-graph.

### FASE 6 — backend OpenGL ES (35ac978)

**[A] Confirmado correto.**

- Loader EGL/GLES REAL via dlopen (candidatos cobrem Linux e Android), 57
  funções com validação nula completa (macro aborta o open).
- Versão GLES detectada por tentativa real 3.2→3.1→3.0 com `GL_VERSION`
  reportado em caps; contexto/surface pbuffer/surfaceless com
  destruição em ordem correta (makeCurrent(NO_CONTEXT) → deletar recursos
  → destruir context → surface → eglTerminate).
- Shaders GLSL ES compilados em runtime com info log completo no erro;
  VAO/VBO/EBO reais; `glDrawElements` Uint16/Uint32 com byte-offset.
- **Pixel readback REAL**: `readCenterPixel` lê o centro e o teste valida a
  cor exata do triângulo — validação de SAÍDA, mais forte que a do Vulkan.
- Resize: nova surface criada ANTES de destruir a antiga, contexto
  preservado; `EGL_CONTEXT_LOST`/`EGL_BAD_SURFACE` sinalizados.
- Paridade Vulkan/GLES: caso de teste auto-compilado quando ambos os
  backends existem (`ENG_HAVE_PARITY_VULKAN`).
- Android: `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, `EGL_WINDOW_BIT`,
  `eglCreateWindowSurface` — deltas G1/G2 da FASE 7 auditados.

**[C] Bugs latentes** (ver §5): C-15 (UNORM sem normalização), C-16
(atributo MINOR_VERSION em todo fallback de contexto).

**[B] Drift**: `phase6_audit.md` menciona "scissor" que não existe;
"~55 funções" vs 57 reais.

### FASE 7 — runtime Android nativo (f1bd4ce)

**[A] Confirmado correto.**

- **JNI mínima e auditada**: `jni.h` presente em exatamente 2 TUs do repo
  (`GoniJni.cpp` runtime + `EditorJni.cpp` editor). Zero threads nativas
  (nenhum AttachCurrentThread), zero refs Java retidas entre chamadas, zero
  exceções atravessando a fronteira. Entidades cruzam como `jlong` VALORES
  empacotados (índice+geração), não ponteiros.
- **Decisão render-on-Choreographer/UI-thread é INTENCIONAL e
  documentada**: ADR-039 tem seção dedicada com tabela de critérios
  (afinidade de contexto EGL, contrato single-thread do Vulkan, zero locks,
  ordenação pause), alternativas rejeitadas e consequências (re-avaliação
  deferida ao runtime de jogo). `GoniActivity.kt` implementa o chain do
  Choreographer com remoção do callback ANTES de `nativeOnPause`.
- Surface lifecycle: máquina de estados
  NoSurface→Available→ChangedPending→Destroyed implementada e TESTADA em
  Linux contra backends reais (`AndroidRuntimeTests.cpp`: 10 casos/86
  asserções — create/render/destroy/recreate, resize no próximo frame,
  pause/resume, ordens incomuns, setBackend ao vivo com hand-off de
  window, explicit-unavailable mantém runtime vivo).
- Backend selection: Auto = Vulkan → GLES (ordem documentada ADR-036);
  explícito sem fallback silencioso.
- APK: `abiFilters arm64-v8a` único, minSdk 24/targetSdk 34, NDK
  27.0.12077973, CMake 3.31.6, `c++_shared`, zero dependências Gradle,
  zero permissões, `gradlew` committado com mode 100755.
- CMake do APK reaproveita `add_subdirectory(engine)` + `editor` — zero
  duplicação de fontes; CI inspeciona `NEEDED` do .so.
- Separação: `__ANDROID__` aparece em `engine/` apenas nos backends
  (loader/window) e em `AAudioBackend.cpp` (TU vazia no Linux) — exceção
  documentada; **zero JNI em engine/**.

**[C] Bugs reais**: C-1 (overflow de string JNI — crítico), C-2 (vazamento
de referência ANativeWindow por ciclo de surface), C-17 (suite sem guarda de
SKIP).

**[D] Riscos**: handle de runtime do JNI é `reinterpret_cast` puro sem
mágica/registro (validação depende do Kotlin zerar); fallback 64×48 no
setBackend sem surfaceChanged prévio.

**[E]**: sem teste da conversão de fases do `onTouchEvent` no nível runtime
(o InputSystem em si é testado em `eng::input`).

### FASE 8 — Native Mobile Editor (9a144e4)

**[A] Confirmado correto.**

- `EditorDocument`: projeto+cena+seleção+PLAY/STOP; todos os comandos
  retornam `Result` com erros precisos.
- **Inspector é reflexão REAL**: consome o catálogo ÚNICO do serializer
  (`eng::scene::detail::componentEntries()`), resolve campos por
  offset+tipo, edita enums por NOME estável, valida faixas/NaN/inf. Zero
  `if` por componente.
- **PLAY/STOP por construção**: `play()` = save da cena de edição → load em
  `Scene` nova (clone por serialização); comandos de edição rejeitados em
  Play (`requireEditMode`); o tick avança input+física+animação+partículas
  APENAS no clone; `stop()` descarta. Provado por testes (bola cai no
  clone, edição continua "10"; animação/partículas avançam e morrem com o
  clone; input congelado em Edit).
- `AssetBrowser`: registry×disco, import por staging `.import_tmp` + SAF,
  rename/move preservando AssetId (testado), path traversal bloqueado.
- `Viewport`/`ViewportRenderer`: câmera 2D com pan/zoom ancorado, hit-test
  top-most, quads derivados de `computeWorldMatrix`; renderização pela
  ABSTRACTION `eng::rhi` (zero headers de backend), VBO dinâmico,
  move-only RAII.
- `EditorHost`: composition root; registra fábricas reais uma vez; máquina
  de surface idêntica à da FASE 7; `jni.h` não aparece em `editor/`.
- `EditorActivity.kt` (1.172 linhas, zero lógica de engine): workspace em
  `filesDir/projects` (zero permissões), SAF real, gestos
  tap/drag/pinch, PLAY/STOP, lifecycle correto.
- Registro de componentes gameplay no CONSUMIDOR
  (`ComponentRegistration.cpp` + `ensureEditorComponentsRegistered()`),
  mantendo `engine/scene` livre de gameplay — padrão ADR-043/048.

**[C] Bugs reais**: C-3 (`create()` sem cena → UB latente pré-projeto), C-4
(`setGameViewportSize` nunca chamado → zonas de toque quebradas no
dispositivo), C-5 (tap sintético Down+Up na mesma janela → `pressed`/`down`
nunca observáveis), C-6 (pointerId sempre 0 → sem multitouch no Play do
editor).

**[B] Drift**: design promete prova de conteúdo por `readCenterPixel` (não
existe; APIs `lastFrameVertices` sem nenhum assert); "~35 funções JNI" vs
49; `exported=false` vs `exported="true"` real; contagem "26 casos/278
asserções" vs 31 casos/319 asserções atuais; scripts
`gen_editor_shaders.py`/`gen_ui_font.py` citados não existem no repo.

### FASE 9 — Input + UI + Audio (969ac2d)

**[A] Confirmado correto.**

- `eng::input`: eventos canônicos → fila → UM `update()` por frame (janela
  testada); `TouchState` por pointer ID com defesas (Down duplicado → Move,
  Move órfão ignorado); ações por combinação de fontes (tecla/zona de toque
  em FRAÇÕES — resolução-independente/botão gamepad declarado); bindings
  JSON com validação estrita e round-trip. Thread model documentado.
- `eng::ui`: árvore retained; rect relativo + anchors + design-resolution
  com DPI; hit-test top-most respeitando visibilidade; Button com
  out-of-rect release e Cancel não clicando; Slider com clamp + callback;
  draw-list de quads ordenados por layer; fonte 5×7 (95 glifos ASCII
  completos). **SEM RHI** (CMake linka core/math/input) — o host desenha.
- `eng::audio`: parser WAV genuinamente robusto (RIFF, chunks com padding
  ímpar, PCM8/16/24/32int/32float normalizados, erros precisos); mixer
  f32 aditivo com clamp final, vozes geracionais (stale = no-op seguro),
  pause/resume/loop/stopAll, GC no tick; **stress real de 2 threads**
  (mix puxado numa thread enquanto o jogo faz 400 play/pause/stop/tick).
- `AAudioBackend` REAL (não stub): dlopen libaaudio + 10 entry points
  validados, builder com callback de dados, API<26 → erro preciso; TU toda
  atrás de `#ifdef __ANDROID__` — zero header Android em engine no Linux.
- Integração: `GoniActivity` → `nativeOnTouch` → `InputSystem` com pointer
  ID real; `surfaceChanged` alimenta `setScreenSize`; `renderFrame` chama
  `input.update()` 1×/frame. Editor Play roteia game-touch com gate de
  ferramenta (lado C++ correto e testado).

**[C] Bugs reais**: C-7 (`TouchPoint::delta` nunca computado — campo morto
documentado como testado), C-8 (`frameStamp` nunca escrito), C-9 (`pressed`
de zona dispara a CADA frame com dedo parado), C-10 (UB de aritmética de
ponteiro na mensagem de erro de bits WAV não suportados), C-11 (`playMusic`
decodifica o arquivo inteiro duas vezes), C-12 (stat `underruns` morto).

**[D] Riscos**: **`eng::ui` e `eng::audio` não têm consumidor no produto**
  (grep: draw-list/mixer usados apenas pelos próprios testes — a FASE 8/10
  não os conectaram; `eng::input` sim); mixer global `gMixer` único por
  processo no callback AAudio; `decodeNextWindow` roda sob o mutex na thread
  de áudio; **zero conversão de sample-rate** (WAV 44.1k num mixer 48k toca
  ~8,8% fora); cache de glifos lazy não thread-safe.

**[B] Drift**: design FASE 9 menciona buffer SPSC inexistente (callback
chama `mix()` direto sob mutex); "streaming real" superestimado (decode
integral em memória + janelas); deps de áudio erridas no design; contagem
de testes de `17-input-ui-audio.md` precisa de refresh (9/61, 8/40, 13/70 —
verificadas corretas).

### FASE 10 — Physics + Animation + Particles (f8314d8)

**[A] Confirmado correto.**

- `eng::physics`: RigidBody (massa 0 = estático), colliders esfera/AABB,
  layers/masks bidirecionais, triggers SEM resolução/impulso (testado),
  raycast esfera/AABB com closest-wins + erros precisos, **timestep fixo
  por acumulador com clamp anti-spiral de 8 passos**, integração
  semi-implícita Euler, CharacterBody move-and-slide (penetração mais
  profunda). Determinismo genuinamente testado (invariância de fatiamento
  3×1/60 ≡ 1/30+1/60).
- `eng::animation`: clips TRS com keys carimbadas, lerp/slerp (arco curto
  com nlerp fallback), sampling em tempo arbitrário, Animator
  play/pause/stop-natural/loop/speed/seek, flags de aplicação por canal,
  congelamento seguro em clip desconhecido. Skinning explicitamente fora de
  escopo (documentado em 3 lugares).
- `eng::particles`: spawn determinístico **sem RNG** (van der Corput),
  acumulador de rate dt-independente, ordem integrate→death→spawn
  (invariância testada), burst com cap por maxParticles, pool runtime-only
  não serializada. Teste de determinismo bit-exato.
- Integração editor: tick em Play com acumulador de física + N steps fixos,
  animação/partículas com dt de frame; clone por serialização prova
  isolamento; os 5 componentes registrados no catálogo com nomes
  casando caracter-por-caracter com o reflect; inspector edita e a cena
  persiste (round-trip testado com mass=2.5).
- Camadas: physics/animation/particles linkam exatamente
  `core+math+scene+reflect` — zero rhi/editor/android.

**[C] Bugs reais**: C-13 (**crossfade nunca aplicado** — `blend()` existe,
estado `previous/blendRemaining` existe, mas `AnimationSystem::update`
amostra SÓ o clip atual: transição "estala"; docs/ADR-048 afirmam
cross-fade linear), C-14 (física escreve vetores WORLD em transforms
LOCAIS — quebra com pai rotacionado/escalado; todos os testes usam nós
raiz), C-15 (`snapToGround` serializado e morto).

**[B] Drift (grave)**: `phase10_audit.md` D6 e `18-physics-animation-particles.md`
afirmam que **o viewport desenha partículas como quads — FALSO**
(`buildQuads` só percorre a hierarquia de nós; `ParticlePool` não é lido em
lugar algum de editor/android); "raycast origem-dentro testado" (não há
assert); "velocity é consumida" no moveAndSlide (nunca lida/limpa);
"escala média" (usa só a coluna X); comentário "cone" (é leque planar).

**[D]**: solver cancela a velocidade de cada corpo independentemente (não
conserva momento — documentado como "impulso escalar"); sem broadphase
(O(n²), documentado); sem sleeping; física não valida NaN.

**[E]**: damping linear, useGravity=false, rotationSpeed de partículas,
origem-dentro do raycast, resolução dinâmica AABB-AABB — implementados sem
cobertura.

## 5. Bugs reais encontrados (consolidados)

| # | Fase | Severidade | Descrição | Local |
|---|---|---|---|---|
| C-1 | 7/8 | **CRÍTICA (segurança)** | Bound-check com `GetStringLength` (UTF-16) mas cópia com `GetStringUTFRegion` (MUTF-8 até 3 bytes/unidade) → overflow de stack com strings CJK via intent extra do `GoniActivity` exportado (~61 bytes) e nos 49 argumentos do EditorJni | `GoniJni.cpp:38-52`, `EditorJni.cpp:63-78` |
| C-2 | 7/8 | ALTA (vazamento) | `ANativeWindow_fromSurface` adquire +1 e a referência do lado JNI nunca é liberada → +1 leak por ciclo de surface (a cada rotação) até a morte do processo | `GoniJni.cpp:90`, `EditorJni.cpp:137` |
| C-3 | 8 | ALTA (UB latente) | `EditorDocument::create()` não emite cena (`scene_` vazio); `sceneInFocus()` faz `&*scene_` vazio antes de qualquer projeto | `EditorDocument.cpp:119-127,880-890` |
| C-4 | 8 | ALTA (produto) | `setGameViewportSize` não tem JNI/Kotlin que a chame → zonas de toque de jogo nunca disparam no dispositivo (screenW/H=1×1) | sem `nativeEditorSetGameViewportSize` |
| C-5 | 8 | MÉDIA (produto) | Tap sintético enfileira Down+Up na MESMA janela de update → `pressed`/`down` nunca observáveis no Play do editor | `EditorActivity.kt:966-985` |
| C-6 | 8 | MÉDIA (produto) | Editor Play sempre passa `pointerId=0` → multitouch de jogo impossível pelo editor | `EditorActivity.kt:969-994` |
| C-7 | 9 | MÉDIA | `TouchPoint::delta` nunca computado (campo morto; docs dizem testado) | `Input.cpp:53-57` |
| C-8 | 9 | BAIXA | `TouchPoint::frameStamp` nunca escrito | `Input.hpp:57` |
| C-9 | 9 | MÉDIA (semântica) | `pressed` de zona de toque dispara a CADA frame com dedo parado (teclado dispara 1×) | `Input.cpp:405-407` |
| C-10 | 9 | MÉDIA (UB) | `bits + " bits não suportados"` soma uint16_t a literal → aritmética de ponteiro + leitura OOB p/ bits ≥ 20 | `Wav.cpp:140` |
| C-11 | 9 | BAIXA (perf) | `playMusic` decodifica o arquivo INTEIRO para ler sampleRate e descarta, depois decodifica de novo em janelas | `Audio.cpp:188-198` |
| C-12 | 9 | BAIXA | `MixerStats::underruns` nunca incrementado | `Audio.hpp:81` |
| C-13 | 10 | ALTA (docs×código) | Crossfade declarado (ADR-048 D6) mas nunca aplicado — transição estala; `previousTime` write-only; `AnimationTransition` sem uso | `Animation.cpp:187` |
| C-14 | 10 | MÉDIA (latente) | Gravidade/velocidade/correção WORLD aplicadas a `localTransform()` — errado sob pai rotacionado/escalado | `Physics.cpp:258-262,316-326` |
| C-15 | 6 | MÉDIA (latente) | Atributos UNORM passam `GL_FALSE` em `normalized` → bytes lidos como 0-255 brutos | `GlesBackend.cpp:375-385` |
| C-16 | 6 | BAIXA (robustez) | Todo fallback de contexto carrega `EGL_CONTEXT_MINOR_VERSION` (falha em EGL 1.4 sem KHR_create_context) | `GlesBackend.cpp:212-226` |
| C-17 | 7/8 | MÉDIA (CI/evm) | `android_runtime` (7/10) e editor (3/31) FALHAM (não SKIPam) sem driver — degradação inconsistente com rhi_vulkan/rhi_gles | `AndroidRuntimeTests.cpp`, `EditorTests.cpp:735-802` |
| C-18 | 10 | MÉDIA (dead code) | `snapToGround` serializado sem efeito e sem docs | `Physics.hpp:58` |

**Decisão de remediação**: C-1..C-6, C-9, C-10, C-13, C-15, C-17 e C-18
serão corrigidos antes da FASE 11 (commit de remediação separado do commit
de documentação). C-7/C-8 (campos mortos: delta/frameStamp) e C-11/C-12
(ineficiência/stat morto) e C-14 (local×world na física) e C-16 (fallback
EGL) ficam registrados como limitações/pendências — C-14 exige decisão de
conversão de espaço e C-16 é robustez de driver raro; ambos sem risco para
a FASE 11.

## 6. Riscos arquiteturais (aceitos/monitorados)

1. **HandleTable duplicada** Vulkan/GLES (correções em duplicidade).
2. **Handle de runtime JNI sem validação** (reinterpret_cast puro;
   segurança depende do Kotlin).
3. **`eng::ui`/`eng::audio` sem consumidor no produto** — bibliotecas
   corretas e testadas, mas o APK/editor nunca desenha UI nem inicia o
   AAudio (a FASE 11 de NI-Script e o runtime de jogo são o consumidor
   natural).
4. **Zero conversão de sample-rate** no áudio.
5. **Solver de impulso escalar** (não conserva momento — documentado).
6. **`GoniActivity` é demo-only** (launcher real é o editor).
7. Física assume nós raiz (C-14).

## 7. Testes faltantes ([E], além dos citados por fase)

- eglErrorName (GLES); updateBuffer happy-path; pipeline format mismatch.
- Pré-projeto do EditorDocument (pegaria C-3); rejeição de path traversal
  do save/load; conteúdo de vértices do ViewportRenderer.
- PCM8/24/32f do WAV (onde C-10 vivia); zona held-sem-move (onde C-9
  vivia); delta (C-7).
- damping/useGravity/rotationSpeed/origem-dentro; pose blend no nó (C-13).
- Editor: ChangedPending do host; autosave (feature ausente).

## 8. Limitações conhecidas (honestas, não-bugs)

- Dispositivo/emulador **UNAVAILABLE** neste ambiente (sem /dev/kvm, sem
  adb): IMPLEMENTED ≠ DEVICE TESTED. O CI inspeciona o APK; nada além
  disso é afirmado.
- Física: sem OBB/cápsula/mesh/CCD/juntas/fricção/stacking iterativo/
  sleeping/enter-exit de trigger; AABB ignora rotação do nó (documentado).
- Animação: sem skinning (documentado); keys devem estar ordenadas.
- Partículas: sem cor/size-over-life, sem textura, sem sorting; emissor
  pontual; leque planar (não cone).
- UI: sem clipping, sem medida de texto; gamepad/mouse declarados não
  implementados; `audio.json` de buses não existe.
- Editor: sem undo/redo, sem multi-seleção, sem 3D, viewport 2D com quads
  de marcador de entidade (não meshes).

## 9. Status por fase (vocabulário explícito)

| Fase | IMPLEMENTED | BUILT | UNIT TESTED | INTEGRATION TESTED | CI VERIFIED | APK PACKAGED | APK INSTALLED | DEVICE TESTED |
|---|---|---|---|---|---|---|---|---|
| 4 RHI | YES | YES | YES (16/410, local+CI) | YES (FakeBackend protocolo) | YES | N/A | N/A | N/A |
| 5 Vulkan | YES | YES | YES (6 casos) | YES (triangle GPU no CI c/ lavapipe) | YES | N/A | N/A | N/A |
| 6 GLES | YES | YES | YES (5 casos) | YES (pixel readback no CI c/ EGL) | YES | N/A | N/A | N/A |
| 7 Android | YES | YES | YES (10/86 no CI) | YES (runtime×backends reais no CI) | YES | YES (CI inspect) | **NO** | **NO** |
| 8 Editor | YES | YES | YES (31/319 no CI) | YES (host×backends reais no CI) | YES | YES | **NO** | **NO** |
| 9 Input/UI/Audio | YES | YES | YES (30 casos/171 asserções) | PARCIAL (input integrado; ui/audio sem consumidor) | YES | YES | **NO** | **NO** |
| 10 Phys/Anim/Part | YES | YES | YES (29/161 + 3 editor) | YES (PLAY no clone no CI) | YES | YES | **NO** | **NO** |

**APK INSTALLED = NO · DEVICE TESTED = NO · REAL DEVICE FRAME VERIFIED =
NO** para todas as fases — o Realme C33 não foi conectado a nenhum ambiente
que executou este trabalho. Nenhuma validação física é afirmada.

## 10. Gate final da auditoria

- ✅ Histórico íntegro e verificável (cadeia exata, sem rebase/amend/force).
- ✅ CI Linux + CI Android verdes no HEAD `f8314d8`.
- ✅ Build local debug do zero, zero warnings, 24/26 suites localmente (2
  falhas puramente ambientais — sem driver; C-17 registra a inconsistência
  de degradação).
- ✅ Arquitetura respeitada: engine sem JNI/Android fora das exceções
  documentadas; gameplay nunca vaza para core/scene; rhi consumido apenas
  por editor/android via abstraction.
- ⚠️ 18 bugs reais catalogados (§5), dos quais os críticos e de produto
  **bloqueiam honestidade de documentação** — corrigidos no commit de
  remediação antes da FASE 11; os demais registrados como limitações.
- ⚠️ Drift de documentação significativo em FASES 8/9/10 (afirmações de
  features não entregues: readCenterPixel no editor, SPSC, partículas
  renderizadas, crossfade) — corrigido no Stage B.

**Decisão: APROVADO COM CONDIÇÕES** — prosseguir para FASE 11 após (1)
remediação dos bugs C-1..C-6, C-9, C-10, C-13, C-15, C-17, C-18 e (2)
sincronização da documentação (Stage B).
