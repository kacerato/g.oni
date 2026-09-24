# Auditoria FASE 5 — Backend Vulkan real (pré-implementação)

- **Base:** `b36f350` (phase4: renderer abstraction)
- **Objetivo (missão §16):** auditar a abstraction criada na FASE 4 e
  corrigir problemas arquiteturais ANTES de escrever o backend Vulkan.

## 1. Método

Mapeamento sistemático do contrato `RhiBackend` (ADR-035) contra o fluxo
Vulkan real (instance → layers → device → queues → surface → swapchain →
pipeline → buffers → submit → present), usando a especificação da missão
(§17–§29) e a realidade do ambiente validada na auditoria FASE 4
(lavapipe 1.4.305, `VK_EXT_headless_surface`, validation layers
Khronos 1.4.309 disponíveis no sysroot local).

## 2. Veredito geral

A abstraction é **suficiente e correta** para o milestone Vulkan (triangle
real submetido à GPU, missão §28): seleção com validação completa (§10),
handles opacos (§8), frame lifecycle (§12), device-only × presentation
(§13), validação honesta (§18). Três lacunas encontradas e corrigidas
nesta auditoria (commits próprios antes do backend):

### L1 — Formato do render target não é conhecível antes da pipeline

**Problema:** `GraphicsPipelineDesc::renderTarget.colorFormat` era
obrigatório, mas o chamador não tem como saber o formato NEGOCIADO da
swapchain antes de criar a pipeline (a capabilities não o carrega; o
formato real só existe após `initialize` criar a surface).

**Correção:** `colorFormat == Format::Undefined` passa a significar
**"herdar o formato da surface"** (default). Formato explícito que não
coincide com o da surface continua sendo erro do backend (a FASE 5 valida
com `vkCreateFramebuffer`/render pass clássico). Frontend passa a aceitar
Undefined; fake e testes ajustados.

### L2 — Auto exigia `probe >= Available`, nível que backends reais não atestam barato

**Problema:** "Available = instance criável (validação completa passou)"
não é verificável sem criar a instance — ou seja, `probe()` real nunca
reporta `Available` honestamente. O Auto da FASE 4 pularia Vulkan mesmo
com loader presente.

**Correção:** `Auto` tenta `initialize` a partir de `Detected` (loader
presente); apenas `Unavailable` pula a tentativa. Níveis ≥ `Available`
continuam sendo atingidos/verificados em `initialize` e reportados por
capabilities/testes (ADR-036 com nota de revisão). FakeBackend ganha
cenário `Detected` nos testes.

### L3 — `present()` ambíguo com múltiplos frames submetidos

**Problema:** o contrato não definia o comportamento de `present()` quando
há mais de um frame `end()`-ado sem present (a FASE 4 permitia
begin-while-ended com o fake). Em Vulkan, frame submetido cuja imagem
nunca é apresentada vaza a imagem da swapchain (acquire trava).

**Correção (contrato):** `end()` marca o frame como submetido;
`present()` apresenta TODOS os submetidos-não-apresentados, EM ORDEM.
FakeBackend passa a contar pendentes e drená-los; testes atualizados.

## 3. Decisões de implementação Vulkan (registradas antes do código)

| Tópico | Decisão | Justificativa |
|---|---|---|
| Headers (§14) | FetchContent Khronos/Vulkan-Headers **v1.4.309** com `URL_HASH` sha256 fixado | mesmo padrão Catch2/nlohmann (ADR-030); zero dependência de sistema em compile |
| Loader | `dlopen`/`dlsym` de `libvulkan.so.1` (volk-like, ~45 funções) | nenhum link gráfico; funciona em CI e Android (Fase 7) sem mudança |
| Validation (§18) | `VK_LAYER_KHRONOS_validation` + `VK_EXT_debug_utils` quando pedida E disponível; estados `Enabled/DisabledByConfiguration/Unavailable/FailedToInitialize` | honestidade exigida; layers do sysroot/CI reais |
| Seleção de GPU (§19) | enumerar TODAS; requisitos: fila graphics (+ present se surface); motivo de rejeição por candidata; sem `devices[0]` | missão §19 |
| Memória (§21) | allocator próprio MINIMAL (vkAllocateMemory + seleção de memory type); VMA adiado | carregar VMA agora seria dependência sem necessidade atual; reavaliar quando a FASE 7/8 escalar |
| Render pass (§24) | `VkRenderPass` clássico (compatível 1.0+), dynamic rendering AVALIADO e adiado (ADR-037) | universalmente compatível (lavapipe/Android 1.1); dynamic rendering entra com o render-graph futuro |
| Viewport/scissor | dynamic state (pipeline não depende do tamanho) | resize sem recriar pipeline |
| Buffers (§25) | vertex/index `DEVICE_LOCAL` + upload por staging com barrier + fence própria | caminho REAL de upload; `updateBuffer` síncrono documentado (pode esperar) |
| Frames in flight (§22) | clamp(config, 1, 2) — fences por frame, semáforos image-available/render-finished | padrão canônico |
| Swapchain (§23) | formato preferido B8G8R8A8Srgb (senão formatos[0]); FIFO garantido (MAILBOX se houver); `VK_IMAGE_USAGE_TRANSFER_SRC`?? NÃO — uso `{COLOR_ATTACHMENT, TRANSFER_DST?}` mínimo: COLOR_ATTACHMENT | present FIFO é garantido; recriação em OUT_OF_DATE/SUBOPTIMAL |
| OutOfDate (§12) | acquire OUT_OF_DATE → recriar swapchain → re-tentar UMA vez → Renderable ou erro | absorve recuperação no backend (ADR-035) |
| Minimized (§12) | extent atual 0 → status `Minimized` | protocolo, não erro |
| Shaders (§27) | SPIR-V obrigatório (erro preciso se ausente); validação: múltiplo de 4 bytes + magic `0x07230203`; GLSL do `ShaderDesc` é ignorado pelo Vulkan (é do GLES) | missão §27/§40 |
| Surface kinds | Xcb/Xlib/Wayland/Headless → extension de instance correspondente; `Android` → `NotSupported` preciso ("FASE 7") | sem JNI aqui (missão §30) |
| Headless surface | `VK_EXT_headless_surface` (lavapipe expõe) → swapchain + present reais sem display | milestone §28 executável no ambiente |
| Device-only (§13) | initialize sem surface cria instance+device normalmente; capabilities.presentation=false | missão §13 |
| Índices | `IndexType::Uint16/Uint32` → `VK_INDEX_TYPE_*` | direto |
| Readback de pixels | NÃO implementado nesta fase: milestone §28 = frame real submetido (validação por layers); readback registrado como pendência do futuro offscreen-target | honestidade (§47): não declarar "Validated" por pixels |

## 4. Ambiente de validação (real)

- Loader do sistema `libvulkan.so.1` (1.4.309); ICD **lavapipe** (llvmpipe
  19.1.7, `PHYSICAL_DEVICE_TYPE_CPU`, apiVersion 1.4.305) via sysroot
  (`VK_ICD_FILENAMES`);
- Validation layers Khronos **1.4.309** via sysroot (`VK_LAYER_PATH`);
- CI (ubuntu-24.04): `libvulkan1` + `mesa-vulkan-drivers` +
  `vulkan-validationlayers` + `glslang-tools` via apt (extensão do CI,
  sem substituir jobs existentes — missão §46);
- Comandos canônicos documentados em `docs/build.md` (env vars do sysroot
  opcionais — sem elas os hardware tests reportam UNAVAILABLE/SKIP com
  motivo, nunca falham por ausência de GPU — missão §29/§41).

## 5. Testes planejados (missão §29)

- **UNIT** (sem Vulkan): mapeamentos VkResult↔mensagem, codificação de
  handles, validação de SPIR-V (magic/size), seleção de memory type —
  funções puras extraídas testáveis.
- **HARDWARE** (loader+ICD): probe; device-only create (capabilities
  REAIS: llvmpipe, softwareRendering=true, limits); explicit Vulkan;
  validation states (com/sem layers); headless surface → swapchain →
  pipeline → triangle → submit → present (framesSubmitted ≥ 1);
  resize/recriação; stale handle contra o backend real; device-only
  beginFrame → NotSupported. Labels CTest `rhi_hardware`; SKIP com motivo
  quando loader/ICD ausentes (nunca PASS falso — missão §47).
- **INTEGRAÇÃO da abstraction**: paridade Vulkan/GLES fica na FASE 6
  (missão §40) com o MESMO vertex data + shaders.

## 6. Conclusão

Sem bloqueadores; três ajustes da abstraction (L1/L2/L3) aplicados antes do
backend. Vulkan real é implementável do jeito especificado; o milestone §28
(instance → GPU → device → surface → swapchain → views → pipeline →
buffers → recording → submit → present, triangle) é executável e VALIDÁVEL
neste ambiente com software rendering reportado honestamente.
