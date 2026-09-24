/// Backend Vulkan — frame lifecycle: swapchain, acquire, gravação, submit,
/// present, resize/recriação.

#include <cstring>
#include <utility>

#include "eng/log/Macros.hpp"
#include "eng/rhi/vulkan/VulkanBackend.hpp"

ENG_LOG_CATEGORY("rhi.vulkan")

namespace eng::rhi::vulkan {
namespace {

using eng::core::Result;
using eng::core::StatusCode;

[[nodiscard]] eng::core::Error vkErr(StatusCode code, std::string_view what, VkResult result) {
    return eng::core::Error{code, std::string{what} + " (" + vkResultName(result) + ")"};
}

[[nodiscard]] VkIndexType toVkIndexType(eng::rhi::IndexType type) noexcept {
    return type == eng::rhi::IndexType::Uint32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

/// Viewport com Y invertido (altura negativa, core no Vulkan 1.1). Os
/// shaders e todo o código de desenho usam a convenção de clip do GL
/// (+1 = topo); no Vulkan +1 é a base, e sem isto o quadro inteiro
/// (sprites, texto, grade) saía de cabeça para baixo no aparelho.
[[nodiscard]] VkViewport glConventionViewport(float x, float y, float width, float height,
                                              float minDepth, float maxDepth) noexcept {
    return VkViewport{x, y + height, width, -height, minDepth, maxDepth};
}

} // namespace

// =============================================================================
// Estado auxiliar
// =============================================================================

Result<void> VulkanBackend::requireInitialized() const {
    if (!initialized_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan: não inicializado"));
    }
    return {};
}

VulkanBackend::FrameSlot* VulkanBackend::findRecordingSlot(std::uint64_t frameId) {
    for (auto& slot : frameSlots_) {
        if (slot.recording && slot.frameId == frameId) {
            return &slot;
        }
    }
    return nullptr;
}

// =============================================================================
// Swapchain (missão §23)
// =============================================================================

void VulkanBackend::destroySwapchain() noexcept {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    const auto& fn = library_.functions();
    for (const VkFramebuffer framebuffer : framebuffers_) {
        fn.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (const VkImageView view : swapchainViews_) {
        fn.vkDestroyImageView(device_, view, nullptr);
    }
    swapchainViews_.clear();
    swapchainImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        fn.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

Result<void> VulkanBackend::createSwapchain(std::uint32_t width, std::uint32_t height) {
    const auto& fn = library_.functions();
    if (surface_ == VK_NULL_HANDLE) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotSupported, "rhi.vulkan: swapchain sem surface"));
    }

    // Capacidades/formatos/modos REAIS da surface (missão §23).
    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result = fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_,
                                                                    &capabilities);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::NotSupported, "rhi.vulkan: surface capabilities", result));
    }

    std::uint32_t formatCount = 0;
    result = fn.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                     nullptr);
    if (result != VK_SUCCESS || formatCount == 0) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::NotSupported, "rhi.vulkan: surface formats", result));
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = fn.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                     formats.data());
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::NotSupported, "rhi.vulkan: surface formats", result));
    }

    std::uint32_t presentModeCount = 0;
    result = fn.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_,
                                                          &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes{};
    if (result == VK_SUCCESS && presentModeCount > 0) {
        presentModes.resize(presentModeCount);
        result = fn.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_,
                                                              &presentModeCount,
                                                              presentModes.data());
    }

    // Formato: preferido B8G8R8A8Srgb (ou o único disponível — real).
    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = format;
            break;
        }
    }
    // Present mode: FIFO é GARANTIDO pela spec — MAILBOX só se reportado.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }

    // Extent: usar o ATUAL reportado pela surface quando definido.
    VkExtent2D extent{};
    if (capabilities.currentExtent.width != 0xFFFFFFFFu) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = width;
        extent.height = height;
    }
    extent.width = (extent.width < capabilities.minImageExtent.width)
                       ? capabilities.minImageExtent.width
                       : extent.width;
    extent.width = (extent.width > capabilities.maxImageExtent.width)
                       ? capabilities.maxImageExtent.width
                       : extent.width;
    extent.height = (extent.height < capabilities.minImageExtent.height)
                        ? capabilities.minImageExtent.height
                        : extent.height;
    extent.height = (extent.height > capabilities.maxImageExtent.height)
                        ? capabilities.maxImageExtent.height
                        : extent.height;

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainKHR oldSwapchain = swapchain_;
    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface_;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = chosenFormat.format;
    swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    // TRANSFER_SRC quando a surface permite: habilita readPixels (testes e
    // capturas); custo zero no caminho normal.
    swapchainReadable_ =
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (swapchainReadable_) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    swapchainInfo.imageUsage = usage;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = oldSwapchain;
    result = fn.vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        return eng::core::makeUnexpected(
            vkErr(StatusCode::NotSupported, "rhi.vulkan: vkCreateSwapchainKHR", result));
    }
    if (oldSwapchain != VK_NULL_HANDLE) {
        fn.vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    }

    swapchainFormat_ = chosenFormat.format;
    swapchainExtent_ = extent;
    lastPresentedImage_ = -1;
    swapchainSuboptimal_ = false;
    ++stats_.swapchainRecreations;

    // Imagens + views REAIS.
    std::uint32_t actualImageCount = 0;
    result = fn.vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, nullptr);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown,
                                                "rhi.vulkan: vkGetSwapchainImagesKHR", result));
    }
    swapchainImages_.resize(actualImageCount);
    result = fn.vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount,
                                         swapchainImages_.data());
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown,
                                                "rhi.vulkan: vkGetSwapchainImagesKHR", result));
    }
    swapchainViews_.resize(actualImageCount);
    for (std::uint32_t image = 0; image < actualImageCount; ++image) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages_[image];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = fn.vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[image]);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(
                vkErr(StatusCode::Unknown, "rhi.vulkan: vkCreateImageView", result));
        }
    }

    // Render pass clássico compartilhado: color-only, loadOp CLEAR
    // (cor padrão preta — Frame::clear aplica a cor exata via
    // vkCmdClearAttachments, ver TU do frame).
    if (renderPass_ != VK_NULL_HANDLE) {
        fn.vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    result = fn.vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_);
    if (result != VK_SUCCESS) {
        renderPass_ = VK_NULL_HANDLE;
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkCreateRenderPass", result));
    }

    // Framebuffers: um por imagem da swapchain.
    framebuffers_.resize(actualImageCount);
    for (std::uint32_t image = 0; image < actualImageCount; ++image) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &swapchainViews_[image];
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        result = fn.vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                                        &framebuffers_[image]);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(
                vkErr(StatusCode::Unknown, "rhi.vulkan: vkCreateFramebuffer", result));
        }
    }

    ENG_INFO("rhi.vulkan: swapchain {}x{} ({} imagens, formato {}, present {})",
             extent.width, extent.height, actualImageCount, vkResultName(VK_SUCCESS),
             presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");
    return {};
}

Result<void> VulkanBackend::recreateSwapchain() {
    // Seguro: sem frames em gravação/submissão pendente de apresentação
    // (chamado em pontos de sincronização — beginFrame/present/resize).
    library_.functions().vkDeviceWaitIdle(device_);
    pendingPresents_.clear();
    for (auto& slot : frameSlots_) {
        slot.inFlight = false;  // submissões já concluídas (idle)
    }
    return createSwapchain(swapchainExtent_.width, swapchainExtent_.height);
}

// =============================================================================
// beginFrame (missão §12/§13/§23)
// =============================================================================

Result<BeginFrameResult> VulkanBackend::beginFrame() {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan: beginFrame sem surface (modo device-only — missão §13)"));
    }
    if (surfaceLost_) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan: surface perdida — recrie via resize() ou um novo Renderer"));
    }
    for (const auto& slot : frameSlots_) {
        if (slot.recording) {
            return eng::core::makeUnexpected(makeError(
                StatusCode::InvalidArgument,
                "rhi.vulkan: frame anterior não finalizado (uma sessão de gravação)"));
        }
    }

    const auto& fn = library_.functions();

    // Suboptimal da apresentação anterior: recria ANTES de adquirir.
    if (swapchainSuboptimal_) {
        const auto recreated = recreateSwapchain();
        if (!recreated) {
            return eng::core::makeUnexpected(recreated.error());
        }
    }

    // Slot disponível: espera a fence da primeira slot ocupada (in flight).
    FrameSlot* slot = &frameSlots_[acquireSlot_ % frameSlots_.size()];
    ++acquireSlot_;
    VkResult result = fn.vkWaitForFences(device_, 1, &slot->fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkWaitForFences", result));
    }
    fn.vkResetFences(device_, 1, &slot->fence);

    // Extent atual == 0 → minimizado (missão §12: protocolo, não erro).
    VkSurfaceCapabilitiesKHR capabilities{};
    result = fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_,
                                                          &capabilities);
    if (result == VK_SUCCESS && capabilities.currentExtent.width == 0 &&
        capabilities.currentExtent.height == 0) {
        return BeginFrameResult{eng::rhi::FrameAcquireStatus::Minimized, 0};
    }

    // Adquire imagem REAL; OUT_OF_DATE → recria e TENTA UMA vez.
    std::uint32_t imageIndex = 0;
    result = fn.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, slot->imageAvailable,
                                      VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        const auto recreated = recreateSwapchain();
        if (!recreated) {
            return eng::core::makeUnexpected(recreated.error());
        }
        result = fn.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, slot->imageAvailable,
                                          VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Semáforo não sinalizado (acquire falhou) — reuso seguro.
            return BeginFrameResult{eng::rhi::FrameAcquireStatus::OutOfDate, 0};
        }
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        surfaceLost_ = true;
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported, "rhi.vulkan: surface perdida ao adquirir imagem"));
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkAcquireNextImageKHR", result));
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        swapchainSuboptimal_ = true;  // recria no próximo begin
    }

    // Gravação: reset + begin + render pass (framebuffer = imagem adquirida).
    result = fn.vkResetCommandBuffer(slot->command, 0);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkResetCommandBuffer", result));
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    result = fn.vkBeginCommandBuffer(slot->command, &beginInfo);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkBeginCommandBuffer", result));
    }

    VkClearValue clearValue{};
    clearValue.color = {{0.f, 0.f, 0.f, 1.f}};  // default preto
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    fn.vkCmdBeginRenderPass(slot->command, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport/scissor default = extent inteira (dynamic state exige valores
    // antes do draw — Frame::setViewport sobrepõe).
    const VkViewport viewport = glConventionViewport(
        0.f, 0.f, static_cast<float>(swapchainExtent_.width),
        static_cast<float>(swapchainExtent_.height), 0.f, 1.f);
    fn.vkCmdSetViewport(slot->command, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, swapchainExtent_};
    fn.vkCmdSetScissor(slot->command, 0, 1, &scissor);

    slot->recording = true;
    slot->uniformCursor = 0;  // P3 §2: orçamento do frame recomeça
    slot->inFlight = false;
    slot->imageIndex = imageIndex;
    slot->frameId = ++nextFrameId_;
    slot->boundPipelineLayout = VK_NULL_HANDLE;  // pipeline define por frame
    return BeginFrameResult{eng::rhi::FrameAcquireStatus::Renderable, slot->frameId};
}

// =============================================================================
// Comandos do frame (missão §12) — gravação no command buffer da sessão
// =============================================================================

Result<void> VulkanBackend::frameClear(std::uint64_t frameId, const ClearDesc& clear) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    if (clear.clearColor) {
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color = {{clear.color[0], clear.color[1], clear.color[2],
                                        clear.color[3]}};
        VkClearRect rect{};
        rect.rect.offset = {0, 0};
        rect.rect.extent = swapchainExtent_;
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        library_.functions().vkCmdClearAttachments(slot->command, 1, &attachment, 1, &rect);
    }
    return {};  // clearDepth ignorado em render target color-only (validado na pipeline)
}

Result<void> VulkanBackend::frameSetViewport(std::uint64_t frameId, const Viewport& viewport) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    const VkViewport vkViewport =
        glConventionViewport(viewport.x, viewport.y, viewport.width, viewport.height,
                             viewport.minDepth, viewport.maxDepth);
    library_.functions().vkCmdSetViewport(slot->command, 0, 1, &vkViewport);
    const VkRect2D scissor{
        {0, 0},
        {static_cast<std::uint32_t>(viewport.width), static_cast<std::uint32_t>(viewport.height)}};
    library_.functions().vkCmdSetScissor(slot->command, 0, 1, &scissor);
    return {};
}

Result<void> VulkanBackend::frameSetPipeline(std::uint64_t frameId,
                                             GraphicsPipelineHandle pipeline) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    PipelineEntry* entry = pipelines_.find(pipeline.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan.frame: pipeline nulo/stale"));
    }
    // Guard honesto (sem validation layers — release mobile não as tem):
    // pipeline criado SEM surface usa um render pass de compatibilidade no
    // formato RESOLVIDO; desenhar na surface REAL exige formato idêntico.
    // Vulkan detectaria isso só no draw (ou nunca, sem layers) — o erro
    // PRECISO aqui chega antes, no primeiro comando do frame.
    if (hasSurface_ && entry->colorFormat != fromVkFormat(swapchainFormat_)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.frame: pipeline criado para formato " +
                std::to_string(static_cast<int>(entry->colorFormat)) +
                " difere do formato da surface " +
                std::to_string(static_cast<int>(fromVkFormat(swapchainFormat_))) +
                " (pipeline device-only numa surface diferente — recrie o pipeline)"));
    }
    library_.functions().vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            entry->pipeline);
    slot->boundPipelineLayout = entry->layout;  // p/ bind de descriptor set
    return {};
}

Result<void> VulkanBackend::frameBindVertexBuffer(std::uint64_t frameId, BufferHandle buffer) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    BufferEntry* entry = buffers_.find(buffer.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: buffer nulo/stale"));
    }
    if ((entry->usage & eng::rhi::BufferUsage::Vertex) == eng::rhi::BufferUsage::None) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan.frame: buffer sem uso Vertex"));
    }
    const VkDeviceSize offset = 0;
    library_.functions().vkCmdBindVertexBuffers(slot->command, 0, 1, &entry->buffer, &offset);
    return {};
}

Result<void> VulkanBackend::frameBindIndexBuffer(std::uint64_t frameId, BufferHandle buffer,
                                                 IndexType indexType) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    BufferEntry* entry = buffers_.find(buffer.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: buffer nulo/stale"));
    }
    if ((entry->usage & eng::rhi::BufferUsage::Index) == eng::rhi::BufferUsage::None) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: buffer sem uso Index"));
    }
    library_.functions().vkCmdBindIndexBuffer(slot->command, entry->buffer, 0,
                                              toVkIndexType(indexType));
    return {};
}

Result<void> VulkanBackend::frameBindTexture(std::uint64_t frameId, TextureHandle texture,
                                             SamplerHandle sampler, std::uint32_t slotIndex) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    if (slot->boundPipelineLayout == VK_NULL_HANDLE) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.frame: bindTexture exige pipeline já definido (layout do bind)"));
    }
    if (slotIndex >= kMaxTextureSlots) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.frame: slot de textura inválido (kMaxTextureSlots=" +
                std::to_string(kMaxTextureSlots) + ")"));
    }
    TextureEntry* textureEntry = textures_.find(texture.id);
    if (textureEntry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: textura nula/stale"));
    }
    SamplerEntry* samplerEntry = samplers_.find(sampler.id);
    if (samplerEntry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sampler nulo/stale"));
    }

    const auto key = std::make_pair(texture.id, sampler.id);
    auto cached = textureSetCache_.find(key);
    if (cached == textureSetCache_.end()) {
        // Aloca + escreve UM descriptor set por par (textura, sampler) —
        // set imutável pós-criação (sem update enquanto em uso; resets só
        // acontecem após vkDeviceWaitIdle em destroy*).
        const auto& fn = library_.functions();
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = textureDescriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &textureSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = fn.vkAllocateDescriptorSets(device_, &setInfo, &set);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::OutOfMemory,
                "rhi.vulkan.frame: descriptor set de textura (pool exaurido?)", result));
        }
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = samplerEntry->sampler;
        imageInfo.imageView = textureEntry->view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        fn.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        cached = textureSetCache_.emplace(key, set).first;
    }

    // Layout REAL do pipeline em vigor (rastreado em frameSetPipeline) —
    // TODOS os pipelines do backend declaram o MESMO set 0 (textura).
    library_.functions().vkCmdBindDescriptorSets(
        slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS, slot->boundPipelineLayout, 0, 1,
        &cached->second, 0, nullptr);
    return {};
}

Result<void> VulkanBackend::frameSetUniformData(
    std::uint64_t frameId, std::span<const std::byte> data) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    if (slot->boundPipelineLayout == VK_NULL_HANDLE) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.frame: setUniformData exige pipeline já definido (layout do bind)"));
    }
    if (data.empty()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.frame: setUniformData com dados vazios"));
    }
    if (data.size() > kMaxFrameUniformData) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument,
                      "rhi.vulkan.frame: setUniformData excede kMaxFrameUniformData"));
    }
    // Região bump-alocada, alinhada (dynamic offset exige alinhamento do
    // device; 256B é o mínimo garantido pela spec p/ UNIFORM_BUFFER).
    const std::uint32_t cursor = slot->uniformCursor;
    const std::uint32_t aligned = (cursor + kUniformRegionAlign - 1) &
                                  ~(kUniformRegionAlign - 1);
    const std::uint32_t end = aligned + static_cast<std::uint32_t>(data.size());
    if (end > kMaxFrameUniformData) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.frame: orçamento de uniforms do frame exaurido ("
            + std::to_string(kMaxFrameUniformData) + " bytes)"));
    }
    // Escrita ANTES da submissão deste frame (região do PRÓPRIO slot):
    // sem hazard com frames in flight (modelo do UBO dinâmico).
    std::memcpy(static_cast<char*>(slot->uniformMapped) + aligned, data.data(),
                data.size());
    slot->uniformCursor = end;
    const auto& fn = library_.functions();
    const std::uint32_t dynamicOffsets[1] = {aligned};
    fn.vkCmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               slot->boundPipelineLayout, 1, 1,
                               &slot->uniformDescriptor, 1, dynamicOffsets);
    return {};
}

Result<void> VulkanBackend::frameDraw(std::uint64_t frameId, std::uint32_t vertexCount,
                                      std::uint32_t firstVertex) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    if (vertexCount == 0) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: draw com vertexCount 0"));
    }
    library_.functions().vkCmdDraw(slot->command, vertexCount, 1, firstVertex, 0);
    return {};
}

Result<void> VulkanBackend::frameDrawIndexed(std::uint64_t frameId, std::uint32_t indexCount,
                                             std::uint32_t firstIndex) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.frame: sessão inválida"));
    }
    if (indexCount == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan.frame: drawIndexed com indexCount 0"));
    }
    library_.functions().vkCmdDrawIndexed(slot->command, indexCount, 1, firstIndex, 0, 0);
    return {};
}

// =============================================================================
// endFrame (submissão REAL — missão §22/§28) e present
// =============================================================================

Result<void> VulkanBackend::endFrame(std::uint64_t frameId) {
    FrameSlot* slot = findRecordingSlot(frameId);
    if (slot == nullptr) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan: endFrame sem sessão ativa/end duplo"));
    }
    const auto& fn = library_.functions();

    fn.vkCmdEndRenderPass(slot->command);
    VkResult result = fn.vkEndCommandBuffer(slot->command);
    if (result != VK_SUCCESS) {
        slot->recording = false;
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkEndCommandBuffer", result));
    }

    // Submissão REAL: espera a imagem (imageAvailable), sinaliza renderFinished
    // e a fence do slot (missão §22 — sincronização canônica).
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &slot->imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot->command;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &slot->renderFinished;
    result = fn.vkQueueSubmit(graphicsQueue_, 1, &submitInfo, slot->fence);
    if (result != VK_SUCCESS) {
        slot->recording = false;
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkQueueSubmit", result));
    }

    slot->recording = false;
    slot->inFlight = true;
    ++stats_.framesSubmitted;
    // Frame submetido fica PENDENTE de apresentação (auditoria F5, L3).
    pendingPresents_.push_back(PendingPresent{slot->imageIndex, slot->renderFinished});
    return {};
}

Result<void> VulkanBackend::present() {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported, "rhi.vulkan: present sem surface (device-only)"));
    }
    if (pendingPresents_.empty()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported, "rhi.vulkan: present sem frame submetido (missão §12)"));
    }

    const auto& fn = library_.functions();
    // L3: apresenta TODOS os submetidos pendentes, EM ORDEM.
    while (!pendingPresents_.empty()) {
        const PendingPresent pending = pendingPresents_.front();
        pendingPresents_.erase(pendingPresents_.begin());
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &pending.renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &pending.imageIndex;
        ++stats_.presentsSubmitted;
        const VkResult result = fn.vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (result == VK_SUCCESS) {
            ++stats_.presentsOk;
            lastPresentedImage_ = pending.imageIndex;
            continue;
        }
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Frame consumido; recria antes do próximo begin.
            swapchainSuboptimal_ = true;
            ++stats_.presentsOk;  // apresentado (suboptimal) ou reconfigurado
            continue;
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            surfaceLost_ = true;
            return eng::core::makeUnexpected(makeError(
                StatusCode::NotSupported, "rhi.vulkan: surface perdida ao apresentar"));
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            return eng::core::makeUnexpected(makeError(
                StatusCode::Unknown, "rhi.vulkan: DEVICE LOST ao apresentar"));
        }
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkQueuePresentKHR", result));
    }
    return {};
}

// =============================================================================
// resize / surfaceLost (missão §12/§23)
// =============================================================================

Result<void> VulkanBackend::resize(std::uint32_t width, std::uint32_t height) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotSupported, "rhi.vulkan: resize sem surface"));
    }
    const auto recreated = createSwapchain(width, height);
    if (recreated) {
        surfaceLost_ = false;  // recriação restaura (paridade com o contrato)
        ENG_INFO("rhi.vulkan: swapchain recriada para {}x{}", width, height);
    }
    return recreated;
}

bool VulkanBackend::surfaceLost() const {
    return surfaceLost_;
}

} // namespace eng::rhi::vulkan
