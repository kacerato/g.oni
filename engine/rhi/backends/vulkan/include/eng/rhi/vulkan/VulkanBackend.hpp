#pragma once

/// eng::rhi::vulkan — backend Vulkan REAL.
///
/// Implementa `eng::rhi::RhiBackend` com a API Vulkan real: instance real,
/// validation layers reais, GPUs físicas enumeradas com motivo de rejeição
/// por candidata, device real, filas reais, surface (Headless nesta fase —
/// Xcb/Wayland/Android entregues pelas fases de plataforma), swapchain
/// real com recriação, render pass clássico, buffers com upload
/// por staging REAL, SPIR-V validado, submissão e apresentação reais.
///
/// NENHUM Fake/Mock: se o loader/ICD não existir, o probe/reporta
/// honestamente (missão §11/§47 — este backend nunca é simulado).

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "eng/rhi/RhiBackend.hpp"
#include "eng/rhi/Types.hpp"
#include "eng/rhi/vulkan/VulkanInternal.hpp"
#include "eng/rhi/vulkan/VulkanLoader.hpp"

namespace eng::rhi::vulkan {

/// Diagnóstico do backend (para testes de hardware — números REAIS).
struct VulkanStats {
    std::uint64_t framesSubmitted{0};     ///< vkQueueSubmit com sucesso
    std::uint64_t presentsSubmitted{0};    ///< vkQueuePresentKHR chamados
    std::uint64_t presentsOk{0};           ///< apresentações com sucesso
    std::uint32_t swapchainRecreations{0}; ///< recriações (resize/out-of-date)
    std::uint32_t gpusEnumerated{0};       ///< GPUs físicas vistas
    std::uint32_t gpusRejected{0};         ///< rejeitadas por requisitos
    bool validationLayerActive{false};     ///< layer Khronos de fato ativa
    std::string rejectedGpuReasons{};      ///< motivo por candidata (log)
};

/// Fábrica para `Renderer::registerBackend(BackendType::Vulkan, ...)`.
[[nodiscard]] std::unique_ptr<RhiBackend> createBackend();

class VulkanBackend final : public RhiBackend {
public:
    VulkanBackend() = default;
    ~VulkanBackend() override;

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

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
    [[nodiscard]] const VulkanStats& stats() const noexcept { return stats_; }
    [[nodiscard]] VkInstance instanceForDiagnostics() const noexcept { return instance_; }

private:
    struct BufferEntry {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        std::size_t size{0};
        eng::rhi::BufferUsage usage{};
    };
    struct ShaderEntry {
        VkShaderModule vertex{VK_NULL_HANDLE};
        VkShaderModule fragment{VK_NULL_HANDLE};
        std::string debugName{};
    };
    struct PipelineEntry {
        VkPipeline pipeline{VK_NULL_HANDLE};
        VkPipelineLayout layout{VK_NULL_HANDLE};
        eng::rhi::Format colorFormat{eng::rhi::Format::Undefined};
    };
    struct TextureEntry {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        std::uint32_t width{0};
        std::uint32_t height{0};
        VkFormat format{VK_FORMAT_UNDEFINED};
        std::uint32_t mipLevels{1};
    };
    struct SamplerEntry {
        VkSampler sampler{VK_NULL_HANDLE};
    };
    /// Frame in flight: um slot por frame pendente.
    struct FrameSlot {
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        VkSemaphore imageAvailable{VK_NULL_HANDLE};
        VkSemaphore renderFinished{VK_NULL_HANDLE};
        bool inFlight{false};
        bool recording{false};
        std::uint64_t frameId{0};
        std::uint32_t imageIndex{0};
        VkFramebuffer framebuffer{VK_NULL_HANDLE};
        /// Layout do pipeline em vigor nesta sessão (bind de descriptor set
        // precisa do layout REAL — rastreado em frameSetPipeline).
        VkPipelineLayout boundPipelineLayout{VK_NULL_HANDLE};
        // --- UBO dinâmico do frame (P3 §2: uniforms — luzes 2D) ---------------
        // Buffer HOST_VISIBLE persistente mapeado, bump-alocado por chamada
        // de frameSetUniformData. Cada FRAME usa apenas o SEU slot — sem
        // hazard entre frames in flight (região escrita ANTES da
        // submissão, lida apenas por este frame).
        VkBuffer uniformBuffer{VK_NULL_HANDLE};
        VkDeviceMemory uniformMemory{VK_NULL_HANDLE};
        void* uniformMapped{nullptr};
        VkDescriptorSet uniformDescriptor{VK_NULL_HANDLE};
        std::uint32_t uniformCursor{0};
    };

    // --- helpers internos (definidos nos .cpp correspondentes) ------------------
    void destroyAll() noexcept;
    [[nodiscard]] eng::core::Result<void> requireInitialized() const;
    [[nodiscard]] FrameSlot* findRecordingSlot(std::uint64_t frameId);
    [[nodiscard]] eng::core::Result<std::uint32_t> pickMemoryType(
        std::uint32_t typeBits, VkMemoryPropertyFlags properties,
        const char* what) const;
    [[nodiscard]] eng::core::Result<void> uploadToDeviceLocal(
        BufferEntry& entry, std::size_t offset, std::span<const std::byte> data);
    /// Sincroniza a destruição de texturas/samplers com frames em voo.
    void invalidateTextureDescriptorCache() noexcept;
    [[nodiscard]] eng::core::Result<void> createSwapchain(std::uint32_t width,
                                                          std::uint32_t height);
    void destroySwapchain() noexcept;
    [[nodiscard]] eng::core::Result<void> recreateSwapchain();

    // --- estado ------------------------------------------------------------------
    VulkanLibrary library_{};
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    std::uint32_t graphicsFamily_{0xFFFFFFFFu};
    std::uint32_t presentFamily_{0xFFFFFFFFu};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    VkQueue presentQueue_{VK_NULL_HANDLE};
    VkCommandPool commandPool_{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};

    // --- swapchain/render pass ---------------------------------------------------
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    std::vector<VkImage> swapchainImages_{};
    std::vector<VkImageView> swapchainViews_{};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};
    VkRenderPass renderPass_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_{};
    bool swapchainSuboptimal_{false};

    // Render passes de COMPATIBILIDADE device-only (um por formato pedido —
    // pipeline sem surface é legítimo; ver createGraphicsPipeline). Estes
    // NÃO participam da renderização (sem surface não há frames) — existem
    // para o vkCreateGraphicsPipelines ser uso VÁLIDO da API (renderPass
    // NULL exige dynamicRendering, que o backend não habilita — ADR-037).
    std::map<VkFormat, VkRenderPass> deviceOnlyRenderPasses_{};

    // --- recursos -----------------------------------------------------------------
    HandleTable<BufferEntry> buffers_{};
    HandleTable<ShaderEntry> shaders_{};
    HandleTable<PipelineEntry> pipelines_{};
    HandleTable<TextureEntry> textures_{};
    HandleTable<SamplerEntry> samplers_{};

    // --- texturas: descriptor set compartilhado (binding 0) ----------------------
    /// Layout com 1 combined image sampler no set 0 (fragment). TODOS os
    /// pipelines usam este layout — shaders que não amostram apenas ignoram
    /// o binding (legal em Vulkan).
    VkDescriptorSetLayout textureSetLayout_{VK_NULL_HANDLE};
    /// Pool de sets (pares textura+sampler cacheados — sem update pós-criação,
    /// então não há hazard com frames em voo).
    VkDescriptorPool textureDescriptorPool_{VK_NULL_HANDLE};
    /// Cache (textureHandle.id, samplerHandle.id) → set. Inválida por
    /// completo em destroyTexture/destroySampler (simples e correto).
    std::map<std::pair<std::uint64_t, std::uint64_t>, VkDescriptorSet>
        textureSetCache_{};

    // --- uniforms do frame (P3 §2): UBO DINÂMICO no set 1/binding 0 ---------
    /// Layout do set 1 (UBO dinâmico, vertex|fragment). Pipelines declaram
    /// [set0 texturas, set1 uniforms]; shaders sem bloco apenas ignoram.
    VkDescriptorSetLayout uniformSetLayout_{VK_NULL_HANDLE};
    /// Alinhamento das regiões do UBO (mín. garantido: 256B — spec).
    static constexpr std::uint32_t kUniformRegionAlign = 256;

    // --- frames ---------------------------------------------------------------------
    std::vector<FrameSlot> frameSlots_{};
    std::uint64_t nextFrameId_{0};
    std::size_t acquireSlot_{0};
    struct PendingPresent {
        std::uint32_t imageIndex{0};
        VkSemaphore renderFinished{VK_NULL_HANDLE};
    };
    std::vector<PendingPresent> pendingPresents_{};

    // --- estado geral ------------------------------------------------------------------
    bool initialized_{false};
    bool hasSurface_{false};
    bool surfaceLost_{false};
    RendererCapabilities capabilities_{};
    VulkanStats stats_{};
};

} // namespace eng::rhi::vulkan
