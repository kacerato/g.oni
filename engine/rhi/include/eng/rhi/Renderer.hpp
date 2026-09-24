#pragma once

/// eng::rhi — frontend alto nível.
///
/// `Renderer` é a API que o restante do engine consome: criação/seleção de
/// backend com validação completa (missão §10), resources por handles
/// opacos (missão §8) e frame lifecycle `beginFrame → comandos → endFrame
/// → present` (missão §12). Nenhum tipo de API gráfica específica aparece
/// aqui (missão §4/§14).
///
/// Backends são linkados NO EXECUTÁVEL FINAL e registram fábricas
/// (grafo alvo de docs/architecture/00-overview.md, regra 1): `eng::rhi`
/// não conhece — nem em compilação, nem em link — Vulkan ou OpenGL ES.
///
/// Ownership/lifetime:
/// - `Renderer` e `Frame` são move-only e RAII; moved-from é INUTILIZÁVEL
///   de forma definida (operações retornam erro, não UB);
/// - o destrutor do `Renderer` encerra frame pendente e libera TODOS os
///   recursos restantes via backend;
/// - um `Frame` vivo mantém o backend vivo (shared state): destruir o
///   `Renderer` com frame gravando é SEGURO — o frame termina no dtor e o
///   backend é liberado depois (sem UB, ordem documentada).
///
/// Thread affinity: o `Renderer` inteiro é single-threaded
/// (uma thread de render). O registry de backends é thread-safe
/// (escrita exclusiva, leitura compartilhada — política ADR-021).

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "eng/core/Error.hpp"
#include "eng/core/Result.hpp"
#include "eng/rhi/Progress.hpp"
#include "eng/rhi/RhiBackend.hpp"
#include "eng/rhi/Types.hpp"

namespace eng::rhi {

namespace detail {
/// Estado compartilhado Renderer↔Frame: garante que um `Frame` vivo nunca
/// aponta para um backend destruído (ownership compartilhado mínimo).
struct RendererState {
    std::unique_ptr<RhiBackend> backend{};
    BackendType type{BackendType::Auto};
    bool hasSurface{false};
};
}  // namespace detail

/// Sessão de gravação de um frame — move-only, RAII (missão §12).
///
/// Comandos: clear / setViewport / setPipeline / bindVertexBuffer /
/// bindIndexBuffer / draw / drawIndexed. `end()` submete (exatamente uma
/// vez); o destrutor executa `end()` pendente como fail-safe (logado).
class Frame {
public:
    Frame() = default;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&&) noexcept;
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    /// Um frame default-construído (sem sessão) ou moved-from é inválido;
    /// TODA operação nele retorna erro — nunca UB.
    [[nodiscard]] bool isValid() const noexcept { return state_ != nullptr; }
    [[nodiscard]] bool isEnded() const noexcept { return ended_; }

    [[nodiscard]] eng::core::Result<void> clear(const ClearDesc& clear);
    [[nodiscard]] eng::core::Result<void> setViewport(const Viewport& viewport);
    [[nodiscard]] eng::core::Result<void> setPipeline(GraphicsPipelineHandle pipeline);
    [[nodiscard]] eng::core::Result<void> bindVertexBuffer(BufferHandle buffer);
    [[nodiscard]] eng::core::Result<void> bindIndexBuffer(BufferHandle buffer,
                                                          IndexType indexType);
    /// Textura+sampler para os draws seguintes (slot 0 — kMaxTextureSlots).
    [[nodiscard]] eng::core::Result<void> bindTexture(TextureHandle texture,
                                                      SamplerHandle sampler,
                                                      std::uint32_t slot = 0);
    /// Dados de uniform do frame (bloco std140 "PerFrame" — ex.: luzes 2D):
    /// copiados para região própria do frame-slot; valem para os draws
    /// seguintes até a próxima chamada. Teto somado: kMaxFrameUniformData.
    /// Múltiplas chamadas = múltiplas regiões (ex.: conjunto de luzes por
    /// camada, re-bindado entre grupos de draw).
    [[nodiscard]] eng::core::Result<void> setUniformData(
        std::span<const std::byte> data);
    [[nodiscard]] eng::core::Result<void> draw(std::uint32_t vertexCount,
                                              std::uint32_t firstVertex = 0);
    [[nodiscard]] eng::core::Result<void> drawIndexed(std::uint32_t indexCount,
                                                     std::uint32_t firstIndex = 0);
    /// Submete o frame. Segunda chamada → erro `InvalidArgument`.
    [[nodiscard]] eng::core::Result<void> end();

private:
    friend class Renderer;

    Frame(std::shared_ptr<detail::RendererState> state, std::uint64_t frameId);

    [[nodiscard]] eng::core::Result<void> checkUsable() const;

    std::shared_ptr<detail::RendererState> state_{};
    std::uint64_t frameId_{0};
    bool ended_{false};
};

/// Resultado de `Renderer::beginFrame` — estados de protocolo (missão §12):
/// `OutOfDate`/`Minimized` NÃO são erro. `frame` só é válido quando
/// `status == Renderable`.
struct AcquiredFrame {
    FrameAcquireStatus status{FrameAcquireStatus::Renderable};
    Frame frame{};
};

/// Frontend da interface gráfica — move-only, RAII (missão §4).
class Renderer {
public:
    /// Fábrica de backends, registrada pelo executável final.
    using BackendFactory = std::unique_ptr<RhiBackend> (*)();

    // --- registry (executável final linka o que quiser) -----------------------

    /// Registra uma fábrica para `type`. `AlreadyExists` se já registrada.
    /// Thread-safe (escrita exclusiva).
    static eng::core::Result<void> registerBackend(BackendType type, BackendFactory factory);

    /// Remove todos os registros. **Somente testes** — documentado como tal.
    static void clearRegisteredBackends();

    /// Tipos com fábrica registrada, em ordem de preferência.
    [[nodiscard]] static std::vector<BackendType> registeredBackends();

    // --- criação e seleção (missão §10) ---------------------------------------

    /// Cria o `Renderer` com seleção e validação completa:
    /// - `Auto`: tenta Vulkan → OpenGL ES (ordem documentada); cada rejeição
    ///   é logada; nenhuma disponível → erro agregando TODOS os motivos;
    /// - explícito: validação completa; falha → erro preciso SEM fallback;
    /// - `allowFallback == true`: após falha explícita, segue a ordem de
    ///   preferência com `ENG_WARN` (fallback NUNCA silencioso).
    /// Sem surface no config → modo device-only (missão §13).
    static eng::core::Result<Renderer> create(const RendererConfig& config);

    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // --- introspecção ----------------------------------------------------------

    /// Moved-from → capabilities vazias (zeros). Caso contrário, valores
    /// reais reportados pelo backend ativo.
    [[nodiscard]] const RendererCapabilities& capabilities() const noexcept;
    /// Moved-from → `Auto`.
    [[nodiscard]] BackendType activeBackend() const noexcept;
    /// Moved-from → `false`.
    [[nodiscard]] bool hasSurface() const noexcept;
    /// Atalho para `capabilities().validationState` (missão §18).
    [[nodiscard]] ValidationState validationState() const noexcept;

    // --- recursos (missão §8) --------------------------------------------------

    [[nodiscard]] eng::core::Result<BufferHandle> createBuffer(const BufferDesc& desc);
    [[nodiscard]] eng::core::Result<ShaderHandle> createShader(const ShaderDesc& desc);
    [[nodiscard]] eng::core::Result<GraphicsPipelineHandle> createGraphicsPipeline(
        const GraphicsPipelineDesc& desc);
    [[nodiscard]] eng::core::Result<void> updateBuffer(BufferHandle handle,
                                                      std::size_t offset,
                                                      std::span<const std::byte> data);
    [[nodiscard]] eng::core::Result<void> destroyBuffer(BufferHandle handle);
    [[nodiscard]] eng::core::Result<void> destroyShader(ShaderHandle handle);
    [[nodiscard]] eng::core::Result<void> destroyGraphicsPipeline(
        GraphicsPipelineHandle handle);

    // --- texturas/samplers -------------------------------------------------------

    [[nodiscard]] eng::core::Result<TextureHandle> createTexture(const TextureDesc& desc);
    [[nodiscard]] eng::core::Result<SamplerHandle> createSampler(const SamplerDesc& desc);
    [[nodiscard]] eng::core::Result<void> destroyTexture(TextureHandle handle);
    [[nodiscard]] eng::core::Result<void> destroySampler(SamplerHandle handle);

    // --- frame (missão §12) ----------------------------------------------------

    [[nodiscard]] eng::core::Result<AcquiredFrame> beginFrame();
    /// Apresenta o frame submetido (após `Frame::end()`). Sem frame
    /// submetido, sem surface ou surface indisponível → erro preciso.
    [[nodiscard]] eng::core::Result<void> present();
    /// Recrea a surface/swapchain (resize). Sem surface → `NotSupported`.
    [[nodiscard]] eng::core::Result<void> resize(std::uint32_t width, std::uint32_t height);

    /// Readback do pixel central da surface (RGBA8) — validação visual em
    /// testes. Backend sem readback → `NotSupported` preciso.
    [[nodiscard]] eng::core::Result<void> readCenterPixel(std::uint8_t outRgba[4]);
    /// Readback da surface inteira (RGBA8, base primeiro) — capturas.
    [[nodiscard]] eng::core::Result<void> readPixels(std::uint32_t width,
                                                     std::uint32_t height,
                                                     std::uint8_t* out);

private:
    Renderer(std::unique_ptr<RhiBackend> backend, BackendType type, bool hasSurface);

    [[nodiscard]] eng::core::Result<void> checkUsable() const;

    std::shared_ptr<detail::RendererState> state_{};
};

} // namespace eng::rhi
