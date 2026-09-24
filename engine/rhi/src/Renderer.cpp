#include "eng/rhi/Renderer.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "eng/log/Macros.hpp"

ENG_LOG_CATEGORY("rhi")

namespace eng::rhi {
namespace {

// =============================================================================
// Helpers
// =============================================================================

[[nodiscard]] eng::core::Error makeError(eng::core::StatusCode code,
                                         std::string_view message) {
    return eng::core::Error{code, std::string{message}};
}

[[nodiscard]] std::string_view backendTypeName(BackendType type) noexcept {
    switch (type) {
    case BackendType::Vulkan:
        return "Vulkan";
    case BackendType::OpenGLES:
        return "OpenGL ES";
    case BackendType::Auto:
        return "Auto";
    }
    return "?";
}

/// Ordem de preferência do `Auto` (documentada em ADR-036).
constexpr std::array<BackendType, 2> kPreferenceOrder{
    BackendType::Vulkan, BackendType::OpenGLES};

// =============================================================================
// Registry de fábricas (escrita exclusiva, leitura compartilhada — ADR-021)
// =============================================================================

struct BackendRegistry {
    std::mutex mutex{};
    std::map<BackendType, Renderer::BackendFactory> factories{};
};

BackendRegistry& backendRegistry() {
    static BackendRegistry registry;
    return registry;
}

/// Busca a fábrica registrada para `type` (nullptr se ausente).
[[nodiscard]] Renderer::BackendFactory lookupFactory(BackendType type) {
    BackendRegistry& registry = backendRegistry();
    const std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.factories.find(type);
    return it == registry.factories.end() ? nullptr : it->second;
}

// =============================================================================
// Hook de progresso (micro-marks RHI_*). Escrita na thread do host
// (antes de Renderer::create), leitura na MESMA thread durante a criação
// (contrato: sem concorrência — o hook é install/remove apenas ali).
// =============================================================================
ProgressHook g_progressHook = nullptr;
void* g_progressUserdata = nullptr;

} // namespace

void setProgressHook(ProgressHook hook, void* userdata) {
    g_progressHook = hook;
    g_progressUserdata = hook != nullptr ? userdata : nullptr;
}

void reportProgress(const char* stage, const char* status,
                     const char* detail) noexcept {
    if (g_progressHook != nullptr && stage != nullptr) {
        g_progressHook(g_progressUserdata, stage,
                       status != nullptr ? status : "ok", detail);
    }
}

namespace {

/// Inicializa a instância de backend JÁ CRIADA (o probe e a inicialização
/// compartilham a MESMA instância — o probe é sem efeitos colaterais).
/// Erro preenchido com motivo preciso (para o agregado do Auto — missão §10).
[[nodiscard]] eng::core::Result<std::unique_ptr<RhiBackend>> initializeBackend(
    BackendType type, std::unique_ptr<RhiBackend> backend, const RendererConfig& config,
    std::string& outReason) {
    if (backend == nullptr) {
        outReason = std::string{backendTypeName(type)} + ": fábrica retornou null";
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::Unknown, outReason));
    }

    // O sub-passo mais opaco da janela resume→surface fica
    // cercado — o log exportado nomeia EXATAMENTE o backend/estágio que
    // nunca completa no Realme C33.
    reportProgress(rhi_stage::BackendSelect, "begin",
                   std::string{backendTypeName(type)}.c_str());
    RendererCapabilities capabilities{};
    const auto initialized = backend->initialize(config, capabilities);
    if (!initialized) {
        outReason = std::string{backendTypeName(type)} + ": " +
                    initialized.error().message;
        reportProgress(rhi_stage::BackendSelect, "failed", outReason.c_str());
        return eng::core::makeUnexpected(initialized.error());
    }
    reportProgress(rhi_stage::BackendSelect, "ok",
                   std::string{backendTypeName(type)}.c_str());
    return backend;
}

} // namespace

// =============================================================================
// Registry público
// =============================================================================

eng::core::Result<void> Renderer::registerBackend(BackendType type,
                                                  BackendFactory factory) {
    if (factory == nullptr) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi: registerBackend com fábrica nula"));
    }
    BackendRegistry& registry = backendRegistry();
    const std::lock_guard<std::mutex> lock(registry.mutex);
    const auto [it, inserted] = registry.factories.emplace(type, factory);
    if (!inserted) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::AlreadyExists,
                      "rhi: backend " + std::string{backendTypeName(type)} +
                          " já registrado"));
    }
    return {};
}

void Renderer::clearRegisteredBackends() {
    BackendRegistry& registry = backendRegistry();
    const std::lock_guard<std::mutex> lock(registry.mutex);
    registry.factories.clear();
}

std::vector<BackendType> Renderer::registeredBackends() {
    std::vector<BackendType> out{};
    {
        BackendRegistry& registry = backendRegistry();
        const std::lock_guard<std::mutex> lock(registry.mutex);
        for (const BackendType type : kPreferenceOrder) {
            if (registry.factories.count(type) != 0) {
                out.push_back(type);
            }
        }
    }
    return out;
}

// =============================================================================
// Criação e seleção (missão §10)
// =============================================================================

eng::core::Result<Renderer> Renderer::create(const RendererConfig& config) {
    // --- validação de config ---------------------------------------------------
    if (!config.surface.window.isValid() &&
        (config.surface.width != 0 || config.surface.height != 0)) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi: surface com dimensões mas sem window — configure "
            "SurfaceDesc::window"));
    }
    if (config.surface.window.isValid() &&
        (config.surface.width == 0 || config.surface.height == 0)) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi: window sem dimensões — configure SurfaceDesc::width/height"));
    }
    if (config.framesInFlight == 0) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi: framesInFlight zero (backends clampam ao limite real)"));
    }
    if (config.backend == BackendType::Auto && config.allowFallback) {
        // Auto já tenta todos os backends por definição; aceitamos o flag
        // sem erro para não punir configs reutilizadas, mas registramos.
        ENG_INFO("rhi: allowFallback é redundante com backend Auto");
    }

    const bool hasSurface = config.surface.isValid();

    // --- seleção ----------------------------------------------------------------
    if (registeredBackends().empty()) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::NotFound,
                      "rhi: nenhum backend registrado neste executável (linke "
                      "um backend e chame Renderer::registerBackend)"));
    }
    if (config.backend != BackendType::Auto) {
        // Explícito: validação completa, SEM fallback silencioso.
        const Renderer::BackendFactory factory = lookupFactory(config.backend);
        if (factory == nullptr) {
            return eng::core::makeUnexpected(
                makeError(eng::core::StatusCode::NotFound,
                          "rhi: backend " +
                              std::string{backendTypeName(config.backend)} +
                              " não registrado neste executável"));
        }
        std::string reason{};
        auto attempted = initializeBackend(config.backend, factory(), config, reason);
        if (attempted) {
            ENG_INFO("rhi: backend {} inicializado ({})", backendTypeName(config.backend),
                     hasSurface ? "com surface" : "device-only");
            return Renderer(std::move(attempted.value()), config.backend, hasSurface);
        }
        if (!config.allowFallback) {
            return eng::core::makeUnexpected(attempted.error());
        }
        // Fallback EXPLÍCITO (allowFallback): prossegue na ordem de
        // preferência, logando cada passo — nunca silencioso.
        ENG_WARN("rhi: backend {} falhou ({}); allowFallback=true — tentando "
                 "os demais",
                 backendTypeName(config.backend), attempted.error().message);
    }

    // Auto (ou explícito+fallback): ordem de preferência, motivos agregados.
    std::vector<std::string> reasons{};
    for (const BackendType type : kPreferenceOrder) {
        if (config.backend == type) {
            continue;  // já tentado acima (falha com fallback explícito)
        }
        const Renderer::BackendFactory factory = lookupFactory(type);
        if (factory == nullptr) {
            reasons.push_back(std::string{backendTypeName(type)} +
                              ": não registrado");
            continue;
        }
        // UMA instância por tipo: probe sem efeitos + inicialização na
        // MESMA instância (o cenário de teste/fábrica aplica-se a ambas).
        std::unique_ptr<RhiBackend> backend = factory();
        if (backend == nullptr) {
            reasons.push_back(std::string{backendTypeName(type)} +
                              ": fábrica retornou null");
            continue;
        }
        const BackendProbe probe = backend->probe();
        // Auditoria FASE 5 (L2): probe nunca atesta honestamente acima de
        // Detected ("Available" exigiria criar a instance). Auto tenta a
        // inicialização completa a partir de Detected; só Unavailable pula.
        if (probe.availability == Availability::Unavailable) {
            reasons.push_back(std::string{backendTypeName(type)} + ": " + probe.detail);
            continue;
        }
        std::string reason{};
        auto attempted = initializeBackend(type, std::move(backend), config, reason);
        if (attempted) {
            if (!reasons.empty()) {
                ENG_INFO("rhi: backends rejeitados antes de {}: {}", backendTypeName(type),
                         [&] {
                             std::string joined{};
                             for (const auto& r : reasons) {
                                 joined += r + "; ";
                             }
                             return joined;
                         }());
            }
            ENG_INFO("rhi: backend {} inicializado ({})", backendTypeName(type),
                     hasSurface ? "com surface" : "device-only");
            return Renderer(std::move(attempted.value()), type, hasSurface);
        }
        reasons.push_back(reason);
        ENG_WARN("rhi: {} rejeitado: {}", backendTypeName(type), reason);
    }

    std::string aggregate{"nenhum backend rhi disponível"};
    for (const auto& reason : reasons) {
        aggregate += "; " + reason;
    }
    return eng::core::makeUnexpected(makeError(eng::core::StatusCode::NotSupported,
                                               "rhi: " + aggregate));
}

// =============================================================================
// Construtores / move / introspecção
// =============================================================================

Renderer::Renderer(std::unique_ptr<RhiBackend> backend, BackendType type,
                   bool hasSurface) {
    state_ = std::make_shared<detail::RendererState>();
    state_->backend = std::move(backend);
    state_->type = type;
    state_->hasSurface = hasSurface;
}

Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
Renderer::~Renderer() = default;

const RendererCapabilities& Renderer::capabilities() const noexcept {
    static const RendererCapabilities kEmpty{};
    if (state_ == nullptr || state_->backend == nullptr) {
        return kEmpty;
    }
    return state_->backend->capabilities();
}

BackendType Renderer::activeBackend() const noexcept {
    return state_ != nullptr ? state_->type : BackendType::Auto;
}

bool Renderer::hasSurface() const noexcept {
    return state_ != nullptr && state_->hasSurface;
}

ValidationState Renderer::validationState() const noexcept {
    return capabilities().validationState;
}

eng::core::Result<void> Renderer::checkUsable() const {
    if (state_ == nullptr || state_->backend == nullptr) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi: renderer não inicializado (moved-from?)"));
    }
    return {};
}

// =============================================================================
// Recursos
// =============================================================================

eng::core::Result<BufferHandle> Renderer::createBuffer(const BufferDesc& desc) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (desc.size == 0) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument, "rhi.buffer: tamanho zero"));
    }
    if (desc.usage == BufferUsage::None) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument, "rhi.buffer: uso None"));
    }
    if (desc.initialData.size() > desc.size) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi.buffer: initialData (" + std::to_string(desc.initialData.size()) +
                          " bytes) maior que size (" + std::to_string(desc.size) + ")"));
    }
    return state_->backend->createBuffer(desc);
}

eng::core::Result<ShaderHandle> Renderer::createShader(const ShaderDesc& desc) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    const bool hasAnyRepresentation = !desc.vertexSpirv.empty() ||
                                     !desc.fragmentSpirv.empty() ||
                                     !desc.vertexGlsl.empty() || !desc.fragmentGlsl.empty();
    if (!hasAnyRepresentation) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi.shader: nenhuma representação (SPIR-V e GLSL vazios)"));
    }
    return state_->backend->createShader(desc);
}

eng::core::Result<GraphicsPipelineHandle> Renderer::createGraphicsPipeline(
    const GraphicsPipelineDesc& desc) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!desc.shader.isValid()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument, "rhi.pipeline: sem shader"));
    }
    if (desc.vertexLayout.bindings.empty()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi.pipeline: vertexLayout.bindings vazio"));
    }
    if (desc.vertexLayout.attributes.empty()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi.pipeline: vertexLayout.attributes vazio"));
    }
    for (const auto& attribute : desc.vertexLayout.attributes) {
        const bool declared = std::any_of(
            desc.vertexLayout.bindings.begin(), desc.vertexLayout.bindings.end(),
            [&](const VertexLayout::Binding& binding) {
                return binding.binding == attribute.binding;
            });
        if (!declared) {
            return eng::core::makeUnexpected(
                makeError(eng::core::StatusCode::InvalidArgument,
                          "rhi.pipeline: attribute com binding " +
                              std::to_string(attribute.binding) + " não declarado"));
        }
    }
    // Nota (auditoria FASE 5, L1): colorFormat == Undefined significa
    // "herdar o formato da surface" (default) — válido por contrato; o
    // backend valida o formato explícito contra o real da surface.
    return state_->backend->createGraphicsPipeline(desc);
}

eng::core::Result<void> Renderer::updateBuffer(BufferHandle handle, std::size_t offset,
                                               std::span<const std::byte> data) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!handle.isValid()) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi.buffer: handle nulo em updateBuffer"));
    }
    return state_->backend->updateBuffer(handle, offset, data);
}

eng::core::Result<void> Renderer::destroyBuffer(BufferHandle handle) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!handle.isValid()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument, "rhi.buffer: handle nulo em destroy"));
    }
    return state_->backend->destroyBuffer(handle);
}

eng::core::Result<void> Renderer::destroyShader(ShaderHandle handle) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!handle.isValid()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument, "rhi.shader: handle nulo em destroy"));
    }
    return state_->backend->destroyShader(handle);
}

eng::core::Result<void> Renderer::destroyGraphicsPipeline(GraphicsPipelineHandle handle) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!handle.isValid()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi.pipeline: handle nulo em destroy"));
    }
    return state_->backend->destroyGraphicsPipeline(handle);
}

// --- texturas/samplers (evolução) ---------------------------------------------

eng::core::Result<TextureHandle> Renderer::createTexture(const TextureDesc& desc) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (desc.width == 0 || desc.height == 0) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi.texture: dimensões zero (" + std::to_string(desc.width) + "x" +
                          std::to_string(desc.height) + ")"));
    }
    if (desc.format != Format::R8G8B8A8Unorm && desc.format != Format::R8G8B8A8Srgb) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi.texture: formato não suportado (apenas R8G8B8A8Unorm/Srgb nesta "
                      "evolução)"));
    }
    if (desc.initialData.size() != desc.expectedDataSize()) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi.texture: initialData (" + std::to_string(desc.initialData.size()) +
                          " bytes) != width*height*4 (" +
                          std::to_string(desc.expectedDataSize()) + " bytes)"));
    }
    if (desc.width > 16384u || desc.height > 16384u) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument,
            "rhi.texture: dimensão acima do limite prático 16384"));
    }
    return state_->backend->createTexture(desc);
}

eng::core::Result<SamplerHandle> Renderer::createSampler(const SamplerDesc& desc) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->createSampler(desc);
}

eng::core::Result<void> Renderer::destroyTexture(TextureHandle handle) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!handle.isValid()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument, "rhi.texture: handle nulo em destroy"));
    }
    return state_->backend->destroyTexture(handle);
}

eng::core::Result<void> Renderer::destroySampler(SamplerHandle handle) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (!handle.isValid()) {
        return eng::core::makeUnexpected(makeError(
            eng::core::StatusCode::InvalidArgument, "rhi.sampler: handle nulo em destroy"));
    }
    return state_->backend->destroySampler(handle);
}

// =============================================================================
// Frame
// =============================================================================

eng::core::Result<AcquiredFrame> Renderer::beginFrame() {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    auto begun = state_->backend->beginFrame();
    if (!begun) {
        return eng::core::makeUnexpected(begun.error());
    }
    AcquiredFrame out{};
    out.status = begun.value().status;
    if (out.status == FrameAcquireStatus::Renderable) {
        out.frame = Frame(state_, begun.value().frameId);
    }
    return out;
}

eng::core::Result<void> Renderer::present() {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->present();
}

eng::core::Result<void> Renderer::resize(std::uint32_t width, std::uint32_t height) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (width == 0 || height == 0) {
        return eng::core::makeUnexpected(
            makeError(eng::core::StatusCode::InvalidArgument,
                      "rhi: resize com dimensão zero (minimized é status de frame)"));
    }
    return state_->backend->resize(width, height);
}

eng::core::Result<void> Renderer::readCenterPixel(std::uint8_t outRgba[4]) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->readCenterPixel(outRgba);
}

} // namespace eng::rhi
