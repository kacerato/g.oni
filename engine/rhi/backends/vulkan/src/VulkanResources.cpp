/// Backend Vulkan — recursos: buffers (staging REAL), shaders SPIR-V
/// validados, pipelines com render pass clássico.

#include <algorithm>
#include <cstring>
#include <utility>

#include "eng/rhi/vulkan/VulkanBackend.hpp"
// (Sem ENG_LOG_CATEGORY: este TU não loga — declará-lo sem uso é warning
// no clang [-Wunused-const-variable]; a política de zero-warnings manda
// declarar a categoria apenas onde ela é usada.)

namespace eng::rhi::vulkan {
namespace {

using eng::core::Result;
using eng::core::StatusCode;
using eng::rhi::BufferUsage;

[[nodiscard]] eng::core::Error vkErr(StatusCode code, std::string_view what, VkResult result) {
    return eng::core::Error{code,
                            std::string{what} + " (" + vkResultName(result) + ")"};
}

[[nodiscard]] VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) noexcept {
    VkBufferUsageFlags flags = 0;
    if ((usage & BufferUsage::Vertex) != BufferUsage::None) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if ((usage & BufferUsage::Index) != BufferUsage::None) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if ((usage & BufferUsage::Uniform) != BufferUsage::None) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if ((usage & BufferUsage::Storage) != BufferUsage::None) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    // Todo buffer do backend recebe uploads por staging: destino
    // de transferência + origem para a cópia staging→device.
    flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}

[[nodiscard]] VkCullModeFlags toVkCullMode(eng::rhi::CullMode mode) noexcept {
    switch (mode) {
    case eng::rhi::CullMode::None: return VK_CULL_MODE_NONE;
    case eng::rhi::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    case eng::rhi::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

/// O viewport do Vulkan é invertido em Y (altura negativa, ver
/// VulkanFrame.cpp) para seguir a convenção de clip do GL, o que também
/// espelha o sentido dos triângulos na tela. Trocar o mapeamento mantém o
/// mesmo "front" nos dois backends.
[[nodiscard]] VkFrontFace toVkFrontFace(eng::rhi::FrontFace face) noexcept {
    return face == eng::rhi::FrontFace::Clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                  : VK_FRONT_FACE_CLOCKWISE;
}

[[nodiscard]] VkPolygonMode toVkPolygonMode(eng::rhi::FillMode mode,
                                            bool& outNeedsNonSolidFeature) noexcept {
    outNeedsNonSolidFeature = false;
    switch (mode) {
    case eng::rhi::FillMode::Solid: return VK_POLYGON_MODE_FILL;
    case eng::rhi::FillMode::Wireframe:
        outNeedsNonSolidFeature = true;
        return VK_POLYGON_MODE_LINE;
    }
    return VK_POLYGON_MODE_FILL;
}

[[nodiscard]] VkCompareOp toVkCompareOp(eng::rhi::CompareOp op) noexcept {
    using O = eng::rhi::CompareOp;
    switch (op) {
    case O::Never: return VK_COMPARE_OP_NEVER;
    case O::Less: return VK_COMPARE_OP_LESS;
    case O::Equal: return VK_COMPARE_OP_EQUAL;
    case O::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case O::Greater: return VK_COMPARE_OP_GREATER;
    case O::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case O::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case O::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

[[nodiscard]] VkBlendFactor toVkBlendFactor(eng::rhi::BlendFactor factor) noexcept {
    using F = eng::rhi::BlendFactor;
    switch (factor) {
    case F::Zero: return VK_BLEND_FACTOR_ZERO;
    case F::One: return VK_BLEND_FACTOR_ONE;
    case F::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case F::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case F::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case F::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

[[nodiscard]] VkBlendOp toVkBlendOp(eng::rhi::BlendOp op) noexcept {
    using O = eng::rhi::BlendOp;
    switch (op) {
    case O::Add: return VK_BLEND_OP_ADD;
    case O::Subtract: return VK_BLEND_OP_SUBTRACT;
    case O::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case O::Min: return VK_BLEND_OP_MIN;
    case O::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

} // namespace

// =============================================================================
// Buffers (missão §25): DEVICE_LOCAL + upload por staging REAL
// =============================================================================

Result<std::uint32_t> VulkanBackend::pickMemoryType(std::uint32_t typeBits,
                                                    VkMemoryPropertyFlags properties,
                                                    const char* what) const {
    for (std::uint32_t type = 0; type < memoryProperties_.memoryTypeCount; ++type) {
        if ((typeBits & (1u << type)) != 0u &&
            (memoryProperties_.memoryTypes[type].propertyFlags & properties) == properties) {
            return type;
        }
    }
    return eng::core::makeUnexpected(makeError(
        StatusCode::OutOfMemory,
        std::string{"rhi.vulkan: sem memory type para "} + what +
            " (flags exigidas não disponíveis)"));
}

Result<void> VulkanBackend::uploadToDeviceLocal(VulkanBackend::BufferEntry& entry,
                                                 std::size_t offset,
                                                 std::span<const std::byte> data) {
    const auto& fn = library_.functions();
    if (data.empty()) {
        return {};
    }

    // 1) Buffer de staging HOST_VISIBLE|HOST_COHERENT com o conteúdo.
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = data.size();
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkResult result = fn.vkCreateBuffer(device_, &stagingInfo, nullptr, &staging);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: staging", result));
    }
    VkMemoryRequirements stagingRequirements{};
    fn.vkGetBufferMemoryRequirements(device_, staging, &stagingRequirements);
    auto stagingType = pickMemoryType(stagingRequirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      "staging");
    if (!stagingType) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        return eng::core::makeUnexpected(stagingType.error());
    }
    VkMemoryAllocateInfo stagingAlloc{};
    stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingRequirements.size;
    stagingAlloc.memoryTypeIndex = stagingType.value();
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    result = fn.vkAllocateMemory(device_, &stagingAlloc, nullptr, &stagingMemory);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::OutOfMemory, "rhi.vulkan: staging", result));
    }
    result = fn.vkBindBufferMemory(device_, staging, stagingMemory, 0);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: staging bind", result));
    }
    void* mapped = nullptr;
    result = fn.vkMapMemory(device_, stagingMemory, 0, data.size(), 0, &mapped);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: staging map", result));
    }
    std::memcpy(mapped, data.data(), data.size());
    fn.vkUnmapMemory(device_, stagingMemory);

    // 2) Comando de cópia em buffer dedicado + fence própria (espera REAL).
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = fn.vkAllocateCommandBuffers(device_, &commandInfo, &command);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: staging cmd", result));
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    result = fn.vkCreateFence(device_, &fenceInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: staging fence", result));
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(command, &beginInfo);
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = offset;
    region.size = data.size();
    fn.vkCmdCopyBuffer(command, staging, entry.buffer, 1, &region);
    // O fence espera a cópia CONCLUIR: write visible para submits futuros.
    fn.vkEndCommandBuffer(command);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;
    result = fn.vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence);
    if (result == VK_SUCCESS) {
        result = fn.vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    fn.vkDestroyFence(device_, fence, nullptr);
    fn.vkFreeCommandBuffers(device_, commandPool_, 1, &command);
    fn.vkDestroyBuffer(device_, staging, nullptr);
    fn.vkFreeMemory(device_, stagingMemory, nullptr);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: upload staging submit/wait", result));
    }
    return {};
}

Result<BufferHandle> VulkanBackend::createBuffer(const BufferDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (desc.size == 0) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.buffer: tamanho zero"));
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = toVkBufferUsage(desc.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    BufferEntry entry{};
    entry.size = desc.size;
    entry.usage = desc.usage;
    VkResult result =
        library_.functions().vkCreateBuffer(device_, &bufferInfo, nullptr, &entry.buffer);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.buffer: vkCreateBuffer", result));
    }
    VkMemoryRequirements requirements{};
    library_.functions().vkGetBufferMemoryRequirements(device_, entry.buffer, &requirements);
    auto memoryType = pickMemoryType(requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "vertex/index");
    if (!memoryType) {
        library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
        return eng::core::makeUnexpected(memoryType.error());
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = memoryType.value();
    result = library_.functions().vkAllocateMemory(device_, &allocInfo, nullptr, &entry.memory);
    if (result != VK_SUCCESS) {
        library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::OutOfMemory, "rhi.vulkan.buffer: vkAllocateMemory", result));
    }
    result = library_.functions().vkBindBufferMemory(device_, entry.buffer, entry.memory, 0);
    if (result != VK_SUCCESS) {
        library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
        library_.functions().vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.buffer: vkBindBufferMemory", result));
    }

    // Upload inicial REAL (staging) quando há dados.
    if (!desc.initialData.empty()) {
        if (desc.initialData.size() > desc.size) {
            library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
            library_.functions().vkFreeMemory(device_, entry.memory, nullptr);
            return eng::core::makeUnexpected(makeError(
                StatusCode::InvalidArgument, "rhi.vulkan.buffer: initialData > size"));
        }
        const auto uploaded = uploadToDeviceLocal(entry, 0, desc.initialData);
        if (!uploaded) {
            library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
            library_.functions().vkFreeMemory(device_, entry.memory, nullptr);
            return eng::core::makeUnexpected(uploaded.error());
        }
    }
    return BufferHandle{buffers_.insert(std::move(entry))};
}

Result<void> VulkanBackend::updateBuffer(BufferHandle handle, std::size_t offset,
                                          std::span<const std::byte> data) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    BufferEntry* entry = buffers_.find(handle.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.buffer: handle nulo/stale"));
    }
    if (offset > entry->size || data.size() > entry->size - offset) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.buffer: update fora dos limites (offset=" + std::to_string(offset) +
                " tamanho=" + std::to_string(data.size()) + " buffer=" +
                std::to_string(entry->size) + ")"));
    }
    return uploadToDeviceLocal(*entry, offset, data);
}

Result<void> VulkanBackend::destroyBuffer(BufferHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    BufferEntry entry{};
    if (!buffers_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.buffer: handle nulo/stale/double"));
    }
    library_.functions().vkDeviceWaitIdle(device_);  // simples e correto nesta escala
    library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
    library_.functions().vkFreeMemory(device_, entry.memory, nullptr);
    return {};
}

// =============================================================================
// Shaders (missão §27): SPIR-V validado (magic/tamanho) — nenhuma simulação
// =============================================================================

Result<ShaderHandle> VulkanBackend::createShader(const ShaderDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (desc.vertexSpirv.empty() || desc.fragmentSpirv.empty()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan.shader: backend Vulkan exige SPIR-V (vertexSpirv e "
            "fragmentSpirv) — a representação GLSL é do backend GLES"));
    }
    std::string error{};
    if (!spirvLooksValid(reinterpret_cast<const unsigned char*>(desc.vertexSpirv.data()),
                         desc.vertexSpirv.size(), error)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.shader: vertex: " + error));
    }
    if (!spirvLooksValid(reinterpret_cast<const unsigned char*>(desc.fragmentSpirv.data()),
                         desc.fragmentSpirv.size(), error)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.shader: fragment: " + error));
    }

    ShaderEntry entry{};
    entry.debugName = std::string{desc.debugName};
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = desc.vertexSpirv.size();
    moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(desc.vertexSpirv.data());
    VkResult result =
        library_.functions().vkCreateShaderModule(device_, &moduleInfo, nullptr, &entry.vertex);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::InvalidArgument, "rhi.vulkan.shader: vkCreateShaderModule (vertex)",
                  result));
    }
    moduleInfo.codeSize = desc.fragmentSpirv.size();
    moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(desc.fragmentSpirv.data());
    result =
        library_.functions().vkCreateShaderModule(device_, &moduleInfo, nullptr, &entry.fragment);
    if (result != VK_SUCCESS) {
        library_.functions().vkDestroyShaderModule(device_, entry.vertex, nullptr);
        return eng::core::makeUnexpected(vkErr(
            StatusCode::InvalidArgument, "rhi.vulkan.shader: vkCreateShaderModule (fragment)",
            result));
    }
    return ShaderHandle{shaders_.insert(std::move(entry))};
}

Result<void> VulkanBackend::destroyShader(ShaderHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    ShaderEntry entry{};
    if (!shaders_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.shader: handle nulo/stale/double"));
    }
    library_.functions().vkDeviceWaitIdle(device_);
    library_.functions().vkDestroyShaderModule(device_, entry.vertex, nullptr);
    library_.functions().vkDestroyShaderModule(device_, entry.fragment, nullptr);
    return {};
}

// =============================================================================
// Pipeline (missão §24): render pass clássico, viewport/scissor dinâmicos
// =============================================================================

Result<GraphicsPipelineHandle> VulkanBackend::createGraphicsPipeline(
    const GraphicsPipelineDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    ShaderEntry* shader = shaders_.find(desc.shader.id);
    if (shader == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan.pipeline: shader nulo/stale"));
    }
    // L1 (auditoria F5): Undefined = formato da surface; explícito deve
    // coincidir com o REAL da swapchain (validado abaixo).
    const VkFormat targetFormat = toVkFormat(desc.renderTarget.colorFormat);
    if (desc.renderTarget.colorFormat != eng::rhi::Format::Undefined &&
        (!hasSurface_ || targetFormat != swapchainFormat_)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.pipeline: colorFormat explícito difere do formato real da surface"));
    }
    if (desc.renderTarget.depthFormat != eng::rhi::Format::Undefined) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan.pipeline: depth attachment entra com o render-graph futuro "
            "(FASE 5: color-only)"));
    }
    if (desc.depth.test || desc.depth.write) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.pipeline: DepthState com render target color-only"));
    }
    bool needsNonSolid = false;
    const VkPolygonMode polygonMode = toVkPolygonMode(desc.raster.fill, needsNonSolid);
    if (needsNonSolid) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.pipeline: wireframe exige fillModeNonSolid (não habilitada — "
            "ADR-037)"));
    }

    const auto& fn = library_.functions();
    PipelineEntry entry{};

    // Formato RESOLVIDO do target: explícito > surface > default
    // device-only. O default é B8G8R8A8 (o formato de swapchain
    // onipresente — Android incluído); o pipeline registra o formato
    // RESOLVIDO para o guard honesto do frameSetPipeline.
    const VkFormat resolvedFormat =
        desc.renderTarget.colorFormat != eng::rhi::Format::Undefined
            ? targetFormat
            : (hasSurface_ ? swapchainFormat_ : VK_FORMAT_B8G8R8A8_UNORM);
    entry.colorFormat = fromVkFormat(resolvedFormat);

    // Render pass do pipeline: com surface → o clássico compartilhado
    //; DEVICE-ONLY → pass de COMPATIBILIDADE por formato (cache
    // por backend). Pipelines sem surface são legítimos (CI/testes criam
    // a biblioteca inteira device-only); renderPass NULL + dynamicRendering
    // desabilitada é uso INVÁLIDO da API — a validation layer rejeitava o
    // vkCreateGraphicsPipelines (bug achado pelo teste P3 no CI).
    VkRenderPass pipelineRenderPass = renderPass_;
    if (pipelineRenderPass == VK_NULL_HANDLE) {
        auto cachedPass = deviceOnlyRenderPasses_.find(resolvedFormat);
        if (cachedPass != deviceOnlyRenderPasses_.end()) {
            pipelineRenderPass = cachedPass->second;
        } else {
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = resolvedFormat;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colorReference{
                0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorReference;
            VkRenderPassCreateInfo passInfo{};
            passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            passInfo.attachmentCount = 1;
            passInfo.pAttachments = &colorAttachment;
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            VkRenderPass created{VK_NULL_HANDLE};
            const VkResult passResult =
                fn.vkCreateRenderPass(device_, &passInfo, nullptr, &created);
            if (passResult != VK_SUCCESS) {
                return eng::core::makeUnexpected(vkErr(
                    StatusCode::Unknown,
                    "rhi.vulkan.pipeline: vkCreateRenderPass (device-only)", passResult));
            }
            pipelineRenderPass = created;
            deviceOnlyRenderPasses_.emplace(resolvedFormat, created);
        }
    }

    // Layout com set 0 (textura) + set 1 (UBO dinâmico de uniforms do
    // frame — P3 §2: luzes 2D). TODOS os pipelines compartilham o MESMO
    // par de sets — shaders que não usam apenas ignoram os bindings
    // (evolução: sprites/UI/preview/iluminação).
    VkDescriptorSetLayout setLayouts[2] = {textureSetLayout_, uniformSetLayout_};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    VkResult result = fn.vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &entry.layout);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.pipeline: vkCreatePipelineLayout", result));
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shader->vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader->fragment;
    stages[1].pName = "main";

    // Vertex input a partir do VertexLayout (intenção — missão §37).
    std::vector<VkVertexInputBindingDescription> bindings{};
    bindings.reserve(desc.vertexLayout.bindings.size());
    for (const auto& binding : desc.vertexLayout.bindings) {
        bindings.push_back({binding.binding, binding.stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes{};
    attributes.reserve(desc.vertexLayout.attributes.size());
    for (const auto& attribute : desc.vertexLayout.attributes) {
        attributes.push_back(
            {attribute.location, attribute.binding, toVkFormat(attribute.format),
             attribute.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor DINÂMICOS: pipeline não depende do tamanho da surface.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = polygonMode;
    rasterization.cullMode = toVkCullMode(desc.raster.cull);
    rasterization.frontFace = toVkFrontFace(desc.raster.front);
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.minSampleShading = 1.0f;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;  // color-only (validado acima)
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = toVkCompareOp(desc.depth.compare);  // coerente

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = desc.blend.enabled ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = toVkBlendFactor(desc.blend.srcColor);
    blendAttachment.dstColorBlendFactor = toVkBlendFactor(desc.blend.dstColor);
    blendAttachment.colorBlendOp = toVkBlendOp(desc.blend.colorOp);
    blendAttachment.srcAlphaBlendFactor = toVkBlendFactor(desc.blend.srcColor);
    blendAttachment.dstAlphaBlendFactor = toVkBlendFactor(desc.blend.dstColor);
    blendAttachment.alphaBlendOp = toVkBlendOp(desc.blend.colorOp);
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = entry.layout;
    pipelineInfo.renderPass = pipelineRenderPass;  // surface clássico OU
    // pass de compatibilidade device-only (formato resolvido acima)
    pipelineInfo.subpass = 0;
    result = fn.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &entry.pipeline);
    if (result != VK_SUCCESS) {
        fn.vkDestroyPipelineLayout(device_, entry.layout, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::InvalidArgument,
                                                "rhi.vulkan.pipeline: vkCreateGraphicsPipelines",
                                                result));
    }
    return GraphicsPipelineHandle{pipelines_.insert(std::move(entry))};
}

Result<void> VulkanBackend::destroyGraphicsPipeline(GraphicsPipelineHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    PipelineEntry entry{};
    if (!pipelines_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan.pipeline: handle nulo/stale/double"));
    }
    library_.functions().vkDeviceWaitIdle(device_);
    library_.functions().vkDestroyPipeline(device_, entry.pipeline, nullptr);
    library_.functions().vkDestroyPipelineLayout(device_, entry.layout, nullptr);
    return {};
}

// =============================================================================
// Texturas e samplers (evolução — sprites/UI/preview)
// =============================================================================

namespace {

[[nodiscard]] std::uint32_t mipLevelCount(std::uint32_t width, std::uint32_t height,
                                          bool generate) noexcept {
    if (!generate) {
        return 1;
    }
    std::uint32_t levels = 1;
    while (width > 1u || height > 1u) {
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
        ++levels;
    }
    return levels;
}

/// Barreira de layout de imagem (uma subresource range completa por nível
/// dado — usada no upload com mip).
void imageBarrier(const VulkanFunctions& fn, VkCommandBuffer command, VkImage image,
                  std::uint32_t mipBase, std::uint32_t mipCount,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess) noexcept {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = mipBase;
    barrier.subresourceRange.levelCount = mipCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    fn.vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
}

}  // namespace

Result<TextureHandle> VulkanBackend::createTexture(const TextureDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (desc.width == 0 || desc.height == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.texture: dimensões zero (" + std::to_string(desc.width) + "x" +
                std::to_string(desc.height) + ")"));
    }
    if (desc.initialData.size() != desc.expectedDataSize()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.vulkan.texture: initialData (" + std::to_string(desc.initialData.size()) +
                " bytes) != width*height*4 (" +
                std::to_string(desc.expectedDataSize()) + " bytes)"));
    }
    const VkFormat format = toVkFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan.texture: formato sem mapeamento (use R8G8B8A8Unorm/Srgb)"));
    }

    const auto& fn = library_.functions();
    const std::uint32_t mipLevels = mipLevelCount(desc.width, desc.height,
                                                   desc.generateMipmaps);

    // 1) Image DEVICE_LOCAL com TRANSFER_DST | SAMPLED.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    TextureEntry entry{};
    entry.width = desc.width;
    entry.height = desc.height;
    entry.format = format;
    entry.mipLevels = mipLevels;
    VkResult result = fn.vkCreateImage(device_, &imageInfo, nullptr, &entry.image);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: vkCreateImage", result));
    }

    // 2) Memória DEVICE_LOCAL + bind.
    VkMemoryRequirements requirements{};
    fn.vkGetImageMemoryRequirements(device_, entry.image, &requirements);
    auto memoryType = pickMemoryType(requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "texture");
    if (!memoryType) {
        fn.vkDestroyImage(device_, entry.image, nullptr);
        return eng::core::makeUnexpected(memoryType.error());
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = memoryType.value();
    result = fn.vkAllocateMemory(device_, &allocInfo, nullptr, &entry.memory);
    if (result != VK_SUCCESS) {
        fn.vkDestroyImage(device_, entry.image, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::OutOfMemory, "rhi.vulkan.texture: vkAllocateMemory", result));
    }
    result = fn.vkBindImageMemory(device_, entry.image, entry.memory, 0);
    if (result != VK_SUCCESS) {
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: vkBindImageMemory", result));
    }

    // 3) Staging HOST_VISIBLE com os pixels (padrão do uploadToDeviceLocal).
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = desc.initialData.size();
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    result = fn.vkCreateBuffer(device_, &stagingInfo, nullptr, &staging);
    if (result != VK_SUCCESS) {
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::OutOfMemory, "rhi.vulkan.texture: staging", result));
    }
    VkMemoryRequirements stagingRequirements{};
    fn.vkGetBufferMemoryRequirements(device_, staging, &stagingRequirements);
    auto stagingType = pickMemoryType(
        stagingRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "staging");
    if (!stagingType) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(stagingType.error());
    }
    VkMemoryAllocateInfo stagingAlloc{};
    stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingRequirements.size;
    stagingAlloc.memoryTypeIndex = stagingType.value();
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    result = fn.vkAllocateMemory(device_, &stagingAlloc, nullptr, &stagingMemory);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::OutOfMemory, "rhi.vulkan.texture: staging mem", result));
    }
    result = fn.vkBindBufferMemory(device_, staging, stagingMemory, 0);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: staging bind", result));
    }
    void* mapped = nullptr;
    result = fn.vkMapMemory(device_, stagingMemory, 0, desc.initialData.size(), 0, &mapped);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: staging map", result));
    }
    std::memcpy(mapped, desc.initialData.data(), desc.initialData.size());
    fn.vkUnmapMemory(device_, stagingMemory);

    // 4) Comando dedicado + fence (espera REAL — mesmo padrão do buffer):
    //    barrier UNDEFINED→TRANSFER_DST, copy buffer→image (mip 0),
    //    blits para os mips (quando pedidos), barrier →SHADER_READ_ONLY.
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = fn.vkAllocateCommandBuffers(device_, &commandInfo, &command);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: cmd", result));
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    result = fn.vkCreateFence(device_, &fenceInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
        fn.vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        fn.vkDestroyBuffer(device_, staging, nullptr);
        fn.vkFreeMemory(device_, stagingMemory, nullptr);
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: fence", result));
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(command, &beginInfo);

    imageBarrier(fn, command, entry.image, 0, mipLevels, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {desc.width, desc.height, 1};
    fn.vkCmdCopyBufferToImage(command, staging, entry.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (desc.generateMipmaps && mipLevels > 1) {
        // Blit em cadeia: nível i-1 → i (linear), com barreira por nível.
        for (std::uint32_t level = 1; level < mipLevels; ++level) {
            const std::uint32_t srcW = std::max(1u, desc.width >> (level - 1u));
            const std::uint32_t srcH = std::max(1u, desc.height >> (level - 1u));
            const std::uint32_t dstW = std::max(1u, desc.width >> level);
            const std::uint32_t dstH = std::max(1u, desc.height >> level);
            imageBarrier(fn, command, entry.image, level - 1, 1,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(srcW),
                                  static_cast<std::int32_t>(srcH), 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(dstW),
                                  static_cast<std::int32_t>(dstH), 1};
            fn.vkCmdBlitImage(command, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                              VK_FILTER_LINEAR);
        }
    }

    imageBarrier(fn, command, entry.image, 0, mipLevels,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
    fn.vkEndCommandBuffer(command);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;
    result = fn.vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence);
    if (result == VK_SUCCESS) {
        result = fn.vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    fn.vkDestroyFence(device_, fence, nullptr);
    fn.vkFreeCommandBuffers(device_, commandPool_, 1, &command);
    fn.vkDestroyBuffer(device_, staging, nullptr);
    fn.vkFreeMemory(device_, stagingMemory, nullptr);
    if (result != VK_SUCCESS) {
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: submit/wait", result));
    }

    // 5) View (todos os mips) — sampling pronto.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = entry.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    result = fn.vkCreateImageView(device_, &viewInfo, nullptr, &entry.view);
    if (result != VK_SUCCESS) {
        fn.vkDestroyImage(device_, entry.image, nullptr);
        fn.vkFreeMemory(device_, entry.memory, nullptr);
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.texture: vkCreateImageView", result));
    }
    return TextureHandle{textures_.insert(std::move(entry))};
}

Result<SamplerHandle> VulkanBackend::createSampler(const SamplerDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    const auto& fn = library_.functions();
    using F = eng::rhi::FilterMode;
    using A = eng::rhi::AddressMode;
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = desc.magFilter == F::Nearest ? VK_FILTER_NEAREST
                                                         : VK_FILTER_LINEAR;
    // Min com mip: nearest/linear por nível + nearest/linear ENTRE níveis.
    const bool minNearest = desc.minFilter == F::Nearest;
    const bool mipNearest = desc.mipFilter == F::Nearest;
    samplerInfo.minFilter = minNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = mipNearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = desc.addressU == A::ClampToEdge
                                  ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                  : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = desc.addressV == A::ClampToEdge
                                  ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                  : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = samplerInfo.addressModeU;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    SamplerEntry entry{};
    const VkResult result =
        fn.vkCreateSampler(device_, &samplerInfo, nullptr, &entry.sampler);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan.sampler: vkCreateSampler", result));
    }
    return SamplerHandle{samplers_.insert(std::move(entry))};
}

void VulkanBackend::invalidateTextureDescriptorCache() noexcept {
    // Sets podem estar referenciados por frames em voo — espera REAL antes
    // de reciclar o pool (simples e correto nesta escala; ADR-037).
    library_.functions().vkDeviceWaitIdle(device_);
    textureSetCache_.clear();
    if (textureDescriptorPool_ != VK_NULL_HANDLE) {
        (void)library_.functions().vkResetDescriptorPool(
            device_, textureDescriptorPool_, 0);
    }
}

Result<void> VulkanBackend::destroyTexture(TextureHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    TextureEntry entry{};
    if (!textures_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan.texture: handle nulo/stale/double"));
    }
    invalidateTextureDescriptorCache();
    const auto& fn = library_.functions();
    fn.vkDestroyImageView(device_, entry.view, nullptr);
    fn.vkDestroyImage(device_, entry.image, nullptr);
    fn.vkFreeMemory(device_, entry.memory, nullptr);
    return {};
}

Result<void> VulkanBackend::destroySampler(SamplerHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    SamplerEntry entry{};
    if (!samplers_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.vulkan.sampler: handle nulo/stale/double"));
    }
    invalidateTextureDescriptorCache();
    library_.functions().vkDestroySampler(device_, entry.sampler, nullptr);
    return {};
}

Result<void> VulkanBackend::readPixels(std::uint32_t width, std::uint32_t height,
                                       std::uint8_t* out) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_ || !swapchainReadable_ || lastPresentedImage_ < 0 ||
        width == 0 || height == 0 || width > swapchainExtent_.width ||
        height > swapchainExtent_.height) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan: readback exige um frame apresentado numa swapchain legível"));
    }
    const bool bgra = swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM ||
                      swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba = swapchainFormat_ == VK_FORMAT_R8G8B8A8_UNORM ||
                      swapchainFormat_ == VK_FORMAT_R8G8B8A8_SRGB;
    if (!bgra && !rgba) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported, "rhi.vulkan: readback só em formatos de 8 bits RGBA/BGRA"));
    }
    const auto& fn = library_.functions();
    fn.vkDeviceWaitIdle(device_);

    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4u;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = fn.vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: readback buffer", result));
    }
    VkMemoryRequirements requirements{};
    fn.vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    auto memoryType = pickMemoryType(requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     "readback");
    if (!memoryType) {
        fn.vkDestroyBuffer(device_, buffer, nullptr);
        return eng::core::makeUnexpected(memoryType.error());
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = memoryType.value();
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = fn.vkAllocateMemory(device_, &alloc, nullptr, &memory);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, buffer, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::OutOfMemory, "rhi.vulkan: readback", result));
    }
    fn.vkBindBufferMemory(device_, buffer, memory, 0);

    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = fn.vkAllocateCommandBuffers(device_, &commandInfo, &command);
    if (result != VK_SUCCESS) {
        fn.vkDestroyBuffer(device_, buffer, nullptr);
        fn.vkFreeMemory(device_, memory, nullptr);
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: readback cmd", result));
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(command, &beginInfo);

    const VkImage image = swapchainImages_[static_cast<std::size_t>(lastPresentedImage_)];
    imageBarrier(fn, command, image, 0, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_READ_BIT);
    // A imagem guarda a linha do TOPO primeiro; o contrato (GL) começa
    // pela base: copia as `height` linhas de baixo e inverte na saída.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, static_cast<std::int32_t>(swapchainExtent_.height - height), 0};
    region.imageExtent = {width, height, 1};
    fn.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                              &region);
    imageBarrier(fn, command, image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
    fn.vkEndCommandBuffer(command);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;
    result = fn.vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) {
        result = fn.vkDeviceWaitIdle(device_);
    }
    fn.vkFreeCommandBuffers(device_, commandPool_, 1, &command);
    if (result == VK_SUCCESS) {
        void* mapped = nullptr;
        result = fn.vkMapMemory(device_, memory, 0, size, 0, &mapped);
        if (result == VK_SUCCESS) {
            const auto* src = static_cast<const std::uint8_t*>(mapped);
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
            for (std::uint32_t row = 0; row < height; ++row) {
                const std::uint8_t* line = src + static_cast<std::size_t>(height - 1u - row) * rowBytes;
                std::uint8_t* dst = out + static_cast<std::size_t>(row) * rowBytes;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint8_t* p = line + x * 4u;
                    dst[x * 4u + 0] = bgra ? p[2] : p[0];
                    dst[x * 4u + 1] = p[1];
                    dst[x * 4u + 2] = bgra ? p[0] : p[2];
                    dst[x * 4u + 3] = p[3];
                }
            }
            fn.vkUnmapMemory(device_, memory);
        }
    }
    fn.vkDestroyBuffer(device_, buffer, nullptr);
    fn.vkFreeMemory(device_, memory, nullptr);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown, "rhi.vulkan: readback", result));
    }
    return {};
}

} // namespace eng::rhi::vulkan
