#pragma once

/// eng::rhi::gles — backend OpenGL ES REAL.
///
/// Contexto EGL REAL (plataforma surfaceless + pbuffer para present),
/// GLSL ES compilado/linkado de verdade com info log nos erros, VAO/VBO
/// reais, estado aplicado a partir da INTENÇÃO do GraphicsPipelineDesc
/// (missão §37). NENHUM Fake — ausência de EGL/GLES reportada
/// honestamente pelo probe (Unavailable).

#include <cstdint>
#include <string>
#include <vector>

#include "eng/rhi/RhiBackend.hpp"
#include "eng/rhi/Types.hpp"
#include "eng/rhi/gles/GlesInternal.hpp"
#include "eng/rhi/gles/GlesLoader.hpp"

namespace eng::rhi::gles {

/// Diagnóstico (testes de hardware — números REAIS).
struct GlesStats {
    std::uint64_t framesSubmitted{0};  ///< endFrame com glFinish bem-sucedido
    std::uint64_t presentsSubmitted{0}; ///< eglSwapBuffers chamados
    std::uint64_t presentsOk{0};        ///< apresentações com sucesso
    std::uint32_t surfaceRecreations{0};
    std::string contextVersion{};       ///< "OpenGL ES 3.2 Mesa ..."
};

/// Fábrica para `Renderer::registerBackend(BackendType::OpenGLES, ...)`.
[[nodiscard]] std::unique_ptr<RhiBackend> createBackend();

class GlesBackend final : public RhiBackend {
public:
    GlesBackend() = default;
    ~GlesBackend() override;

    GlesBackend(const GlesBackend&) = delete;
    GlesBackend& operator=(const GlesBackend&) = delete;

    // --- RhiBackend ------------------------------------------------------------
    [[nodiscard]] BackendProbe probe() override;
    eng::core::Result<void> initialize(const RendererConfig& config,
                                       RendererCapabilities& outCapabilities) override;
    [[nodiscard]] const RendererCapabilities& capabilities() const override;

    [[nodiscard]] eng::core::Result<BufferHandle> createBuffer(const BufferDesc& desc) override;
    [[nodiscard]] eng::core::Result<ShaderHandle> createShader(const ShaderDesc& desc) override;
    [[nodiscard]] eng::core::Result<GraphicsPipelineHandle> createGraphicsPipeline(
        const GraphicsPipelineDesc& desc) override;
    eng::core::Result<void> updateBuffer(BufferHandle handle, std::size_t offset,
                                         std::span<const std::byte> data) override;
    eng::core::Result<void> destroyBuffer(BufferHandle handle) override;
    eng::core::Result<void> destroyShader(ShaderHandle handle) override;
    eng::core::Result<void> destroyGraphicsPipeline(GraphicsPipelineHandle handle) override;

    [[nodiscard]] eng::core::Result<TextureHandle> createTexture(
        const TextureDesc& desc) override;
    [[nodiscard]] eng::core::Result<SamplerHandle> createSampler(
        const SamplerDesc& desc) override;
    eng::core::Result<void> destroyTexture(TextureHandle handle) override;
    eng::core::Result<void> destroySampler(SamplerHandle handle) override;

    [[nodiscard]] eng::core::Result<BeginFrameResult> beginFrame() override;
    eng::core::Result<void> frameClear(std::uint64_t frameId, const ClearDesc& clear) override;
    eng::core::Result<void> frameSetViewport(std::uint64_t frameId,
                                             const Viewport& viewport) override;
    eng::core::Result<void> frameSetPipeline(std::uint64_t frameId,
                                              GraphicsPipelineHandle pipeline) override;
    eng::core::Result<void> frameBindVertexBuffer(std::uint64_t frameId,
                                                   BufferHandle buffer) override;
    eng::core::Result<void> frameBindIndexBuffer(std::uint64_t frameId, BufferHandle buffer,
                                                 IndexType indexType) override;
    eng::core::Result<void> frameBindTexture(std::uint64_t frameId, TextureHandle texture,
                                            SamplerHandle sampler, std::uint32_t slot) override;
    eng::core::Result<void> frameSetUniformData(
        std::uint64_t frameId, std::span<const std::byte> data) override;
    eng::core::Result<void> frameDraw(std::uint64_t frameId, std::uint32_t vertexCount,
                                      std::uint32_t firstVertex) override;
    eng::core::Result<void> frameDrawIndexed(std::uint64_t frameId, std::uint32_t indexCount,
                                             std::uint32_t firstIndex) override;
    eng::core::Result<void> endFrame(std::uint64_t frameId) override;
    eng::core::Result<void> present() override;
    eng::core::Result<void> resize(std::uint32_t width, std::uint32_t height) override;
    [[nodiscard]] bool surfaceLost() const override;

    // --- diagnóstico (testes de hardware) ----------------------------------------
    [[nodiscard]] const GlesStats& stats() const noexcept { return stats_; }
    /// Readback do pixel central (VALIDAÇÃO REAL de output — missão §39).
    /// Requer contexto atual; RGBA8 em `outRgba` (4 bytes).
    [[nodiscard]] eng::core::Result<void> readCenterPixel(
        std::uint8_t outRgba[4]) override;
    [[nodiscard]] eng::core::Result<void> readPixels(
        std::uint32_t width, std::uint32_t height, std::uint8_t* out) override;

private:
    struct BufferEntry {
        GLuint buffer{0};
        std::size_t size{0};
        eng::rhi::BufferUsage usage{};
    };
    struct ShaderEntry {
        GLuint program{0};  // program linkado (stages anexadas e liberadas)
    };
    struct PipelineEntry {
        GLuint program{0};
        GLuint vao{0};
        eng::rhi::GraphicsPipelineDesc desc{};  // intenção aplicada no bind
    };
    struct TextureEntry {
        GLuint texture{0};
        std::uint32_t width{0};
        std::uint32_t height{0};
        eng::rhi::Format format{eng::rhi::Format::Undefined};
        bool mipmaps{false};
    };
    struct SamplerEntry {
        eng::rhi::SamplerDesc desc{};
    };

    void destroyAll() noexcept;
    [[nodiscard]] eng::core::Result<void> requireInitialized() const;
    /// Configura atributos do VAO ativo com o VBO bound — chamado no draw
    /// (modelo clássico GL: glVertexAttribPointer captura o buffer bound).
    void configureVertexAttributes(const GlesFunctions& fn);
    [[nodiscard]] bool isRecording(std::uint64_t frameId) const;
    [[nodiscard]] eng::core::Result<void> createSurface(std::uint32_t width,
                                                         std::uint32_t height);
    [[nodiscard]] eng::core::Result<void> makeContextCurrent(bool withSurface);

    GlesLibrary library_{};
    EGLDisplay display_{EGL_NO_DISPLAY};
    EGLConfig config_{nullptr};
    EGLContext context_{EGL_NO_CONTEXT};
    EGLSurface surface_{EGL_NO_SURFACE};
#ifdef __ANDROID__
    EGLNativeWindowType window_{nullptr};  ///< ANativeWindow* (borrowed — ADR-040)
#endif
    std::uint32_t surfaceWidth_{0};
    std::uint32_t surfaceHeight_{0};

    HandleTable<BufferEntry> buffers_{};
    HandleTable<ShaderEntry> shaders_{};
    HandleTable<PipelineEntry> pipelines_{};
    HandleTable<TextureEntry> textures_{};
    HandleTable<SamplerEntry> samplers_{};

    eng::rhi::IndexType currentIndexType_{eng::rhi::IndexType::Uint16};
    bool pipelineSet_{false};
    GLuint boundVbo_{0};
    GLuint boundEbo_{0};
    eng::rhi::GraphicsPipelineDesc activePipelineDesc_{};
    /// Último (texture, sampler) aplicado por slot — evita redundância de
    /// estado GL em rebinds do mesmo par.
    struct AppliedTexture {
        std::uint64_t texture{0};
        std::uint64_t sampler{0};
    };
    AppliedTexture applied_[kMaxTextureSlots]{};

    // --- uniforms do frame (P3 §2 — luzes 2D) -------------------------------
    /// Buffer GL_UNIFORM_BUFFER compartilhado, região bump-alocada por
    /// chamada de frameSetUniformData (o cursor renasce a cada frame). O
    /// modelo é sequencial (uma gravação por vez — ADR-035), então um
    /// único buffer basta; o bind por região é o análogo GLES do dynamic
    /// offset do Vulkan.
    GLuint uniformGlBuffer_{0};
    GLsizeiptr uniformCursor_{0};
    /// Alinhamento das regiões (análogo do dynamic offset — 256B).
    static constexpr GLsizeiptr kUniformRegionAlign = 256;

    bool initialized_{false};
    bool hasSurface_{false};
    bool surfaceLost_{false};
    bool contextLost_{false};
    std::uint64_t activeFrameId_{0};  // uma sessão de gravação por vez
    std::uint64_t nextFrameId_{0};
    std::uint32_t pendingPresents_{0};

    RendererCapabilities capabilities_{};
    GlesStats stats_{};
};

} // namespace eng::rhi::gles
