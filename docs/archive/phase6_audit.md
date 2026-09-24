> **CORREÇÃO (auditoria final 4–10):** `frameSetViewport` NÃO configura
> scissor (apenas `glViewport`) — removida a menção; o loader expõe 57
> funções (não ~55); atributos UNORM agora são normalizados
> (`GL_TRUE`, bug C-15). Ver `docs/final_phase4_10_audit.md`.

# Auditoria FASE 6 — Backend OpenGL ES real (pré-implementação)

- **Base:** `24d3950` (phase5: vulkan backend)
- **Objetivo (missão §32):** auditar a abstraction e garantir PARIDADE
  ARQUITETURAL com o backend Vulkan antes de implementar o GLES.

## 1. Método

Mapeamento do contrato `RhiBackend` para o modelo OpenGL ES (contexto EGL,
state machine, program linking), verificando que NENHUM conceito da
abstraction é "vulkanismo" que GLES não consiga representar (missão §40).

## 2. Paridade por operação do contrato

| Contrato (ADR-035) | Vulkan (FASE 5) | OpenGL ES (esta fase) |
|---|---|---|
| `probe()` | dlopen loader; `Unavailable/Detected` | dlopen `libEGL`/`libGLESv2`; idem |
| `initialize` | instance→layers→GPU→device→queues→surface→swapchain | display→contexto ES→(surface pbuffer) |
| `createShader` | exige SPIR-V (validação magic) | exige GLSL ES — **compila e linka de verdade** com info log no erro (missão §35) |
| `createGraphicsPipeline` | render pass clássico + estado fixo no objeto | program + estado aplicado em `setPipeline` (state machine) + VAO do `VertexLayout` |
| `createBuffer`/`updateBuffer` | DEVICE_LOCAL + staging REAL + fence | `glGenBuffers/glBufferData/glBufferSubData` (semântica síncrona do GL — diferença documentada, não escondida) |
| `beginFrame` | acquire + render pass begin (clear preto default) | make current + `glClear` preto default — **comportamento alinhado ao Vulkan** |
| `frameClear` | `vkCmdClearAttachments` | `glClearColor`+`glClear` |
| `frameSetViewport` | dynamic state | `glViewport` (+scissor) |
| `frameDraw/Indexed` | `vkCmdDraw*` | `glDrawArrays/glDrawElements` |
| `endFrame`/`present` | submit (fences/semáforos) + present queue | `glFinish` + `eglSwapBuffers` |
| `resize` | recria swapchain | recria pbuffer (surface loss/context loss tratados — missão §38) |
| `surfaceLost` | OUT_OF_DATE/SURFACE_LOST | `EGL_CONTEXT_LOST`/erro de surface |

**Veredito de paridade:** o contrato cobre GLES sem concessões — nenhum
método do `RhiBackend` é específico de Vulkan. O caminho do triangle
(mesmos handles, mesmos descs, mesmo protocolo) é executável nos dois.

## 3. Lacunas/decisões da abstraction identificadas

1. **`ShaderDesc` bimodal (L1/§40)** — já carrega AMBAS as representações;
   GLES exige GLSL e ignora SPIR-V (simétrico ao Vulkan). Nenhuma mudança
   necessária — é a prova viva de paridade.
2. **Clear default no begin** — o Vulkan limpa com preto no início do
   render pass (loadOp CLEAR). GLES alinha o comportamento: `beginFrame`
   aplica `glClear` preto; `frameClear` sobrepõe. Documentado nos DOIS
   backends (mesmo comportamento observável).
3. **`ValidationState`** — OpenGL ES não possui layers de validação:
   pedida → `Unavailable` (motivo real), nunca `Enabled` (missão §18 —
   a ausência não é mascarada).
4. **`wireframe`** — GLES 3.x não possui polygon mode: pedido → erro
   preciso; capabilities `false` (no Vulkan também `false` nesta fase —
   feature não habilitada; ADR-037/038 registram os motivos distintos).
5. **`OutOfDate/Minimized`** — pbuffer headless não fica "out of date"
   (tamanho fixo por recriação); `resize()` recria o pbuffer. Context loss
   (`EGL_CONTEXT_LOST` no swap) → `surfaceLost()=true` + erro preciso.

## 4. Decisões de implementação (registradas antes do código)

| Tópico | Decisão | Justificativa |
|---|---|---|
| Headers | FetchContent Khronos **EGL-Registry** + **OpenGL-Registry**, COMMITS fixados com `URL_HASH` (sem tags estáveis nos repos) | hermético, mesmo padrão de Vulkan-Headers (ADR-037) |
| Loader | dlopen `libEGL.so.1` + `libGLESv2.so.2` (`libEGL`/`libGLESv2` no Android — §30); ~55 funções via dlsym | zero link; reuso do padrão VulkanLoader |
| Versão ES (§33) | tenta contexto **3.2 → 3.1 → 3.0**; mínimo suportado **ES 3.0** (gl3.h base); versão REAL reportada em capabilities | "NÃO assumir que todos suportam 3.2" |
| Surface (§33/§38) | `NativeWindowKind::Headless` → `EGL_PLATFORM_SURFACELESS_MESA` + **pbuffer** (present REAL via `eglSwapBuffers`); demais kinds → `NotSupported` preciso (fases de plataforma) | ausência de janela ≠ ausência de GLES (§13) |
| Device-only | contexto surfaceless (sem surface) — recursos/capabilities ok; frames → `NotSupported` | paridade com Vulkan (§13) |
| Estado do pipeline | aplicado em `setPipeline` (program, raster, depth, blend) | GLES é state machine — a abstraction representa intenção (§37) |
| VAO | criado no `createGraphicsPipeline` a partir do `VertexLayout`; bind no `setPipeline` | vertex input pertence à pipeline |
| Índices | `GL_ELEMENT_ARRAY_BUFFER` no VAO | idiomático ES 3 |
| Validade de GLSL (§35) | `glGetShaderiv`+`glGetShaderInfoLog`+`glGetProgramiv`+`glGetProgramInfoLog` SEMPRE verificados; erros chegam como `Result` com o log completo | missão §35 |
| Capabilities (§34) | `GL_VERSION/VENDOR/RENDERER/GLSL version/EXTENSIONS` + limites (`GL_MAX_TEXTURE_SIZE`, `GL_MAX_VERTEX_ATTRIBS`, `GL_MAX_COLOR_ATTACHMENTS`, `GL_MAX_UNIFORM_BLOCK_SIZE`, `GL_NUM_EXTENSIONS`); instancing/compute por versão real; formatos = conjunto core ES 3.0 | valores REAIS consultados (missão §9/§34) |
| softwareRendering | `GL_RENDERER` contém llvmpipe/softpipe/SwiftShader → `true` | honestidade (§47) |
| Readback (§39) | `glReadPixels` do pixel central após o draw → cor esperada verificável (Validated REAL — mais forte que a validação por layers do Vulkan nesta fase) | GLES permite; missão §39 |
| Paridade (§40) | MESMO triangle/mesmos 3 vértices/mesma intenção via `Renderer` nos DOIS backends; shaders = `tests/shaders/triangle_{vk,gles}.*` | provar que a abstraction não é modelada em Vulkan |

## 5. Ambiente de validação (real — já comprovado na FASE 4)

- EGL 1.5 (Mesa) + **OpenGL ES 3.2** (llvmpipe) via surfaceless — smoke
  test da auditoria F4: `glReadPixels` retornou a cor exata (`64 128 191
  255`) — pipeline de validação confirmada;
- CI (ubuntu-24.04): `libegl1 libgles2` (runtime, já presentes) — os
  hardware tests SKIPAM honestamente sem eles;
- Variáveis de ambiente do sysroot local: `__EGL_VENDOR_LIBRARY_FILENAMES`
  (glvnd) + `LD_LIBRARY_PATH` — documentadas em docs/build.md.

## 6. Testes planejados (missão §41)

- **UNIT (sem EGL):** mapeamentos formato↔GL, parse de versão
  `GL_VERSION`, verificação de GLSL mínimo;
- **HARDWARE (EGL presente):** probe; device-only (contexto sem surface);
  capabilities REAIS (ES 3.2 llvmpipe, softwareRendering); explícito
  GLES; **triangle REAL**: context+shader+program+VBO+VAO+estado+draw+
  swap, com **readback do pixel central verificado** (§39 — RENDERING e
  VALIDATED); resize recria pbuffer; stale handle real; shader GLSL
  inválido → erro com info log; device-only beginFrame → NotSupported;
  registro no Renderer + Auto caindo para GLES quando Vulkan ausente;
- **PARIDADE (§40):** mesmo vertex data + mesma sequência Renderer nos
  dois backends registrados — ambos inicializam, desenham e apresentam.

## 7. Conclusão

Sem bloqueadores; a abstraction atende GLES sem mudanças estruturais (apenas
documentação de paridade nos pontos §3). O modelo de estados e o contrato
provam a intenção "API-agnóstica" — o teste de paridade final (§40) é
executável com leitura de pixels real no lado GLES.
