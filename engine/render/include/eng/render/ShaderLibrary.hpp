#pragma once

/// eng::render::ShaderLibrary — o Shader Core do 2D.
///
/// Camada ENTRE o engine high-level e o eng::rhi:
///
///   Editor/runtime (draw code) → Shader/Material abstraction → eng::rhi
///   → Vulkan / GLES
///
/// Responsabilidades (e NADA além):
/// - registra os shaders REAIS do 2D ("editor.color" para o chrome do
///   editor, "sprite.unlit" e "sprite.lit" para materiais) criando os
///   pares shader+pipeline no Renderer dado;
/// - conhece os LAYOUTS de vértice de cada pipeline (fonte única para o
///   batcher — 40B unlit, 48B lit com posição mundo);
/// - resolve nome de shader de material ("unlit"/"lit") → pipeline;
/// - empacota e sobe o bloco PerFrame (FrameUniforms) num frame dado.
///
/// NENHUM tipo Vulkan/GLES aparece aqui (o RHI absorve as diferenças —
/// SPIR-V+set/binding no Vulkan, GLSL ES+block binding no GLES, ambas as
/// representações embutidas em RenderShaders.hpp).
///
/// Ownership: handles são do RENDERER dono; a library é um
/// VALUE move-only SEM dtor destrutivo — o dono chama destroy(renderer)
/// no SEU destroyResources (o mesmo protocolo dos demais handles do
/// ViewportRenderer).

#include <string_view>

#include "eng/core/Result.hpp"
#include "eng/rhi/Renderer.hpp"
#include "eng/rhi/Types.hpp"
#include "eng/render/FrameParams.hpp"

namespace eng::render {

class ShaderLibrary final {
public:
    ShaderLibrary() = default;
    ShaderLibrary(ShaderLibrary&& other) noexcept;
    ShaderLibrary& operator=(ShaderLibrary&& other) noexcept;
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;
    ~ShaderLibrary() = default;  // dono destrói via destroy(renderer)

    /// Cria shaders + pipelines no renderer. Falha → erro preciso (o
    /// chamador aborta a criação do viewport renderer).
    [[nodiscard]] static eng::core::Result<ShaderLibrary> create(
        eng::rhi::Renderer& renderer);

    /// Destrói TODOS os handles via renderer (dono chama no teardown).
    void destroy(eng::rhi::Renderer& renderer) noexcept;

    [[nodiscard]] bool valid() const noexcept { return colorPipeline_.isValid(); }

    // --- pipelines por nome (material → shader) ----------------------------

    /// "unlit" → pipeline de sprite sem bloco PerFrame (caminho clássico).
    [[nodiscard]] eng::rhi::GraphicsPipelineHandle spriteUnlitPipeline()
        const noexcept
    {
        return spriteUnlitPipeline_;
    }
    /// "lit" → pipeline de sprite com bloco PerFrame (luzes 2D).
    [[nodiscard]] eng::rhi::GraphicsPipelineHandle spriteLitPipeline()
        const noexcept
    {
        return spriteLitPipeline_;
    }
    /// Chrome do editor (grade/bordas/gizmo — pos+cor, sem blending).
    [[nodiscard]] eng::rhi::GraphicsPipelineHandle colorPipeline()
        const noexcept
    {
        return colorPipeline_;
    }

    /// Nome → pipeline. "unlit"/"lit" são os shaders registrados; outro
    /// nome = erro preciso (material com shader desconhecido NÃO desenha
    /// por engano no pipeline errado).
    [[nodiscard]] eng::core::Result<eng::rhi::GraphicsPipelineHandle>
    pipelineForShader(std::string_view shader) const;

    /// Sobe o bloco PerFrame no frame (região própria do frame-slot) —
    /// vale para os draws seguintes. Usado antes de cada grupo de sprites
    /// lit (a luz por camada seleciona o conjunto).
    [[nodiscard]] eng::core::Result<void> bindFrameUniforms(
        eng::rhi::Frame& frame, const FrameUniforms& block) const;

    // --- layouts de vértice (fonte única do batcher) ------------------------

    /// Editor chrome: pos vec4 + cor vec4 (32B).
    [[nodiscard]] static eng::rhi::VertexLayout colorLayout();
    /// Sprite unlit: pos vec4 + cor vec4 + uv vec2 (40B).
    [[nodiscard]] static eng::rhi::VertexLayout spriteUnlitLayout();
    /// Sprite lit: + posição MUNDO vec2 (48B — iluminação por fragmento
    /// precisa da posição mundial interpolada).
    [[nodiscard]] static eng::rhi::VertexLayout spriteLitLayout();

private:
    eng::rhi::ShaderHandle colorShader_{};
    eng::rhi::ShaderHandle spriteUnlitShader_{};
    eng::rhi::ShaderHandle spriteLitShader_{};
    eng::rhi::GraphicsPipelineHandle colorPipeline_{};
    eng::rhi::GraphicsPipelineHandle spriteUnlitPipeline_{};
    eng::rhi::GraphicsPipelineHandle spriteLitPipeline_{};
};

}  // namespace eng::render
