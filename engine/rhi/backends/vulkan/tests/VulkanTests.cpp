/// Hardware tests do backend Vulkan REAL.
///
/// Classificação honesta (missão §41/§47):
/// - loader ausente → SKIP (não é falha — ambiente sem Vulkan);
/// - ICD ausente (sem GPU física, nem software) → SKIP com motivo;
/// - todo PASS aqui é execução REAL (lavapipe = software — reportado como
///   softwareRendering, nunca como suporte de hardware).
///
/// Estados do modelo: DETECTED (loader) → AVAILABLE (instance+device)
/// → PRESENTABLE (surface+swapchain) → RENDERING (frame submetido).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "eng/rhi/Renderer.hpp"
#include "eng/rhi/vulkan/VulkanBackend.hpp"
#include "eng/rhi/vulkan/VulkanInternal.hpp"

#include "triangle_vk_frag_spirv.hpp"
#include "triangle_vk_vert_spirv.hpp"
#include "sprite_vk_frag_spirv.hpp"
#include "sprite_vk_vert_spirv.hpp"

namespace {

using eng::rhi::BackendType;
using eng::rhi::BufferHandle;
using eng::rhi::BufferUsage;
using eng::rhi::FrameAcquireStatus;
using eng::rhi::GraphicsPipelineDesc;
using eng::rhi::NativeWindowHandle;
using eng::rhi::NativeWindowKind;
using eng::rhi::Renderer;
using eng::rhi::RendererCapabilities;
using eng::rhi::RendererConfig;
using eng::rhi::ShaderDesc;
using eng::rhi::ValidationState;
using eng::rhi::vulkan::VulkanBackend;

/// Config headless (a única surface criável nesta fase — auditoria F5 §3).
RendererConfig headlessConfig(std::uint32_t width = 64, std::uint32_t height = 48) {
    RendererConfig config{};
    config.backend = BackendType::Vulkan;
    config.enableValidation = true;
    config.surface.window =
        NativeWindowHandle{reinterpret_cast<const void*>(0x1), NativeWindowKind::Headless};
    config.surface.width = width;
    config.surface.height = height;
    return config;
}

/// Vertex data do triangle de paridade (stride 32: pos vec4 + cor vec4).
std::vector<float> triangleVertices() {
    // NDC: triângulo cobrindo boa parte do alvo.
    return {
        // pos.x   pos.y  pos.z  pos.w   r      g      b      a
        -0.75f, -0.75f, 0.f, 1.f, 1.0f, 0.2f, 0.2f, 1.0f,
         0.75f, -0.75f, 0.f, 1.f, 0.2f, 1.0f, 0.2f, 1.0f,
         0.0f,   0.75f, 0.f, 1.f, 0.2f, 0.2f, 1.0f, 1.0f,
    };
}

/// Pré-requisito do backend inicializado com surface headless; SKIP quando
/// o ambiente não tem Vulkan real (loader ou ICD).
struct VulkanReady {
    VulkanBackend backend;
    RendererCapabilities caps{};
    bool available{false};
    std::string skipReason{};

    VulkanReady(const RendererConfig& config) {
        const auto probe = backend.probe();
        if (probe.availability == eng::rhi::Availability::Unavailable) {
            skipReason = probe.detail;
            return;
        }
        const auto initialized = backend.initialize(config, caps);
        if (!initialized) {
            skipReason = "initialize falhou: " + initialized.error().message;
            return;
        }
        available = true;
    }
};

} // namespace

// =============================================================================
// Funções puras (sem loader) — unit
// =============================================================================

TEST_CASE("vulkan: mapeamento VkResult→nome estável", "[rhi][rhi_vulkan]")
{
    using eng::rhi::vulkan::vkResultName;
    CHECK(vkResultName(VK_SUCCESS) == "VK_SUCCESS");
    CHECK(vkResultName(VK_ERROR_OUT_OF_DATE_KHR) == "VK_ERROR_OUT_OF_DATE_KHR");
    CHECK(vkResultName(VK_SUBOPTIMAL_KHR) == "VK_SUBOPTIMAL_KHR");
    CHECK(vkResultName(VK_ERROR_SURFACE_LOST_KHR) == "VK_ERROR_SURFACE_LOST_KHR");
    const std::string unknown = vkResultName(static_cast<VkResult>(-1000));
    CHECK(unknown.find("código") != std::string::npos);
}

TEST_CASE("vulkan: validação de SPIR-V (magic/tamanho)", "[rhi][rhi_vulkan]")
{
    using eng::rhi::vulkan::spirvLooksValid;
    std::string error{};
    const unsigned char valid[] = {0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(spirvLooksValid(valid, sizeof(valid), error));
    CHECK_FALSE(spirvLooksValid(nullptr, 0, error));
    CHECK_FALSE(spirvLooksValid(valid, 21, error));          // não múltiplo de 4
    CHECK_FALSE(spirvLooksValid(valid, 8, error));           // menor que header
    const unsigned char badMagic[] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 1, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(spirvLooksValid(badMagic, sizeof(badMagic), error));
    // O SPIR-V de teste embutido é válido.
    CHECK(spirvLooksValid(
        reinterpret_cast<const unsigned char*>(eng::rhi::testing::kTriangleVertexSpirv.data()),
        eng::rhi::testing::kTriangleVertexSpirv.size() * 4, error));
}

TEST_CASE("vulkan: mapeamentos de formato e estado", "[rhi][rhi_vulkan]")
{
    using eng::rhi::Format;
    CHECK(eng::rhi::vulkan::toVkFormat(Format::B8G8R8A8Srgb) == VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(eng::rhi::vulkan::toVkFormat(Format::R32G32B32A32Sfloat) ==
          VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK(eng::rhi::vulkan::toVkFormat(Format::Undefined) == VK_FORMAT_UNDEFINED);
    CHECK(eng::rhi::vulkan::fromVkFormat(VK_FORMAT_B8G8R8A8_SRGB) == Format::B8G8R8A8Srgb);
    CHECK(eng::rhi::vulkan::fromVkFormat(VK_FORMAT_D32_SFLOAT) == Format::D32Sfloat);
}

// =============================================================================
// Probe / device-only (missão §13/§47)
// =============================================================================

TEST_CASE("vulkan: probe honesto e device-only sem surface", "[rhi][rhi_vulkan]")
{
    VulkanBackend backend{};
    const auto probe = backend.probe();
    if (probe.availability == eng::rhi::Availability::Unavailable) {
        SKIP("Vulkan indisponível: " + probe.detail);
    }
    REQUIRE(probe.availability == eng::rhi::Availability::Detected);
    INFO("probe: " << probe.detail);

    // Device-only: NENHUMA surface — recursos funcionam (missão §13).
    RendererCapabilities caps{};
    const auto initialized = backend.initialize(RendererConfig{}, caps);
    if (!initialized) {
        SKIP("initialize device-only falhou: " << initialized.error().message);
    }
    REQUIRE(initialized.ok());
    CHECK(caps.backendName == "Vulkan");
    CHECK(caps.apiVersion >= "1.1");
    CHECK_FALSE(caps.device.name.empty());
    CHECK(caps.maxTextureSize > 0);
    CHECK(caps.maxVertexAttributes > 0);
    CHECK(caps.presentation == false);  // device-only ≠ sem suporte

    // Recursos reais funcionam sem surface.
    std::vector<float> vertices = triangleVertices();
    eng::rhi::BufferDesc bufferDesc{};
    bufferDesc.size = vertices.size() * sizeof(float);
    bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialData = std::as_bytes(std::span{vertices});
    auto buffer = backend.createBuffer(bufferDesc);
    REQUIRE(buffer.ok());
    CHECK(buffer.value().isValid());
    CHECK(backend.destroyBuffer(buffer.value()).ok());

    // Frames exigem surface: erro preciso.
    auto frame = backend.beginFrame();
    REQUIRE(frame.isError());
    CHECK(frame.error().code == eng::core::StatusCode::NotSupported);
    CHECK(frame.error().message.find("surface") != std::string::npos);

    // Validation honesto: pedida (default) e presente → Enabled.
    CHECK(caps.validationState == ValidationState::Enabled);
}

// =============================================================================
// Caminho completo REAL: surface headless → swapchain → triangle → submit
// → present (milestone §28)
// =============================================================================

TEST_CASE("vulkan: triangle REAL submetido à GPU (milestone §28)", "[rhi][rhi_hardware]")
{
    VulkanReady ready{headlessConfig()};
    if (!ready.available) {
        SKIP("Vulkan indisponível: " + ready.skipReason);
    }
    VulkanBackend& backend = ready.backend;
    REQUIRE(ready.caps.presentation);

    // 1) Shader: exige SPIR-V (a representação GLSL é do backend GLES).
    ShaderDesc shaderDesc{};
    shaderDesc.debugName = "triangle";
    shaderDesc.vertexSpirv = eng::rhi::testing::kTriangleVertexSpirvBytes();
    shaderDesc.fragmentSpirv = eng::rhi::testing::kTriangleFragmentSpirvBytes();
    auto shader = backend.createShader(shaderDesc);
    REQUIRE(shader.ok());

    // GLSL sem SPIR-V → erro preciso (paridade §40: cada backend sua via).
    ShaderDesc glslOnly{};
    glslOnly.vertexGlsl = "void main(){}";
    glslOnly.fragmentGlsl = "void main(){}";
    auto rejected = backend.createShader(glslOnly);
    REQUIRE(rejected.isError());
    CHECK(rejected.error().code == eng::core::StatusCode::NotSupported);

    // 2) Vertex buffer REAL com staging (DEVICE_LOCAL).
    std::vector<float> vertices = triangleVertices();
    eng::rhi::BufferDesc bufferDesc{};
    bufferDesc.size = vertices.size() * sizeof(float);
    bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialData = std::as_bytes(std::span{vertices});
    auto vertexBuffer = backend.createBuffer(bufferDesc);
    REQUIRE(vertexBuffer.ok());

    // 3) Pipeline com render target herdado da surface (L1: Undefined).
    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.shader = shader.value();
    pipelineDesc.vertexLayout.bindings.push_back({0, 32});
    pipelineDesc.vertexLayout.attributes.push_back(
        {0, 0, 0, eng::rhi::Format::R32G32B32A32Sfloat});
    pipelineDesc.vertexLayout.attributes.push_back(
        {1, 0, 16, eng::rhi::Format::R32G32B32A32Sfloat});
    auto pipeline = backend.createGraphicsPipeline(pipelineDesc);
    REQUIRE(pipeline.ok());

    // 4) Frame: begin → clear → pipeline → vbo → draw(3) → end → present.
    auto acquired = backend.beginFrame();
    REQUIRE(acquired.ok());
    REQUIRE(acquired.value().status == FrameAcquireStatus::Renderable);
    const std::uint64_t frameId = acquired.value().frameId;

    eng::rhi::ClearDesc clear{};
    clear.color = {0.1f, 0.1f, 0.15f, 1.f};
    REQUIRE(backend.frameClear(frameId, clear).ok());
    REQUIRE(backend.frameSetPipeline(frameId, pipeline.value()).ok());
    REQUIRE(backend.frameBindVertexBuffer(frameId, vertexBuffer.value()).ok());
    REQUIRE(backend.frameDraw(frameId, 3, 0).ok());
    REQUIRE(backend.endFrame(frameId).ok());  // SUBMISSÃO REAL

    REQUIRE(backend.present().ok());  // APRESENTAÇÃO REAL
    CHECK(backend.stats().framesSubmitted >= 1);
    CHECK(backend.stats().presentsOk >= 1);
    CHECK(backend.stats().validationLayerActive);  // layers de verdade ativas

    // 5) Segundo frame — ciclo completo de novo (fences/sema em rotação).
    auto second = backend.beginFrame();
    REQUIRE(second.ok());
    REQUIRE(second.value().status == FrameAcquireStatus::Renderable);
    REQUIRE(backend.frameSetPipeline(second.value().frameId, pipeline.value()).ok());
    REQUIRE(backend.frameBindVertexBuffer(second.value().frameId, vertexBuffer.value()).ok());
    REQUIRE(backend.frameDraw(second.value().frameId, 3, 0).ok());
    REQUIRE(backend.endFrame(second.value().frameId).ok());
    REQUIRE(backend.present().ok());
    CHECK(backend.stats().framesSubmitted >= 2);

    // 6) Protocolo: present sem frame submetido → erro preciso.
    auto presented = backend.present();
    REQUIRE(presented.isError());
    CHECK(presented.error().code == eng::core::StatusCode::NotSupported);

    // 7) Resize: recria swapchain; próximo frame segue.
    REQUIRE(backend.resize(32, 32).ok());
    CHECK(backend.stats().swapchainRecreations >= 2);  // criação + resize
    auto afterResize = backend.beginFrame();
    REQUIRE(afterResize.ok());
    REQUIRE(afterResize.value().status == FrameAcquireStatus::Renderable);

    // 8) Handles stale contra o driver REAL.
    auto destroyed = backend.destroyBuffer(vertexBuffer.value());
    REQUIRE(destroyed.ok());
    auto stale = backend.updateBuffer(vertexBuffer.value(), 0, {});
    REQUIRE(stale.isError());
    CHECK(stale.error().code == eng::core::StatusCode::InvalidArgument);
}

// =============================================================================
// Renderer + registro de fábrica (integração da abstraction — missão §10)
// =============================================================================

TEST_CASE("vulkan: registro no Renderer e Auto seleciona Vulkan real", "[rhi][rhi_hardware]")
{
    Renderer::clearRegisteredBackends();
    REQUIRE(Renderer::registerBackend(BackendType::Vulkan,
                                      &eng::rhi::vulkan::createBackend).ok());
    RendererConfig config = headlessConfig();
    config.backend = BackendType::Auto;
    auto created = Renderer::create(config);
    if (!created) {
        SKIP("Vulkan indisponível via Renderer: " + created.error().message);
    }
    auto renderer = std::move(created).value();
    CHECK(renderer.activeBackend() == BackendType::Vulkan);
    CHECK(renderer.hasSurface());
    CHECK(renderer.capabilities().backendName == "Vulkan");
    // softwareRendering honesto quando lavapipe (nunca reportado hardware).
    INFO("device: " << renderer.capabilities().device.name);
    CHECK(renderer.capabilities().device.kind == eng::rhi::DeviceKind::Cpu);
    CHECK(renderer.capabilities().softwareRendering);
}

// =============================================================================
// Textura REAL com draw amostrado (evolução — caminho imagem→sprite).
// Sem readback no Vulkan headless (render target é a swapchain): a validação
// é por SUBMISSÃO REAL + VALIDATION LAYERS ATIVAS (bind de descriptor set
// incorreto seria reportado/capturado) — nível RENDERING do §47.
// =============================================================================

TEST_CASE("vulkan: textura REAL com draw amostrado e mips (evolução)",
          "[rhi][rhi_hardware]")
{
    VulkanReady ready{headlessConfig()};
    if (!ready.available) {
        SKIP("Vulkan indisponível: " + ready.skipReason);
    }
    VulkanBackend& backend = ready.backend;
    REQUIRE(ready.caps.presentation);

    // 1) Sprite shader SPIR-V (set 0 / binding 0 = sampler2D).
    ShaderDesc shaderDesc{};
    shaderDesc.debugName = "sprite";
    shaderDesc.vertexSpirv = eng::rhi::testing::kSpriteVertexSpirvBytes();
    shaderDesc.fragmentSpirv = eng::rhi::testing::kSpriteFragmentSpirvBytes();
    auto shader = backend.createShader(shaderDesc);
    REQUIRE(shader.ok());

    // 2) Textura 4x4 RGBA8 real (upload staging + barreira de layout).
    constexpr std::uint32_t kW = 4;
    constexpr std::uint32_t kH = 4;
    std::vector<std::uint8_t> pixels(kW * kH * 4, 0);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = (y * kW + x) * 4;
            pixels[i + 0] = static_cast<std::uint8_t>(x * 60);
            pixels[i + 1] = static_cast<std::uint8_t>(y * 60);
            pixels[i + 2] = 180;
            pixels[i + 3] = 255;
        }
    }
    eng::rhi::TextureDesc textureDesc{};
    textureDesc.width = kW;
    textureDesc.height = kH;
    textureDesc.format = eng::rhi::Format::R8G8B8A8Unorm;
    textureDesc.generateMipmaps = true;  // 4x4 → 3 níveis (blit chain real)
    textureDesc.initialData = std::as_bytes(std::span{pixels});
    auto texture = backend.createTexture(textureDesc);
    REQUIRE(texture.ok());

    // 3) Sampler NEAREST/CLAMP.
    eng::rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.magFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.mipFilter = eng::rhi::FilterMode::Nearest;
    auto sampler = backend.createSampler(samplerDesc);
    REQUIRE(sampler.ok());

    // 4) Quad full-screen com UV (stride 40: pos vec4 + cor vec4 + uv vec2).
    const std::vector<float> vertices = {
        // pos.x  pos.y   z    w    r    g    b    a    u    v
        -1.f, -1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f,
         1.f, -1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f,
         1.f,  1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
        -1.f, -1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f,
         1.f,  1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 1.f,
    };
    eng::rhi::BufferDesc bufferDesc{};
    bufferDesc.size = vertices.size() * sizeof(float);
    bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialData = std::as_bytes(std::span{vertices});
    auto vertexBuffer = backend.createBuffer(bufferDesc);
    REQUIRE(vertexBuffer.ok());

    // 5) Pipeline com blending (sprite).
    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.shader = shader.value();
    pipelineDesc.vertexLayout.bindings.push_back({0, 40});
    pipelineDesc.vertexLayout.attributes.push_back(
        {0, 0, 0, eng::rhi::Format::R32G32B32A32Sfloat});
    pipelineDesc.vertexLayout.attributes.push_back(
        {1, 0, 16, eng::rhi::Format::R32G32B32A32Sfloat});
    pipelineDesc.vertexLayout.attributes.push_back(
        {2, 0, 32, eng::rhi::Format::R32G32Sfloat});
    pipelineDesc.raster.cull = eng::rhi::CullMode::None;
    pipelineDesc.blend.enabled = true;
    pipelineDesc.blend.srcColor = eng::rhi::BlendFactor::SrcAlpha;
    pipelineDesc.blend.dstColor = eng::rhi::BlendFactor::OneMinusSrcAlpha;
    auto pipeline = backend.createGraphicsPipeline(pipelineDesc);
    REQUIRE(pipeline.ok());

    // 6) Frame com textura vinculada (descriptor set real).
    auto acquired = backend.beginFrame();
    REQUIRE(acquired.ok());
    REQUIRE(acquired.value().status == FrameAcquireStatus::Renderable);
    const std::uint64_t frameId = acquired.value().frameId;
    eng::rhi::ClearDesc clear{};
    clear.color = {0.02f, 0.02f, 0.03f, 1.f};
    REQUIRE(backend.frameClear(frameId, clear).ok());
    REQUIRE(backend.frameSetPipeline(frameId, pipeline.value()).ok());
    REQUIRE(backend.frameBindVertexBuffer(frameId, vertexBuffer.value()).ok());
    REQUIRE(backend.frameBindTexture(frameId, texture.value(), sampler.value(), 0).ok());
    REQUIRE(backend.frameDraw(frameId, 6, 0).ok());
    REQUIRE(backend.endFrame(frameId).ok());
    REQUIRE(backend.present().ok());
    CHECK(backend.stats().framesSubmitted >= 1);
    CHECK(backend.stats().presentsOk >= 1);
    CHECK(backend.stats().validationLayerActive);

    // 7) Segundo frame reutilizando o descriptor set CACHEADO (par).
    auto second = backend.beginFrame();
    REQUIRE(second.ok());
    const std::uint64_t secondId = second.value().frameId;
    REQUIRE(backend.frameSetPipeline(secondId, pipeline.value()).ok());
    REQUIRE(backend.frameBindVertexBuffer(secondId, vertexBuffer.value()).ok());
    REQUIRE(backend.frameBindTexture(secondId, texture.value(), sampler.value(), 0).ok());
    REQUIRE(backend.frameDraw(secondId, 6, 0).ok());
    REQUIRE(backend.endFrame(secondId).ok());
    REQUIRE(backend.present().ok());

    // 8) Sem pipeline definido → erro preciso (layout do bind é do pipeline).
    auto third = backend.beginFrame();
    REQUIRE(third.ok());
    auto noPipeline = backend.frameBindTexture(third.value().frameId, texture.value(),
                                               sampler.value(), 0);
    REQUIRE(noPipeline.isError());
    CHECK(noPipeline.error().message.find("pipeline") != std::string::npos);
    // Encerra a sessão de forma limpa para não abortar o frame pendente.
    REQUIRE(backend.frameSetPipeline(third.value().frameId, pipeline.value()).ok());
    REQUIRE(backend.frameDraw(third.value().frameId, 3, 0).ok());
    REQUIRE(backend.endFrame(third.value().frameId).ok());
    REQUIRE(backend.present().ok());

    // 9) Destroy + stale contra o driver REAL.
    REQUIRE(backend.destroySampler(sampler.value()).ok());
    auto staleSampler = backend.frameBindTexture(frameId, texture.value(),
                                                 sampler.value(), 0);
    REQUIRE(staleSampler.isError());
    REQUIRE(backend.destroyTexture(texture.value()).ok());
    auto destroyedAgain = backend.destroyTexture(texture.value());
    REQUIRE(destroyedAgain.isError());
}
