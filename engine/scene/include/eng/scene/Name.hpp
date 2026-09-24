#pragma once

/// eng::scene::Name — nome exibível de nó.
///
/// Componente de DOMÍNIO (não de editor): nomes são a forma humana de
/// referenciar entidades — hierarchy/inspector os exibem e a futura camada
/// de scripting fará lookup por nome. Registrado no reflect e no
/// SceneSerializer como componente persistido (audit FASE 8, decisão D1).
///
/// - Ausente ⇒ consumidores tratam como anônimo (padrão "Entity" é
///   responsabilidade de quem cria, ex.: editor);
/// - unicidade NÃO é imposta aqui (cenas reais toleram nomes repetidos;
///   desambiguação é por SceneEntityId/Entity — ADR-028/033);
/// - string é o storage natural: nomes saem na serialização como texto.
#include <string>

#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::scene {

struct Name {
    std::string value;
};

} // namespace eng::scene

ENG_REFLECT_BEGIN(eng::scene::Name)
    ENG_REFLECT_FIELD(value)
ENG_REFLECT_END()
