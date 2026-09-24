#pragma once

/// Cópia de uma entidade e de todos os descendentes (componentes via o
/// catálogo do serializer). Usada por "Duplicar" no editor e por `spawn`
/// nos scripts.

#include <string_view>

#include "eng/ecs/Ecs.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::editor {

/// Clona `source` e a subárvore. A raiz da cópia recebe `rootName` e fica
/// sob o mesmo pai do original. Com `activate`, a cópia perde o componente
/// Molde (eng::scene::Template) e passa a participar do jogo.
[[nodiscard]] eng::ecs::Entity cloneSubtree(eng::scene::Scene& scene,
                                            eng::ecs::Entity source,
                                            std::string_view rootName,
                                            bool activate);

}  // namespace eng::editor
