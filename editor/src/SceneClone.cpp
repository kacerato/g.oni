#include "eng/editor/SceneClone.hpp"

#include <string>
#include <unordered_map>
#include <vector>

#include "eng/log/Macros.hpp"
#include "eng/scene/Layers.hpp"
#include "eng/scene/Name.hpp"
#include "eng/scene/SceneSerializer.hpp"

ENG_LOG_CATEGORY("editor");

namespace eng::editor {

eng::ecs::Entity cloneSubtree(eng::scene::Scene& scene, eng::ecs::Entity source,
                              std::string_view rootName, bool activate)
{
    if (!scene.isNode(source)) {
        return eng::scene::kNoEntity;
    }
    // Subárvore em ordem por nível (pais antes dos filhos).
    std::vector<eng::ecs::Entity> subtree{source};
    for (std::size_t i = 0; i < subtree.size(); ++i) {
        scene.eachChild(subtree[i],
                        [&](eng::ecs::Entity child) { subtree.push_back(child); });
    }
    std::unordered_map<eng::ecs::Entity, eng::ecs::Entity> remap;
    for (const eng::ecs::Entity node : subtree) {
        remap[node] = scene.createNode();
    }
    for (const eng::ecs::Entity node : subtree) {
        const eng::ecs::Entity clone = remap.at(node);
        for (const auto& [typeName, entry] : eng::scene::detail::componentEntries()) {
            if (typeName == "eng::scene::Name" || !entry.has(scene.world(), node)) {
                continue;
            }
            if (activate && node == source && typeName == "eng::scene::Template") {
                continue;
            }
            auto encoded = entry.encode(entry, scene.world(), node);
            if (encoded.isError()) {
                ENG_WARN("clone: encode de '{}' falhou ({})", typeName,
                         encoded.error().message);
                continue;
            }
            auto decoded =
                entry.decodeAndEmplace(entry, scene.world(), clone, encoded.value());
            if (decoded.isError()) {
                ENG_WARN("clone: decode de '{}' falhou ({})", typeName,
                         decoded.error().message);
            }
        }
        std::string name;
        if (node == source) {
            name = std::string(rootName);
        } else if (const auto* n = scene.world().get<eng::scene::Name>(node)) {
            name = n->value;
        }
        (void)scene.world().emplace<eng::scene::Name>(clone, eng::scene::Name{name});
        const eng::ecs::Entity parent = scene.parentOf(node);
        if (parent != eng::scene::kNoEntity) {
            const auto it = remap.find(parent);
            (void)scene.attach(clone, it != remap.end() ? it->second : parent);
        }
    }
    return remap.at(source);
}

}  // namespace eng::editor
