#pragma once

/// eng::rhi — contrato que backends reais implementam.
///
/// Interface pequena e coesa (missão §7): ciclo de vida (probe/initialize),
/// recursos (buffer/shader/pipeline) e frame (beginFrame → operações →
/// endFrame → present). Vulkan, OpenGL ES e o
/// FakeBackend (SOMENTE testes — missão §11) implementam-na integralmente.
///
/// Regras do contrato:
/// - `probe()` não tem efeitos colaterais e não inicializa nada;
/// - `initialize()` executa a validação COMPLETA (missão §10) — loader,
///   instance/contexto, device, features, queues, surface quando houver —
///   e preenche capabilities com valores CONSULTADOS (missão §9);
/// - handles são opacos; null/stale retornam `InvalidArgument` preciso;
///   NUNCA undefined behavior;
/// - UMA sessão de gravação por vez: `beginFrame` enquanto um frame não
///   terminou é erro `InvalidArgument`;
/// - thread: single-threaded no lado do chamador; o backend pode
///   criar threads internas próprias, mas a interface não é thread-safe.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "eng/core/Error.hpp"
#include "eng/core/Result.hpp"
#include "eng/rhi/Types.hpp"

namespace eng::rhi {

/// Resultado do probe de disponibilidade (sem efeitos colaterais).
struct BackendProbe {
    Availability availability{Availability::Unavailable};
    /// Motivo textual honesto ("loader ausente", "sem device", ...).
    std::string detail{};
};

/// Saída de `beginFrame`: status do protocolo + identificador da sessão de
/// gravação (circula nas operações frameXxx e termina em endFrame).
struct BeginFrameResult {
    FrameAcquireStatus status{FrameAcquireStatus::Renderable};
    std::uint64_t frameId{0};
};

class RhiBackend {
public:
    virtual ~RhiBackend() = default;

    // --- ciclo de vida ---------------------------------------------------------

    /// Probe superficial de disponibilidade (missão §10): verifica o que dá
    /// para verificar SEM criar nada. Níveis acima de `Available` só são
    /// alcançados em `initialize()`.
    [[nodiscard]] virtual BackendProbe probe() = 0;

    /// Inicialização completa com validação real. Preenche `outCapabilities`
    /// com valores CONSULTADOS. `config.surface` ausente → modo device-only:
    /// recursos funcionam, `beginFrame`/`resize`/`present` devolvem erro
    /// preciso (missão §13).
    virtual eng::core::Result<void> initialize(const RendererConfig& config,
                                               RendererCapabilities& outCapabilities) = 0;

    [[nodiscard]] virtual const RendererCapabilities& capabilities() const = 0;

    // --- recursos ---------------------------------------------------------------

    [[nodiscard]] virtual eng::core::Result<BufferHandle> createBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual eng::core::Result<ShaderHandle> createShader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual eng::core::Result<GraphicsPipelineHandle> createGraphicsPipeline(
        const GraphicsPipelineDesc& desc) = 0;

    /// Upload síncrono (staging quando o backend exigir). Fora dos limites
    /// do buffer → `InvalidArgument` preciso.
    [[nodiscard]] virtual eng::core::Result<void> updateBuffer(
        BufferHandle handle, std::size_t offset, std::span<const std::byte> data) = 0;

    [[nodiscard]] virtual eng::core::Result<void> destroyBuffer(BufferHandle handle) = 0;
    [[nodiscard]] virtual eng::core::Result<void> destroyShader(ShaderHandle handle) = 0;
    [[nodiscard]] virtual eng::core::Result<void> destroyGraphicsPipeline(
        GraphicsPipelineHandle handle) = 0;

    // --- texturas/samplers (evolução — sprites/UI/preview) ---------------------

    /// Textura 2D imutável — upload na criação (staging REAL). Dimensões
    /// zero, tamanho de dados ≠ w*h*4 ou formato sem suporte REAL →
    /// erro preciso.
    [[nodiscard]] virtual eng::core::Result<TextureHandle> createTexture(
        const TextureDesc& desc) = 0;
    [[nodiscard]] virtual eng::core::Result<SamplerHandle> createSampler(
        const SamplerDesc& desc) = 0;
    [[nodiscard]] virtual eng::core::Result<void> destroyTexture(TextureHandle handle) = 0;
    [[nodiscard]] virtual eng::core::Result<void> destroySampler(SamplerHandle handle) = 0;

    // --- frame (missão §12) ---------------------------------------------------

    /// Adquire o próximo frame. `OutOfDate`/`Minimized` são protocolo, não
    /// erro; `frameId` só é válido quando status == `Renderable`.
    [[nodiscard]] virtual eng::core::Result<BeginFrameResult> beginFrame() = 0;

    [[nodiscard]] virtual eng::core::Result<void> frameClear(std::uint64_t frameId,
                                                             const ClearDesc& clear) = 0;
    [[nodiscard]] virtual eng::core::Result<void> frameSetViewport(std::uint64_t frameId,
                                                                   const Viewport& viewport) = 0;
    [[nodiscard]] virtual eng::core::Result<void> frameSetPipeline(
        std::uint64_t frameId, GraphicsPipelineHandle pipeline) = 0;
    [[nodiscard]] virtual eng::core::Result<void> frameBindVertexBuffer(
        std::uint64_t frameId, BufferHandle buffer) = 0;
    [[nodiscard]] virtual eng::core::Result<void> frameBindIndexBuffer(
        std::uint64_t frameId, BufferHandle buffer, IndexType indexType) = 0;
    /// Vincula textura+sampler no slot (0..kMaxTextureSlots-1) para os
    /// draws seguintes. Handles nulos/stale ou slot inválido → erro
    /// preciso.
    [[nodiscard]] virtual eng::core::Result<void> frameBindTexture(
        std::uint64_t frameId, TextureHandle texture, SamplerHandle sampler,
        std::uint32_t slot) = 0;
    [[nodiscard]] virtual eng::core::Result<void> frameDraw(std::uint64_t frameId,
                                                           std::uint32_t vertexCount,
                                                           std::uint32_t firstVertex) = 0;
    [[nodiscard]] virtual eng::core::Result<void> frameDrawIndexed(
        std::uint64_t frameId, std::uint32_t indexCount, std::uint32_t firstIndex) = 0;

    /// Dados de uniform do frame (bloco std140 — ex.: iluminação 2D):
    /// `data` é COPIADO no ato da chamada para uma REGIÃO própria do
    /// frame-slot em gravação (bump-alocada; sem hazard com frames in
    /// flight) e vale para os draws SEGUINTES até a próxima chamada.
    /// Limite somado por frame: kMaxFrameUniformData. Regiões alinhadas a
    /// 256B. Implementação de referência: backends Vulkan (UBO dinâmico,
    /// set 1/binding 0) e GLES (GL_UNIFORM_BUFFER binding 0).
    [[nodiscard]] virtual eng::core::Result<void> frameSetUniformData(
        std::uint64_t frameId, std::span<const std::byte> data)
    {
        (void)frameId;
        (void)data;
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::NotSupported,
            "rhi: backend sem dados de uniform do frame"});
    }

    /// Submete o frame (uma vez por frameId). Depois disso `present()`.
    /// Auditoria FASE 5 (L3): o frame submetido fica PENDENTE de
    /// apresentação; `present()` apresenta TODOS os pendentes, em ordem
    /// (frame submetido nunca apresentado vaza imagem de swapchain).
    [[nodiscard]] virtual eng::core::Result<void> endFrame(std::uint64_t frameId) = 0;

    /// Apresenta o frame submetido. Sem frame submetido ou sem surface →
    /// erro preciso (missão §12/§13).
    [[nodiscard]] virtual eng::core::Result<void> present() = 0;

    // --- surface / swapchain --------------------------------------------------

    /// Recreação (resize/surface recriada). Sem surface → `NotSupported`.
    [[nodiscard]] virtual eng::core::Result<void> resize(std::uint32_t width,
                                                         std::uint32_t height) = 0;

    /// Estado da surface após falha recuperável reportada em beginFrame/
    /// present. O chamador decide recriar a surface via `resize()` ou
    /// recriar o Renderer.
    [[nodiscard]] virtual bool surfaceLost() const = 0;

    // --- readback (validação visual — RECOVERY P0) ---------------------------

    /// Lê o pixel CENTRAL da surface de desenho (RGBA8). Fora de um frame,
    /// lê o último conteúdo apresentado. Backends sem readback → erro
    /// `NotSupported` preciso. É o instrumento de VALIDAÇÃO VISUAL dos
    /// testes (missão: "feature visual é validada visualmente").
    [[nodiscard]] virtual eng::core::Result<void> readCenterPixel(
        std::uint8_t outRgba[4])
    {
        (void)outRgba;
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::NotSupported,
            "rhi: backend sem readback de pixels"});
    }

    /// Lê a surface inteira (RGBA8, linha 0 = BASE da imagem, convenção GL)
    /// em `out` (width*height*4 bytes). Backends sem readback → NotSupported.
    [[nodiscard]] virtual eng::core::Result<void> readPixels(
        std::uint32_t width, std::uint32_t height, std::uint8_t* out)
    {
        (void)width;
        (void)height;
        (void)out;
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::NotSupported,
            "rhi: backend sem readback de pixels"});
    }
};

} // namespace eng::rhi
