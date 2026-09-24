#pragma once

/// eng::editor::ViewportRenderer — desenho do viewport via eng::rhi
///.
///
/// DECISÃO (revisada no P3 §2 — o gatilho da ADR-042 aconteceu): o RHI
/// ganhou o caminho de uniforms do frame (Frame::setUniformData — UBO
/// dinâmico por frame-slot). O viewport MANTÉM a transformação CPU
/// world→clip (correta p/ centenas de quads) e usa o caminho de uniforms
/// para o que a CPU não faz: ILUMINAÇÃO por fragmento (bloco PerFrame —
/// sprite.lit). Shaders/pipelines agora vivem na ShaderLibrary do
/// eng::render (Shader Core); a draw data 2D genérica é a DrawList.
///
/// Conteúdo por frame: fundo cinza-escuro, grade (1 unidade; eixo mais
/// claro), um quad por entidade (tint determinístico), borda branca na
/// seleção, borda verde quando em Play.

#include <cstdint>
#include <optional>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/editor/Gizmo.hpp"
#include "eng/project/GridConfig.hpp"
#include "eng/render/RenderTypes.hpp"
#include "eng/render/ShaderLibrary.hpp"
#include "eng/editor/Viewport.hpp"
#include "eng/rhi/Renderer.hpp"
#include "eng/rhi/Types.hpp"

namespace eng::editor {

class AssetBrowser;    // AssetBrowser.hpp
class TextureCache;    // TextureCache.hpp

class ViewportRenderer final {
public:
    ViewportRenderer() = default;
    ~ViewportRenderer();
    ViewportRenderer(ViewportRenderer&&) noexcept;
    ViewportRenderer& operator=(ViewportRenderer&&) noexcept;
    ViewportRenderer(const ViewportRenderer&) = delete;
    ViewportRenderer& operator=(const ViewportRenderer&) = delete;

    /// Cria o Renderer RHI (registro de fábricas é do HOST — EditorHost).
    /// Falha → erro preciso.
    [[nodiscard]] static eng::core::Result<ViewportRenderer> create(
        const eng::rhi::SurfaceDesc& surface,
        eng::rhi::BackendType backend);

    /// Redimensiona (surface mudou).
    eng::core::Result<void> resize(std::uint32_t width, std::uint32_t height);

    /// Um frame do viewport: quads + grade + seleção + partículas + SPRITES
    /// (texturas reais via TextureCache — evolução P0-3) + GIZMO (P1: lote
    /// de cor POR CIMA dos sprites). `assets` nulo (sem projeto) = sprites
    /// caem no caminho de cor. `gizmo` nulo/vazio = sem gizmo. `grid` nulo
    /// = default. false = não desenhou (minimizado/out-of-date
    /// persistente) — NUNCA lança.
    bool renderFrame(const Viewport& viewport,
                     const std::vector<EntityQuad>& quads,
                     const std::vector<ParticleQuad>& particles, bool playMode,
                     const AssetBrowser* assets, TextureCache& textures,
                     const GizmoDrawData* gizmo = nullptr,
                     const eng::project::GridConfig* grid = nullptr);

    /// Caminho legado (testes/hosts sem sprites) — sem texturas.
    bool renderFrame(const Viewport& viewport,
                     const std::vector<EntityQuad>& quads,
                     const std::vector<ParticleQuad>& particles, bool playMode);

    [[nodiscard]] bool isValid() const noexcept { return renderer_.has_value(); }
    /// Cor de fundo do jogo (configuração do projeto).
    void setPlayBackground(float r, float g, float b) noexcept
    {
        playBgR_ = r;
        playBgG_ = g;
        playBgB_ = b;
    }
    [[nodiscard]] eng::rhi::BackendType activeBackend() const noexcept;
    [[nodiscard]] const eng::rhi::RendererCapabilities* capabilities() const
        noexcept;
    /// Acesso ao renderer interno (host destrói texturas do cache ANTES
    /// do renderer morrer — ADR-035; nullptr quando inválido/moved-from).
    [[nodiscard]] eng::rhi::Renderer* renderer() noexcept
    {
        return renderer_.has_value() ? &renderer_.value() : nullptr;
    }

    [[nodiscard]] std::uint64_t framesSubmitted() const noexcept
    {
        return framesSubmitted_;
    }
    [[nodiscard]] std::uint64_t framesPresented() const noexcept
    {
        return framesPresented_;
    }
    /// Vertex completo (pos vec4 + cor vec4) — formato das FASES 5–7.
    struct Vertex {
        float x, y, z, w;        ///< clip space (z=0, w=1)
        float r, g, b, a;
    };
    /// Vertex de sprite (pos vec4 + cor vec4 + uv vec2 — 40 bytes).
    struct SpriteVertex {
        float x, y, z, w;
        float r, g, b, a;
        float u, v;
    };
    /// Vertex de sprite LIT (P3 §5): + posição MUNDO vec2 (48 bytes) — o
    /// fragment de iluminação precisa da posição mundial interpolada.
    struct LitSpriteVertex {
        float x, y, z, w;
        float r, g, b, a;
        float u, v;
        float worldX, worldY;
    };
    static constexpr std::uint32_t kVerticesPerQuad = 6;

    /// Vértices do último frame (prova de conteúdo nos testes sem GPU).
    [[nodiscard]] std::size_t lastFrameVertexCount() const noexcept
    {
        return lastFrameVertexCount_;
    }
    /// Dados CPU construídos e ENVIADOS no último frame (clip space + cor) —
    /// exatamente o que a GPU recebeu. Leitura para testes/diagnóstico.
    [[nodiscard]] const std::vector<Vertex>& lastFrameVertices() const noexcept
    {
        return frameVertices_;
    }
    /// Vértices de SPRITE do último frame (clip + cor + UV).
    [[nodiscard]] const std::vector<SpriteVertex>& lastFrameSpriteVertices()
        const noexcept
    {
        return spriteVertices_;
    }
    /// Vértices de sprite LIT do último frame (clip + cor + UV + mundo).
    [[nodiscard]] const std::vector<LitSpriteVertex>& lastFrameLitSpriteVertices()
        const noexcept
    {
        return litSpriteVertices_;
    }
    /// Bloco PerFrame do último frame por GRUPO de camada (P3 §5 —
    /// prova de conteúdo nos testes sem GPU: luzes que chegaram ao shader).
    [[nodiscard]] const std::vector<eng::render::FrameUniforms>&
    lastFrameFrameUniforms() const noexcept
    {
        return frameUniformsSent_;
    }
    /// Vértices do LOTE DE GIZMO do último frame (P1 — prova de conteúdo
    /// nos testes: handles por cima de sprites).
    [[nodiscard]] const std::vector<Vertex>& lastFrameGizmoVertices()
        const noexcept
    {
        return gizmoVertices_;
    }
    /// Quants sprites foram desenhados com TEXTURA REAL no último frame.
    [[nodiscard]] std::size_t lastFrameTexturedSprites() const noexcept
    {
        return lastFrameTexturedSprites_;
    }
    /// Quantos frame.draw() o último frame emitiu
    /// (métrica do overlay — o batching por (shader+textura+layer) já
    /// existe desde a P3; isto MEDe o efeito dele).
    [[nodiscard]] std::size_t lastFrameDrawCalls() const noexcept
    {
        return lastFrameDrawCalls_;
    }

private:
    void destroyResources() noexcept;

    [[nodiscard]] bool ensureCapacity(std::size_t vertexCount);
    [[nodiscard]] bool ensureSpriteCapacity(std::size_t vertexCount);
    [[nodiscard]] bool ensureLitSpriteCapacity(std::size_t vertexCount);
    [[nodiscard]] bool buildAndDraw(const Viewport& viewport,
                                    const std::vector<EntityQuad>& quads,
                                    const std::vector<ParticleQuad>& particles,
                                    bool playMode, const AssetBrowser* assets,
                                    TextureCache* textures,
                                    const GizmoDrawData* gizmo,
                                    const eng::project::GridConfig* grid);

    std::optional<eng::rhi::Renderer> renderer_{};
    eng::rhi::ShaderHandle shader_{};
    eng::rhi::GraphicsPipelineHandle pipeline_{};
    eng::rhi::BufferHandle vertexBuffer_{};
    std::size_t vertexCapacity_ = 0;

    // --- sprite pipeline (evolução P0-3) --------------------------------------
    /// Shaders/pipelines do 2D (P3 §2): color/unlit/lit — ShaderLibrary
    /// do eng::render; destruída no destroyResources ANTES do renderer.
    eng::render::ShaderLibrary shaders_{};
    eng::rhi::BufferHandle spriteBuffer_{};       ///< VBO unlit (40B)
    std::size_t spriteCapacity_ = 0;
    std::vector<SpriteVertex> spriteVertices_{};
    std::size_t lastFrameTexturedSprites_ = 0;
    std::size_t lastFrameDrawCalls_ = 0;  ///< P4.7.0 B6: frame.draw() do frame

    // --- sprite LIT (P3 §5 — iluminação 2D por fragmento) --------------------
    eng::rhi::BufferHandle litSpriteBuffer_{};    ///< VBO lit (48B)
    std::size_t litSpriteCapacity_ = 0;
    std::vector<LitSpriteVertex> litSpriteVertices_{};
    std::vector<eng::render::FrameUniforms> frameUniformsSent_{};

    /// Arena de vértices do frame (reusada — zero alocação por frame após
    /// estabilizar; missão §10 mobile).
    std::vector<Vertex> frameVertices_{};
    /// Arena do lote de gizmo — separada para NÃO misturar com o
    /// lote 1 (o gizmo desenha DEPOIS dos sprites, por cima).
    std::vector<Vertex> gizmoVertices_{};

    std::uint64_t framesSubmitted_ = 0;
    std::uint64_t framesPresented_ = 0;
    std::size_t lastFrameVertexCount_ = 0;
    float playBgR_ = 0.07f;
    float playBgG_ = 0.08f;
    float playBgB_ = 0.11f;
};

} // namespace eng::editor
