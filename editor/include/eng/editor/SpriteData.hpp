#pragma once

/// eng::editor::SpriteData — componente visual de sprite com TEXTURA REAL
/// (evolução P0-3: o fim do "retângulo colorido").
///
/// Modelo (missão 2D IMAGE/SPRITE WORKFLOW):
/// - `textureAsset`: NOME do asset de textura no projeto (categoria
///   "textures" do AssetBrowser — o TextureCache do host resolve para o
///   AssetId do registry e sobe p/ GPU);
/// - região UV (u0,v0)-(u1,v1): sprite sheet suportado por fatiamento;
/// - pivot: âncora local [0..1]² (centro 0.5,0.5 default);
/// - flip X/Y, tint RGB multiplicativo + opacity [0..1];
/// - sort/z: ordem de desenho (maior = frente);
/// - pixelsPerUnit: densidade da textura (N pixels por 1 unidade de
///   mundo — tamanho mundial = pixels da região / ppu).
///
/// Registrado no catálogo ÚNICO do SceneSerializer: aparece no
/// Inspector, persiste em cena e clona no Play. Campos FLAT (sem arrays —
/// reflexão/inspector/serializer operam por caminho simples).

#include <string>

#include "eng/reflect/Reflect.hpp"

namespace eng::editor {

struct SpriteData {
    /// Nome do asset de textura (vazio = sem textura — quad de cor).
    std::string textureAsset{};

    /// Material (P3 §3): nome do asset materials/<n>.mat.json (vazio =
    /// default "lit" com tint neutro — o look NÃO muda por osmose). O
    /// shader/tint do material dirigem o pipeline do sprite.
    std::string materialAsset{};

    // --- região (UV) -------------------------------------------------------
    float u0{0.f};
    float v0{0.f};
    float u1{1.f};
    float v1{1.f};

    // --- pivot/flip ---------------------------------------------------------
    float pivotX{0.5f};
    float pivotY{0.5f};
    bool flipX{false};
    bool flipY{false};

    // --- aparência -----------------------------------------------------------
    float tintR{1.f};
    float tintG{1.f};
    float tintB{1.f};
    float opacity{1.f};

    // --- ordenação ------------------------------------------------------------
    float sort{0.f};
    /// RECOVERY P0: 48 px por unidade — casa com o zoom padrão da câmera do
    /// editor (Viewport::Camera2D::zoom = 48): uma imagem importada aparece
    /// no viewport em tamanho 1:1 (1 texel = 1 pixel de tela), utilizável
    /// de cara. O default ANTERIOR (1) fazia uma foto de 1080px medir
    /// 1080 unidades ≈ 51.840px de tela — um "mar de cor" no viewport.
    /// Cenas antigas com ppu explícito são preservadas pela serialização.
    float pixelsPerUnit{48.f};
};

}  // namespace eng::editor

/// Reflexão (ADR-043: campos por caminho — Inspector/serializer).
/// Hints de edição: textureAsset → picker de texturas;
/// tintR/G/B → UM editor de cor (grupo 0) — serialização INALTERADA.
/// clang-format off
ENG_REFLECT_BEGIN(eng::editor::SpriteData)
    ENG_REFLECT_FIELD_HINT(textureAsset, "texture")
    ENG_REFLECT_FIELD_HINT(materialAsset, "material")
    ENG_REFLECT_FIELD(u0)
    ENG_REFLECT_FIELD(v0)
    ENG_REFLECT_FIELD(u1)
    ENG_REFLECT_FIELD(v1)
    ENG_REFLECT_FIELD(pivotX)
    ENG_REFLECT_FIELD(pivotY)
    ENG_REFLECT_FIELD(flipX)
    ENG_REFLECT_FIELD(flipY)
    ENG_REFLECT_FIELD_HINT(tintR, "color:0:r")
    ENG_REFLECT_FIELD_HINT(tintG, "color:0:g")
    ENG_REFLECT_FIELD_HINT(tintB, "color:0:b")
    ENG_REFLECT_FIELD(opacity)
    ENG_REFLECT_FIELD(sort)
    ENG_REFLECT_FIELD(pixelsPerUnit)
ENG_REFLECT_END()
/// clang-format on
