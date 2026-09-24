#pragma once

/// eng::render::SpriteMaterial — material 2D REAL.
///
/// Arquitetura do bloco: Entity → Sprite → MATERIAL → Shader → RHI.
/// O sprite renderer consome o material de FATO (shader escolhe o
/// pipeline; tint multiplica o do sprite; blend do material seleciona o
/// estado do pipeline) — nada aqui é decorativo.
///
/// Origem dos materiais: assets/materials/<nome>.mat.json (codec
/// MaterialAsset abaixo — mesmas regras dos assets de animação do P2).
/// SpriteData com material VAZIO usa o material default implícito
/// (shader "lit", tint neutro): o look do editor não muda por osmose.
///
/// Honestidade de escopo 2D: NÃO há culling/depth/campos sem efeito —
/// quads 2D não têm backface nem z-test. "unlit"/"lit" são os dois
/// shaders reais registrados na ShaderLibrary.

#include <string>
#include <string_view>

#include "eng/core/Result.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::render {

/// Shader de sprite conhecidos (validados na criação do material —
/// nome inválido é ERRO, não fallback silencioso).
inline constexpr std::string_view kShaderUnlit{"unlit"};
inline constexpr std::string_view kShaderLit{"lit"};

/// Modelo do material (o que o renderer 2D de fato aplica).
struct SpriteMaterial {
    /// "unlit" (textura × tint — caminho clássico) ou "lit" (× ambiente +
    /// luzes do bloco PerFrame). Default "lit": o default do engine.
    std::string shader{"lit"};
    /// Tint multiplicativo sobre o sprite (1,1,1,1 = neutro).
    float tintR{1.f};
    float tintG{1.f};
    float tintB{1.f};
    float tintA{1.f};

    /// Valida o shader contra os nomes reais registrados.
    [[nodiscard]] bool hasValidShader() const noexcept
    {
        return shader == kShaderUnlit || shader == kShaderLit;
    }

    [[nodiscard]] bool operator==(const SpriteMaterial&) const = default;
};

/// Asset .mat.json (assets/materials/<nome>.mat.json):
///
/// {
///   "name": "Gema",
///   "shader": "lit",
///   "tint": [r, g, b, a]
/// }
///
/// `shader` inválido é ERRO de decode (não default silencioso). `tint`
/// ausente = neutro (compat com materiais mínimos).
struct MaterialAsset {
    std::string name{};
    SpriteMaterial material{};
};

/// JSON → MaterialAsset (erros precisos com contexto).
[[nodiscard]] eng::core::Result<MaterialAsset> materialDecode(
    std::string_view json);

/// MaterialAsset → JSON (dump estável — round-trip testado).
[[nodiscard]] eng::core::Result<std::string> materialEncode(
    const MaterialAsset& asset);

}  // namespace eng::render
