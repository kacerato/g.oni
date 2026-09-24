#include "eng/render/ShaderLibrary.hpp"

/// ShaderLibrary — implementação. Shaders e layouts são os
/// MESMOS fixtures canônicos do repositório (RenderShaders.hpp —
/// proveniência das FASES 5/6/P0-3 + par LIT novo gerado com
/// glslangValidator 15.2.0 --spirv-val).

#include <cstring>
#include <utility>

#include "eng/render/RenderShaders.hpp"
#include "eng/render/SpriteMaterial.hpp"

namespace eng::render {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

/// Pipeline de sprite: cull off, sem depth, blending alpha (o MESMO estado
/// do pipeline de sprite do editor desde P0-3 — não muda o look).
[[nodiscard]] eng::rhi::GraphicsPipelineDesc spritePipelineDesc(
    eng::rhi::ShaderHandle shader)
{
    eng::rhi::GraphicsPipelineDesc desc{};
    desc.shader = shader;
    desc.raster.cull = eng::rhi::CullMode::None;
    desc.depth.test = false;
    desc.depth.write = false;
    desc.blend.enabled = true;
    desc.blend.srcColor = eng::rhi::BlendFactor::SrcAlpha;
    desc.blend.dstColor = eng::rhi::BlendFactor::OneMinusSrcAlpha;
    return desc;
}

}  // namespace

// =============================================================================
// Move (protocolo de handles do ViewportRenderer — ADR-035)
// =============================================================================

ShaderLibrary::ShaderLibrary(ShaderLibrary&& other) noexcept
    : colorShader_(std::exchange(other.colorShader_, {})),
      spriteUnlitShader_(std::exchange(other.spriteUnlitShader_, {})),
      spriteLitShader_(std::exchange(other.spriteLitShader_, {})),
      colorPipeline_(std::exchange(other.colorPipeline_, {})),
      spriteUnlitPipeline_(std::exchange(other.spriteUnlitPipeline_, {})),
      spriteLitPipeline_(std::exchange(other.spriteLitPipeline_, {}))
{
}

ShaderLibrary& ShaderLibrary::operator=(ShaderLibrary&& other) noexcept
{
    if (this != &other) {
        colorShader_ = std::exchange(other.colorShader_, {});
        spriteUnlitShader_ = std::exchange(other.spriteUnlitShader_, {});
        spriteLitShader_ = std::exchange(other.spriteLitShader_, {});
        colorPipeline_ = std::exchange(other.colorPipeline_, {});
        spriteUnlitPipeline_ = std::exchange(other.spriteUnlitPipeline_, {});
        spriteLitPipeline_ = std::exchange(other.spriteLitPipeline_, {});
    }
    return *this;
}

void ShaderLibrary::destroy(eng::rhi::Renderer& renderer) noexcept
{
    if (spriteLitPipeline_.isValid()) {
        (void)renderer.destroyGraphicsPipeline(spriteLitPipeline_);
    }
    if (spriteLitShader_.isValid()) {
        (void)renderer.destroyShader(spriteLitShader_);
    }
    if (spriteUnlitPipeline_.isValid()) {
        (void)renderer.destroyGraphicsPipeline(spriteUnlitPipeline_);
    }
    if (spriteUnlitShader_.isValid()) {
        (void)renderer.destroyShader(spriteUnlitShader_);
    }
    if (colorPipeline_.isValid()) {
        (void)renderer.destroyGraphicsPipeline(colorPipeline_);
    }
    if (colorShader_.isValid()) {
        (void)renderer.destroyShader(colorShader_);
    }
    *this = ShaderLibrary{};
}

// =============================================================================
// Criação
// =============================================================================

Result<ShaderLibrary> ShaderLibrary::create(eng::rhi::Renderer& renderer)
{
    ShaderLibrary self;

    // --- 1) chrome do editor (pos+cor — mesma procedência FASES 5/6) -------
    {
        eng::rhi::ShaderDesc desc;
        desc.debugName = "editor.color";
        desc.vertexSpirv = kEditorVertexSpirvBytes();
        desc.fragmentSpirv = kEditorFragmentSpirvBytes();
        desc.vertexGlsl = kEditorVertexGlsl;
        desc.fragmentGlsl = kEditorFragmentGlsl;
        auto shader = renderer.createShader(desc);
        if (shader.isError()) {
            return makeUnexpected(shader.error());
        }
        self.colorShader_ = shader.value();

        eng::rhi::GraphicsPipelineDesc pipeline;
        pipeline.shader = self.colorShader_;
        pipeline.vertexLayout = colorLayout();
        pipeline.raster.cull = eng::rhi::CullMode::None;
        pipeline.depth.test = false;
        pipeline.depth.write = false;
        auto created = renderer.createGraphicsPipeline(pipeline);
        if (created.isError()) {
            (void)renderer.destroyShader(self.colorShader_);
            return makeUnexpected(created.error());
        }
        self.colorPipeline_ = created.value();
    }

    // --- 2) sprite UNLIT (pos+cor+uv — shader P0-3, layout 40B) ------------
    {
        eng::rhi::ShaderDesc desc;
        desc.debugName = "sprite.unlit";
        desc.vertexSpirv = kSpriteVertexSpirvBytes();
        desc.fragmentSpirv = kSpriteFragmentSpirvBytes();
        desc.vertexGlsl = kSpriteVertexGlsl;
        desc.fragmentGlsl = kSpriteFragmentGlsl;
        auto shader = renderer.createShader(desc);
        if (shader.isError()) {
            self.destroy(renderer);
            return makeUnexpected(shader.error());
        }
        self.spriteUnlitShader_ = shader.value();

        auto pipeline = spritePipelineDesc(self.spriteUnlitShader_);
        pipeline.vertexLayout = spriteUnlitLayout();
        auto created = renderer.createGraphicsPipeline(pipeline);
        if (created.isError()) {
            (void)renderer.destroyShader(self.spriteUnlitShader_);
            self.destroy(renderer);
            return makeUnexpected(created.error());
        }
        self.spriteUnlitPipeline_ = created.value();
    }

    // --- 3) sprite LIT (P3 §5 — bloco PerFrame, layout 48B com mundo) ------
    {
        eng::rhi::ShaderDesc desc;
        desc.debugName = "sprite.lit";
        desc.vertexSpirv = kSpriteLitVertexSpirvBytes();
        desc.fragmentSpirv = kSpriteLitFragmentSpirvBytes();
        desc.vertexGlsl = kSpriteLitVertexGlsl;
        desc.fragmentGlsl = kSpriteLitFragmentGlsl;
        // Contrato do bloco: o GLES atribui "PerFrame" ao binding 0 de
        // GL_UNIFORM_BUFFER no link (o SPIR-V já declara set=1/binding=0).
        desc.uniformBlockName = kFrameUniformBlockName;
        auto shader = renderer.createShader(desc);
        if (shader.isError()) {
            self.destroy(renderer);
            return makeUnexpected(shader.error());
        }
        self.spriteLitShader_ = shader.value();

        auto pipeline = spritePipelineDesc(self.spriteLitShader_);
        pipeline.vertexLayout = spriteLitLayout();
        auto created = renderer.createGraphicsPipeline(pipeline);
        if (created.isError()) {
            (void)renderer.destroyShader(self.spriteLitShader_);
            self.destroy(renderer);
            return makeUnexpected(created.error());
        }
        self.spriteLitPipeline_ = created.value();
    }
    return self;
}

// =============================================================================
// Consulta/bind
// =============================================================================

Result<eng::rhi::GraphicsPipelineHandle> ShaderLibrary::pipelineForShader(
    std::string_view shader) const
{
    if (shader == kShaderUnlit) {
        return spriteUnlitPipeline_;
    }
    if (shader == kShaderLit) {
        return spriteLitPipeline_;
    }
    return makeUnexpected(Error{StatusCode::InvalidArgument,
                                "render.shader: shader desconhecido '" +
                                    std::string{shader} +
                                    "' (registrados: unlit, lit)"});
}

Result<void> ShaderLibrary::bindFrameUniforms(eng::rhi::Frame& frame,
                                               const FrameUniforms& block) const
{
    static_assert(sizeof(FrameUniforms) == kFrameUniformSize);
    return frame.setUniformData(
        {reinterpret_cast<const std::byte*>(&block), kFrameUniformSize});
}

// =============================================================================
// Layouts de vértice
// =============================================================================

eng::rhi::VertexLayout ShaderLibrary::colorLayout()
{
    eng::rhi::VertexLayout layout;
    layout.bindings.push_back({0, 8 * sizeof(float)});
    layout.attributes.push_back(
        {0, 0, 0, eng::rhi::Format::R32G32B32A32Sfloat});
    layout.attributes.push_back(
        {1, 0, 16, eng::rhi::Format::R32G32B32A32Sfloat});
    return layout;
}

eng::rhi::VertexLayout ShaderLibrary::spriteUnlitLayout()
{
    eng::rhi::VertexLayout layout;
    layout.bindings.push_back({0, 10 * sizeof(float)});
    layout.attributes.push_back(
        {0, 0, 0, eng::rhi::Format::R32G32B32A32Sfloat});
    layout.attributes.push_back(
        {1, 0, 16, eng::rhi::Format::R32G32B32A32Sfloat});
    layout.attributes.push_back({2, 0, 32, eng::rhi::Format::R32G32Sfloat});
    return layout;
}

eng::rhi::VertexLayout ShaderLibrary::spriteLitLayout()
{
    eng::rhi::VertexLayout layout = spriteUnlitLayout();
    layout.bindings[0].stride = 12 * sizeof(float);  // + worldXY
    layout.attributes.push_back({3, 0, 40, eng::rhi::Format::R32G32Sfloat});
    return layout;
}

}  // namespace eng::render
