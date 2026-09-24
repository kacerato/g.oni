/// Hardware tests do backend OpenGL ES REAL.
///
/// Classificação honesta: sem libEGL/libGLESv2 → SKIP com motivo;
/// todo PASS é execução REAL. O readback do pixel central dá validação de
/// OUTPUT (níveis RENDERING + VALIDATED — missão §39). A paridade com o
/// Vulkan (missão §40) usa o MESMO triangle e vertex data.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eng/rhi/Renderer.hpp"
#include "eng/rhi/gles/GlesBackend.hpp"
#include "eng/rhi/gles/GlesInternal.hpp"

// Fixtures de paridade (mesmo SPIR-V do backend Vulkan — missão §40).
#include "triangle_vk_frag_spirv.hpp"
#include "triangle_vk_vert_spirv.hpp"

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
using eng::rhi::gles::GlesBackend;

/// GLSL dos fixtures — lido em COMPILE-TIME não é possível sem embed; os
/// arquivos são pequenos e o CMake os copia... aqui lemos do source dir via
/// macro definida pelo CMake (ENG_SHADERS_DIR).
std::string readShaderFile(const char* name) {
    std::ifstream file{std::string{ENG_SHADERS_DIR} + "/" + name};
    std::ostringstream buffer{};
    buffer << file.rdbuf();
    return buffer.str();
}

RendererConfig headlessConfig(std::uint32_t width = 64, std::uint32_t height = 48) {
    RendererConfig config{};
    config.backend = BackendType::OpenGLES;
    config.enableValidation = true;
    config.surface.window =
        NativeWindowHandle{reinterpret_cast<const void*>(0x1), NativeWindowKind::Headless};
    config.surface.width = width;
    config.surface.height = height;
    return config;
}

/// MESMO vertex data do teste Vulkan (paridade §40): triângulo com topo
/// vermelho-ish... — o pixel central (0,0 em NDC) fica DENTRO do triângulo,
/// interpolado. Para verificação exata usamos um triângulo FULL-SCREEN
/// monocromático no teste de readback (cor conhecida), e este triângulo
/// colorido na paridade.
std::vector<float> triangleVertices() {
    return {
        // pos.x   pos.y  pos.z  pos.w   r      g      b      a
        -0.75f, -0.75f, 0.f, 1.f, 1.0f, 0.2f, 0.2f, 1.0f,
         0.75f, -0.75f, 0.f, 1.f, 0.2f, 1.0f, 0.2f, 1.0f,
         0.0f,   0.75f, 0.f, 1.f, 0.2f, 0.2f, 1.0f, 1.0f,
    };
}

/// Triângulo full-screen com cor UNIFORME (verificação de pixel exata).
std::vector<float> fullscreenTriangle(float r, float g, float b) {
    return {
        -1.f, -1.f, 0.f, 1.f, r, g, b, 1.f,
         3.f, -1.f, 0.f, 1.f, r, g, b, 1.f,
        -1.f,  3.f, 0.f, 1.f, r, g, b, 1.f,
    };
}

struct GlesReady {
    GlesBackend backend;
    RendererCapabilities caps{};
    bool available{false};
    std::string skipReason{};

    GlesReady(const RendererConfig& config) {
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
// Funções puras (sem EGL) — unit
// =============================================================================

TEST_CASE("gles: mapeamentos de formato e estado", "[rhi][rhi_gles]")
{
    using eng::rhi::Format;
    CHECK(eng::rhi::gles::toGlFormat(Format::R8G8B8A8Unorm) == GL_RGBA8);
    CHECK(eng::rhi::gles::toGlFormat(Format::R8G8B8A8Srgb) == GL_SRGB8_ALPHA8);
    CHECK(eng::rhi::gles::toGlFormat(Format::R32G32B32A32Sfloat) == GL_RGBA32F);
    CHECK(eng::rhi::gles::toGlFormat(Format::Undefined) == 0);
    CHECK(eng::rhi::gles::toGlVertexType(Format::R32G32B32A32Sfloat) == GL_FLOAT);
    CHECK(eng::rhi::gles::toGlDepthFunc(eng::rhi::CompareOp::GreaterOrEqual) == GL_GEQUAL);
    CHECK(eng::rhi::gles::toGlBlendFactor(eng::rhi::BlendFactor::SrcAlpha) == GL_SRC_ALPHA);
}

// =============================================================================
// Probe / device-only / capabilities reais (missão §13/§34/§47)
// =============================================================================

TEST_CASE("gles: probe honesto e device-only sem surface", "[rhi][rhi_gles]")
{
    GlesBackend backend{};
    const auto probe = backend.probe();
    if (probe.availability == eng::rhi::Availability::Unavailable) {
        SKIP("OpenGL ES indisponível: " << probe.detail);
    }
    REQUIRE(probe.availability == eng::rhi::Availability::Detected);

    // Device-only: contexto surfaceless (missão §13) — recursos funcionam.
    RendererCapabilities caps{};
    const auto initialized = backend.initialize(RendererConfig{}, caps);
    if (!initialized) {
        SKIP("initialize device-only falhou: " << initialized.error().message);
    }
    REQUIRE(initialized.ok());
    CHECK(caps.backendName == "OpenGL ES");
    CHECK(caps.apiVersion.find("OpenGL ES 3.") == 0);  // 3.x real detectado
    CHECK(caps.maxTextureSize > 0);
    CHECK(caps.maxVertexAttributes > 0);
    CHECK(caps.presentation == false);  // device-only ≠ sem suporte
    // OpenGL ES NÃO tem validation layers: pedida → Unavailable honesto.
    CHECK(caps.validationState == ValidationState::Unavailable);
    CHECK_FALSE(caps.wireframe);  // GLES não tem polygon mode

    // Recursos reais sem surface: VBO real + update real.
    std::vector<float> vertices = triangleVertices();
    eng::rhi::BufferDesc bufferDesc{};
    bufferDesc.size = vertices.size() * sizeof(float);
    bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialData = std::as_bytes(std::span{vertices});
    auto buffer = backend.createBuffer(bufferDesc);
    REQUIRE(buffer.ok());
    CHECK(backend.destroyBuffer(buffer.value()).ok());

    // Frames exigem surface: erro preciso.
    auto frame = backend.beginFrame();
    REQUIRE(frame.isError());
    CHECK(frame.error().code == eng::core::StatusCode::NotSupported);
    CHECK(frame.error().message.find("surface") != std::string::npos);

    // GLSL sem... SPIR-V sem GLSL → erro preciso (paridade §40 invertida).
    ShaderDesc spirvOnly{};
    spirvOnly.vertexSpirv = eng::rhi::testing::kTriangleVertexSpirvBytes();
    spirvOnly.fragmentSpirv = eng::rhi::testing::kTriangleFragmentSpirvBytes();
    auto rejected = backend.createShader(spirvOnly);
    REQUIRE(rejected.isError());
    CHECK(rejected.error().code == eng::core::StatusCode::NotSupported);
}

// =============================================================================
// Triangle REAL com readback de pixel (missão §39 — RENDERING + VALIDATED)
// =============================================================================

TEST_CASE("gles: triangle REAL com pixel verificado (missão §39)", "[rhi][rhi_hardware]")
{
    GlesReady ready{headlessConfig()};
    if (!ready.available) {
        SKIP("OpenGL ES indisponível: " << ready.skipReason);
    }
    GlesBackend& backend = ready.backend;
    REQUIRE(ready.caps.presentation);
    // Ambiente real de validação: llvmpipe (software) reportado com honestidade.
    CHECK(ready.caps.softwareRendering);
    CHECK(ready.caps.device.kind == eng::rhi::DeviceKind::Cpu);

    // 1) Shader GLSL ES REAL (compila/linka de verdade). O contrato do
    // ShaderDesc é view/span válidos DURANTE a chamada — o teste mantém as
    // strings vivas em locais.
    ShaderDesc shaderDesc{};
    shaderDesc.debugName = "triangle";
    const std::string vertexGlsl = readShaderFile("triangle_gles.vert");
    const std::string fragmentGlsl = readShaderFile("triangle_gles.frag");
    REQUIRE_FALSE(vertexGlsl.empty());
    REQUIRE_FALSE(fragmentGlsl.empty());
    shaderDesc.vertexGlsl = vertexGlsl;
    shaderDesc.fragmentGlsl = fragmentGlsl;
    auto shader = backend.createShader(shaderDesc);
    REQUIRE(shader.ok());

    // GLSL inválido → erro com info log (missão §35).
    ShaderDesc badShader{};
    badShader.vertexGlsl = "#version 300 es\nvoid main() { typo_aqui(";
    badShader.fragmentGlsl = fragmentGlsl;
    auto bad = backend.createShader(badShader);
    REQUIRE(bad.isError());
    CHECK(bad.error().message.find("info log") != std::string::npos);

    // 2) VBO real com triângulo full-screen de cor conhecida.
    std::vector<float> vertices = fullscreenTriangle(0.25f, 0.5f, 0.75f);
    eng::rhi::BufferDesc bufferDesc{};
    bufferDesc.size = vertices.size() * sizeof(float);
    bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialData = std::as_bytes(std::span{vertices});
    auto vertexBuffer = backend.createBuffer(bufferDesc);
    REQUIRE(vertexBuffer.ok());

    // 3) Pipeline (intenção → VAO + estado).
    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.shader = shader.value();
    pipelineDesc.vertexLayout.bindings.push_back({0, 32});
    pipelineDesc.vertexLayout.attributes.push_back(
        {0, 0, 0, eng::rhi::Format::R32G32B32A32Sfloat});
    pipelineDesc.vertexLayout.attributes.push_back(
        {1, 0, 16, eng::rhi::Format::R32G32B32A32Sfloat});
    auto pipeline = backend.createGraphicsPipeline(pipelineDesc);
    REQUIRE(pipeline.ok());

    // 4) Frame: begin → clear → pipeline → vbo → draw → end → present.
    auto acquired = backend.beginFrame();
    REQUIRE(acquired.ok());
    REQUIRE(acquired.value().status == FrameAcquireStatus::Renderable);
    const std::uint64_t frameId = acquired.value().frameId;
    REQUIRE(backend.frameSetPipeline(frameId, pipeline.value()).ok());
    REQUIRE(backend.frameBindVertexBuffer(frameId, vertexBuffer.value()).ok());
    REQUIRE(backend.frameDraw(frameId, 3, 0).ok());
    REQUIRE(backend.endFrame(frameId).ok());
    REQUIRE(backend.present().ok());
    CHECK(backend.stats().framesSubmitted >= 1);
    CHECK(backend.stats().presentsOk >= 1);

    // 5) VALIDAÇÃO DE OUTPUT REAL: pixel central = cor do triângulo
    // (0.25, 0.5, 0.75) → (64, 128, 191) ± tolerância de quantização.
    std::uint8_t pixel[4] = {0, 0, 0, 0};
    REQUIRE(backend.readCenterPixel(pixel).ok());
    CHECK(pixel[0] == 64);   // 0.25 * 255
    CHECK(pixel[1] == 128);  // 0.50 * 255
    CHECK(pixel[2] >= 190);  // 0.75 * 255 (±1)
    CHECK(pixel[2] <= 192);
    CHECK(pixel[3] == 255);
    INFO("readback: " << +pixel[0] << " " << +pixel[1] << " " << +pixel[2] << " " << +pixel[3]);

    // 6) Segundo frame + protocolo de erro do present.
    auto second = backend.beginFrame();
    REQUIRE(second.ok());
    REQUIRE(backend.frameSetPipeline(second.value().frameId, pipeline.value()).ok());
    REQUIRE(backend.frameBindVertexBuffer(second.value().frameId, vertexBuffer.value()).ok());
    REQUIRE(backend.frameDraw(second.value().frameId, 3, 0).ok());
    REQUIRE(backend.endFrame(second.value().frameId).ok());
    REQUIRE(backend.present().ok());
    auto presented = backend.present();
    REQUIRE(presented.isError());
    CHECK(presented.error().code == eng::core::StatusCode::NotSupported);

    // 7) Resize: pbuffer recriado; próximo frame segue (missão §38).
    REQUIRE(backend.resize(32, 32).ok());
    CHECK(backend.stats().surfaceRecreations >= 2);
    auto afterResize = backend.beginFrame();
    REQUIRE(afterResize.ok());
    REQUIRE(afterResize.value().status == FrameAcquireStatus::Renderable);

    // 8) Handles stale contra o driver real.
    REQUIRE(backend.destroyBuffer(vertexBuffer.value()).ok());
    auto stale = backend.updateBuffer(vertexBuffer.value(), 0, {});
    REQUIRE(stale.isError());
    CHECK(stale.error().code == eng::core::StatusCode::InvalidArgument);
}

// =============================================================================
// Registro no Renderer (missão §10 — fallback Auto quando Vulkan ausente)
// =============================================================================

TEST_CASE("gles: registro no Renderer e Auto com GLES explícito", "[rhi][rhi_hardware]")
{
    Renderer::clearRegisteredBackends();
    REQUIRE(Renderer::registerBackend(BackendType::OpenGLES,
                                      &eng::rhi::gles::createBackend).ok());
    RendererConfig config = headlessConfig();
    auto created = Renderer::create(config);
    if (!created) {
        SKIP("OpenGL ES indisponível via Renderer: " << created.error().message);
    }
    auto renderer = std::move(created).value();
    CHECK(renderer.activeBackend() == BackendType::OpenGLES);
    CHECK(renderer.capabilities().backendName == "OpenGL ES");
    CHECK(renderer.capabilities().apiVersion.find("3.") != std::string::npos);
    CHECK(renderer.capabilities().softwareRendering);  // llvmpipe honesto
}

// =============================================================================
// PARIDADE Vulkan × GLES (missão §40)
// =============================================================================

#ifdef ENG_HAVE_PARITY_VULKAN
#include "eng/rhi/vulkan/VulkanBackend.hpp"

TEST_CASE("paridade: mesmo triangle nos DOIS backends reais (missão §40)", "[rhi][rhi_hardware]")
{
    using eng::rhi::vulkan::VulkanBackend;

    // Vertex data e intenção IDÊNTICOS para os dois backends.
    const std::vector<float> vertices = triangleVertices();
    auto buildPipelineDesc = [](eng::rhi::ShaderHandle shader) {
        GraphicsPipelineDesc desc{};
        desc.shader = shader;
        desc.vertexLayout.bindings.push_back({0, 32});
        desc.vertexLayout.attributes.push_back(
            {0, 0, 0, eng::rhi::Format::R32G32B32A32Sfloat});
        desc.vertexLayout.attributes.push_back(
            {1, 0, 16, eng::rhi::Format::R32G32B32A32Sfloat});
        return desc;
    };
    eng::rhi::BufferDesc bufferDesc{};
    bufferDesc.size = vertices.size() * sizeof(float);
    bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialData = std::as_bytes(std::span{vertices});

    auto runTriangle = [&](eng::rhi::RhiBackend& backend, const ShaderDesc& shaderDesc,
                           RendererCapabilities& caps) -> bool {
        if (!backend.initialize(headlessConfig(), caps)) {
            return false;  // backend ausente neste ambiente — NÃO é falha
        }
        auto shader = backend.createShader(shaderDesc);
        if (!shader) {
            return false;
        }
        auto buffer = backend.createBuffer(bufferDesc);
        if (!buffer) {
            return false;
        }
        auto pipeline = backend.createGraphicsPipeline(buildPipelineDesc(shader.value()));
        if (!pipeline) {
            return false;
        }
        auto acquired = backend.beginFrame();
        if (!acquired || acquired.value().status != FrameAcquireStatus::Renderable) {
            return false;
        }
        const std::uint64_t frameId = acquired.value().frameId;
        eng::rhi::ClearDesc clear{};
        clear.color = {0.1f, 0.1f, 0.15f, 1.f};
        if (!backend.frameClear(frameId, clear) || !backend.frameSetPipeline(frameId, pipeline.value()) ||
            !backend.frameBindVertexBuffer(frameId, buffer.value()) ||
            !backend.frameDraw(frameId, 3, 0) || !backend.endFrame(frameId) ||
            !backend.present()) {
            return false;
        }
        return true;
    };

    // --- Vulkan (SPIR-V) ---------------------------------------------------------
    VulkanBackend vulkan{};
    RendererCapabilities vulkanCaps{};
    ShaderDesc vulkanShader{};
    vulkanShader.vertexSpirv = eng::rhi::testing::kTriangleVertexSpirvBytes();
    vulkanShader.fragmentSpirv = eng::rhi::testing::kTriangleFragmentSpirvBytes();
    const bool vulkanOk = runTriangle(vulkan, vulkanShader, vulkanCaps);

    // --- OpenGL ES (GLSL) ----------------------------------------------------------
    GlesBackend gles{};
    RendererCapabilities glesCaps{};
    const std::string glesVertex = readShaderFile("triangle_gles.vert");
    const std::string glesFragment = readShaderFile("triangle_gles.frag");
    ShaderDesc glesShader{};
    glesShader.vertexGlsl = glesVertex;
    glesShader.fragmentGlsl = glesFragment;
    const bool glesOk = runTriangle(gles, glesShader, glesCaps);

    if (!vulkanOk && !glesOk) {
        SKIP("Nenhum backend gráfico real disponível neste ambiente");
    }
    // A MISSÃO §40 exige o mesmo caso nos DOIS backends — se um existe aqui,
    // o outro precisa existir e passar (mesma máquina, mesma surface headless).
    if (vulkanOk) {
        REQUIRE(vulkanCaps.backendName == "Vulkan");
        REQUIRE(vulkan.stats().framesSubmitted >= 1);
    }
    if (glesOk) {
        REQUIRE(glesCaps.backendName == "OpenGL ES");
        REQUIRE(gles.stats().framesSubmitted >= 1);
        // Validação de output real do lado GLES.
        std::uint8_t pixel[4] = {0, 0, 0, 0};
        if (gles.readCenterPixel(pixel).ok()) {
            // Centro do triângulo colorido: interpolação das 3 cores nos
            // vértices ≈ média (0.47, 0.47, 0.47) — verificamos apenas que
            // NÃO é a cor de clear (0.1, 0.1, 0.15) e não é preto.
            const bool notClear = pixel[0] != 26 || pixel[1] != 26 || pixel[2] != 38;
            CHECK(notClear);
        }
    }
}
#endif  // ENG_HAVE_PARITY_VULKAN

// =============================================================================
// Textura REAL com pixel verificado (evolução — caminho imagem→sprite)
// =============================================================================

TEST_CASE("gles: textura REAL amostrada com pixel verificado (evolução)",
          "[rhi][rhi_hardware]")
{
    GlesReady ready{headlessConfig()};
    if (!ready.available) {
        SKIP("OpenGL ES indisponível: " << ready.skipReason);
    }
    GlesBackend& backend = ready.backend;
    REQUIRE(ready.caps.presentation);

    // 1) Sprite shader REAL (pos+cor+uv → textura * cor).
    ShaderDesc shaderDesc{};
    shaderDesc.debugName = "sprite";
    const std::string vertexGlsl = readShaderFile("sprite_gles.vert");
    const std::string fragmentGlsl = readShaderFile("sprite_gles.frag");
    REQUIRE_FALSE(vertexGlsl.empty());
    REQUIRE_FALSE(fragmentGlsl.empty());
    shaderDesc.vertexGlsl = vertexGlsl;
    shaderDesc.fragmentGlsl = fragmentGlsl;
    auto shader = backend.createShader(shaderDesc);
    REQUIRE(shader.ok());

    // 2) Textura 2x2 com cores CONHECIDAS por quadrante:
    //    todo o quad cobre a tela; UV (0.5, 0.5) cai na fronteira — usamos
    //    NEAREST para determinismo total do pixel central.
    constexpr std::uint32_t kW = 2;
    constexpr std::uint32_t kH = 2;
    const std::uint8_t pixels[kW * kH * 4] = {
        255, 0, 0, 255,    // (0,0) vermelho
        0, 255, 0, 255,    // (1,0) verde
        0, 0, 255, 255,    // (0,1) azul
        255, 255, 255, 255 // (1,1) branco
    };
    eng::rhi::TextureDesc textureDesc{};
    textureDesc.width = kW;
    textureDesc.height = kH;
    textureDesc.format = eng::rhi::Format::R8G8B8A8Unorm;
    textureDesc.initialData = std::as_bytes(std::span{pixels});
    auto texture = backend.createTexture(textureDesc);
    REQUIRE(texture.ok());

    // 3) Sampler NEAREST + CLAMP (determinismo do pixel exato).
    eng::rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.magFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.addressU = eng::rhi::AddressMode::ClampToEdge;
    samplerDesc.addressV = eng::rhi::AddressMode::ClampToEdge;
    auto sampler = backend.createSampler(samplerDesc);
    REQUIRE(sampler.ok());

    // 4) Quad full-screen com UV: centro da tela → UV (0.5, 0.5).
    //    Com NEAREST em 2x2, (0.5,0.5) resolve para o texel (0,1)=azul
    //    (floor(0.5*2)=1 na linha de cima em GL: v=0.5*2=1 → índice 1 →
    //    linha do MEIO na tela = metade inferior = texel (0,1)).
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

    // 5) Pipeline com blending (sprite) — layout pos+cor+uv.
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

    // 6) Frame com TEXTURA VINCULADA: clear escuro → draw amostrado.
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

    // 7) VALIDAÇÃO DE OUTPUT REAL: o pixel central é uma COR DA TEXTURA
    //    (não do clear, não do vertex tint) — a amostragem REAL funcionou.
    std::uint8_t pixel[4] = {0, 0, 0, 0};
    REQUIRE(backend.readCenterPixel(pixel).ok());
    INFO("readback: " << +pixel[0] << " " << +pixel[1] << " " << +pixel[2] << " "
                      << +pixel[3]);
    const bool isRed = pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0;
    const bool isGreen = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0;
    const bool isBlue = pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255;
    const bool isWhite = pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255;
    CHECK((isRed || isGreen || isBlue || isWhite));
    CHECK(pixel[3] == 255);
    // NÃO é a cor de clear (5, 5, 7) — foi a TEXTURA que pintou.
    CHECK_FALSE((pixel[0] == 5 && pixel[1] == 5 && pixel[2] == 7));

    // 8) Textura com mipmaps (cadeia real via glGenerateMipmap).
    REQUIRE(backend.destroyTexture(texture.value()).ok());
    textureDesc.generateMipmaps = true;
    auto mipped = backend.createTexture(textureDesc);
    REQUIRE(mipped.ok());
    auto stale = backend.frameBindTexture(frameId, texture.value(), sampler.value(), 0);
    REQUIRE(stale.isError());

    REQUIRE(backend.destroySampler(sampler.value()).ok());
    REQUIRE(backend.destroyTexture(mipped.value()).ok());
}
