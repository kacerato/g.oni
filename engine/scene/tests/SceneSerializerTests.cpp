#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "eng/assets/Assets.hpp"
#include "eng/log/Log.hpp"
#include "eng/math/Transform.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/scene/SceneIdentity.hpp"
#include "eng/scene/SceneSerializer.hpp"
#include "eng/serial/Serial.hpp"

// =============================================================================
// Componentes de teste (registro em inicialização estática — ADR-033)
// =============================================================================

namespace {

/// Componente com referências de ASSET e de ENTIDADE — exercita os codecs
/// de campo por nome de tipo (UUID como string; ADR-033).
struct MeshRef {
    eng::assets::AssetId mesh;
    eng::scene::SceneEntityId bindTo;
    std::string slotName;
    float weight = 1.0f;
};

ENG_REFLECT_BEGIN(MeshRef)
    ENG_REFLECT_FIELD_AS(mesh, "eng::assets::AssetId")
    ENG_REFLECT_FIELD_AS(bindTo, "eng::scene::SceneEntityId")
    ENG_REFLECT_FIELD(slotName)
    ENG_REFLECT_FIELD(weight)
ENG_REFLECT_END()

const bool meshRefRegistered = [] {
    (void)eng::scene::SceneSerializer::registerComponentType<MeshRef>(
        "MeshRef");
    return true;
}();

ENG_LOG_CATEGORY("runtime");

/// Sink de captura para validar warnings da camada de composição.
class CapturingSink final : public eng::log::LogSink {
public:
    void write(eng::log::LogLevel level, std::string_view category,
               std::string_view message) override
    {
        if (level >= eng::log::LogLevel::Warn) {
            lines_.emplace_back(category);
            lines_.back() += ": ";
            lines_.back().append(message);
        }
    }

    [[nodiscard]] bool contains(std::string_view needle) const
    {
        for (const std::string& line : lines_) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> lines_;
};

} // namespace

// =============================================================================
// Round-trips (critérios B/D da missão)
// =============================================================================

TEST_CASE("scene-serial: cena vazia round-trip", "[scene][serial]")
{
    eng::scene::Scene scene;

    const auto saved = eng::scene::SceneSerializer::save(scene);
    REQUIRE(saved.ok());
    // Evolução P0-5: a seção "layers" é SEMPRE emitida (defaults
    // GAME/SUBGAME incluídos) — compatível nos dois sentidos (o parser da
    // PRÉ-P0-5 ignora chaves de topo desconhecidas).
    CHECK(saved.value() ==
          "{\"entities\":[],\"formatVersion\":1,\"layers\":["
          "{\"name\":\"GAME\",\"physics\":true,\"render\":true,"
          "\"timeScale\":1.0,\"update\":true},"
          "{\"name\":\"SUBGAME\",\"physics\":true,\"render\":true,"
          "\"timeScale\":1.0,\"update\":true}],\"sceneEntityIds\":[]}");

    eng::scene::Scene clone;
    const auto loaded = eng::scene::SceneSerializer::load(clone, saved.value());
    REQUIRE(loaded.ok());
    CHECK(clone.nodeCount() == 0);

    const auto resaved = eng::scene::SceneSerializer::save(clone);
    REQUIRE(resaved.ok());
    CHECK(resaved.value() == saved.value()); // bytes idênticos
}

TEST_CASE("scene-serial: entidade solitária com transform", "[scene][serial]")
{
    eng::scene::Scene scene;
    const auto node = scene.createNode();
    scene.localTransform(node)->position = {1.5f, -2.0f, 0.25f};
    scene.localTransform(node)->scale = {2.0f, 2.0f, 2.0f};

    const auto saved = eng::scene::SceneSerializer::save(scene);
    REQUIRE(saved.ok());

    // Estrutura: 1 entidade, sem pai, com Transform
    const auto parsed = eng::serial::parseJson(saved.value());
    REQUIRE(parsed.ok());
    const auto entities = parsed.value().find("entities");
    REQUIRE(entities.has_value());
    REQUIRE(entities->size() == 1);
    const auto entity = entities->at(0);
    CHECK(entity.find("id").has_value());
    CHECK(entity.find("parent")->isNull());
    const auto components = entity.find("components");
    REQUIRE(components.has_value());
    REQUIRE(components->size() == 1);
    CHECK(components->at(0).find("type")->asString() ==
          "eng::math::Transform");

    eng::scene::Scene clone;
    REQUIRE(eng::scene::SceneSerializer::load(clone, saved.value()).ok());
    REQUIRE(clone.nodeCount() == 1);

    // Transform preservado EXATAMENTE (floats via shortest round-trip)
    clone.world().each<eng::math::Transform>(
        [&](eng::ecs::Entity, const eng::math::Transform& transform) {
            CHECK(transform.position.x == 1.5f);
            CHECK(transform.position.y == -2.0f);
            CHECK(transform.position.z == 0.25f);
            CHECK(transform.scale.x == 2.0f);
        });

    const auto resaved = eng::scene::SceneSerializer::save(clone);
    REQUIRE(resaved.ok());
    CHECK(resaved.value() == saved.value()); // determinismo byte-a-byte
}

TEST_CASE("scene-serial: hierarquia parent/child", "[scene][serial]")
{
    eng::scene::Scene scene;
    const auto root = scene.createNode();
    const auto childA = scene.createNode();
    const auto childB = scene.createNode();
    const auto grandChild = scene.createNode();
    REQUIRE(scene.attach(childA, root));
    REQUIRE(scene.attach(childB, root));
    REQUIRE(scene.attach(grandChild, childA));

    const auto saved = eng::scene::SceneSerializer::save(scene);
    REQUIRE(saved.ok());

    // Identidades estáveis da cena ORIGINAL (para verificação na clone —
    // handles de runtime NÃO atravessam o round-trip; ids sim).
    const auto idOf = [&](eng::ecs::Entity e) {
        return scene.world().get<eng::scene::SceneIdentity>(e)->id;
    };
    const eng::scene::SceneEntityId rootId = idOf(root);
    const eng::scene::SceneEntityId childAId = idOf(childA);
    const eng::scene::SceneEntityId grandId = idOf(grandChild);

    eng::scene::Scene clone;
    REQUIRE(eng::scene::SceneSerializer::load(clone, saved.value()).ok());
    REQUIRE(clone.nodeCount() == 4);

    // Mapa id → entidade na CLONE (verificação por identidade)
    std::unordered_map<eng::scene::SceneEntityId, eng::ecs::Entity> entityOf;
    clone.world().each<eng::scene::SceneIdentity>(
        [&](eng::ecs::Entity e, const eng::scene::SceneIdentity& identity) {
            entityOf[identity.id] = e;
        });
    REQUIRE(entityOf.size() == 4);

    // root: sem pai, 2 filhos
    const auto rootClone = entityOf[rootId];
    CHECK(clone.parentOf(rootClone) == eng::scene::kNoEntity);
    CHECK(clone.childCount(rootClone) == 2);

    // grandChild: cadeia grandChild → childA → root (profundidade 2)
    std::size_t depth = 0;
    for (auto node = entityOf[grandId];
         clone.parentOf(node) != eng::scene::kNoEntity;
         node = clone.parentOf(node)) {
        ++depth;
    }
    CHECK(depth == 2);

    // O pai do grandChild na clone é o childA (por identidade)
    const auto parentOfGrand = clone.parentOf(entityOf[grandId]);
    const auto* parentId =
        clone.world().get<eng::scene::SceneIdentity>(parentOfGrand);
    REQUIRE(parentId != nullptr);
    CHECK(parentId->id == childAId);

    const auto resaved = eng::scene::SceneSerializer::save(clone);
    REQUIRE(resaved.ok());
    CHECK(resaved.value() == saved.value());
}

TEST_CASE("scene-serial: componente com AssetId e SceneEntityId", "[scene][serial]")
{
    eng::scene::Scene scene;
    const auto target = scene.createNode();
    const auto node = scene.createNode();

    // Primeiro save: atribui SceneEntityId a todos os nós
    const auto pre = eng::scene::SceneSerializer::save(scene);
    REQUIRE(pre.ok());

    const eng::assets::AssetId meshId = eng::assets::AssetId::generate();
    const eng::scene::SceneEntityId targetId =
        scene.world().get<eng::scene::SceneIdentity>(target)->id;

    MeshRef ref;
    ref.mesh = meshId;
    // referência por IDENTIDADE de entidade (não por handle de runtime)
    ref.bindTo = targetId;
    ref.slotName = "slot-difuso";
    ref.weight = 0.75f;
    (void)scene.world().emplace<MeshRef>(node, ref);

    const auto saved = eng::scene::SceneSerializer::save(scene);
    REQUIRE(saved.ok());

    // AssetId serializado como STRING UUID (missão §2.7)
    CHECK(saved.value().find(meshId.toString()) != std::string::npos);
    CHECK(saved.value().find("\"slotName\":\"slot-difuso\"") !=
          std::string::npos);

    eng::scene::Scene clone;
    REQUIRE(eng::scene::SceneSerializer::load(clone, saved.value()).ok());

    bool found = false;
    clone.world().each<MeshRef>(
        [&](eng::ecs::Entity, const MeshRef& loaded) {
            found = true;
            CHECK(loaded.mesh == meshId);
            CHECK(loaded.bindTo == targetId); // identidade atravessou
            CHECK(loaded.slotName == "slot-difuso");
            CHECK(loaded.weight == 0.75f);
        });
    CHECK(found);

    const auto resaved = eng::scene::SceneSerializer::save(clone);
    REQUIRE(resaved.ok());
    CHECK(resaved.value() == saved.value());
}

TEST_CASE("scene-serial: referência de asset quebrada NÃO impede o load",
          "[scene][serial]")
{
    eng::scene::Scene scene;
    const auto node = scene.createNode();

    MeshRef ref;
    ref.mesh = eng::assets::AssetId::generate(); // NÃO está em registry nenhum
    ref.slotName = "perdido";
    (void)scene.world().emplace<MeshRef>(node, ref);

    const auto saved = eng::scene::SceneSerializer::save(scene);
    REQUIRE(saved.ok());

    // load: só valida FORMA — referência quebrada não é erro de parse
    eng::scene::Scene clone;
    REQUIRE(eng::scene::SceneSerializer::load(clone, saved.value()).ok());
    CHECK(clone.nodeCount() == 1);

    // A checagem contra o registry é da CAMADA DE COMPOSIÇÃO:
    // runtime/editor que possui scene+assets reporta por eng::log.
    eng::assets::AssetRegistry emptyRegistry; // nada catalogado

    CapturingSink sink;
    eng::log::Logger::get().addSink(sink);
    clone.world().each<MeshRef>(
        [&](eng::ecs::Entity, const MeshRef& loaded) {
            if (emptyRegistry.find(loaded.mesh) == nullptr) {
                ENG_WARN("asset {} referenciado mas ausente do registry",
                         loaded.mesh.toString());
            }
        });
    eng::log::Logger::get().removeSink(sink);
    eng::log::Logger::get().flush();

    CHECK(sink.contains("referenciado mas ausente do registry"));
}

// =============================================================================
// Erros claros (corrupção tolerada sem crash/UB)
// =============================================================================

TEST_CASE("scene-serial: JSON ruim e formato futuro → erros claros",
          "[scene][serial]")
{
    using eng::core::StatusCode;

    eng::scene::Scene scene;

    for (const std::string_view bad :
         {"", "{", "[]", "{\"formatVersion\":1}",
          "{\"formatVersion\":99,\"entities\":[]}",
          "{\"formatVersion\":1,\"entities\":{}}",
          "{\"formatVersion\":1,\"entities\":[{\"id\":\"nao-uuid\"}]}",
          "{\"formatVersion\":1,\"entities\":[{\"id\":\"00112233-4455-4677-"
          "8899-aabbccddeeff\",\"parent\":\"ffff0000-0000-4000-8000-"
          "000000000000\"}]}",
          "{\"formatVersion\":1,\"sceneEntityIds\":[],\"entities\":[{\"id\":"
          "\"00112233-4455-4677-8899-aabbccddeeff\"}]}",
          "{\"formatVersion\":1,\"entities\":[{\"id\":\"00112233-4455-4677-"
          "8899-aabbccddeeff\",\"components\":[{\"type\":\"Desconhecido\","
          "\"data\":{}}]}]}"}) {
        const auto r = eng::scene::SceneSerializer::load(scene, bad);
        INFO("input: " << bad);
        CHECK(r.isError());
    }

    // formato futuro → NotSupported específico
    {
        const auto r = eng::scene::SceneSerializer::load(
            scene, "{\"formatVersion\":99,\"entities\":[]}");
        REQUIRE(r.isError());
        CHECK(r.error().code == StatusCode::NotSupported);
    }

    // componente desconhecido → NotSupported com o nome
    {
        const auto r = eng::scene::SceneSerializer::load(
            scene,
            "{\"formatVersion\":1,\"sceneEntityIds\":[\"00112233-4455-4677-"
            "8899-aabbccddeeff\"],\"entities\":[{\"id\":\"00112233-4455-"
            "4677-8899-aabbccddeeff\",\"parent\":null,\"components\":"
            "[{\"type\":\"Desconhecido\",\"data\":{}}]}]}");
        REQUIRE(r.isError());
        CHECK(r.error().code == StatusCode::NotSupported);
        CHECK(r.error().message.find("Desconhecido") != std::string::npos);
    }
}

TEST_CASE("scene-serial: componentes internos tolerados com WARN",
          "[scene][serial]")
{
    // Escritor externo vaza SceneIdentity/Hierarchy/WorldMatrix no array
    // de componentes → load tolera (a forma canônica é id/parent).
    eng::scene::Scene scene;
    const auto r = eng::scene::SceneSerializer::load(
        scene,
        "{\"formatVersion\":1,\"sceneEntityIds\":[\"00112233-4455-4677-"
        "8899-aabbccddeeff\"],\"entities\":[{\"id\":\"00112233-4455-4677-"
        "8899-aabbccddeeff\",\"parent\":null,\"components\":"
        "[{\"type\":\"eng::scene::Hierarchy\",\"data\":{}},"
        "{\"type\":\"eng::math::Transform\",\"data\":{\"position\":"
        "{\"x\":1.0,\"y\":2.0,\"z\":3.0},\"rotation\":{\"x\":0.0,\"y\":0.0,"
        "\"z\":0.0,\"w\":1.0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0}}}]}]}");
    REQUIRE(r.ok());
    REQUIRE(scene.nodeCount() == 1);

    bool sawTransform = false;
    scene.world().each<eng::math::Transform>(
        [&](eng::ecs::Entity, const eng::math::Transform& transform) {
            sawTransform = true;
            CHECK(transform.position.x == 1.0f);
        });
    CHECK(sawTransform);
}

TEST_CASE("scene-serial: ids duplicados rejeitados", "[scene][serial]")
{
    eng::scene::Scene scene;
    const auto r = eng::scene::SceneSerializer::load(
        scene,
        "{\"formatVersion\":1,\"sceneEntityIds\":[\"00112233-4455-4677-"
        "8899-aabbccddeeff\",\"00112233-4455-4677-8899-aabbccddeeff\"],"
        "\"entities\":[{\"id\":\"00112233-4455-4677-8899-aabbccddeeff\","
        "\"parent\":null,\"components\":[]},{\"id\":\"00112233-4455-4677-"
        "8899-aabbccddeeff\",\"parent\":null,\"components\":[]}]}");
    REQUIRE(r.isError());
    CHECK(r.error().code == eng::core::StatusCode::ParseError);
}

TEST_CASE("scene-serial: save atribui identidade estável e estável entre saves",
          "[scene][serial]")
{
    eng::scene::Scene scene;
    const auto node = scene.createNode();

    const auto first = eng::scene::SceneSerializer::save(scene);
    REQUIRE(first.ok());

    // Identidade atribuída no primeiro save persiste no segundo (mesmo id)
    const auto second = eng::scene::SceneSerializer::save(scene);
    REQUIRE(second.ok());
    CHECK(first.value() == second.value());

    const auto* identity =
        scene.world().get<eng::scene::SceneIdentity>(node);
    REQUIRE(identity != nullptr);
    CHECK_FALSE(identity->id.isNil());
    CHECK(first.value().find(identity->id.toString()) != std::string::npos);
}
