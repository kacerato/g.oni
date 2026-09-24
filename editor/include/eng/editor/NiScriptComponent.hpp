#pragma once

/// eng::editor::NiScriptComponent — script NI-Script anexado a um nó
/// (FASE 11, missão: integração com o PLAY do editor).
///
/// Componente de GAMEPLAY registrado no catálogo ÚNICO do SceneSerializer
/// (ADR-043 — mesmo padrão de RigidBody/Animator/ParticleEmitter: o
/// registro vive no CONSUMIDOR, engine/scene não conhece scripting).
///
/// - `source` é o texto .nis COMPLETO (anexado via scriptAssign do asset
///   .nis do projeto, ou editável direto pelo Inspector — UI DEDICADA de
///   script existe desde a evolução P0-7: painel Scripts com
///   compilação/diagnósticos, ver ADR-053);
/// - em PLAY, o EditorDocument compila o source de cada instância no
///   CLONE (ADR-044: a edição nunca é tocada), instancia o NiScriptState
///   e roda @init → up start → up update (por tick) → up destroy.
///
/// Semântica de falha (honestidade): erro de COMPILAÇÃO → script
/// desabilitado com log (a cena continua); FAULT de runtime → registrado
/// em lastFault (design §5.3.5 — o script não morre).

#include <string>

#include "eng/reflect/Reflect.hpp"

namespace eng::editor {

struct NiScriptComponent {
    std::string source; ///< fonte .nis completa

    /// P4.7.0 Bloco 6: OPT-OUT do logic LOD — com o setting da cena ON
    /// (default OFF), scripts off-screen pulam o `up update`; um script
    /// marcado (gameplay crítico: spawner, placar, IA global) roda
    /// SEMPRE. Additive (default false = participa) — cenas antigas
    /// migram pelo pre-pass (mesmo padrão da câmera do B4).
    bool lodOptOut = false;

    /// Script do projeto ligado a este componente (nome em assets/scripts).
    /// Vazio = fonte embutida. Salvar o arquivo atualiza `source`.
    std::string scriptAsset;
};

} // namespace eng::editor

ENG_REFLECT_BEGIN(eng::editor::NiScriptComponent)
    ENG_REFLECT_FIELD_HINT(source, "code")
    ENG_REFLECT_FIELD(lodOptOut)
    ENG_REFLECT_FIELD_HINT(scriptAsset, "script")
ENG_REFLECT_END()
