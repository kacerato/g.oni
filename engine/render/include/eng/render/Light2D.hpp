#pragma once

/// eng::render::Light2D — componente de luz 2D.
///
/// LUZ REAL, não um círculo branco: o renderer alimenta o bloco "PerFrame"
/// (FrameUniforms) e o FRAGMENT do sprite.lit resolve a iluminação
/// por-pixel (distância mundo → atenuação com falloff). A posição vem do
/// TRANSFORM da entidade (não é campo — fonte única de verdade).
///
/// Camadas (layer/mask REAL — usa o LayerRegistry existente da Scene,
/// ADR-051): a luz ilumina apenas sprites cuja entidade é membro da MESMA
/// camada (LayerMember.layer; sem LayerMember = "GAME" default).
///
/// Registrado no catálogo ÚNICO do SceneSerializer pelo editor
/// (ComponentRegistration.cpp) — Inspector/serialização/Play/clone vêm
/// de graça pelo mesmo caminho de RigidBody/Animator/CameraData.

#include <string>

#include "eng/reflect/Reflect.hpp"

namespace eng::render {

struct Light2D {
    /// Luz desligada não entra no bloco do frame (custo zero).
    bool enabled{true};

    // --- cor (grupo 0 do Inspector — mesmo padrão do SpriteData) ---------
    float colorR{1.f};
    float colorG{0.93f};
    float colorB{0.78f};

    /// Multiplicador escalar da contribuição (0 = apagada).
    float intensity{1.f};

    /// Alcance em UNIDADES DE MUNDO (atenuação linear até a borda).
    float radius{4.f};

    /// Expoente da atenuação (1 = linear; >1 = concentra no centro).
    float falloff{1.5f};

    /// Camada que esta luz ilumina (membro da cena — ADR-051).
    /// "GAME" ilumina entidades sem LayerMember (a default).
    std::string layer{"GAME"};
};

}  // namespace eng::render

/// Reflexão (ADR-043: campos por caminho — Inspector/serializer).
/// Hints: colorR/G/B → UM editor de cor (grupo 0).
/// clang-format off
ENG_REFLECT_BEGIN(eng::render::Light2D)
    ENG_REFLECT_FIELD(enabled)
    ENG_REFLECT_FIELD_HINT(colorR, "color:0:r")
    ENG_REFLECT_FIELD_HINT(colorG, "color:0:g")
    ENG_REFLECT_FIELD_HINT(colorB, "color:0:b")
    ENG_REFLECT_FIELD(intensity)
    ENG_REFLECT_FIELD(radius)
    ENG_REFLECT_FIELD(falloff)
    ENG_REFLECT_FIELD(layer)
ENG_REFLECT_END()
/// clang-format on
