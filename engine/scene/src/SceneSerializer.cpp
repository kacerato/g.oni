#include "eng/scene/SceneSerializer.hpp"

#include <algorithm>
#include <unordered_map>

#include "eng/log/Log.hpp"
#include <utility>
#include <vector>

#include "eng/log/Macros.hpp"
#include "eng/math/Quat.hpp"
#include "eng/math/Transform.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Name.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/serial/Json.hpp"

ENG_LOG_CATEGORY("scene");

// =============================================================================
// Registro reflect dos tipos math usados pelos componentes persistidos
// (nomes QUALIFICADOS = chaves estáveis entre builds — ADR-033). Registro
// idempotente por nome (primeiro vence — ADR-021), seguro em múltiplas TUs.
// =============================================================================

ENG_REFLECT_BEGIN(eng::math::Vec3)
    ENG_REFLECT_FIELD(x)
    ENG_REFLECT_FIELD(y)
    ENG_REFLECT_FIELD(z)
ENG_REFLECT_END()

ENG_REFLECT_BEGIN(eng::math::Quat)
    ENG_REFLECT_FIELD(x)
    ENG_REFLECT_FIELD(y)
    ENG_REFLECT_FIELD(z)
    ENG_REFLECT_FIELD(w)
ENG_REFLECT_END()

ENG_REFLECT_BEGIN(eng::math::Transform)
    ENG_REFLECT_FIELD_AS(position, "eng::math::Vec3")
    ENG_REFLECT_FIELD_AS(rotation, "eng::math::Quat")
    ENG_REFLECT_FIELD_AS(scale, "eng::math::Vec3")
ENG_REFLECT_END()

namespace eng::scene::detail {

namespace {

/// Registro global de componentes serializáveis. std::map → iteração
/// ORDENADA POR NOME (determinismo dos componentes por tipo, ADR-033).
/// Registro esperado em init single-threaded.
std::map<std::string, ComponentEntry>& componentRegistry()
{
    static std::map<std::string, ComponentEntry> registry;
    return registry;
}

/// Componentes INTERNOS da Scene: tolerados no JSON com WARN (escritores
/// externos podem vazar) — a representação canônica é "parent"/"id".
[[nodiscard]] bool isInternalComponentName(const std::string& name)
{
    return name == "eng::scene::SceneIdentity" ||
           name == "eng::scene::Hierarchy" ||
           name == "eng::scene::WorldMatrix" ||
           name == "SceneIdentity" || name == "Hierarchy" ||
           name == "WorldMatrix";
}

} // namespace

void registerComponentEntry(std::string typeName, ComponentEntry entry)
{
    componentRegistry()[std::move(typeName)] = entry;
}

const std::map<std::string, ComponentEntry>& componentEntries()
{
    return componentRegistry();
}

void registerComponentContract(
    std::string_view typeName, ComponentContract contract,
    HookAttach onAttach, HookDetach onDetach, HookValidate onValidate,
    void* hookUser)
{
    auto& registry = componentRegistry();
    const auto it = registry.find(std::string(typeName));
    if (it == registry.end()) {
        return; // tipo não registrado: contrato ignorado (o chamador
                // que re-registrar o tipo obterá o contrato completo)
    }
    it->second.contract = std::move(contract);
    it->second.onAttach = onAttach;
    it->second.onDetach = onDetach;
    it->second.onValidate = onValidate;
    it->second.hookUser = hookUser;
}

} // namespace eng::scene::detail

namespace eng::scene {

namespace {

// --- built-ins: registrado no carregamento do módulo ------------------------
// (SceneSerializer.cpp só entra no link se alguém usa o serializer —
// garantia de que o registro acompanha o uso.)
const bool eng_scene_builtin_components_registered = [] {
    (void)SceneSerializer::registerComponentType<eng::math::Transform>(
        "eng::math::Transform");
    (void)SceneSerializer::registerComponentType<eng::scene::Name>(
        "eng::scene::Name");
    // Evolução P0-5: camadas são ESTRUTURA DE CENA e o componente
    // vive NO módulo scene — registro built-in (idempotente: consumidor que
    // re-registra só sobrescreve a mesma entrada).
    (void)SceneSerializer::registerComponentType<eng::scene::LayerMember>(
        "eng::scene::LayerMember");
    (void)SceneSerializer::registerComponentType<eng::scene::Template>(
        "eng::scene::Template");

    // Contratos dos built-ins (categoria do Inspector;
    // LayerMember é organização de tick/camadas → "Lógica"; Name é
    // rótulo → "Transform"). Sem hooks nativos aqui (nada a registrar).
    using eng::scene::detail::ComponentContract;
    {
        ComponentContract c;
        c.category = "Transform";
        eng::scene::detail::registerComponentContract(
            "eng::math::Transform", std::move(c));
    }
    {
        ComponentContract c;
        c.category = "Transform";
        eng::scene::detail::registerComponentContract(
            "eng::scene::Name", std::move(c));
    }
    {
        ComponentContract c;
        c.category = "Lógica";
        c.scriptAlias = "layer";
        eng::scene::detail::registerComponentContract(
            "eng::scene::LayerMember", std::move(c));
    }
    {
        ComponentContract c;
        c.category = "Lógica";
        c.scriptAlias = "template";
        eng::scene::detail::registerComponentContract(
            "eng::scene::Template", std::move(c));
    }
    return true;
}();

using eng::core::Error;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error sceneError(StatusCode code, std::string message)
{
    return Error{code, "SceneSerializer: " + std::move(message)};
}

/// Registro coletado por nó durante o save.
struct NodeRecord {
    eng::ecs::Entity entity{};
    SceneEntityId id;
    eng::ecs::Entity parent{}; ///< kNoEntity = raiz
};

} // namespace

// =============================================================================
// save
// =============================================================================

eng::core::Result<std::string> SceneSerializer::save(Scene& scene)
{
    // 0. Links com ponta morta (bypass do world) não persistem — varredura
    //    antes de qualquer leitura (tolerância oportunista, ADR-051).
    (void)scene.sweepLinks();

    // 1. Enumerar nós (achado crítico 2: todo nó tem Hierarchy; each
    //    itera o pool com snapshot mutável-seguro — emplace de
    //    SceneIdentity durante a iteração é seguro, ADR-024).
    std::vector<NodeRecord> records;
    std::unordered_map<eng::ecs::Entity, SceneEntityId> idOf;
    scene.world().each<Hierarchy>(
        [&](eng::ecs::Entity e, const Hierarchy& hierarchy) {
            NodeRecord record;
            record.entity = e;
            record.parent = hierarchy.parent;
            if (const SceneIdentity* identity =
                    scene.world().get<SceneIdentity>(e)) {
                record.id = identity->id;
            } else {
                record.id = SceneEntityId::generate();
                // primeira serialização: identidade persistente
                (void)scene.world().emplace<SceneIdentity>(
                    e, SceneIdentity{record.id});
            }
            records.push_back(record);
        });

    for (const NodeRecord& record : records) {
        idOf[record.entity] = record.id;
    }

    // 2. Componentes (entradas ordenadas por nome — ADR-033).
    struct EntityJson {
        SceneEntityId id;
        eng::serial::JsonValue json;
    };
    const auto& entries = detail::componentEntries();

    std::vector<EntityJson> entities;
    entities.reserve(records.size());
    for (const NodeRecord& record : records) {
        eng::serial::JsonValue components = eng::serial::JsonValue::array();
        for (const auto& [name, entry] : entries) {
            if (!entry.has(scene.world(), record.entity)) {
                continue;
            }
            auto encoded =
                entry.encode(entry, scene.world(), record.entity);
            if (encoded.isError()) {
                return makeUnexpected(encoded.error());
            }
            eng::serial::JsonValue component = eng::serial::JsonValue::object();
            component.set("type", eng::serial::JsonValue::string(name));
            component.set("data", std::move(encoded.value()));
            components.append(std::move(component));
        }

        eng::serial::JsonValue entity = eng::serial::JsonValue::object();
        entity.set("id", eng::serial::JsonValue::string(record.id.toString()));
        const auto parentId = idOf.find(record.parent);
        if (record.parent != kNoEntity && parentId != idOf.end()) {
            entity.set("parent",
                       eng::serial::JsonValue::string(
                           parentId->second.toString()));
        } else {
            // Raiz — inclusive pai obsoleto por bypass do world
            // (política de ADR-025: tratado como raiz).
            entity.set("parent", eng::serial::JsonValue::null());
        }
        entity.set("components", std::move(components));
        entities.push_back(EntityJson{record.id, std::move(entity)});
    }

    // 3. Ordem canônica: DFS pela HIERARQUIA (P4.3/Bloco 1 — "ordem
    //    estável"). Raízes por índice de ECS (a MESMA ordem que o autor vê
    //    na Hierarquia/viewport — hierarchySnapshot/buildQuads); filhos na
    //    ordem interna do Scene. O LOAD recria por esta ordem e anexa nesta
    //    ordem — irmãos e raízes SOBREVIVEM ao round-trip (a ordem antiga
    //    por SceneEntityId/UUID recriava a cena com raízes/irmãos
    //    embaralhados — o autor perdia a arrumação). Determinismo
    //    preservado: mesmo estado → mesma sequência → mesmos
    //    bytes; resave de um clone é idêntico (ordem do clone = ordem do
    //    arquivo = DFS da original).
    {
        std::unordered_map<SceneEntityId, std::size_t> jsonOf;
        jsonOf.reserve(entities.size());
        for (std::size_t i = 0; i < entities.size(); ++i) {
            jsonOf.emplace(entities[i].id, i);
        }
        std::vector<eng::ecs::Entity> roots;
        for (const NodeRecord& record : records) {
            if (record.parent == kNoEntity ||
                idOf.find(record.parent) == idOf.end()) {
                roots.push_back(record.entity);
            }
        }
        std::sort(roots.begin(), roots.end(),
                  [](eng::ecs::Entity a, eng::ecs::Entity b) {
                      return a.index < b.index;
                  });
        std::vector<std::size_t> dfsOrder;
        dfsOrder.reserve(entities.size());
        std::vector<bool> visited(entities.size(), false);
        // DFS pré-ordem ITERATIVA — filhos empilhados em ordem reversa
        // para saírem na ordem natural do Scene (sem recursão profunda
        // em cenas encadeadas).
        std::vector<eng::ecs::Entity> stack;
        for (const eng::ecs::Entity root : roots) {
            stack.push_back(root);
            while (!stack.empty()) {
                const eng::ecs::Entity node = stack.back();
                stack.pop_back();
                const auto idIt = idOf.find(node);
                if (idIt == idOf.end()) {
                    continue;  // nó obsoleto (defensivo — sem abortar)
                }
                const auto jsonIt = jsonOf.find(idIt->second);
                if (jsonIt != jsonOf.end() && !visited[jsonIt->second]) {
                    visited[jsonIt->second] = true;
                    dfsOrder.push_back(jsonIt->second);
                }
                std::vector<eng::ecs::Entity> children;
                scene.eachChild(node, [&](eng::ecs::Entity child) {
                    children.push_back(child);
                });
                for (auto rit = children.rbegin(); rit != children.rend();
                     ++rit) {
                    stack.push_back(*rit);
                }
            }
        }
        // Órfãos fora da floresta (estado impossível por contrato — ADR-025
        // trata pai obsoleto como raiz): entram no FIM por índice — nada é
        // descartado do arquivo (zero perda).
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (!visited[i]) {
                dfsOrder.push_back(i);
            }
        }
        std::vector<EntityJson> ordered;
        ordered.reserve(entities.size());
        for (const std::size_t idx : dfsOrder) {
            ordered.push_back(std::move(entities[idx]));
        }
        entities = std::move(ordered);
    }

    eng::serial::JsonValue ids = eng::serial::JsonValue::array();
    eng::serial::JsonValue entitiesArray = eng::serial::JsonValue::array();
    for (EntityJson& entity : entities) {
        ids.append(eng::serial::JsonValue::string(entity.id.toString()));
        entitiesArray.append(std::move(entity.json));
    }

    // 4. Camadas (evolução P0-5, ADR-051): SEMPRE emitidas — defaults
    //    incluídos. Ordem da registry (GAME, SUBGAME, nomeadas na ordem de
    //    adição) é estável entre saves.
    eng::serial::JsonValue layersArray = eng::serial::JsonValue::array();
    for (const LayerDefinition& layer : scene.layers().definitions()) {
        eng::serial::JsonValue entry = eng::serial::JsonValue::object();
        entry.set("name", eng::serial::JsonValue::string(layer.name));
        entry.set("update",
                  eng::serial::JsonValue::boolean(
                      layer.participation.update));
        entry.set("physics",
                  eng::serial::JsonValue::boolean(
                      layer.participation.physics));
        entry.set("render",
                  eng::serial::JsonValue::boolean(
                      layer.participation.render));
        entry.set("timeScale", eng::serial::JsonValue::real(
                                   static_cast<double>(layer.timeScale)));
        layersArray.append(std::move(entry));
    }

    // 5. Links (evolução P0-5, ADR-051): tipos registrados + entradas em
    //    ordem de criação (round-trip estável — ADR-051).
    eng::serial::JsonValue linksSection = eng::serial::JsonValue::object();
    bool hasLinkContent = false;
    if (!scene.links().types().empty()) {
        eng::serial::JsonValue linkTypes = eng::serial::JsonValue::array();
        for (const auto& [type, flags] : scene.links().types()) {
            eng::serial::JsonValue entry = eng::serial::JsonValue::object();
            entry.set("type", eng::serial::JsonValue::string(type));
            entry.set("hierarchical",
                      eng::serial::JsonValue::boolean(flags.hierarchical));
            linkTypes.append(std::move(entry));
        }
        linksSection.set("linkTypes", std::move(linkTypes));
        hasLinkContent = true;
    }
    if (scene.links().size() > 0) {
        eng::serial::JsonValue entries = eng::serial::JsonValue::array();
        scene.links().each(
            [&](LinkId /*id*/, const LinkRecord& record) {
                const auto fromId = idOf.find(record.from);
                const auto toId = idOf.find(record.to);
                if (fromId == idOf.end() || toId == idOf.end()) {
                    return;  // sweep garantiu vivos; defesa extra
                }
                eng::serial::JsonValue entry =
                    eng::serial::JsonValue::object();
                entry.set("type",
                          eng::serial::JsonValue::string(record.type));
                entry.set("from", eng::serial::JsonValue::string(
                                     fromId->second.toString()));
                entry.set("to", eng::serial::JsonValue::string(
                                   toId->second.toString()));
                entries.append(std::move(entry));
            });
        linksSection.set("entries", std::move(entries));
        hasLinkContent = true;
    }

    eng::serial::JsonValue root = eng::serial::JsonValue::object();
    root.set("formatVersion",
             eng::serial::JsonValue::uinteger(kFormatVersion));
    root.set("sceneEntityIds", std::move(ids));
    root.set("entities", std::move(entitiesArray));
    root.set("layers", std::move(layersArray));
    if (hasLinkContent) {
        root.set("links", std::move(linksSection));
    }
    return eng::serial::dumpJson(root);
}

// =============================================================================
// load
// =============================================================================

eng::core::Result<void> SceneSerializer::load(Scene& scene,
                                              std::string_view text)
{
    const auto parsed = eng::serial::parseJson(text);
    if (parsed.isError()) {
        return makeUnexpected(parsed.error());
    }
    const eng::serial::JsonValue& root = parsed.value();
    if (!root.isObject()) {
        return makeUnexpected(
            sceneError(StatusCode::ParseError, "raiz não é objeto"));
    }

    const auto format = root.find("formatVersion");
    if (!format.has_value() || !format->isUnsigned() ||
        format->asU64() > kFormatVersion) {
        return makeUnexpected(sceneError(
            StatusCode::NotSupported,
            "formatVersion ausente/inválida/maior que a suportada (" +
                std::to_string(kFormatVersion) + ")"));
    }

    // Camadas (evolução P0-5, ADR-051): definições ANTES das entidades —
    // LayerMember é validado contra a registry no fim do load. Arquivos
    // antigos (sem a seção) carregam com os defaults GAME/SUBGAME.
    const auto layersField = root.find("layers");
    if (layersField.has_value()) {
        if (!layersField->isArray()) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError, "'layers' não é array"));
        }
        for (std::size_t i = 0; i < layersField->size(); ++i) {
            const eng::serial::JsonValue layer = layersField->at(i);
            if (!layer.isObject()) {
                return makeUnexpected(sceneError(
                    StatusCode::ParseError,
                    "layers[" + std::to_string(i) + "] não é objeto"));
            }
            const auto nameField = layer.find("name");
            if (!nameField.has_value() || !nameField->isString()) {
                return makeUnexpected(sceneError(
                    StatusCode::ParseError,
                    "layers[" + std::to_string(i) +
                        "] sem 'name' string"));
            }
            const std::string layerName = nameField->asString();

            LayerParticipation participation;
            const auto readFlag =
                [&layer, &layerName,
                 i](const char* key, bool& out)
                    -> eng::core::Result<void> {
                    const auto flag = layer.find(key);
                    if (!flag.has_value() || !flag->isBool()) {
                        return makeUnexpected(sceneError(
                            StatusCode::ParseError,
                            "layers[" + std::to_string(i) + "] ('" +
                                layerName + "') sem '" + key + "' bool"));
                    }
                    out = flag->asBool();
                    return {};
                };
            if (const auto r = readFlag("update", participation.update);
                r.isError()) {
                return makeUnexpected(r.error());
            }
            if (const auto r = readFlag("physics", participation.physics);
                r.isError()) {
                return makeUnexpected(r.error());
            }
            if (const auto r = readFlag("render", participation.render);
                r.isError()) {
                return makeUnexpected(r.error());
            }
            float timeScale = 1.f;
            const auto timeField = layer.find("timeScale");
            if (timeField.has_value()) {
                if (!timeField->isNumber() || !(timeField->asF64() >= 0.0)) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "layers[" + std::to_string(i) + "] ('" +
                            layerName + "') timeScale inválido"));
                }
                timeScale = static_cast<float>(timeField->asF64());
            }

            if (!scene.layers().has(layerName)) {
                const auto added = scene.layers().addLayer(layerName);
                if (added.isError()) {
                    return makeUnexpected(added.error());
                }
            }
            const auto setParticipation = scene.layers().setParticipation(
                layerName, participation);
            if (setParticipation.isError()) {
                return makeUnexpected(setParticipation.error());
            }
            const auto setTimeScale =
                scene.layers().setTimeScale(layerName, timeScale);
            if (setTimeScale.isError()) {
                return makeUnexpected(setTimeScale.error());
            }
        }
    }

    // sceneEntityIds: lista canônica (consistência validada contra entities)
    std::vector<SceneEntityId> declaredIds;
    const auto declared = root.find("sceneEntityIds");
    if (declared.has_value() && declared->isArray()) {
        for (std::size_t i = 0; i < declared->size(); ++i) {
            const auto& entry = declared->at(i);
            if (!entry.isString()) {
                return makeUnexpected(sceneError(
                    StatusCode::ParseError,
                    "sceneEntityIds[" + std::to_string(i) +
                        "] não é string"));
            }
            auto id = SceneEntityId::fromString(entry.asString());
            if (id.isError()) {
                return makeUnexpected(id.error());
            }
            declaredIds.push_back(id.value());
        }
    }

    const auto entitiesField = root.find("entities");
    if (!entitiesField.has_value() || !entitiesField->isArray()) {
        return makeUnexpected(sceneError(StatusCode::ParseError,
                                         "'entities' ausente ou não-array"));
    }

    struct LoadedEntity {
        eng::ecs::Entity entity{};
        SceneEntityId id;
        SceneEntityId parent;
        bool hasParent = false;
    };

    const auto& entries = detail::componentEntries();
    std::vector<LoadedEntity> loaded;
    loaded.reserve(entitiesField->size());

    // Mapa incremental: detecta ids DUPLICADOS no próprio arquivo.
    std::unordered_map<SceneEntityId, eng::ecs::Entity> entityOf;

    for (std::size_t i = 0; i < entitiesField->size(); ++i) {
        const eng::serial::JsonValue entity = entitiesField->at(i);
        if (!entity.isObject()) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "entities[" + std::to_string(i) + "] não é objeto"));
        }

        const auto idField = entity.find("id");
        if (!idField.has_value() || !idField->isString()) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "entities[" + std::to_string(i) + "] sem 'id' string"));
        }
        auto id = SceneEntityId::fromString(idField->asString());
        if (id.isError()) {
            return makeUnexpected(id.error());
        }

        LoadedEntity record;
        record.id = id.value();
        record.entity = scene.createNode();
        (void)scene.world().emplace<SceneIdentity>(
            record.entity, SceneIdentity{record.id});
        if (!entityOf.emplace(record.id, record.entity).second) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "id duplicado: " + record.id.toString()));
        }

        const auto parentField = entity.find("parent");
        if (parentField.has_value() && parentField->isString()) {
            auto parent = SceneEntityId::fromString(parentField->asString());
            if (parent.isError()) {
                return makeUnexpected(parent.error());
            }
            record.parent = parent.value();
            record.hasParent = true;
        } else if (parentField.has_value() && !parentField->isNull()) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "entities[" + std::to_string(i) +
                    "]: 'parent' deve ser uuid ou null"));
        }

        // Componentes (na ordem do arquivo — que é a canônica por nome).
        const auto componentsField = entity.find("components");
        if (componentsField.has_value()) {
            if (!componentsField->isArray()) {
                return makeUnexpected(sceneError(
                    StatusCode::ParseError,
                    "entities[" + std::to_string(i) +
                        "]: 'components' não é array"));
            }
            for (std::size_t c = 0; c < componentsField->size(); ++c) {
                const eng::serial::JsonValue component =
                    componentsField->at(c);
                if (!component.isObject()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "components[" + std::to_string(c) +
                            "] não é objeto"));
                }
                const auto typeField = component.find("type");
                if (!typeField.has_value() || !typeField->isString()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "componente sem 'type' string"));
                }
                const std::string typeName = typeField->asString();

                if (detail::isInternalComponentName(typeName)) {
                    // Representação canônica é "id"/"parent" — tolerado.
                    ENG_WARN("componente interno '{}' ignorado no load",
                             typeName);
                    continue;
                }

                const auto it = entries.find(typeName);
                if (it == entries.end()) {
                    return makeUnexpected(sceneError(
                        StatusCode::NotSupported,
                        "tipo de componente não registrado: '" + typeName +
                            "'"));
                }
                const auto dataField = component.find("data");
                if (!dataField.has_value() || !dataField->isObject()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "componente '" + typeName +
                            "' sem 'data' objeto"));
                }
                const auto emplaced = it->second.decodeAndEmplace(
                    it->second, scene.world(), record.entity, *dataField);
                if (emplaced.isError()) {
                    return makeUnexpected(emplaced.error());
                }
            }
        }
        loaded.push_back(record);
    }

    // Consistência sceneEntityIds ⇄ entities (lista canônica = conteúdo).
    {
        std::vector<SceneEntityId> actual;
        actual.reserve(loaded.size());
        for (const LoadedEntity& record : loaded) {
            actual.push_back(record.id);
        }
        std::sort(actual.begin(), actual.end());
        std::sort(declaredIds.begin(), declaredIds.end());
        if (actual != declaredIds) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "sceneEntityIds diverge do conjunto de entidades"));
        }
    }

    // Parent/child: resolução em segunda passada (pais podem vir depois).
    for (const LoadedEntity& record : loaded) {
        if (!record.hasParent) {
            continue;
        }
        const auto parent = entityOf.find(record.parent);
        if (parent == entityOf.end()) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "parent " + record.parent.toString() +
                    " não existe na cena"));
        }
        if (!scene.attach(record.entity, parent->second)) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "attach de " + record.id.toString() +
                    " sob " + record.parent.toString() +
                    " falhou (ciclo? inválido?)"));
        }
    }

    // LayerMember (evolução P0-5): membros precisam referenciar camadas
    // DEFINIDAS — sem fallback silencioso. Validação após o load
    // das definições e das entidades.
    {
        bool orphan = false;
        std::string orphanLayer;
        scene.world().each<LayerMember>(
            [&](eng::ecs::Entity /*e*/, const LayerMember& member) {
                if (!scene.layers().has(member.layer) && !orphan) {
                    orphan = true;
                    orphanLayer = member.layer;
                }
            });
        if (orphan) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError,
                "LayerMember referencia camada não definida: '" +
                    orphanLayer + "'"));
        }
    }

    // Links (evolução P0-5, ADR-051): tipos primeiro, entradas em ordem de
    // arquivo (== ordem de criação no save). Pontas por uuid — ausente é
    // ParseError, igual ao parent.
    const auto linksField = root.find("links");
    if (linksField.has_value()) {
        if (!linksField->isObject()) {
            return makeUnexpected(sceneError(
                StatusCode::ParseError, "'links' não é objeto"));
        }
        const auto linkTypes = linksField->find("linkTypes");
        if (linkTypes.has_value()) {
            if (!linkTypes->isArray()) {
                return makeUnexpected(sceneError(
                    StatusCode::ParseError,
                    "links.linkTypes não é array"));
            }
            for (std::size_t i = 0; i < linkTypes->size(); ++i) {
                const eng::serial::JsonValue type = linkTypes->at(i);
                if (!type.isObject()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "linkTypes[" + std::to_string(i) +
                            "] não é objeto"));
                }
                const auto typeName = type.find("type");
                if (!typeName.has_value() || !typeName->isString()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "linkTypes[" + std::to_string(i) +
                            "] sem 'type' string"));
                }
                const std::string name = typeName->asString();
                LinkTypeFlags flags;
                const auto hierarchical = type.find("hierarchical");
                if (hierarchical.has_value()) {
                    if (!hierarchical->isBool()) {
                        return makeUnexpected(sceneError(
                            StatusCode::ParseError,
                            "linkTypes[" + std::to_string(i) +
                                "] ('" + name +
                                "') 'hierarchical' não é bool"));
                    }
                    flags.hierarchical = hierarchical->asBool();
                }
                if (scene.links().hasType(name)) {
                    const LinkTypeFlags* existing =
                        scene.links().typeFlags(name);
                    if (existing == nullptr ||
                        existing->hierarchical != flags.hierarchical) {
                        return makeUnexpected(sceneError(
                            StatusCode::ParseError,
                            "tipo de link '" + name +
                                "' já registrado com flags diferentes"));
                    }
                } else {
                    const auto registered =
                        scene.links().registerType(name, flags);
                    if (registered.isError()) {
                        return makeUnexpected(registered.error());
                    }
                }
            }
        }
        const auto linkEntries = linksField->find("entries");
        if (linkEntries.has_value()) {
            if (!linkEntries->isArray()) {
                return makeUnexpected(sceneError(
                    StatusCode::ParseError,
                    "links.entries não é array"));
            }
            for (std::size_t i = 0; i < linkEntries->size(); ++i) {
                const eng::serial::JsonValue entry = linkEntries->at(i);
                if (!entry.isObject()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "links.entries[" + std::to_string(i) +
                            "] não é objeto"));
                }
                const auto typeName = entry.find("type");
                if (!typeName.has_value() || !typeName->isString()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "links.entries[" + std::to_string(i) +
                            "] sem 'type' string"));
                }
                const auto fromField = entry.find("from");
                if (!fromField.has_value() || !fromField->isString()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "links.entries[" + std::to_string(i) +
                            "] sem 'from' string"));
                }
                const auto toField = entry.find("to");
                if (!toField.has_value() || !toField->isString()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "links.entries[" + std::to_string(i) +
                            "] sem 'to' string"));
                }
                auto fromId =
                    SceneEntityId::fromString(fromField->asString());
                if (fromId.isError()) {
                    return makeUnexpected(fromId.error());
                }
                auto toId = SceneEntityId::fromString(toField->asString());
                if (toId.isError()) {
                    return makeUnexpected(toId.error());
                }
                const auto from = entityOf.find(fromId.value());
                if (from == entityOf.end()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "links.entries[" + std::to_string(i) +
                            "] from " + fromId.value().toString() +
                            " não existe na cena"));
                }
                const auto to = entityOf.find(toId.value());
                if (to == entityOf.end()) {
                    return makeUnexpected(sceneError(
                        StatusCode::ParseError,
                        "links.entries[" + std::to_string(i) +
                            "] to " + toId.value().toString() +
                            " não existe na cena"));
                }
                const auto created = scene.createLink(
                    typeName->asString(), from->second, to->second);
                if (created.isError()) {
                    return makeUnexpected(created.error());
                }
            }
        }
    }
    return {};
}

} // namespace eng::scene
