#include "eng/editor/ViewportRenderer.hpp"

#include "eng/editor/Diagnostics.hpp"

/// ViewportRenderer — pipeline pos+cor, VBO dinâmico CPU→clip.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

#include "EditorShaders.hpp"
#include "eng/editor/AssetBrowser.hpp"
#include "eng/editor/OverlayMath.hpp"
#include "eng/editor/TextureCache.hpp"
#include "eng/log/Macros.hpp"
#include "eng/rhi/RhiBackend.hpp"

namespace eng::editor {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

ENG_LOG_CATEGORY("editor");

[[nodiscard]] Error rendererError(StatusCode code, std::string message)
{
    return Error{code, "ViewportRenderer: " + std::move(message)};
}

/// Default da grade quando o host não passa config (caminho
/// legado de testes) — o MESMO default do project.goni.json.
const eng::project::GridConfig kDefaultGridConfig{};

/// Cor de fundo do clear do viewport (usada pelo fade da grade e pelo
/// outline subtil dos handles — P4.6 L2/L4).
constexpr float kViewportBgR = 0.13f;
constexpr float kViewportBgG = 0.14f;
constexpr float kViewportBgB = 0.16f;

/// Um quad (2 triângulos) em PX DE TELA com cor uniforme e rotação.
void pushQuadPx(std::vector<ViewportRenderer::Vertex>& out,
                const OverlayMapper& mapper, float cxPx, float cyPx,
                float halfWPx, float halfHPx, float rotation, float r,
                float g, float b)
{
    // Cantos em ordem CCW: (-,-), (+,-), (+,+), (-,+) — rotação em px.
    PxCorner corners[4];
    quadCornersPx(cxPx, cyPx, halfWPx, halfHPx, rotation, corners);
    const ViewportRenderer::Vertex quad[6] = {
        {mapper.toClipX(corners[0].x), mapper.toClipY(corners[0].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[1].x), mapper.toClipY(corners[1].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[2].x), mapper.toClipY(corners[2].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[0].x), mapper.toClipY(corners[0].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[2].x), mapper.toClipY(corners[2].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[3].x), mapper.toClipY(corners[3].y), 0.f, 1.f, r, g, b, 1.f},
    };
    out.insert(out.end(), std::begin(quad), std::end(quad));
}

/// Quadrado CHAMFERADO (octógono — "handle arredondado" sobre
/// um pipeline de quads): fill + 4 cortes de canto a 45°. A face do corte
/// é EXATA: h = c/√2 com c = 0.4*half — zero bleed além da face (a face
/// do quadrado rotacionado É a corda P1P2 do octógono).
void pushChamferQuadPx(std::vector<ViewportRenderer::Vertex>& out,
                       const OverlayMapper& mapper, float cxPx, float cyPx,
                       float halfPx, float rotation, float r, float g,
                       float b)
{
    pushQuadPx(out, mapper, cxPx, cyPx, halfPx, halfPx, rotation, r, g, b);
    constexpr float kPiOver2 = 1.57079637f;
    const float cut = halfPx * 0.4f;
    const float h = cut * 0.70710678f;
    const float cosr = std::cos(rotation);
    const float sinr = std::sin(rotation);
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            const float lx = sx * halfPx;
            const float ly = sy * halfPx;
            pushQuadPx(out, mapper, cxPx + lx * cosr - ly * sinr,
                       cyPx + lx * sinr + ly * cosr, h, h,
                       rotation + kPiOver2, kViewportBgR, kViewportBgG,
                       kViewportBgB);
        }
    }
}

/// Paleta do editor (HSV→RGB simples p/ tints determinísticos).
[[nodiscard]] float hueToRgb(float p, float q, float t) noexcept
{
    if (t < 0.f) { t += 1.f; }
    if (t > 1.f) { t -= 1.f; }
    if (t < 1.f / 6.f) { return p + (q - p) * 6.f * t; }
    if (t < 0.5f) { return q; }
    if (t < 2.f / 3.f) { return p + (q - p) * (2.f / 3.f - t) * 6.f; }
    return p;
}

void hsvToRgb(std::uint32_t hue, float& r, float& g,
                            float& b) noexcept
{
    const float h = static_cast<float>(hue % 360u) / 360.f;
    const float s = 0.58f;
    const float v = 0.92f;
    if (s <= 0.f) {
        r = g = b = v;
        return;
    }
    const float q = v < 0.5f ? v * (1.f + s) : v + s - v * s;
    const float p = 2.f * v - q;
    r = hueToRgb(p, q, h + 1.f / 3.f);
    g = hueToRgb(p, q, h);
    b = hueToRgb(p, q, h - 1.f / 3.f);
}

// TODO quad/segmento do editor
// nasce em PX DE TELA (centro, meia-extensões, rotação, espessura) e só
// vira clip no ÚLTIMO passo, por eixo (OverlayMapper). A rotação acontece
// EM PX (isotrópica): quadrados são quadrados e círculos são círculos em
// qualquer aspect — o clip-space anisotrópico nunca mais vê uma rotação
// (era a causa do sprite deformado a 90° e do anel "elíptico").


/// Segmento ESPESSO em PX (RECOVERY §10 — contornos de collider; N3 —
/// espessura constante em px em QUALQUER direção): perpendicular
/// calculada em px (isotrópico), clip só no último passo.
void pushSegmentPx(std::vector<ViewportRenderer::Vertex>& out,
                   const OverlayMapper& mapper, float axPx, float ayPx,
                   float bxPx, float byPx, float halfThickPx, float r,
                   float g, float b)
{
    PxCorner corners[4];
    segmentCornersPx(axPx, ayPx, bxPx, byPx, halfThickPx, corners);
    // Degenerado (comprimento ~0): quad colapsado — inofensivo no pipeline.
    const ViewportRenderer::Vertex quad[6] = {
        {mapper.toClipX(corners[0].x), mapper.toClipY(corners[0].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[1].x), mapper.toClipY(corners[1].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[2].x), mapper.toClipY(corners[2].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[0].x), mapper.toClipY(corners[0].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[2].x), mapper.toClipY(corners[2].y), 0.f, 1.f, r, g, b, 1.f},
        {mapper.toClipX(corners[3].x), mapper.toClipY(corners[3].y), 0.f, 1.f, r, g, b, 1.f},
    };
    out.insert(out.end(), std::begin(quad), std::end(quad));
}

/// TRIÂNGULO preenchido em PX — setas REAIS do gizmo
/// (aponta para o +X local da rotação; halfLenPx = centro→ápice,
/// halfBasePx = meia-base). Halo de 1dp SOB o triângulo (mesma forma
/// escalada em bg) quando `rim` > 0 — contraste sobre qualquer sprite.
void pushTrianglePx(std::vector<ViewportRenderer::Vertex>& out,
                    const OverlayMapper& mapper, float cxPx, float cyPx,
                    float halfLenPx, float halfBasePx, float rotation,
                    float rim, float r, float g, float b)
{
    auto emit = [&](float len, float base, float cr, float cg, float cb) {
        const float cosR = std::cos(rotation);
        const float sinR = std::sin(rotation);
        // Apice (+len, 0) e base (−len·0.5, ±base).
        const float lx[3] = {len, -len * 0.5f, -len * 0.5f};
        const float ly[3] = {0.f, base, -base};
        float vx[3];
        float vy[3];
        for (int i = 0; i < 3; ++i) {
            vx[i] = cxPx + lx[i] * cosR - ly[i] * sinR;
            vy[i] = cyPx + lx[i] * sinR + ly[i] * cosR;
        }
        const ViewportRenderer::Vertex tri[3] = {
            {mapper.toClipX(vx[0]), mapper.toClipY(vy[0]), 0.f, 1.f, cr, cg, cb, 1.f},
            {mapper.toClipX(vx[1]), mapper.toClipY(vy[1]), 0.f, 1.f, cr, cg, cb, 1.f},
            {mapper.toClipX(vx[2]), mapper.toClipY(vy[2]), 0.f, 1.f, cr, cg, cb, 1.f},
        };
        out.insert(out.end(), std::begin(tri), std::end(tri));
    };
    if (rim > 0.f) {
        // Halo: mesma forma crescida sobre o CENTRO (mantém a ponta).
        emit(halfLenPx + rim, halfBasePx + rim, kViewportBgR, kViewportBgG,
             kViewportBgB);
    }
    emit(halfLenPx, halfBasePx, r, g, b);
}

/// Um quad de sprite LIT (2 triângulos, pos+cor+uv+MUNDO — P3 §5): o
/// fragment ilumina por DISTÂNCIA MUNDIAL (luzes do bloco PerFrame).
/// Cantos em PX (rotação em px); o mundo per-vertex é
/// emitido em unidades MUNDIAIS à parte (mesma orientação).
void pushLitSpriteQuadPx(std::vector<ViewportRenderer::LitSpriteVertex>& out,
                         const OverlayMapper& mapper, float cxPx, float cyPx,
                         float halfWPx, float halfHPx, float rotation,
                         float u0, float v0, float u1, float v1, float r,
                         float g, float b, float a, float worldX,
                         float worldY, float worldHalfW, float worldHalfH)
{
    const float cosR = std::cos(rotation);
    const float sinR = std::sin(rotation);
    const float lx[4] = {-halfWPx, halfWPx, halfWPx, -halfWPx};
    const float ly[4] = {-halfHPx, -halfHPx, halfHPx, halfHPx};
    const float uu[4] = {u0, u1, u1, u0};
    const float vv[4] = {v0, v0, v1, v1};
    float vx[4];
    float vy[4];
    for (int i = 0; i < 4; ++i) {
        // Offset rotacionado EM MUNDO projetado (y negado —
        // a mesma projeção de w2sX/w2sY por ponto). Cantos px corretos
        // (sem cisalhamento) e ordem canónica preservada (UV v0 = topo).
        vx[i] = cxPx + lx[i] * cosR - ly[i] * sinR;
        vy[i] = cyPx - (lx[i] * sinR + ly[i] * cosR);
    }
    // Mundo: mesmo quad em unidades MUNDIAIS (rotacionado, y para cima) —
    // normaliza os offsets px pela meia-extensão px correspondente.
    float wx[4];
    float wy[4];
    for (int i = 0; i < 4; ++i) {
        wx[i] = worldX + lx[i] / halfWPx * worldHalfW * cosR -
                ly[i] / halfHPx * worldHalfH * sinR;
        wy[i] = worldY + lx[i] / halfWPx * worldHalfW * sinR +
                ly[i] / halfHPx * worldHalfH * cosR;
    }
    const ViewportRenderer::LitSpriteVertex quad[6] = {
        {mapper.toClipX(vx[0]), mapper.toClipY(vy[0]), 0.f, 1.f, r, g, b, a, uu[0], vv[0], wx[0], wy[0]},
        {mapper.toClipX(vx[1]), mapper.toClipY(vy[1]), 0.f, 1.f, r, g, b, a, uu[1], vv[1], wx[1], wy[1]},
        {mapper.toClipX(vx[2]), mapper.toClipY(vy[2]), 0.f, 1.f, r, g, b, a, uu[2], vv[2], wx[2], wy[2]},
        {mapper.toClipX(vx[0]), mapper.toClipY(vy[0]), 0.f, 1.f, r, g, b, a, uu[0], vv[0], wx[0], wy[0]},
        {mapper.toClipX(vx[2]), mapper.toClipY(vy[2]), 0.f, 1.f, r, g, b, a, uu[2], vv[2], wx[2], wy[2]},
        {mapper.toClipX(vx[3]), mapper.toClipY(vy[3]), 0.f, 1.f, r, g, b, a, uu[3], vv[3], wx[3], wy[3]},
    };
    out.insert(out.end(), std::begin(quad), std::end(quad));
}

/// Um quad de sprite (2 triângulos, pos+cor+uv) em PX DE TELA.
/// Rotação em px — sprite rodado mantém proporção px.
void pushSpriteQuadPx(std::vector<ViewportRenderer::SpriteVertex>& out,
                      const OverlayMapper& mapper, float cxPx, float cyPx,
                      float halfWPx, float halfHPx, float rotation, float u0,
                      float v0, float u1, float v1, float r, float g, float b,
                      float a)
{
    const float cosR = std::cos(rotation);
    const float sinR = std::sin(rotation);
    // Cantos CCW com UV: (-,-)=uv00, (+,-)=uv10, (+,+)=uv11, (-,+)=uv01.
    const float lx[4] = {-halfWPx, halfWPx, halfWPx, -halfWPx};
    const float ly[4] = {-halfHPx, -halfHPx, halfHPx, halfHPx};
    const float uu[4] = {u0, u1, u1, u0};
    const float vv[4] = {v0, v0, v1, v1};
    float vx[4];
    float vy[4];
    for (int i = 0; i < 4; ++i) {
        // Projeção do offset rotacionado (y negado — mesmo
        // flip de w2sY); ordem canónica preservada (UV v0 = topo).
        vx[i] = cxPx + lx[i] * cosR - ly[i] * sinR;
        vy[i] = cyPx - (lx[i] * sinR + ly[i] * cosR);
    }
    const ViewportRenderer::SpriteVertex quad[6] = {
        {mapper.toClipX(vx[0]), mapper.toClipY(vy[0]), 0.f, 1.f, r, g, b, a, uu[0], vv[0]},
        {mapper.toClipX(vx[1]), mapper.toClipY(vy[1]), 0.f, 1.f, r, g, b, a, uu[1], vv[1]},
        {mapper.toClipX(vx[2]), mapper.toClipY(vy[2]), 0.f, 1.f, r, g, b, a, uu[2], vv[2]},
        {mapper.toClipX(vx[0]), mapper.toClipY(vy[0]), 0.f, 1.f, r, g, b, a, uu[0], vv[0]},
        {mapper.toClipX(vx[2]), mapper.toClipY(vy[2]), 0.f, 1.f, r, g, b, a, uu[2], vv[2]},
        {mapper.toClipX(vx[3]), mapper.toClipY(vy[3]), 0.f, 1.f, r, g, b, a, uu[3], vv[3]},
    };
    out.insert(out.end(), std::begin(quad), std::end(quad));
}

}  // namespace

// =============================================================================
// Ciclo de vida
// =============================================================================

ViewportRenderer::~ViewportRenderer()
{
    destroyResources();
}

ViewportRenderer::ViewportRenderer(ViewportRenderer&& other) noexcept
    : renderer_(std::move(other.renderer_)),
      vertexBuffer_(std::exchange(other.vertexBuffer_, {})),
      vertexCapacity_(std::exchange(other.vertexCapacity_, 0)),
      shaders_(std::move(other.shaders_)),
      spriteVertices_(std::move(other.spriteVertices_)),
      lastFrameTexturedSprites_(std::exchange(other.lastFrameTexturedSprites_, 0)),
      litSpriteBuffer_(std::exchange(other.litSpriteBuffer_, {})),
      litSpriteCapacity_(std::exchange(other.litSpriteCapacity_, 0)),
      litSpriteVertices_(std::move(other.litSpriteVertices_)),
      frameUniformsSent_(std::move(other.frameUniformsSent_)),
      frameVertices_(std::move(other.frameVertices_)),
      framesSubmitted_(std::exchange(other.framesSubmitted_, 0)),
      framesPresented_(std::exchange(other.framesPresented_, 0)),
      lastFrameVertexCount_(std::exchange(other.lastFrameVertexCount_, 0))
{
}

ViewportRenderer& ViewportRenderer::operator=(ViewportRenderer&& other) noexcept
{
    if (this != &other) {
        destroyResources();
        renderer_ = std::move(other.renderer_);
        vertexBuffer_ = std::exchange(other.vertexBuffer_, {});
        vertexCapacity_ = std::exchange(other.vertexCapacity_, 0);
        shaders_ = std::move(other.shaders_);
        spriteVertices_ = std::move(other.spriteVertices_);
        lastFrameTexturedSprites_ = std::exchange(other.lastFrameTexturedSprites_, 0);
        litSpriteBuffer_ = std::exchange(other.litSpriteBuffer_, {});
        litSpriteCapacity_ = std::exchange(other.litSpriteCapacity_, 0);
        litSpriteVertices_ = std::move(other.litSpriteVertices_);
        frameUniformsSent_ = std::move(other.frameUniformsSent_);
        frameVertices_ = std::move(other.frameVertices_);
        framesSubmitted_ = std::exchange(other.framesSubmitted_, 0);
        framesPresented_ = std::exchange(other.framesPresented_, 0);
        lastFrameVertexCount_ = std::exchange(other.lastFrameVertexCount_, 0);
    }
    return *this;
}

void ViewportRenderer::destroyResources() noexcept
{
    // Renderer vivo destrói os handles via backend.
    // ShaderLibrary PRIMEIRO (handles de shader/pipeline são dela — P3 §2).
    if (renderer_.has_value()) {
        if (litSpriteBuffer_.isValid()) {
            (void)renderer_->destroyBuffer(litSpriteBuffer_);
        }
        shaders_.destroy(*renderer_);
        if (vertexBuffer_.isValid()) {
            (void)renderer_->destroyBuffer(vertexBuffer_);
        }
    }
    vertexBuffer_ = {};
    vertexCapacity_ = 0;
    litSpriteBuffer_ = {};
    litSpriteCapacity_ = 0;
}

Result<ViewportRenderer> ViewportRenderer::create(
    const eng::rhi::SurfaceDesc& surface, eng::rhi::BackendType backend)
{
    // As FÁBRICAS são registradas pelo host (EditorHost::create) — aqui
    // apenas a seleção.
    eng::rhi::RendererConfig config;
    config.backend = backend;
    config.enableValidation = true;
    config.allowFallback = false;
    config.surface = surface;
    config.applicationName = "goni.editor";
    diag::mark("STARTUP_RHI", "begin", "creating renderer");
    auto renderer = eng::rhi::Renderer::create(config);
    if (renderer.isError()) {
        diag::mark("STARTUP_RHI", "failed", renderer.error().message.c_str());
        return makeUnexpected(renderer.error());
    }

    ViewportRenderer self;
    self.renderer_ = std::move(renderer.value());
    // Backend EFETIVO (Auto resolve aqui: Vulkan→GLES na ausência de GPU).
    const char* backendStage = self.renderer_->activeBackend() ==
                                       eng::rhi::BackendType::Vulkan
                                   ? "STARTUP_VULKAN"
                                   : "STARTUP_GLES";
    diag::mark(backendStage, "ok",
               self.renderer_->capabilities().backendName.c_str());

    // Shader Core (P3 §2): os shaders/pipelines do 2D vivem na
    // ShaderLibrary do eng::render (color/unlit/lit — MESMOS fixtures
    // canônicos de sempre + o par LIT novo com bloco PerFrame).
    auto shaders = eng::render::ShaderLibrary::create(*self.renderer_);
    if (shaders.isError()) {
        diag::mark("STARTUP_SHADER_CORE", "failed",
                   shaders.error().message.c_str());
        return makeUnexpected(shaders.error());
    }
    diag::mark("STARTUP_SHADER_CORE", "ok");
    self.shaders_ = std::move(shaders.value());
    return self;
}

Result<void> ViewportRenderer::resize(std::uint32_t width,
                                      std::uint32_t height)
{
    if (!renderer_.has_value()) {
        return makeUnexpected(
            rendererError(StatusCode::InvalidState, "renderer inexistente"));
    }
    auto resized = renderer_->resize(width, height);
    if (resized.isError()) {
        return makeUnexpected(resized.error());
    }
    return {};
}

// =============================================================================
// Frame
// =============================================================================

bool ViewportRenderer::ensureCapacity(std::size_t vertexCount)
{
    if (!renderer_.has_value()) {
        return false;
    }
    if (vertexBuffer_.isValid() && vertexCapacity_ >= vertexCount) {
        return true;
    }

    // Crescimento ×1.5 amortizado; buffer grande o bastante para o frame.
    std::size_t capacity = vertexCapacity_ == 0 ? 1024 : vertexCapacity_;
    while (capacity < vertexCount) {
        capacity = capacity + capacity / 2;
    }

    eng::rhi::BufferDesc desc;
    desc.size = capacity * sizeof(Vertex);
    desc.usage = eng::rhi::BufferUsage::Vertex | eng::rhi::BufferUsage::CopyDst;
    auto buffer = renderer_->createBuffer(desc);
    if (buffer.isError()) {
        ENG_WARN("viewport: falha ao crescer VBO ({})",
                 buffer.error().message);
        return false;
    }
    if (vertexBuffer_.isValid()) {
        (void)renderer_->destroyBuffer(vertexBuffer_);
    }
    vertexBuffer_ = buffer.value();
    vertexCapacity_ = capacity;
    return true;
}

/// VBO de sprites LIT (48B — P3 §5): mesmo crescimento amortizado.
bool ViewportRenderer::ensureLitSpriteCapacity(std::size_t vertexCount)
{
    if (!renderer_.has_value()) {
        return false;
    }
    if (litSpriteBuffer_.isValid() && litSpriteCapacity_ >= vertexCount) {
        return true;
    }
    std::size_t capacity = litSpriteCapacity_ == 0 ? 1024 : litSpriteCapacity_;
    while (capacity < vertexCount) {
        capacity = capacity + capacity / 2;
    }
    eng::rhi::BufferDesc desc;
    desc.size = capacity * sizeof(LitSpriteVertex);
    desc.usage = eng::rhi::BufferUsage::Vertex | eng::rhi::BufferUsage::CopyDst;
    auto buffer = renderer_->createBuffer(desc);
    if (buffer.isError()) {
        ENG_WARN("viewport: falha ao crescer VBO de sprites lit ({})",
                 buffer.error().message);
        return false;
    }
    if (litSpriteBuffer_.isValid()) {
        (void)renderer_->destroyBuffer(litSpriteBuffer_);
    }
    litSpriteBuffer_ = buffer.value();
    litSpriteCapacity_ = capacity;
    return true;
}

bool ViewportRenderer::ensureSpriteCapacity(std::size_t vertexCount)
{
    if (!renderer_.has_value()) {
        return false;
    }
    if (spriteBuffer_.isValid() && spriteCapacity_ >= vertexCount) {
        return true;
    }
    std::size_t capacity = spriteCapacity_ == 0 ? 1024 : spriteCapacity_;
    while (capacity < vertexCount) {
        capacity = capacity + capacity / 2;
    }
    eng::rhi::BufferDesc desc;
    desc.size = capacity * sizeof(SpriteVertex);
    desc.usage = eng::rhi::BufferUsage::Vertex | eng::rhi::BufferUsage::CopyDst;
    auto buffer = renderer_->createBuffer(desc);
    if (buffer.isError()) {
        ENG_WARN("viewport: falha ao crescer VBO de sprites ({})",
                 buffer.error().message);
        return false;
    }
    if (spriteBuffer_.isValid()) {
        (void)renderer_->destroyBuffer(spriteBuffer_);
    }
    spriteBuffer_ = buffer.value();
    spriteCapacity_ = capacity;
    return true;
}

bool ViewportRenderer::buildAndDraw(const Viewport& viewport,
                                    const std::vector<EntityQuad>& quads,
                                    const std::vector<ParticleQuad>& particles,
                                    bool playMode, const AssetBrowser* assets,
                                    TextureCache* textures,
                                    const GizmoDrawData* gizmo,
                                    const eng::project::GridConfig* grid)
{
    frameVertices_.clear();
    spriteVertices_.clear();
    litSpriteVertices_.clear();     // lote lit do frame
    frameUniformsSent_.clear();    // blocos PerFrame enviados
    gizmoVertices_.clear();  // acessores de teste não vazam frame velho
    lastFrameTexturedSprites_ = 0;
    lastFrameDrawCalls_ = 0;  // P4.7.0 B6: métrica do frame

    // --- sprites resolvidos ANTES (agrupamento por textura p/ batching) -------
    // Resolve cada sprite com textura via TextureCache; os que falham caem
    // no caminho de cor (honesto: sem textura, marcador hue + log único).
    struct ResolvedSprite {
        const EntityQuad* quad{};
        const TextureCache::GpuTexture* gpu{};
    };
    std::vector<ResolvedSprite> resolvedSprites{};
    resolvedSprites.reserve(quads.size());
    std::vector<const EntityQuad*> untexturedQuads{};
    untexturedQuads.reserve(quads.size());
    for (const EntityQuad& quad : quads) {
        if (!quad.textureAsset.empty() && assets != nullptr && textures != nullptr) {
            if (const auto* gpu = textures->acquire(*assets, *renderer_,
                                                    quad.textureAsset)) {
                resolvedSprites.push_back({&quad, gpu});
                continue;
            }
        }
        untexturedQuads.push_back(&quad);
    }
    // Ordenação por sort (estável — empates mantêm a hierarquia).
    std::stable_sort(resolvedSprites.begin(), resolvedSprites.end(),
                     [](const ResolvedSprite& a, const ResolvedSprite& b) {
                         return a.quad->sort < b.quad->sort;
                     });

    // --- DrawList (P3 §4): a render world do frame — luzes/ambiente na
    // estrutura GENÉRICA do eng::render (o bloco PerFrame dos grupos lit
    // sai daqui: packUniformsFor). Sprites entram na sequência abaixo.
    eng::render::DrawList drawList;
    drawList.ambientR = 1.f;  // default honesto: sem luzes = look clássico
    drawList.ambientG = 1.f;
    drawList.ambientB = 1.f;
    drawList.ambientIntensity = 1.f;
    for (const EntityQuad& quad : quads) {
        if (!quad.hasLight) {
            continue;
        }
        eng::render::DrawList::LightItem light;
        light.worldX = quad.worldX;
        light.worldY = quad.worldY;
        light.radius = quad.lightRadius;
        light.intensity = quad.lightIntensity;
        light.r = quad.lightColorR;
        light.g = quad.lightColorG;
        light.b = quad.lightColorB;
        light.falloff = quad.lightFalloff;
        light.layer = quad.lightLayer;
        drawList.lights.push_back(std::move(light));
    }

    // --- grade ------
    // Passo em UNIDADES DE MUNDO (config do projeto), linhas minor/major
    // ("Primary Line Every" a cada N — major mais clara e GROSSA), LOD
    // adaptativo ao zoom (anti-moiré: minors fazem fade e somem; subdivisão
    // emerge ao aproximar — majors viram minors do nível seguinte) e EIXOS
    // DA ORIGEM coloridos (X vermelho / Y verde — coerentes com o gizmo).
    // Conversão canônica ÚNICA — px→clip pelo OverlayMapper.
    const float w = viewport.screenWidth();
    const float h = viewport.screenHeight();
    const OverlayMapper mapper{w, h};
    auto w2sX = [&](float wx) { return viewport.worldToScreenX(wx); };
    auto w2sY = [&](float wy) { return viewport.worldToScreenY(wy); };
    // P4.7.0 B4: rotação da VISTA — conversão PAR (ponto completo). Com
    // rotation == 0 (toda cena pré-P4.7, câmera do editor) é passthrough.
    // O helper gira o OFFSET de tela ao redor do centro da surface: com
    // 90°, o +X do mundo aparece PARA CIMA na tela (contrato testado).
    const float viewRot = viewport.effectiveCamera().rotation;
    auto w2sPair = [&](float wx, float wy) -> std::pair<float, float> {
        const float cx = w2sX(wx);
        const float cy = w2sY(wy);
        if (viewRot == 0.f) {
            return {cx, cy};
        }
        const float cosR = std::cos(viewRot);
        const float sinR = std::sin(viewRot);
        const float ox = cx - w * 0.5f;
        const float oy = cy - h * 0.5f;
        return {w * 0.5f + ox * cosR + oy * sinR,
                h * 0.5f - ox * sinR + oy * cosR};
    };
    // P4.7.0 B4: offset de tela girado pela rotação da VISTA (mesma
    // matriz do w2sPair — para direções/offsets que nascem em px a partir
    // de vetores de mundo: seta do emissor, cantos de marcador).
    auto rotOffset = [&](float ox, float oy) -> std::pair<float, float> {
        if (viewRot == 0.f) {
            return {ox, oy};
        }
        const float cosR = std::cos(viewRot);
        const float sinR = std::sin(viewRot);
        return {ox * cosR + oy * sinR, -ox * sinR + oy * cosR};
    };
    const eng::project::GridConfig& gridConf =
        grid != nullptr ? *grid : kDefaultGridConfig;
    if (gridConf.visible) {
        const float zoom = viewport.effectiveCamera().zoom;  // P0-5
        const eng::project::GridLod lod =
            eng::project::computeGridLod(gridConf, zoom);
        // O fade das minors é um LERP para o FUNDO (pipeline sem
        // blending: cor = alpha visual).
        const float bgR = kViewportBgR, bgG = kViewportBgG, bgB = kViewportBgB;
        const float kGridHalfThickPx = 0.7f;  // minor: 1.4 px
        const float kMajorHalfThickPx = 1.0f; // major: 2 px (mais grossa)
        // Minors (com fade; alpha 0 → NEM desenham — anti-moiré duro).
        if (lod.minorAlpha > 0.f) {
            const float a = lod.minorAlpha;
            const float r = bgR + (gridConf.minorR - bgR) * a;
            const float g = bgG + (gridConf.minorG - bgG) * a;
            const float b = bgB + (gridConf.minorB - bgB) * a;
            const float x0 = std::floor(viewport.screenToWorldX(0.f) /
                                        lod.minorStep) *
                            lod.minorStep;
            const float x1 = viewport.screenToWorldX(w);
            const float y0 = std::floor(viewport.screenToWorldY(h) /
                                        lod.minorStep) *
                            lod.minorStep;
            const float y1 = viewport.screenToWorldY(0.f);
            for (float gx = x0; gx <= x1; gx += lod.minorStep) {
                pushQuadPx(frameVertices_, mapper, w2sX(gx), h * 0.5f,
                           kGridHalfThickPx, h * 0.5f, 0.f, r, g, b);
            }
            for (float gy = y0; gy <= y1; gy += lod.minorStep) {
                pushQuadPx(frameVertices_, mapper, w * 0.5f, w2sY(gy),
                           w * 0.5f, kGridHalfThickPx, 0.f, r, g, b);
            }
        }
        // Majors (sempre no nível do LOD — mais claras e grossas).
        {
            const float x0 = std::floor(viewport.screenToWorldX(0.f) /
                                        lod.majorStep) *
                            lod.majorStep;
            const float x1 = viewport.screenToWorldX(w);
            const float y0 = std::floor(viewport.screenToWorldY(h) /
                                        lod.majorStep) *
                            lod.majorStep;
            const float y1 = viewport.screenToWorldY(0.f);
            for (float gx = x0; gx <= x1; gx += lod.majorStep) {
                pushQuadPx(frameVertices_, mapper, w2sX(gx), h * 0.5f,
                           kMajorHalfThickPx, h * 0.5f, 0.f, gridConf.majorR,
                           gridConf.majorG, gridConf.majorB);
            }
            for (float gy = y0; gy <= y1; gy += lod.majorStep) {
                pushQuadPx(frameVertices_, mapper, w * 0.5f, w2sY(gy),
                           w * 0.5f, kMajorHalfThickPx, 0.f, gridConf.majorR,
                           gridConf.majorG, gridConf.majorB);
            }
        }
        // Eixos da ORIGEM coloridos (X vermelho / Y verde — mesma família
        // do gizmo de move: TransformGizmo::kXAxis*/kYAxis*).
        pushQuadPx(frameVertices_, mapper, w * 0.5f,
                   w2sY(0.f), w * 0.5f, 1.0f, 0.f,
                   eng::editor::TransformGizmo::kXAxisR,
                   eng::editor::TransformGizmo::kXAxisG,
                   eng::editor::TransformGizmo::kXAxisB);
        pushQuadPx(frameVertices_, mapper, w2sX(0.f), h * 0.5f, 1.0f,
                   h * 0.5f, 0.f, eng::editor::TransformGizmo::kYAxisR,
                   eng::editor::TransformGizmo::kYAxisG,
                   eng::editor::TransformGizmo::kYAxisB);
    }

    // --- entidades SEM textura + BORDAS de sprites (pipeline pos+cor) -------
    // Ordem: play-indicator → seleção (borda) → quad. Sem blending: desenho
    // por sobreposição (ordem estável — quads em depth-first).
    const float zoom = viewport.effectiveCamera().zoom;  // P0-5
    // Marcadores em px — bordas +4px (play) / +2px (seleção) POR
    // LADO, na rotação px da entidade (mesmo visual, agora honesto).
    auto pushEntityMarkers = [&](const EntityQuad& quad, float halfWPx,
                                 float halfHPx) {
        // P4.7.0 B4: par com rotação da vista + rotação do quad ajustada
        // (a vista girada gira o que se vê — bordas giram junto).
        const auto [mcx, mcy] = w2sPair(quad.worldX, quad.worldY);
        const float markerRot = quad.rotation + viewRot;
        if (playMode) {
            pushQuadPx(frameVertices_, mapper, mcx, mcy, halfWPx + 4.f,
                       halfHPx + 4.f, markerRot, 0.10f, 0.75f, 0.35f);
        }
        if (quad.selected) {
            pushQuadPx(frameVertices_, mapper, mcx, mcy, halfWPx + 2.f,
                       halfHPx + 2.f, markerRot, 0.95f, 0.96f, 0.98f);
        }
    };
    for (const EntityQuad* quadPtr : untexturedQuads) {
        const EntityQuad& quad = *quadPtr;
        // RECOVERY P0 + P4.3 (N4): tamanho em PX (escala N unidades = N*zoom
        // px), mínimo visível — quad inteiro em px, rotação em px.
        const float halfWPx = std::max(quad.sizeX * zoom,
                                       Viewport::kMinQuadPixels) * 0.5f;
        const float halfHPx = std::max(quad.sizeY * zoom,
                                       Viewport::kMinQuadPixels) * 0.5f;

        pushEntityMarkers(quad, halfWPx, halfHPx);

        // PLACEHOLDER de sprite: SpriteData SEM textura vira
        // xadrez magenta/escuro (convenção clássica "sem textura"),
        // CLARAMENTE identificado — não é sprite renderizado nem o hue
        // de entidade crua. Entidades sem SpriteData seguem hue.
        if (quad.isSprite) {
            constexpr int kChecker = 4;  // células por eixo
            constexpr float kChessR = 0.55f, kChessG = 0.22f, kChessB = 0.55f;
            constexpr float kDarkR = 0.13f, kDarkG = 0.13f, kDarkB = 0.13f;
            // Base escura PRIMEIRO (painter: por baixo), xadrez por cima.
            pushQuadPx(frameVertices_, mapper, w2sX(quad.worldX),
                       w2sY(quad.worldY), halfWPx, halfHPx, quad.rotation,
                       kDarkR, kDarkG, kDarkB);
            const float cellW = halfWPx * 2.f / kChecker;
            const float cellH = halfHPx * 2.f / kChecker;
            const float cosR = std::cos(quad.rotation);
            const float sinR = std::sin(quad.rotation);
            const auto [chkX, chkY] = w2sPair(quad.worldX, quad.worldY);
            const float centerPxX = chkX;
            const float centerPxY = chkY;
            for (int iy = 0; iy < kChecker; ++iy) {
                for (int ix = 0; ix < kChecker; ++ix) {
                    if (((ix + iy) & 1) == 0) {
                        continue;  // célula escura já é o fundo
                    }
                    const float lx =
                        -halfWPx + cellW * (static_cast<float>(ix) + 0.5f);
                    const float ly =
                        -halfHPx + cellH * (static_cast<float>(iy) + 0.5f);
                    // Mesma projeção do quad base (y negado — N3/N4).
                    pushQuadPx(frameVertices_, mapper,
                               centerPxX + lx * cosR - ly * sinR,
                               centerPxY - (lx * sinR + ly * cosR),
                               cellW * 0.5f, cellH * 0.5f, quad.rotation,
                               kChessR, kChessG, kChessB);
                }
            }
            continue;
        }

        float r = 0.f;
        float g = 0.f;
        float b = 0.f;
        hsvToRgb(quad.tint, r, g, b);

        const auto [ccx, ccy] = w2sPair(quad.worldX, quad.worldY);
        pushQuadPx(frameVertices_, mapper, ccx, ccy, halfWPx, halfHPx,
                   quad.rotation + viewRot, r, g, b);
    }

    // --- partículas (drift D6 da FASE 10 — auditoria final) ------------------
    // Quads pequenos branco-âmbar POR CIMA das entidades: marcadores de
    // gameplay do estado de Play (pools só existem em runtime/clone). Tamanho
    // mínimo de 2px para permanecerem visíveis em zoom baixo.
    for (const ParticleQuad& particle : particles) {
        // RECOVERY P0 + P4.3 (N4): total = size*zoom px (mín 2px), px space.
        const float halfPx = std::max(particle.size * zoom, 2.f) * 0.5f;
        const auto [pcx, pcy] = w2sPair(particle.worldX, particle.worldY);
        pushQuadPx(frameVertices_, mapper, pcx, pcy, halfPx, halfPx,
                   particle.rotation + viewRot, 1.f, 0.86f, 0.55f);
    }

    // --- COLLIDERS (RECOVERY §10): o autor VÊ o shape de colisão ----------
    // Contorno por cima da camada de cor (abaixo apenas de sprites com
    // blending — o +2px de inflação espreita ao redor do sprite, mesmo
    // padrão da borda de seleção). Teal = sólido; âmbar = trigger (sem
    // resolução — §7.2 da física). Box = retângulo na ROTAÇÃO do nó;
    // esfera = octógono (aproximação honesta num renderer de quads).
    // Geometria idêntica à que o PhysicsWorld usará no Play: o que o
    // autor vê é o que a física resolve.
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr int kSphereSides = 8;
        const float kSolidR = 0.16f, kSolidG = 0.90f, kSolidB = 0.85f;
        const float kTriggerR = 0.98f, kTriggerG = 0.78f, kTriggerB = 0.20f;
        // Inflação +1px e espessura 1.2px em PX — constantes em
        // qualquer aspect/direção (antes variavam com w/h e com a direção).
        const float kInflatePx = 1.f;
        const float kHalfThickPx = 0.6f;  // contorno de 1.2 px
        for (const EntityQuad& quad : quads) {
            if (!quad.hasCollider) {
                continue;
            }
            const float r =
                quad.colliderTrigger ? kTriggerR : kSolidR;
            const float g =
                quad.colliderTrigger ? kTriggerG : kSolidG;
            const float b =
                quad.colliderTrigger ? kTriggerB : kSolidB;
            const auto [ccX, ccY] = w2sPair(quad.worldX, quad.worldY);
            const float cx = ccX;
            const float cy = ccY;
            // RECOVERY P0: colliderHalfX é MEIA-extensão MUNDIAL — o canto
            // fica a halfX*zoom px do centro (px space: direto).
            const float hxPx =
                quad.colliderHalfX * zoom + kInflatePx;
            const float hyPx =
                quad.colliderHalfY * zoom + kInflatePx;
            if (quad.colliderIsSphere) {
                // Octógono (fase inicial na rotação do nó p/ consistência).
                float prevX = cx + hxPx * std::cos(quad.rotation);
                float prevY = cy + hyPx * std::sin(quad.rotation);
                for (int i = 1; i <= kSphereSides; ++i) {
                    const float angle =
                        quad.rotation +
                        (2.f * kPi * static_cast<float>(i)) /
                            static_cast<float>(kSphereSides);
                    const float nextX = cx + hxPx * std::cos(angle);
                    const float nextY = cy + hyPx * std::sin(angle);
                    pushSegmentPx(frameVertices_, mapper, prevX, prevY, nextX,
                                  nextY, kHalfThickPx, r, g, b);
                    prevX = nextX;
                    prevY = nextY;
                }
            } else {
                // Retângulo: 4 cantos girados pela rotação do nó (px).
                const float cosR = std::cos(quad.rotation);
                const float sinR = std::sin(quad.rotation);
                const float corners[4][2] = {
                    {-hxPx, -hyPx}, {hxPx, -hyPx}, {hxPx, hyPx}, {-hxPx, hyPx}};
                float vx[4];
                float vy[4];
                for (int i = 0; i < 4; ++i) {
                    vx[i] = cx + corners[i][0] * cosR -
                            corners[i][1] * sinR;
                    vy[i] = cy + corners[i][0] * sinR +
                            corners[i][1] * cosR;
                }
                for (int i = 0; i < 4; ++i) {
                    const int next = (i + 1) % 4;
                    pushSegmentPx(frameVertices_, mapper, vx[i], vy[i],
                                  vx[next], vy[next], kHalfThickPx, r, g, b);
                }
            }
        }
    }

    // --- CÂMERA de jogo (P2 §11): retângulo da VISTA -----------------------
    // A MESMA conta do Play (entidade + offset, tela/zoom): o autor vê
    // EXATAMENTE o que a câmera cobriria. Ativa = contorno azul-ciano;
    // inativa = tracejado escuro (o renderer de quads aproxima com
    // cantos mais curtos — leitura honesta sem shader dedicado).
    {
        const float kCamR = 0.42f, kCamG = 0.66f, kCamB = 0.98f;
        const float kOffR = 0.30f, kOffG = 0.32f, kOffB = 0.36f;
        const float kHalfThickPx = 0.6f;  // 1.2 px constantes
        for (const EntityQuad& quad : quads) {
            if (!quad.hasCamera) {
                continue;
            }
            const float r = quad.cameraActive ? kCamR : kOffR;
            const float g = quad.cameraActive ? kCamG : kOffG;
            const float b = quad.cameraActive ? kCamB : kOffB;
            const auto [camX, camY] = w2sPair(quad.cameraCenterX,
                                              quad.cameraCenterY);
            const float cx = camX;
            const float cy = camY;
            const float hxPx = quad.cameraHalfW * zoom;
            const float hyPx = quad.cameraHalfH * zoom;
            // P4.7.0 B4: a moldura da vista gira COM a rotação da câmera.
            const float rotC = std::cos(quad.cameraRotation);
            const float rotS = std::sin(quad.cameraRotation);
            const float corners[4][2] = {
                {-hxPx, -hyPx}, {hxPx, -hyPx}, {hxPx, hyPx}, {-hxPx, hyPx}};
            float vx[4];
            float vy[4];
            for (int i = 0; i < 4; ++i) {
                vx[i] = cx + corners[i][0] * rotC - corners[i][1] * rotS;
                vy[i] = cy + corners[i][0] * rotS + corners[i][1] * rotC;
            }
            for (int i = 0; i < 4; ++i) {
                const int next = (i + 1) % 4;
                pushSegmentPx(frameVertices_, mapper, vx[i], vy[i], vx[next],
                              vy[next], kHalfThickPx, r, g, b);
            }
            // P4.7.0 B4: LIMITES do mundo (moldura externa tracejada-ish
            // — retângulo escuro-dim; o retângulo VISÍVEL pós-zoom fica
            // dentro dele no Play).
            if (quad.cameraLimits) {
                constexpr float kLimitR = 0.45f, kLimitG = 0.50f,
                                kLimitB = 0.62f;
                // Cantos do retângulo de limites em MUNDO → tela (par —
                // mesmo caminho do conteúdo).
                const float limitWX[4] = {
                    quad.cameraLimitMinX, quad.cameraLimitMaxX,
                    quad.cameraLimitMaxX, quad.cameraLimitMinX};
                const float limitWY[4] = {
                    quad.cameraLimitMinY, quad.cameraLimitMinY,
                    quad.cameraLimitMaxY, quad.cameraLimitMaxY};
                float lx[4];
                float ly[4];
                for (int i = 0; i < 4; ++i) {
                    const auto [sx, sy] = w2sPair(limitWX[i], limitWY[i]);
                    lx[i] = sx;
                    ly[i] = sy;
                }
                for (int i = 0; i < 4; ++i) {
                    const int next = (i + 1) % 4;
                    pushSegmentPx(frameVertices_, mapper, lx[i], ly[i],
                                  lx[next], ly[next], kHalfThickPx, kLimitR,
                                  kLimitG, kLimitB);
                }
            }
        }
    }

    // --- LUZ 2D: o autor VÊ onde a
    // luz está e QUANTO alcança — anel de raio light*zoom px (círculo
    // PERFEITO em px pela OverlayMath — a família do N3) + dot central.
    // A iluminação REAL continua no fragment dos sprites lit (mesma fonte
    // de dados: EntityQuad.hasLight ← Light2D).
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr int kSides = 24;
        const float kLightR = 0.98f, kLightG = 0.87f, kLightB = 0.55f;
        const float kHalfThickPx = 0.6f;  // 1.2 px constantes
        for (const EntityQuad& quad : quads) {
            if (!quad.hasLight) {
                continue;
            }
            const auto [lX, lY] = w2sPair(quad.worldX, quad.worldY);
            const float cx = lX;
            const float cy = lY;
            // Alcance em px (mínimo 12px para continuar agarrável visível).
            const float rPx = std::max(quad.lightRadius * zoom, 12.f);
            for (int i = 0; i < kSides; ++i) {
                const float a0 = (2.f * kPi * static_cast<float>(i)) /
                                 static_cast<float>(kSides);
                const float a1 = (2.f * kPi * static_cast<float>(i + 1)) /
                                 static_cast<float>(kSides);
                pushSegmentPx(frameVertices_, mapper,
                              cx + rPx * std::cos(a0),
                              cy + rPx * std::sin(a0),
                              cx + rPx * std::cos(a1),
                              cy + rPx * std::sin(a1),
                              kHalfThickPx, kLightR, kLightG, kLightB);
            }
            // Dot central (3px — posição da luz).
            pushQuadPx(frameVertices_, mapper, cx, cy, 3.f, 3.f, 0.f,
                       kLightR, kLightG, kLightB);
        }
    }

    // --- EMISSOR de partículas (P2 §10): marcador editável ------------------
    // Quad roxo + SETA na direção de emissão: o authoring tem feedback
    // visual do que o ParticleTick fará no Play (direção do emitter no
    // frame do nó). Pools vivas continuam sendo quads no Play.
    if (!playMode) {  // em Play o que vale é a SIMULAÇÃO (partículas)
        const float kEmR = 0.72f, kEmG = 0.44f, kEmB = 0.98f;
        const float kHalfThickPx = 0.5f;  // 1 px constantes
        for (const EntityQuad* quadPtr : untexturedQuads) {
            const EntityQuad& quad = *quadPtr;
            if (!quad.hasEmitter) {
                continue;
            }
            const auto [emX, emY] = w2sPair(quad.worldX, quad.worldY);
            const float cx = emX;
            const float cy = emY;
            // Marcador inteiro em PX (meia-aresta = size*zoom px).
            const float halfPx = quad.emitterSize * zoom;
            // Quad-marcador na rotação do nó (composta com a vista — B4).
            pushQuadPx(frameVertices_, mapper, cx, cy, halfPx, halfPx,
                       quad.rotation + viewRot, kEmR * 0.35f, kEmG * 0.35f,
                       kEmB * 0.35f);
            // Seta: direção (mundo) do emissor escalada ~2x o marcador.
            // P4.7.0 B4: o OFFSET px da direção gira com a vista.
            const float dirX = quad.emitterDirX;
            const float dirY = quad.emitterDirY;
            const auto [bx, by] = rotOffset(dirX * halfPx, -dirY * halfPx);
            const auto [tx, ty] = rotOffset(dirX * halfPx * 2.6f,
                                            -dirY * halfPx * 2.6f);
            const float tipX = cx + tx;
            const float tipY = cy + ty;
            pushSegmentPx(frameVertices_, mapper, cx + bx, cy + by, tipX,
                          tipY, kHalfThickPx, kEmR, kEmG, kEmB);
            // Ponta da seta: duas pernas para trás±perpendicular (px —
            // "trás" em mundo = +dirY em px).
            const float leg = halfPx * 0.6f;
            const float perpX = -dirY, perpY = dirX;
            pushSegmentPx(frameVertices_, mapper, tipX, tipY,
                          tipX - dirX * leg - perpX * leg,
                          tipY + dirY * leg + perpY * leg,
                          kHalfThickPx, kEmR, kEmG, kEmB);
            pushSegmentPx(frameVertices_, mapper, tipX, tipY,
                          tipX - dirX * leg + perpX * leg,
                          tipY + dirY * leg - perpY * leg,
                          kHalfThickPx, kEmR, kEmG, kEmB);
        }
    }

    // --- SPRITES (P3 §3/§5): vértices por SHADER do material. Runs
    // consecutivos (mesmo shader+camada+textura) agrupam draws — a ORDEM
    // por sort é preservada (painter's algorithm entre pipelines).
    struct SpriteRun {
        bool lit{false};
        const TextureCache::GpuTexture* gpu{};
        std::string layer{};
        std::uint32_t firstVertex{0};
        std::uint32_t vertexCount{0};
    };
    std::vector<SpriteRun> runs;
    auto runOpen = [&](bool lit, const TextureCache::GpuTexture* gpu,
                      const std::string& layer) -> SpriteRun& {
        SpriteRun run;
        run.lit = lit;
        run.gpu = gpu;
        run.layer = layer;
        run.firstVertex = lit
                              ? static_cast<std::uint32_t>(litSpriteVertices_.size())
                              : static_cast<std::uint32_t>(spriteVertices_.size());
        runs.push_back(std::move(run));
        return runs.back();
    };

    for (const ResolvedSprite& sprite : resolvedSprites) {
        const EntityQuad& quad = *sprite.quad;
        const float regionPx =
            static_cast<float>(sprite.gpu->width) * (quad.u1 - quad.u0);
        const float regionPy =
            static_cast<float>(sprite.gpu->height) * (quad.v1 - quad.v0);
        const float worldW = std::max(quad.sizeX * regionPx / quad.spritePpu,
                                      Viewport::kMinQuadPixels / zoom);
        const float worldH = std::max(quad.sizeY * regionPy / quad.spritePpu,
                                      Viewport::kMinQuadPixels / zoom);
        // Borda de seleção/play no pipeline de cor (embaixo do sprite).
        // Em PX — half = world*zoom/2 px.
        pushEntityMarkers(quad, worldW * zoom * 0.5f, worldH * zoom * 0.5f);

        // Pivot: centro do quad desloca ((pivot - 0.5) * tamanho) nos eixos
        // do sprite (respeita rotação).
        const float pivotOffX = (quad.pivotX - 0.5f) * worldW;
        const float pivotOffY = (quad.pivotY - 0.5f) * worldH;
        const float cosR = std::cos(quad.rotation);
        const float sinR = std::sin(quad.rotation);
        const float centerWX = quad.worldX + pivotOffX * cosR - pivotOffY * sinR;
        const float centerWY = quad.worldY + pivotOffX * sinR + pivotOffY * cosR;
        const auto [tcx, tcy] = w2sPair(centerWX, centerWY);
        const float cx = tcx;
        const float cy = tcy;
        // P4.7.0 B4: a rotação da vista compõe com a do sprite.
        const float spriteRot = quad.rotation + viewRot;

        // UV com flip (região trocada por eixo flipado).
        const float u0 = quad.flipX ? quad.u1 : quad.u0;
        const float u1 = quad.flipX ? quad.u0 : quad.u1;
        // v: origem do UV no canto SUPERIOR da imagem (stb/GL) — flipY
        // inverte a região verticalmente.
        const float v0 = quad.flipY ? quad.v1 : quad.v0;
        const float v1 = quad.flipY ? quad.v0 : quad.v1;

        // RECOVERY P0 + P4.3 (N4): meia-extensão em PX = world*zoom/2 —
        // o total em px é world*zoom; rotação aplicada EM PX.
        const bool lit = quad.materialShader == "lit";

        // Run: abre ANTES do push — firstVertex é o BASE do sprite neste
        // run (abrir depois capturava size() pós-push e desenhava a
        // região ERRADA do VBO: draw(count, base+6) lia vértices que não
        // são do sprite — bug do refactor P3, achado no CI pelo readback).
        const bool runMatches =
            !runs.empty() && runs.back().lit == lit &&
            runs.back().gpu == sprite.gpu && runs.back().layer == quad.layer;
        if (!runMatches) {
            runOpen(lit, sprite.gpu, quad.layer);
        }

        if (lit) {
            // LIT (P3 §5): + posição MUNDO interpolada (attribute 3) para
            // a distância às luzes no fragment.
            pushLitSpriteQuadPx(litSpriteVertices_, mapper, cx, cy,
                                worldW * zoom * 0.5f, worldH * zoom * 0.5f,
                                spriteRot, u0, v0, u1, v1, quad.tintR,
                                quad.tintG, quad.tintB, quad.tintA,
                                quad.worldX, quad.worldY, worldW, worldH);
        } else {
            pushSpriteQuadPx(spriteVertices_, mapper, cx, cy,
                             worldW * zoom * 0.5f, worldH * zoom * 0.5f,
                             spriteRot, u0, v0, u1, v1, quad.tintR,
                             quad.tintG, quad.tintB, quad.tintA);
        }
        ++lastFrameTexturedSprites_;

        // Item na DrawList (§4 — a render world genérica; mesmos dados
        // que os vértices, para consumidores futuros/runtime).
        eng::render::SpriteDrawItem item;
        item.worldX = quad.worldX;
        item.worldY = quad.worldY;
        item.rotation = quad.rotation;
        item.scaleX = quad.sizeX;
        item.scaleY = quad.sizeY;
        item.u0 = quad.u0;
        item.v0 = quad.v0;
        item.u1 = quad.u1;
        item.v1 = quad.v1;
        item.tintR = quad.tintR;
        item.tintG = quad.tintG;
        item.tintB = quad.tintB;
        item.tintA = quad.tintA;
        item.sort = quad.sort;
        item.pivotX = quad.pivotX;
        item.pivotY = quad.pivotY;
        item.flipX = quad.flipX;
        item.flipY = quad.flipY;
        item.spritePpu = quad.spritePpu;
        item.texture = quad.textureAsset;
        item.shader = quad.materialShader;
        item.layer = quad.layer;
        drawList.sprites.push_back(std::move(item));
        runs.back().vertexCount += kVerticesPerQuad;
    }

    if (!ensureCapacity(frameVertices_.size())) {
        return false;
    }
    if (!spriteVertices_.empty() && !ensureSpriteCapacity(spriteVertices_.size())) {
        return false;
    }
    if (!litSpriteVertices_.empty() &&
        !ensureLitSpriteCapacity(litSpriteVertices_.size())) {
        return false;
    }

    auto acquired = renderer_->beginFrame();
    if (acquired.isError()) {
        return false; // surface perdida — próximo frame re-tenta
    }
    if (acquired.value().status != eng::rhi::FrameAcquireStatus::Renderable) {
        return false;
    }
    eng::rhi::Frame& frame = acquired.value().frame;

    eng::rhi::ClearDesc clear;
    clear.color = {0.13f, 0.14f, 0.16f, 1.f};
    if (playMode) {
        clear.color = {0.10f, 0.13f, 0.11f, 1.f}; // tom levemente esverdeado
    }
    bool frameOk = true;
    auto cleared = frame.clear(clear);
    frameOk = frameOk && cleared.ok();
    auto viewportSet = frame.setViewport({0.f, 0.f, w, h, 0.f, 1.f});
    frameOk = frameOk && viewportSet.ok();

    // Lote 1: quads de cor (grade/entidades/bordas/partículas).
    if (frameOk && !frameVertices_.empty()) {
        auto pipelined = frame.setPipeline(shaders_.colorPipeline());
        auto bound = frame.bindVertexBuffer(vertexBuffer_);
        auto uploaded = renderer_->updateBuffer(
            vertexBuffer_, 0,
            {reinterpret_cast<const std::byte*>(frameVertices_.data()),
             frameVertices_.size() * sizeof(Vertex)});
        lastFrameDrawCalls_ = lastFrameDrawCalls_ + 1;
        auto drawn = frame.draw(
            static_cast<std::uint32_t>(frameVertices_.size()), 0);
        frameOk = pipelined.ok() && bound.ok() && uploaded.ok() && drawn.ok();
    }

    // Lote 2 (P3 §3/§5): sprites POR RUN — ordem preservada (painter's),
    // pipeline por SHADER do material, luzes por CAMADA (bloco PerFrame
    // re-sobe quando a camada do run muda), textura bind 1× por run.
    // Batching real: bind/draw por GRUPO, nunca por sprite.
    if (frameOk && !runs.empty()) {
        bool anyLit = false;
        for (const SpriteRun& run : runs) {
            anyLit = anyLit || run.lit;
        }
        // Uploads dos VBOs usados (uma vez por frame e por buffer).
        if (anyLit) {
            auto uploaded = renderer_->updateBuffer(
                litSpriteBuffer_, 0,
                {reinterpret_cast<const std::byte*>(litSpriteVertices_.data()),
                 litSpriteVertices_.size() * sizeof(LitSpriteVertex)});
            frameOk = frameOk && uploaded.ok();
        }
        if (frameOk && !spriteVertices_.empty()) {
            auto uploaded = renderer_->updateBuffer(
                spriteBuffer_, 0,
                {reinterpret_cast<const std::byte*>(spriteVertices_.data()),
                 spriteVertices_.size() * sizeof(SpriteVertex)});
            frameOk = frameOk && uploaded.ok();
        }

        // Uniforms por CAMADA (P3 §5 — mask real): o bloco da camada é
        // empacotado UMA vez e re-bindado quando a camada do run muda.
        std::string boundLayer{};
        bool layerBound = false;

        bool pipelineIsLit = false;
        bool pipelineSet = false;
        bool bufferIsLit = true;
        bool bufferSet = false;
        const TextureCache::GpuTexture* boundGpu = nullptr;
        for (const SpriteRun& run : runs) {
            if (!frameOk) {
                break;
            }
            // Pipeline (shader do material) — troca só quando muda.
            if (!pipelineSet || pipelineIsLit != run.lit) {
                auto pipelined = frame.setPipeline(
                    run.lit ? shaders_.spriteLitPipeline()
                            : shaders_.spriteUnlitPipeline());
                frameOk = frameOk && pipelined.ok();
                pipelineIsLit = run.lit;
                pipelineSet = true;
                layerBound = false;  // pipeline novo: uniforms re-bindam
            }
            // VBO do shader — troca só quando muda.
            if (!bufferSet || bufferIsLit != run.lit) {
                auto bound = frame.bindVertexBuffer(
                    run.lit ? litSpriteBuffer_ : spriteBuffer_);
                frameOk = frameOk && bound.ok();
                bufferIsLit = run.lit;
                bufferSet = true;
            }
            // Luzes da CAMADA (só lit — unlit não consome o bloco).
            if (run.lit && (!layerBound || boundLayer != run.layer)) {
                const auto block = drawList.packUniformsFor(run.layer);
                auto uniformed = shaders_.bindFrameUniforms(frame, block);
                frameOk = frameOk && uniformed.ok();
                boundLayer = run.layer;
                layerBound = true;
                frameUniformsSent_.push_back(block);  // prova de conteúdo
            }
            // Textura do run.
            if (run.gpu != nullptr && run.gpu != boundGpu) {
                auto boundTexture =
                    frame.bindTexture(run.gpu->texture, run.gpu->sampler, 0);
                frameOk = frameOk && boundTexture.ok();
                boundGpu = run.gpu;
            }
            auto drawn = frame.draw(run.vertexCount, run.firstVertex);
            lastFrameDrawCalls_ = lastFrameDrawCalls_ + 1;
            frameOk = frameOk && drawn.ok();
        }
    }

    // Lote 3: GIZMO — quads preenchidos + segmentos + TRIÂNGULOS
    // (setas reais, P4.7.0 B2) NO PIPELINE DE COR, POR CIMA de tudo (a
    // entidade selecionada precisa dos handles visíveis sobre a própria
    // arte). Ordem dentro do lote: quads → segmentos → triângulos — as
    // pontas de seta nascem POR CIMA das hastes (seta limpa). Halo de
    // 1dp (rim) sob CADA forma — contraste garantido sobre sprites claros.
    if (frameOk && gizmo != nullptr &&
        (!gizmo->quads.empty() || !gizmo->segments.empty()
         || !gizmo->triangles.empty())) {
        gizmoVertices_.clear();
        // OUTLINE subtil (rim escuro da cor do fundo) sob cada
        // handle — contraste garantido sobre sprites claros — e handles
        // CHAMFERADOS (octógono, "arredondados" no pipeline de quads).
        const float rim = TransformGizmo::haloPx(viewport.uiScale());
        for (const GizmoQuad& quad : gizmo->quads) {
            // Half (mundo) × zoom = px; rotação em px —
            // handles quadrados em QUALQUER aspect (eram paralelogramos).
            const float cxPx = w2sX(quad.worldX);
            const float cyPx = w2sY(quad.worldY);
            const float halfPx = quad.halfW * zoom;
            const float halfHPx = quad.halfH * zoom;
            pushChamferQuadPx(gizmoVertices_, mapper, cxPx, cyPx,
                              halfPx + rim, quad.rotation, kViewportBgR,
                              kViewportBgG, kViewportBgB);
            pushChamferQuadPx(gizmoVertices_, mapper, cxPx, cyPx, halfPx,
                              quad.rotation, quad.r, quad.g, quad.b);
            (void)halfHPx;  // handles são quadrados (halfW == halfH)
        }
        for (const GizmoSegment& segment : gizmo->segments) {
            // P4.3 (N3) + P4.6 (L4): espessura 2dp CONSISTENTE (densidade —
            // 2px na densidade 1, 4px na densidade 2): o anel é círculo de
            // linha uniforme em qualquer surface.
            const float halfThick =
                std::max(1.f, viewport.uiScale());
            // P4.7.0 B2: HALO de 1dp SOB o segmento (mesma linha mais
            // grossa em bg) — o anel/haste não se perde sobre a arte.
            pushSegmentPx(gizmoVertices_, mapper, w2sX(segment.x0),
                          w2sY(segment.y0), w2sX(segment.x1),
                          w2sY(segment.y1), halfThick + rim,
                          kViewportBgR, kViewportBgG, kViewportBgB);
            pushSegmentPx(gizmoVertices_, mapper, w2sX(segment.x0),
                          w2sY(segment.y0), w2sX(segment.x1),
                          w2sY(segment.y1), halfThick, segment.r,
                          segment.g, segment.b);
        }
        for (const GizmoTriangle& triangle : gizmo->triangles) {
            // P4.7.0 B2: SETAS REAIS — triângulos com halo, rotação em
            // px (isotrópica — regra única do overlay).
            pushTrianglePx(gizmoVertices_, mapper,
                           w2sX(triangle.worldX), w2sY(triangle.worldY),
                           triangle.halfW * zoom, triangle.halfH * zoom,
                           triangle.rotation, rim, triangle.r, triangle.g,
                           triangle.b);
        }
        if (!gizmoVertices_.empty() && ensureCapacity(gizmoVertices_.size())) {
            auto pipelined = frame.setPipeline(shaders_.colorPipeline());
            auto bound = frame.bindVertexBuffer(vertexBuffer_);
            auto uploaded = renderer_->updateBuffer(
                vertexBuffer_, 0,
                {reinterpret_cast<const std::byte*>(gizmoVertices_.data()),
                 gizmoVertices_.size() * sizeof(Vertex)});
            auto drawn = frame.draw(
                static_cast<std::uint32_t>(gizmoVertices_.size()), 0);
            lastFrameDrawCalls_ = lastFrameDrawCalls_ + 1;
            frameOk = pipelined.ok() && bound.ok() && uploaded.ok() &&
                      drawn.ok();
        } else if (!gizmoVertices_.empty()) {
            frameOk = false;  // VBO não cresceu — frame aborta (honesto)
        }
    }

    auto ended = frame.end();
    frameOk = frameOk && ended.ok();
    if (!frameOk) {
        ENG_WARN("viewport: comando rejeitado (frame abortado)");
        return false;
    }
    auto presented = renderer_->present();
    ++framesSubmitted_;
    lastFrameVertexCount_ = frameVertices_.size();
    if (presented.isError()) {
        ENG_WARN("viewport: present falhou ({})", presented.error().message);
        return true; // submetido mesmo assim (contadores honestos)
    }
    ++framesPresented_;
    return true;
}

bool ViewportRenderer::renderFrame(const Viewport& viewport,
                                   const std::vector<EntityQuad>& quads,
                                   const std::vector<ParticleQuad>& particles,
                                   bool playMode)
{
    // VBO nasce sob demanda no buildAndDraw (ensureCapacity) — validar
    // ANTES seria rejeitar o primeiro frame.
    if (!renderer_.has_value() || !shaders_.valid()) {
        return false;
    }
    return buildAndDraw(viewport, quads, particles, playMode, nullptr,
                        nullptr, nullptr, nullptr);
}

bool ViewportRenderer::renderFrame(const Viewport& viewport,
                                   const std::vector<EntityQuad>& quads,
                                   const std::vector<ParticleQuad>& particles,
                                   bool playMode, const AssetBrowser* assets,
                                   TextureCache& textures,
                                   const GizmoDrawData* gizmo,
                                   const eng::project::GridConfig* grid)
{
    if (!renderer_.has_value() || !shaders_.valid()) {
        return false;
    }
    return buildAndDraw(viewport, quads, particles, playMode, assets,
                        &textures, gizmo, grid);
}

eng::rhi::BackendType ViewportRenderer::activeBackend() const noexcept
{
    return renderer_.has_value() ? renderer_->activeBackend()
                                : eng::rhi::BackendType::Auto;
}

const eng::rhi::RendererCapabilities* ViewportRenderer::capabilities() const
    noexcept
{
    return renderer_.has_value() ? &renderer_->capabilities() : nullptr;
}

}  // namespace eng::editor
