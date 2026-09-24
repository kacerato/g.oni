# Design FASE 4 — Renderer Abstraction (`eng::rhi`)

- **Base:** `2c87f7e` (+ auditoria `docs/phase4_audit.md`)
- **Módulo novo:** `engine/rhi` — namespace `eng::rhi`, target `eng_rhi`
- **Backends (Fases 5/6):** `engine/rhi/backends/{vulkan,gles}` — opções
  `ENG_BUILD_RHI_VULKAN`/`ENG_BUILD_RHI_GLES` (OFF na FASE 4; nenhum
  diretório criado ainda)

## 1. Responsabilidades e fronteiras

`eng::rhi` é a **interface gráfica de hardware** do motor (Render Hardware
Interface): representa **intenção** gráfica de alto nível, seleciona e possui
um backend, e define o contrato (`RhiBackend`) que backends reais
implementam. O restante do engine (`scene`, `ecs`, `assets`, `jobs`)
consumirá `Renderer`/handles quando existir um runtime (FASE 8) — nesta fase
nenhum módulo ganha dependência de `rhi`.

### Dependências

- `eng::core` (PUBLIC — `Result`/`Error` aparecem na API pública)
- `eng::log` (PRIVATE — logging interno por `ENG_LOG_CATEGORY("rhi")`)
- **ZERO dependência gráfica**: nenhum include de `vulkan.h`, `GLES3/gl3.h`,
  `EGL/egl.h`, `GL/glx.h`, `windows.h`. Verificado por inspeção + compilação
  (o target não linka nada além de core/log).

### O que `rhi` NÃO faz (missão §6)

Render graph, materiais, PBR, deferred, GPU-driven, shader hot reload,
pós-processamento, partículas, animação, editor, compute, samplers como
objeto, command lists genéricas, MRT, uniform buffers no pipeline, upload
assíncrono, texturas (deferidas até a fase que precisar — decidido na
auditoria §3.2), JNI/Android (Fase 7).

## 2. Modelo de estados (missão §47)

```text
Unavailable    → loader/biblioteca ausente
Detected       → loader presente (probe superficial)
Available      → instance/contexto criável (validação completa passou)
Initialized    → device/contexto criado e funcional
Capable        → features exigidas verificadas
Presentable    → surface/swapchain utilizável
Rendering      → frame submetido à GPU
Validated      → output verificado (readback/val layers)
```

`Availability` é um enum em `Types.hpp`; `RhiBackend::probe()` reporta até o
nível que conseguir **sem efeitos colaterais**; níveis acima de `Available`
são atingidos durante `initialize()` e reportados por capabilities/testes.
Hardware tests (Fases 5/6) imprimem o nível alcançado e classificam
`PASS/SKIPPED/UNAVAILABLE/FAILED`.

Estados de validação (missão §18): `ValidationState { Enabled,
DisabledByConfiguration, Unavailable, FailedToInitialize }` — reportados em
`RendererCapabilities::validationState`, nunca mascarados: se a config pede
validação e ela não existe, o estado é `Unavailable`, não `Enabled`.

## 3. API pública

### 3.1 Tipos de dados (`Types.hpp`)

```cpp
namespace eng::rhi {

enum class BackendType : std::uint8_t { Auto, Vulkan, OpenGLES };

// --- surface (opaca — missão §13/§30) --------------------------------------
enum class NativeWindowKind : std::uint8_t {
    None, Xcb, Xlib, Wayland, Win32, Android, Headless,
};
struct NativeWindowHandle {
    const void* handle{nullptr};   // opaco; nunca desreferenciado por rhi
    NativeWindowKind kind{NativeWindowKind::None};
    [[nodiscard]] bool isValid() const noexcept;
};

struct SurfaceDesc {
    NativeWindowHandle window{};
    std::uint32_t width{0}, height{0};
    Format preferredFormat{Format::B8G8R8A8Srgb};
};

// --- formatos/usos (subsets honestos; backend valida suporte real) --------
enum class Format : std::uint8_t { Undefined, R8G8B8A8Unorm, B8G8R8A8Unorm,
    R8G8B8A8Srgb, B8G8R8A8Srgb, R32G32B32A32Sfloat, R16G16B16A16Sfloat,
    D32Sfloat, D24UnormS8Uint };

enum class BufferUsage : std::uint16_t { None = 0, Vertex = 1<<0,
    Index = 1<<1, Uniform = 1<<2, Storage = 1<<3, CopySrc = 1<<4, CopyDst = 1<<5 };
// operadores |, &, |=, bool(has)

enum class IndexType : std::uint8_t { Uint16, Uint32 };
enum class CullMode : std::uint8_t { None, Back, Front };
enum class FrontFace : std::uint8_t { CounterClockwise, Clockwise };
enum class FillMode : std::uint8_t { Solid, Wireframe };
enum class CompareOp : std::uint8_t { Never, Less, Equal, LessOrEqual,
    Greater, NotEqual, GreaterOrEqual, Always };
enum class BlendFactor : std::uint8_t { Zero, One, SrcAlpha,
    OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };
enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

// --- handles opacos e tipados (missão §8) ----------------------------------
template <typename Tag>
struct Handle {
    std::uint64_t id{0};
    [[nodiscard]] bool isValid() const noexcept { return id != 0; }
    [[nodiscard]] bool operator==(const Handle&) const = default;
};
struct BufferTag; struct ShaderTag; struct GraphicsPipelineTag;
using BufferHandle           = Handle<BufferTag>;
using ShaderHandle           = Handle<ShaderTag>;
using GraphicsPipelineHandle  = Handle<GraphicsPipelineTag>;
// codificação (id != 0): bits baixos = índice; bits altos = geração —
// detalhe do BACKEND; o frontend apenas transporta. Fake usa geração para
// provar detecção de stale.

// --- descs -----------------------------------------------------------------
struct BufferDesc {
    std::size_t size{0};
    BufferUsage usage{BufferUsage::None};
    std::span<const std::byte> initialData{};
};

struct ShaderDesc {                    // shader como DADO (missão §37/§40)
    std::string_view debugName{};
    std::span<const std::byte> vertexSpirv{}, fragmentSpirv{};
    std::string_view vertexGlsl{}, fragmentGlsl{};
};

struct VertexLayout {
    struct Binding   { std::uint32_t binding{0}, stride{0}; };
    struct Attribute { std::uint32_t location{0}, binding{0}, offset{0};
                       Format format{Format::R8G8B8A8Unorm}; };
    std::vector<Binding> bindings{};
    std::vector<Attribute> attributes{};
};

struct RasterState  { CullMode cull{CullMode::Back};
                      FrontFace front{FrontFace::CounterClockwise};
                      FillMode fill{FillMode::Solid}; };
struct DepthState   { bool test{false}, write{false};
                      CompareOp compare{CompareOp::Less}; };
struct BlendState   { bool enabled{false};
                      BlendFactor srcColor{BlendFactor::One},
                                  dstColor{BlendFactor::Zero};
                      BlendOp colorOp{BlendOp::Add}; };
struct RenderTargetDesc { Format colorFormat{Format::B8G8R8A8Srgb};
                          Format depthFormat{Format::Undefined};
                          std::uint32_t sampleCount{1}; };

struct GraphicsPipelineDesc {
    ShaderHandle shader{};
    VertexLayout vertexLayout{};
    RasterState raster{};
    DepthState depth{};
    BlendState blend{};
    RenderTargetDesc renderTarget{};
};

// --- capabilities / device (valores REAIS — missão §9) ---------------------
enum class DeviceKind : std::uint8_t { DiscreteGpu, IntegratedGpu, VirtualGpu,
                                       Cpu, Unknown };

struct DeviceInfo {
    std::string name{}, vendor{}, driver{}, apiVersion{};
    DeviceKind kind{DeviceKind::Unknown};
};

struct RendererCapabilities {
    std::string backendName{};      // "Vulkan", "OpenGL ES", "Fake (TESTES)"
    std::string apiVersion{};
    DeviceInfo device{};
    bool softwareRendering{false};  // honestidade — nunca falso (missão §47)
    std::uint32_t maxTextureSize{0}, maxVertexAttributes{0},
                  maxColorAttachments{0}, maxUniformBufferSize{0};
    bool instancing{false}, compute{false}, multisample{false},
         wireframe{false};
    std::vector<Format> supportedFormats{};
    bool presentation{false};      // surface/swapchain presentes (§13)
    ValidationState validationState{ValidationState::DisabledByConfiguration};
};

// --- frame -----------------------------------------------------------------
struct Viewport { float x{0}, y{0}, width{0}, height{0},
                         minDepth{0}, maxDepth{1}; };

struct ClearDesc { std::array<float, 4> color{0, 0, 0, 1};
                   bool clearColor{true}, clearDepth{false};
                   float depthValue{1.f}; };

enum class FrameAcquireStatus : std::uint8_t {
    Renderable,   // comandos podem ser gravados
    OutOfDate,    // recriado; re-tentar beginFrame
    Minimized,    // nada a renderizar neste tick (NÃO é erro)
};

} // namespace eng::rhi
```

### 3.2 Contrato do backend (`RhiBackend.hpp`)

Interface **pequena e coesa** (missão §7): ciclo de vida + recursos + frame.
Todo backend real (Vulkan FASE 5, GLES FASE 6) e o FakeBackend (testes)
implementam-na integralmente.

```cpp
namespace eng::rhi {

/// Probe sem efeitos colaterais (missão §10).
struct BackendProbe {
    Availability availability{Availability::Unavailable};
    std::string detail{};   // motivo textual ("loader ausente", ...)
};

struct BeginFrameResult {
    FrameAcquireStatus status{FrameAcquireStatus::Renderable};
    std::uint64_t frameId{0};
};

class RhiBackend {
public:
    virtual ~RhiBackend() = default;

    [[nodiscard]] virtual BackendProbe probe() = 0;

    /// Inicialização completa com validação real (loader → instance →
    /// device → queues/features → surface quando houver). Preenche
    /// capabilities/device com valores CONSULTADOS (missão §9).
    virtual eng::core::Result<void> initialize(
        const RendererConfig& config,
        RendererCapabilities& outCapabilities) = 0;

    [[nodiscard]] virtual const RendererCapabilities&
    capabilities() const = 0;

    // recursos — handles opacos; erros determinísticos para null/stale
    virtual eng::core::Result<BufferHandle> createBuffer(
        const BufferDesc&) = 0;
    virtual eng::core::Result<ShaderHandle> createShader(
        const ShaderDesc&) = 0;
    virtual eng::core::Result<GraphicsPipelineHandle> createGraphicsPipeline(
        const GraphicsPipelineDesc&) = 0;
    virtual eng::core::Result<void> updateBuffer(BufferHandle,
        std::size_t offset, std::span<const std::byte> data) = 0;
    virtual eng::core::Result<void> destroyBuffer(BufferHandle) = 0;
    virtual eng::core::Result<void> destroyShader(ShaderHandle) = 0;
    virtual eng::core::Result<void> destroyGraphicsPipeline(
        GraphicsPipelineHandle) = 0;

    // frame — frameId retornado em beginFrame circula nas operações
    virtual eng::core::Result<BeginFrameResult> beginFrame() = 0;
    virtual eng::core::Result<void> frameClear(std::uint64_t frameId,
        const ClearDesc&) = 0;
    virtual eng::core::Result<void> frameSetViewport(std::uint64_t frameId,
        const Viewport&) = 0;
    virtual eng::core::Result<void> frameSetPipeline(std::uint64_t frameId,
        GraphicsPipelineHandle) = 0;
    virtual eng::core::Result<void> frameBindVertexBuffer(
        std::uint64_t frameId, BufferHandle) = 0;
    virtual eng::core::Result<void> frameBindIndexBuffer(
        std::uint64_t frameId, BufferHandle, IndexType) = 0;
    virtual eng::core::Result<void> frameDraw(std::uint64_t frameId,
        std::uint32_t vertexCount, std::uint32_t firstVertex) = 0;
    virtual eng::core::Result<void> frameDrawIndexed(std::uint64_t frameId,
        std::uint32_t indexCount, std::uint32_t firstIndex) = 0;
    virtual eng::core::Result<void> endFrame(std::uint64_t frameId) = 0;
    virtual eng::core::Result<void> present() = 0;

    /// Recreação (resize/surface recriada). Sem surface → erro.
    virtual eng::core::Result<void> resize(std::uint32_t w, std::uint32_t h) = 0;

    /// Estado da surface após beginFrame/present com falha recuperável.
    [[nodiscard]] virtual bool surfaceLost() const = 0;
};
} // namespace eng::rhi
```

Justificativas:
- **frameId por operação**: o backend é a autoridade do estado; o frontend
  `Frame` apenas transporta a sessão de gravação. Sem command buffers
  genéricos (missão §6).
- **`endFrame` + `present` separados**: diagrama canônico da missão §12.
- **`resize` explícito**: reconstrução de swapchain pertence ao backend; o
  estado `OutOfDate` interno dispara recriação no próximo `beginFrame` (o
  frontend nunca vira gerenciador de swapchain).

### 3.3 Frontend (`Renderer.hpp`)

```cpp
namespace eng::rhi {

struct RendererConfig {
    BackendType backend{BackendType::Auto};
    bool enableValidation{true};   // desejo; realidade em ValidationState
    bool allowFallback{false};     // fallback APENAS se explícito (§10)
    SurfaceDesc surface{};         // ausente → device-only (§13)
    std::string_view applicationName{"eng"};
    std::uint32_t framesInFlight{2}; // dica; backend ajusta/clampa
};

struct AcquiredFrame {
    FrameAcquireStatus status{FrameAcquireStatus::Renderable};
    Frame frame{};   // válido apenas quando status == Renderable
};

/// Sessão de gravação do frame — move-only, RAII.
class Frame {
public:
    Frame() = default;
    Frame(Frame&&) noexcept;
    Frame& operator=(Frame&&) noexcept;
    ~Frame();                        // end() pendente → fail-safe
    // sem cópia
    [[nodiscard]] bool isValid() const noexcept;

    [[nodiscard]] eng::core::Result<void> clear(const ClearDesc&);
    [[nodiscard]] eng::core::Result<void> setViewport(const Viewport&);
    [[nodiscard]] eng::core::Result<void> setPipeline(GraphicsPipelineHandle);
    [[nodiscard]] eng::core::Result<void> bindVertexBuffer(BufferHandle);
    [[nodiscard]] eng::core::Result<void> bindIndexBuffer(BufferHandle,
                                                          IndexType);
    [[nodiscard]] eng::core::Result<void> draw(std::uint32_t vertexCount,
                                               std::uint32_t firstVertex = 0);
    [[nodiscard]] eng::core::Result<void> drawIndexed(
        std::uint32_t indexCount, std::uint32_t firstIndex = 0);
    [[nodiscard]] eng::core::Result<void> end();  // submete (uma vez)
    [[nodiscard]] bool isEnded() const noexcept;
};

/// Frontend alto nível (missão §4). Move-only, RAII.
class Renderer {
public:
    // --- registro de backends (executável final linka o que quiser) -------
    using BackendFactory = std::unique_ptr<RhiBackend> (*)();
    /// Registra fábrica por tipo. AlreadyExists se repetido.
    static eng::core::Result<void> registerBackend(BackendType,
                                                   BackendFactory);
    /// Testes apenas — documentado como tal.
    static void clearRegisteredBackends();
    [[nodiscard]] static std::vector<BackendType> registeredBackends();

    // --- criação com seleção e validação (missão §10) ---------------------
    static eng::core::Result<Renderer> create(const RendererConfig&);

    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    ~Renderer();   // frame pendente: end; recursos: destroy-all via backend
    // sem cópia

    [[nodiscard]] const RendererCapabilities& capabilities() const noexcept;
    [[nodiscard]] BackendType activeBackend() const noexcept;
    [[nodiscard]] ValidationState validationState() const noexcept;
    [[nodiscard]] bool hasSurface() const noexcept;

    [[nodiscard]] eng::core::Result<BufferHandle> createBuffer(
        const BufferDesc&);
    [[nodiscard]] eng::core::Result<ShaderHandle> createShader(
        const ShaderDesc&);
    [[nodiscard]] eng::core::Result<GraphicsPipelineHandle>
        createGraphicsPipeline(const GraphicsPipelineDesc&);
    [[nodiscard]] eng::core::Result<void> updateBuffer(BufferHandle,
        std::size_t offset, std::span<const std::byte> data);
    [[nodiscard]] eng::core::Result<void> destroyBuffer(BufferHandle);
    [[nodiscard]] eng::core::Result<void> destroyShader(ShaderHandle);
    [[nodiscard]] eng::core::Result<void> destroyGraphicsPipeline(
        GraphicsPipelineHandle);

    [[nodiscard]] eng::core::Result<AcquiredFrame> beginFrame();
    [[nodiscard]] eng::core::Result<void> present();
    [[nodiscard]] eng::core::Result<void> resize(std::uint32_t w,
                                                 std::uint32_t h);
};
} // namespace eng::rhi
```

#### Seleção de backend (`Renderer::create`) — missão §10

1. **Nenhum backend registrado** → `Error{NotFound, "nenhum backend rhi
   registrado neste executável"}`.
2. **`Auto`**: ordem de preferência documentada `Vulkan → OpenGLES`; para
   cada registrado: `probe()` → se `>= Available`, `initialize()` com
   validação completa; sucesso → escolhido (motivos de rejeição dos demais
   registrados por `ENG_INFO`/`ENG_WARN`). Nenhum → `Error` agregando TODOS
   os motivos (não há fallback silencioso — a escolha Auto é explícita e
   logada por backend rejeitado).
3. **Explícito (`Vulkan`/`OpenGLES`)**: não registrado → `NotFound` preciso;
   registrado → validação completa via `initialize()`; falha → erro preciso
   do backend **sem fallback**, a menos que `allowFallback == true` (aí
   segue a ordem de preferência, com `ENG_WARN` "fallback explícito").
4. Surface presente: `initialize` exige apresentação (futuro `presentable`);
   ausente: modo device-only — recursos/capabilities funcionam, `beginFrame`
   retorna `Error{NotSupported, "...sem surface..."}` (missão §13).

#### Validação de descritores no frontend

Barato e determinístico antes de tocar o backend: `BufferDesc::size == 0`,
`BufferUsage::None`, `ShaderDesc` sem nenhuma representação, pipeline sem
shader, `VertexLayout` vazio ou com referências a binding inexistente,
`initialData.size() > size`, `GraphicsPipelineDesc::renderTarget` com formato
`Undefined` (cor). Backend permanece a autoridade final (estado, suporte
real de formato, etc.).

## 4. Ownership, lifetime e thread-safety

- **Registry**: mutável em init do processo; protegido por mutex (escrita
  exclusiva, leitura compartilhada — política ADR-021/034). `clear*` é
  documentado "somente testes".
- **`Renderer`**: move-only. Moved-from → `isValid() == false`; operações
  retornam `Error{InvalidArgument, "renderer não inicializado"}` (defensivo,
  testado). Destrutor: `end` de frame pendente (fail-safe), destruição de
  TODOS os recursos restantes e do backend via dtor do backend.
- **`Frame`**: move-only. Moved-from → `isValid() == false`; operações →
  erro. Destrutor com frame pendente executa `end()` (fail-safe, logado em
  `Warn`).
- **Recursos**: owned pelo backend; destruição explícítica por handle ou em
  cascata no fim do `Renderer`. Handles são value types triviais.
- **Thread affinity**: `Renderer` inteiro é **single-threaded** (uma thread
  de render — declaração estilo ADR-034, ADR-035 registra). O registry é
  thread-safe; nada mais é.

## 5. Erros

Tudo por `eng::core::Result<T, eng::core::Error>` (ADR-004; validado com
tipos move-only na auditoria). Mensagens com prefixo estável para os casos
de frame: `"rhi.surface: ..."`, `"rhi.frame: ..."`, `"rhi.resource: ..."`.
Códigos usados: `InvalidArgument` (null/stale/desc inválido), `NotFound`
(backend não registrado), `NotSupported` (sem surface, representação de
shader ausente, formato sem suporte), `NotAvailable`... — quando faltar
granularidade, `Unknown` + mensagem precisa (contrato: mensagem SEMPRE
acicionável; testes casam por substrings estáveis).

## 6. Testes (unitários, FakeBackend)

`engine/rhi/tests/` — **FakeBackend determinístico, nunca production,
nunca reportado como suporte real** (missão §11):

1. Registro/seleção (vazio, Auto ordem, duplicado, explícito não
   registrado, falha de initialize explícito sem fallback, allowFallback).
2. Validatio state honesto (enabled + unavailable → `Unavailable`).
3. Capabilities/deviceInfo pass-through com `softwareRendering=true` e nome
   "Fake (TESTES)".
4. Recursos: criação única/válida, update, destroy, double-destroy,
   stale (geração), null handle, desc inválido (frontend), desc rejeitado
   (backend).
5. Frame: happy path completo (clear→set→bind→draw→end→present) com
   verificação de ORDEM gravada pelo fake; draw sem pipeline; end duplo;
   begin sem end prévio; present sem frame; RAII auto-end no dtor de Frame;
   status Minimized/OutOfDate; SurfaceLost (via `surfaceLost()`).
6. Device-only: recursos ok, beginFrame → NotSupported.
7. Resize: chamada repassada; próximo beginFrame ok.
8. Renderer moved-from: operações → erro, sem crash; destruição libera tudo
   (fake reporta `activeResources() == 0`).

## 7. CMake e integração

```cmake
# engine/rhi/CMakeLists.txt (esqueleto — sem backends na FASE 4)
eng_add_module(rhi
    src/Renderer.cpp
    src/Frame.cpp
)
target_link_libraries(eng_rhi PUBLIC eng::core PRIVATE eng::log)

if(ENG_BUILD_TESTS)
    add_executable(eng_rhi_tests
        tests/RhiTests.cpp
        tests/FakeBackend.cpp
    )
    target_link_libraries(eng_rhi_tests PRIVATE eng::rhi Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_rhi_tests)
    add_test(NAME rhi COMMAND eng_rhi_tests)
endif()
```

Gate da fase (missão §15): Debug verde, Release verde, zero warnings,
testes antigos preservados (15 executáveis existentes + `rhi` = 16),
árvore limpa, ADR-035/036, docs `13-rhi.md`, roadmap/README atualizados.

## 8. Decisões registradas (para ADR-035/036)

1. `eng::rhi` é o módulo interface; backends registram fábricas e são
   linkados no executável final (grafo alvo de 00-overview.md, regra 1).
2. Handles opacos tipados com codificação de geração a cargo do backend.
3. `ShaderDesc` carrega AMBAS as representações (SPIR-V + GLSL ES) —
   paridade exigida pela missão §40.
4. Estados de frame esperados (OutOfDate/Minimized) são RESULTADO, não
   erro; SurfaceLost é erro recuperável documentado.
5. Fallback apenas explícito (`Auto` ou `allowFallback`) — nunca silencioso.
6. Texturas/samplers/compute/async: fora do escopo, pontos de extensão.
