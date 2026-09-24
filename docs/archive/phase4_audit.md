# Auditoria FASE 4 — Renderer Abstraction (bloqueante)

- **Base:** `2c87f7e` (docs(fase3): ADR-034, roadmap, layout de projeto, arquitetura e README)
- **Data:** 2026-09-16
- **Verificação inicial obrigatória (missão §2):** concluída na íntegra antes de
  qualquer implementação. Resultados abaixo.

## 1. Estado do repositório (verificação §2, itens 1–10)

| Item | Resultado |
|---|---|
| Clone/checkout em `2c87f7e` | OK — working tree limpo |
| CMake ≥ 3.28 | OK — requerido `3.28` pelo projeto; ambiente: **3.31.6** |
| C++20 | OK — `CMAKE_CXX_STANDARD 20`, `STANDARD_REQUIRED ON`, sem extensões (ADR-001) |
| Compilador | GCC 14.2 (Debian trixie), Ninja 1.13.2 |
| Módulos em `engine/CMakeLists.txt` | Exatamente os 14 das Fases 1–3: `core, math, mem, log, reflect, events, jobs, ecs, scene, fs, platform, serial, assets, project` |
| Backends renderer implementados | **NENHUM** (ver §1.1) |
| Diretórios prematuros (`renderer/vulkan`, `renderer/gles`, ...) | **NENHUM** |

### 1.1 Varredura por código gráfico prematuro

Busca case-insensitive por `vulkan|opengles|GLES3|egl|renderer|swapchain|VkInstance|glCreateShader`
em toda a árvore (excluindo `.git`). Ocorrências encontradas e classificação:

- `docs/` (README, roadmap, 00-overview, phase3_design) e comentários de
  `engine/log/Macros.hpp` — **documentação/roadmap**, sem código.
- `cmake/EngineOptions.cmake` — opção `ENG_MATH_VULKAN_DEPTH` (convenção de
  profundidade [0,1] em `eng::math`, FASE 1). Não é dependência gráfica: é um
  `#ifdef` de matemática. Mantida.
- `.devcontainer/scripts/install-graphics.sh` — provisionamento do devcontainer
  (§12): `libvulkan-dev`, `vulkan-tools`, `glslang-tools`, `spirv-tools`,
  `libgles2-mesa-dev`, `xvfb`. Preparação legítida para FASES 4–6.

**Conclusão: zero código de backend, zero dependência gráfica em `engine/*`.**

### 1.2 Baseline de testes (antes de qualquer modificação)

Presets canônicos, árvore limpa, build do zero:

| Preset | Configure | Build | Warnings | ctest |
|---|---|---|---|---|
| `linux-debug` (ASan+UBSan, `-Werror`) | OK | OK (201 alvos) | **0** | **15/15 verdes** |
| `linux-release` (`-O3`, LTO, `-Werror`) | OK | OK | **0** | **15/15 verdes** |

Totais por executável (linux-debug, Catch2): **234 casos / 215.178 asserções**.

| Suite | Casos | Asserções |
|---|---|---|
| core | 24 | 10.133 |
| math | 42 | 267 |
| mem | 20 | 129 |
| log | 21 | 72 |
| reflect | 11 | 85 |
| events | 16 | 67 |
| jobs | 18 | 58 |
| ecs | 18 | 1.602 |
| scene | 28 | 2.227 |
| fs | 7 | 130 |
| platform | 5 | 36 |
| serial | 9 | 128 |
| assets | 8 | 200.087 (stress) |
| project | 5 | 40 |
| integration (e2e) | 2 | 117 |

### 1.3 CI (GitHub Actions)

`ci-linux.yml`: matrix `{linux-debug, linux-release}` em ubuntu-24.04. Runs
históricos via API: `d9b2d9d`, `8469f7c`, `2c87f7c` → todos **success**,
evento `push`, branch `main`. Filtro de trigger `branches: [main]` conferido
por codepoints (`0x5b 'm' 'a' 'i' 'n' 0x5d`) — correto.

**Nota de método (registrada):** uma leitura inicial via saída colorida de
`rg` sugeria `branches: ain]` ("`[m`" ausente). Verificação byte-a-byte
(od/codepoints) provou que o texto real é `[main]` — o par `[m` era
interpretado como sequência ANSI reset pelo renderizador do terminal de
auditoria, e não pelo GitHub. **Nenhum defeito existe no workflow.** A
verificação por bytes (não por texto renderizado) permanece como prática
obrigatória para afirmações de auditoria deste projeto.

## 2. Ambiente gráfico real (planejamento FASES 5/6)

O ambiente de execução **não é o devcontainer §12**: não há GPU física, não há
display (`$DISPLAY` ausente) e não há root (`sudo` exige senha). Estratégia
adotada para permitir validação REAL de Vulkan e GLES nesta máquina — sysroot
local via `apt-get download` + `dpkg -x` (sem root), instalado sob
`/home/z/my-project/gfx-sysroot/` (fora do repositório):

| Componente | Estado | Evidência (executado nesta máquina) |
|---|---|---|
| Vulkan loader | Sistema: `libvulkan.so.1` (1.4.309) | `vulkaninfo --summary` |
| Vulkan ICD software (lavapipe) | Sysroot: `mesa-vulkan-drivers` 25.0.7 | `llvmpipe (LLVM 19.1.7, 256 bits)`, `PHYSICAL_DEVICE_TYPE_CPU`, apiVersion 1.4.305 |
| `VK_EXT_headless_surface` | **Disponível no lavapipe** | lista de instance extensions (rev 1) |
| EGL | Sysroot: `libegl-mesa0` + glvnd `libegl1` | `EGL 1.5`, vendor Mesa Project |
| OpenGL ES | Sysroot: `libgles2` | **`OpenGL ES 3.2 Mesa 25.0.7`**, GLSL ES 3.20, llvmpipe |
| Render headless GLES | Validado por smoke test (`egl_smoke2.c`) | FBO 64×64, `glReadPixels` → `64 128 191 255` (cor exata esperada) |
| Ferramentas | Sysroot: `glslangValidator` 15.1, `spirv-val` 2025.1, `vulkaninfo`, `vkcube` | — |

Variáveis de ambiente necessárias em runtime (documentação para os testes):
`LD_LIBRARY_PATH` apontando ao sysroot, `VK_ICD_FILENAMES` apontando a
`lvp_icd.json` (Vulkan), `__EGL_VENDOR_LIBRARY_FILENAMES` apontando a
`50_mesa.json` (EGL/glvnd).

**Implicações decisivas para o design:**

1. FASE 5 pode ser **validada de verdade** com lavapipe: instance real, GPU
   real (software), device real, e **swapchain + present reais** via
   `VK_EXT_headless_surface` — o milestone crítico (missão §28) é executável
   headless.
2. FASE 6 pode ser **validada de verdade** com EGL surfaceless + llvmpipe:
   contexto ES 3.2, draw real, leitura de pixels real (missão §39).
3. Todo resultado obtido com driver de software será reportado como
   **software rendering** — nunca como suporte de hardware (missão §47).
4. A CI (ubuntu-24.04) tem mesa/llvmpipe disponíveis via apt; a extensão do CI
   instalará `mesa-vulkan-drivers`/`libegl-mesa-dev` nos jobs de backend.

## 3. Respostas da auditoria (missão §5)

### 3.1 Onde o renderer deve viver

Decisão **já registrada no repositório** (docs/architecture/00-overview.md,
grafo alvo): o módulo de interface é `eng::rhi` ("FASE 4"), com backends
`rhi-vulkan` e `rhi-gl` como **nós separados, linkados no executável final** —
"módulos de interface nunca incluem headers de backends". A missão atual usa o
termo "Renderer Abstraction"; o nome do módulo existente no plano do repo é
`rhi`. **Decisão:** módulo `engine/rhi` (namespace `eng::rhi`, target
`eng_rhi`), com a classe frontend `Renderer` (nomes da missão §4) e backends
futuros em `engine/rhi/backends/vulkan/` e `engine/rhi/backends/gles/`,
adicionados condicionalmente por opções CMake (`ENG_BUILD_RHI_VULKAN`,
`ENG_BUILD_RHI_GLES` — desligadas na FASE 4). A regra "nenhum módulo engine/*
inclui headers de backends" permanece: backends incluem `rhi`, nunca o
contrário; `scene/ecs/assets/jobs` não conhecem `rhi` (nenhum usa).

### 3.2 API pública mínima

O que os milestones exigem (missão §28/§39/§40 — um frame real com
triangle/quad em AMBOS os backends):

- `RendererConfig` + `BackendType {Auto, Vulkan, OpenGLES}` + seleção com
  validação completa e erro preciso (missão §10);
- `Renderer` (create, capabilities, deviceInfo, resize, beginFrame, present);
- handles: `BufferHandle`, `TextureHandle`, `ShaderHandle`,
  `GraphicsPipelineHandle`;
- descs: `BufferDesc`, `TextureDesc`, `ShaderDesc`, `GraphicsPipelineDesc`;
- `RendererCapabilities` + `DeviceInfo` + `ValidationState`;
- `Frame` (RAII do frame: clear/setPipeline/bind/draw/end).

Explicitamente **fora** (missão §6): render graph, materiais, PBR, deferred,
GPU-driven, hot reload, pós-processamento, partículas, animação, editor,
compute, samplers como objeto, command lists genéricas, MRT explícita,
uniform push constants. `assets::loadAsync` citado no roadmap antigo NÃO faz
parte desta missão (divergência registrada em §5).

### 3.3 Conceitos da abstraction vs backends

| Conceito | Abstraction (`eng::rhi`) | Backends (Fase 5/6) |
|---|---|---|
| Config/seleção | `RendererConfig`, `BackendType`, registry de fábricas | — (factories se registram) |
| Intenção de pipeline | `GraphicsPipelineDesc` (vertex layout, raster/depth/blend, formatos) | tradução para `VkGraphicsPipeline` / `GLSL program + estado GL` |
| Shader como dado | `ShaderDesc` carrega **ambas** as representações (SPIR-V e GLSL ES) | Vulkan consome SPIR-V; GLES compila GLSL; ausência da representação exigida = erro preciso |
| Frame | `beginFrame→commands→endFrame→present` | fences/semáforos, command buffers / estado GL + swap |
| Surface | `SurfaceDesc` opaco (handle nativo void* + tag de plataforma) | cria `VkSurfaceKHR`/`EGLSurface` |
| Capacidades | structs de dados | consultam a API real (limites, extensões, versão) |
| Estados de disponibilidade | enums/modelo documentado | probes reais (loader, instance, device, contexto) |

A abstraction representa **intenção**; cada backend traduz para seu modelo
(missão §37). Nenhum tipo `Vk*`/`GLuint`/`EGL*` vaza nos headers de `rhi`.

### 3.4 Estratégia de handles

Handles **opacos, tipados e triviais** (`struct XHandle { std::uint64_t id; }`,
`0` = nulo, `operator==`). O bit alto reserva-se à codificação de geração pelo
backend — o contrato da abstraction exige apenas: (a) nulo é inválido; (b)
handle desconhecido/destruído produce **erro determinístico**, nunca UB; (c)
handles não são pointers — nunca desreferenciados pelo frontend. O FakeBackend
usa codificação index+geração para testar detecção de stale (missão §8).

### 3.5 Ownership / lifetime / destruction / move / copy

- `Renderer`: **move-only, RAII**. Destrutor encerra frame pendente, destrói
  todos os recursos via backend e o backend. Não há `init()/shutdown()`
  soltos — a criação é `Renderer::create(...)` (Result).
- `Frame`: **move-only, RAII**. Destrutor chama `end()` se pendente
  (fail-safe); `end()` submete. `present()` vive no `Renderer` (após `end`).
- Recursos: ownership do **backend** (o frontend só trafega handles).
  Destruir um recurso é explícito (`destroyBuffer` etc.); destruir o
  `Renderer` libera tudo (contracto documentado).
- Copy: proibido para `Renderer`/`Frame`; handles são value types copiáveis.

### 3.6 Thread affinity

Single-threaded por declaração (estilo ADR-034): **uma thread de render**
cria/usa/destrói o `Renderer` inteiro (incluindo `Frame`). Backend decide
internamente sobre threads próprias (Vulkan Fase 5 cria a fila e submete na
thread do chamador). Nenhuma API async nesta fase; `eng::jobs` não é
dependência de `rhi`.

### 3.7 Handles inválidos

Toda operação com handle nulo/desconhecido retorna
`Error{StatusCode::InvalidArgument, mensagem precisa}` (missão §8). O
FakeBackend em testes cobre: nulo, stale (pós-destroy), double-destroy,
handle de tipo errado.

### 3.8 Capacidades

`RendererCapabilities` carrega **somente valores consultados do backend**
(missão §9): nomes/versões de API e device, `maxTextureSize`, limites de
buffer/uniform, `supportsInstancing/compute/multisample/presentation`,
formatos suportados, e `ValidationState`. O backend preenche consultando a
API real; o FakeBackend (testes) preenche valores marcados como fake e
**nunca** é reportado como suporte real.

### 3.9 Lifecycle geral e estados

`registry → Renderer::create(config) → probe+validação por backend →
capabilities → recursos → frames → destruição`. Estados de validação
(missão §18): `Enabled | DisabledByConfiguration | Unavailable |
FailedToInitialize` — reportados, nunca mascarados. Estados de
disponibilidade (missão §47): o modelo distingue `Detected` (loader existe),
`Available` (instance/device criável), `Initialized`, `Capable` (features),
`Presentable` (surface ok), `Rendering` (frame submetido), `Validated`
(verificado). FASE 4 define esses estados no design; FASES 5/6 os emitem.

### 3.10 Errors / Result / Logging

- Erros: `eng::core::Result<T, eng::core::Error>` — já existem (Fase 1) e
  foram **validados com tipos move-only** (`Result<std::unique_ptr<int>>`
  compila; copy nunca instanciada). `StatusCode` cobre os casos rhi
  (`NotSupported`, `NotFound`, `InvalidArgument`, `NotAvailable` mapeia para
  código próprio quando faltar granularidade — registrado no design).
- Logging: `eng::log` com `ENG_LOG_CATEGORY("rhi")` por TU — padrão do repo.

### 3.11 Frame lifecycle / resize / surface / swapchain

`beginFrame` adquire imagem; comandos no `Frame`; `end()` submete; `present`
apresenta. Condições tratadas como **Result com StatusCode dedicado ou
enum**: surface inexistente (frames exigem surface — device-only não renderiza
janela), `SurfaceLost`, `SurfaceOutOfDate` (→ `resize()` e re-tenta),
`SurfaceMinimized` (skip), `PresentationUnavailable`. `Renderer::resize(w,h)`
recria swapchain no backend. `frames in flight`: propriedade do backend
(máximo 2 no Vulkan Fase 5), invisível na abstraction.

### 3.12 Android readiness / offscreen / headless

- **Android (missão §30):** `SurfaceDesc` carrega um handle nativo opaco
  (`void*` + `NativeWindowKind`) — zero JNI, zero headers Android em `rhi`.
  A Fase 7 entregará o handle via `eng::platform`. Nada a implementar agora
  além do tipo opaco.
- **Device-only/headless vs presentation (missão §13):** são estados
  distintos. Sem surface: instance/device/recursos/capabilities funcionam;
  `beginFrame` retorna erro claro. Com surface (incl. headless surface do
  lavapipe): caminho completo. Testes de hardware classificam
  `PASS/SKIPPED/UNAVAILABLE/FAILED` por estado.

### 3.13 Testes (missão §11 + §41)

- **Unit (FASE 4):** FakeBackend determinístico em `engine/rhi/tests/`
  (somente lá — nunca na lib, nunca produção): lifecycle, handles/stale,
  erros, estado, capabilities, ownership, frame lifecycle, seleção/fallback,
  resize/lost/minimized.
- **Integration:** sequências completas contra a abstraction (ainda com fake
  na FASE 4; com backends reais nas Fases 5/6).
- **Hardware (Fases 5/6):** executáveis marcados com `LABELS` CTest
  (`rhi_hardware`); dentro deles, ausência de suporte → `SKIP()` do Catch2
  com motivo — distinguir API/disponível de presentável.

### 3.14 Futuras extensões (não implementar)

Pontos de extensão deliberadamente deixados: samplers independentes,
compute, render passes/MRT explícitas, uniform buffers no pipeline desc,
dynamic viewports além do setViewport, shader reflection, async upload.
Cada um entra quando uma fase futura precisar (missão §6/§48).

## 4. Convenções existentes relevantes (inventário)

- Módulos via `eng_add_module(<name> srcs...)` (cmake/EngineOptions.cmake):
  cria `eng_<name>` estática com include público, warnings, sanitizers,
  `-fno-exceptions -fno-rtti`, LTO opcional.
- Testes: executável `eng_<m>_tests` linkando módulo + `Catch2::Catch2WithMain`,
  política `eng_apply_test_policy` (exceções ON para Catch2, `-fno-rtti`),
  `add_test(NAME <m> ...)`.
- Dependências externas via FetchContent em `cmake/EngineDependencies.cmake`
  com versões fixas (Catch2 v3.5.2, nlohmann/json v3.11.3) — Vulkan-Headers
  (Fase 5) e EGL/GLES headers (Fase 6) seguirão o MESMO padrão, quando
  necessários.
- `eng::core::Result/Error` conforme §3.10; logging conforme §3.10.
- ADRs existentes até **034** — FASE 4 usará 035+.

## 5. Desvios e pendências registradas

1. **Roadmap antigo × missão nova:** README/roadmap/00-overview descrevem
   "FASE 4 = rhi + Vulkan; FASE 5 = GL/GLES". A missão atual separa em
   FASE 4 (abstração) → FASE 5 (Vulkan) → FASE 6 (GLES) e NÃO inclui
   `assets::loadAsync`. Documentação será atualizada nos commits das
   respectivas fases para refletir a sequência real — sem reescrever
   histórico.
2. **ADR-034** antecipa `loadAsync` na "FASE 4" — permanece NÃO implementado
   (fora do escopo desta missão); a numeração de fases diverge
   (documentacional). Registrado, sem ação de código.
3. Ambiente local não é o devcontainer: sysroot de gráficos é artefato de
   máquina (fora do repo), documentado em §2 para reprodutibilidade.

## 6. Conclusão

Baseline **verde e limpa** (2c87f7e, Debug/Release 15/15, zero warnings, CI
success), arquitetura planejada compatível com a missão (`eng::rhi` como
interface, backends linkáveis no executável), nenhuma dependência gráfica
existente, e caminho de validação real identificado para Vulkan (lavapipe +
headless surface) e GLES (EGL surfaceless + llvmpipe) nas Fases 5/6.

**Sem bloqueadores arquiteturais.** Prosseguir para `docs/phase4_design.md`.
