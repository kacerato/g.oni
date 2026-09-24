// ComponentContract v2 + hooks + categorias + event bus.
//
// Contratos são APLICAÇÃO DE AUTORIA: add/remove do Inspector recusam com
// erro PRECISO; hooks nativos (luz casa com camada, validação de
// geometria) rodam pelo MESMO caminho que a UI usa. O catálogo único
// alimenta Inspector, NI-Script e estas provas.

#include <cstdio>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "eng/editor/EditorDocument.hpp"
#include "eng/editor/Inspector.hpp"
#include "eng/fs/MemoryFileSystem.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/SceneEvents.hpp"
#include "eng/scene/SceneSerializer.hpp"

namespace {

using namespace eng::editor;  // EditorDocument/Inspector sem qualificação

// Fixture local (espelha a DocFixture do EditorTests.cpp — TUs separados).
struct ContractFixture {
    eng::fs::MemoryFileSystem fsStorage;
    eng::fs::MemoryFileSystem* fs = &fsStorage;
    std::unique_ptr<EditorDocument> doc;
    ContractFixture()
    {
        auto created = EditorDocument::create(fsStorage, eng::fs::Path{"."});
        REQUIRE(created.ok());
        doc = std::move(created.value());
    }

    void withProject() { REQUIRE(doc->newProject("ContractGame").ok()); }
};

// Componente de PROVA (single=true) — registrado apenas neste TU.
struct P47SingleProbe {
    float value = 0.f;
};

ENG_REFLECT_BEGIN(P47SingleProbe)
ENG_REFLECT_FIELD(value)
ENG_REFLECT_END()

const bool p47_single_probe_registered = [] {
    eng::scene::detail::ComponentContract contract;
    contract.single = true;
    contract.category = "Lógica";
    contract.scriptAlias = "probe";
    // Nome = o do reflect (ENG_REFLECT_BEGIN usa o token literal).
    (void)eng::scene::SceneSerializer::registerComponentType<P47SingleProbe>(
        "P47SingleProbe", std::move(contract));
    return true;
}();

} // namespace

// =============================================================================
// Contratos no add (requires / conflicts / single)
// =============================================================================

TEST_CASE("p47: contrato — CharacterBody exige Collider (add recusa com erro)",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    auto entity = f.doc->createEntity("Heroi", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    // Sem Collider: recusa com o nome do requisito na mensagem.
    auto denied = f.doc->addComponent(entity.value(),
                                      "eng::physics::CharacterBody");
    REQUIRE(denied.isError());
    INFO(denied.error().message);
    CHECK(denied.error().message.find("eng::physics::Collider")
          != std::string::npos);
    CHECK(denied.error().message.find("exige") != std::string::npos);
    // E NADA foi anexado (o add é atômico — sem estado parcial).
    auto present = eng::editor::Inspector::componentsOf(
        *f.doc->sceneInFocus(), entity.value());
    CHECK(std::find(present.begin(), present.end(),
                    "eng::physics::CharacterBody") == present.end());

    // Collider primeiro → CharacterBody entra.
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::physics::CharacterBody")
                .ok());
}

TEST_CASE("p47: contrato — conflito RigidBody × CharacterBody é bidirecional",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    auto entity = f.doc->createEntity("Corpo", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::physics::CharacterBody")
                .ok());

    // CharacterBody presente → RigidBody recusado (conflito).
    auto deniedRb = f.doc->addComponent(entity.value(),
                                        "eng::physics::RigidBody");
    REQUIRE(deniedRb.isError());
    INFO(deniedRb.error().message);
    CHECK(deniedRb.error().message.find("conflita") != std::string::npos);

    // E no outro sentido: nó novo com RigidBody recusa CharacterBody.
    auto entity2 = f.doc->createEntity("Corpo2", eng::scene::kNoEntity);
    REQUIRE(entity2.ok());
    REQUIRE(f.doc->addComponent(entity2.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(entity2.value(),
                                "eng::physics::RigidBody")
                .ok());
    auto deniedCb = f.doc->addComponent(entity2.value(),
                                        "eng::physics::CharacterBody");
    REQUIRE(deniedCb.isError());
    CHECK(deniedCb.error().message.find("conflita") != std::string::npos);
}

TEST_CASE("p47: single — segunda instância na cena é recusada", "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    auto a = f.doc->createEntity("A", eng::scene::kNoEntity);
    auto b = f.doc->createEntity("B", eng::scene::kNoEntity);
    REQUIRE(a.ok());
    REQUIRE(b.ok());

    const std::string probe = "P47SingleProbe";
    REQUIRE(f.doc->addComponent(a.value(), probe).ok());

    auto denied = f.doc->addComponent(b.value(), probe);
    REQUIRE(denied.isError());
    INFO(denied.error().message);
    CHECK(denied.error().message.find("único na cena") != std::string::npos);

    // Removido o único, o add volta a ser possível.
    REQUIRE(f.doc->removeComponent(a.value(), probe).ok());
    REQUIRE(f.doc->addComponent(b.value(), probe).ok());
}

TEST_CASE("p47: remoção com DEPENDENTE recusa e nomeia quem exige",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    auto entity = f.doc->createEntity("Corpo", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::physics::CharacterBody")
                .ok());

    // Collider sustenta o CharacterBody: remoção recusa com o dependente.
    auto denied = f.doc->removeComponent(entity.value(),
                                         "eng::physics::Collider");
    REQUIRE(denied.isError());
    INFO(denied.error().message);
    CHECK(denied.error().message.find("CharacterBody") != std::string::npos);
    CHECK(denied.error().message.find("exige") != std::string::npos);
    // O Collider CONTINUA presente (nada sumiu de surpresa).
    auto present = eng::editor::Inspector::componentsOf(
        *f.doc->sceneInFocus(), entity.value());
    CHECK(std::find(present.begin(), present.end(),
                    "eng::physics::Collider") != present.end());

    // Ordem certa: dependente primeiro, dependência depois.
    REQUIRE(f.doc->removeComponent(entity.value(),
                                   "eng::physics::CharacterBody")
                .ok());
    REQUIRE(f.doc->removeComponent(entity.value(),
                                   "eng::physics::Collider")
                .ok());
}

// =============================================================================
// Contrato → UI: categorias (ordem fixa) e hints de dependência
// =============================================================================

TEST_CASE("p47: catalogEntries — categorias do contrato em ordem fixa",
          "[editor][p47]")
{
    const auto entries = eng::editor::Inspector::catalogEntries();
    REQUIRE_FALSE(entries.empty());

    // Ordem de categoria NUNCA decresce (Transform → Render → Física → …).
    auto orderOf = [](const std::string& category) {
        static const std::vector<std::string> kOrder = {
            "Transform", "Render", "Física", "Lógica",
            "Áudio", "Câmera", "FX", "Outros",
        };
        return static_cast<std::size_t>(
            std::find(kOrder.begin(), kOrder.end(), category)
            - kOrder.begin());
    };
    for (std::size_t i = 1; i < entries.size(); ++i) {
        CAPTURE(entries[i - 1].name, entries[i].name);
        CHECK(orderOf(entries[i - 1].category)
              <= orderOf(entries[i].category));
    }

    // Categorias esperadas por componente (fonte = contrato registrado).
    auto categoryOf = [&](const std::string& name) -> std::string {
        for (const auto& entry : entries) {
            if (entry.name == name) {
                return entry.category;
            }
        }
        return "";
    };
    CHECK(categoryOf("eng::editor::SpriteData") == "Render");
    CHECK(categoryOf("eng::physics::RigidBody") == "Física");
    CHECK(categoryOf("eng::editor::NiScriptComponent") == "Lógica");
    CHECK(categoryOf("eng::editor::AudioSource") == "Áudio");
    CHECK(categoryOf("eng::tick::CameraData") == "Câmera");
    CHECK(categoryOf("eng::render::Light2D") == "FX");
    CHECK(categoryOf("eng::particles::ParticleEmitter") == "FX");
    CHECK(categoryOf("eng::math::Transform") == "Transform");
}

TEST_CASE("p47: hint de dependência vem do CONTRATO (fonte única)",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("X", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    auto addable = f.doc->addableComponents(entity.value());
    auto hintOf = [&](const std::string& name) -> std::string {
        for (const auto& item : addable) {
            if (item.name == name) {
                return item.dependency;
            }
        }
        return "";
    };
    const std::string hint = hintOf("eng::physics::CharacterBody");
    CHECK(hint.find("eng::physics::Collider") != std::string::npos);
}

// =============================================================================
// Hooks: onValidate (geometria do Collider) + onAttach (luz)
// =============================================================================

TEST_CASE("p47: onValidate — radius negativo rejeitado COM rollback",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    auto entity = f.doc->createEntity("Caixa", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider").ok());

    // Escrita válida: entra.
    REQUIRE(f.doc
                ->setInspectorField(entity.value(), "eng::physics::Collider",
                                    "radius", "2")
                .ok());

    // Escrita INVÁLIDA: rejeitada com erro preciso e ROLLBACK pro valor
    // anterior (2) — o componente nunca fica num estado quebrado.
    auto denied = f.doc->setInspectorField(
        entity.value(), "eng::physics::Collider", "radius", "-1");
    REQUIRE(denied.isError());
    INFO(denied.error().message);
    CHECK(denied.error().message.find("radius") != std::string::npos);
    auto radius = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::physics::Collider",
        "radius");
    REQUIRE(radius.ok());
    CHECK(radius.value() == "2");
}

TEST_CASE("p47: onAttach da luz casa com a camada via caminho do Inspector",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    // Sprite em camada "UI" (o hook conta sprites LIT por camada).
    auto sprite = f.doc->createEntity("Sprite", eng::scene::kNoEntity);
    REQUIRE(sprite.ok());
    REQUIRE(f.doc->addComponent(sprite.value(), "eng::editor::SpriteData").ok());
    REQUIRE(f.doc->addComponent(sprite.value(), "eng::scene::LayerMember").ok());
    REQUIRE(f.doc
                ->setInspectorField(sprite.value(), "eng::scene::LayerMember",
                                    "layer", "UI")
                .ok());

    // Luz anexada pelo MESMO caminho da UI (EditorDocument::addComponent →
    // hook onAttach) herda a camada dominante.
    auto light = f.doc->createEntity("Luz", eng::scene::kNoEntity);
    REQUIRE(light.ok());
    REQUIRE(f.doc->addComponent(light.value(), "eng::render::Light2D").ok());
    auto layer = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), light.value(), "eng::render::Light2D", "layer");
    REQUIRE(layer.ok());
    CHECK(layer.value() == "UI");
}

// =============================================================================
// Play: validação de contratos é a última linha de defesa
// =============================================================================

TEST_CASE("p47: play() recusa cena com Collider inválido (erro com nó)",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    auto entity = f.doc->createEntity("Quebrado", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider").ok());

    // Corrompe POR BAIXO do Inspector (caminho externo — hand-edit/bug).
    auto* collider =
        f.doc->sceneInFocus()->world().get<eng::physics::Collider>(
            entity.value());
    REQUIRE(collider != nullptr);
    collider->radius = -0.5f;

    auto started = f.doc->play();
    REQUIRE(started.isError());
    INFO(started.error().message);
    CHECK(started.error().message.find("eng::physics::Collider")
          != std::string::npos);
    CHECK(started.error().message.find("Play") != std::string::npos);
    f.doc->stop();
}

// =============================================================================
// Bridge NI-Script: on_hit do script roda quando a física publica
// =============================================================================

TEST_CASE("p47: bridge NI-Script — up on_hit roda no self atingido",
          "[editor][p47][niscript]")
{
    ContractFixture f;
    f.withProject();

    // Jogador com Collider + script on_hit (move cru: 0,10 — teleporte
    // documentado; serve de FLAG observável do handler).
    auto player = f.doc->createEntity("Jogador", eng::scene::kNoEntity);
    REQUIRE(player.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    const char* source = "up on_hit:\n"
                         "    move(0, 10)\n"
                         "stop\n";
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    // Parede sobreposta (dois colliders estáticos → contato → on_hit).
    auto wall = f.doc->createEntity("Parede", eng::scene::kNoEntity);
    REQUIRE(wall.ok());
    REQUIRE(f.doc->addComponent(wall.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::math::Transform",
                                    "position.x", "0.5")
                .ok());

    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f); // física publica; bridge roda on_hit

    // Leitura pelo leitor TRADUZIDO do documento (edit→runtime —
    // inspectorFields aplica toFocus internamente).
    const auto fields = f.doc->inspectorFields(player.value(),
                                               "eng::math::Transform");
    float py = 0.f;
    bool foundY = false;
    for (const auto& field : fields) {
        if (field.path == "position.y") {
            py = std::stof(field.value);
            foundY = true;
        }
    }
    REQUIRE(foundY);
    INFO("player.y = " << py);
    CHECK(py == Catch::Approx(10.f).margin(0.1f));
    f.doc->stop();
}

// =============================================================================
// Gizmos v3: setas reais + anti-sobreposição
// =============================================================================

TEST_CASE("p47: gizmo MOVE — centro DIAMANTE e 4 setas triangulares para fora",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Alvo", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->select(entity.value()).ok());
    f.doc->setTool(eng::editor::EditorTool::Move);
    REQUIRE(f.doc->tool() == eng::editor::EditorTool::Move);

    const auto draw = f.doc->gizmoDraw(nullptr);
    REQUIRE(draw.triangles.size() == 4); // setas REAIS (não quadrados)
    // Centro é DIAMANTE: o único quad, rotação a 45° (π/4).
    REQUIRE(draw.quads.size() == 1);
    CHECK(draw.quads[0].rotation == Catch::Approx(0.7853981f).margin(1e-4f));

    const auto& bounds = f.doc->selectionBounds(nullptr);
    REQUIRE(bounds.valid);
    const float axisLen = eng::editor::TransformGizmo::axisPx(
                              f.doc->viewport().uiScale())
                          / f.doc->viewport().camera().zoom;
    // Setas nos QUATRO lados, na ponta do eixo, apontando PARA FORA
    // (+X: rotação 0; −X: π; +Y: π/2; −Y: 3π/2 — Y de MUNDO para cima).
    bool right = false;
    bool left = false;
    bool up = false;
    bool down = false;
    for (const auto& tri : draw.triangles) {
        if (tri.worldX > bounds.worldX + axisLen * 0.9f
            && tri.worldY == Catch::Approx(bounds.worldY).margin(1e-4f)) {
            right = true;
            CHECK(tri.rotation == Catch::Approx(0.f).margin(1e-4f));
        }
        if (tri.worldX < bounds.worldX - axisLen * 0.9f
            && tri.worldY == Catch::Approx(bounds.worldY).margin(1e-4f)) {
            left = true;
        }
        if (tri.worldY > bounds.worldY + axisLen * 0.9f) {
            up = true;
        }
        if (tri.worldY < bounds.worldY - axisLen * 0.9f) {
            down = true;
        }
    }
    CHECK(right);
    CHECK(left);
    CHECK(up);
    CHECK(down);

    // Hastes terminam na BASE do triângulo (nunca através dele).
    const float halfLen = eng::editor::TransformGizmo::headTriLenPx(
                              f.doc->viewport().uiScale())
                          * 0.5f / f.doc->viewport().camera().zoom;
    for (const auto& segment : draw.segments) {
        if (segment.y0 == Catch::Approx(bounds.worldY).margin(1e-4f)
            && segment.y1 == Catch::Approx(bounds.worldY).margin(1e-4f)) {
            const float end = std::max(segment.x0, segment.x1);
            CHECK(end <= bounds.worldX + axisLen - halfLen + 1e-3f);
        }
    }
}

TEST_CASE("p47: gizmo SCALE — clamp anti-sobreposição em bounds minúsculo",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Pequeno", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->select(entity.value()).ok());
    f.doc->setTool(eng::editor::EditorTool::Scale);

    eng::editor::TransformGizmo gizmo;
    const auto bounds = f.doc->selectionBounds(nullptr);
    REQUIRE(bounds.valid);

    // Bounds MINÚSCULO (meio-pixel de zoom): sem clamp os 8 handles
    // colapsariam sobre o centro (o "cubo" do round 6).
    auto points = gizmo.scaleHandlePoints(f.doc->viewport(), bounds);
    const auto minDist = [&](const std::pair<float, float>& point) {
        const float dx = f.doc->viewport().worldToScreenX(point.first)
                         - f.doc->viewport().worldToScreenX(bounds.worldX);
        const float dy = f.doc->viewport().worldToScreenY(point.second)
                         - f.doc->viewport().worldToScreenY(bounds.worldY);
        return std::sqrt(dx * dx + dy * dy);
    };
    const float minCorner =
        eng::editor::TransformGizmo::kMinCornerCenterDp
        * f.doc->viewport().uiScale();
    CHECK(minDist(points.ne) >= minCorner - 1e-3f);
    CHECK(minDist(points.sw) >= minCorner - 1e-3f);
    CHECK(minDist(points.e) >=
          eng::editor::TransformGizmo::kMinEdgeCenterDp
              * f.doc->viewport().uiScale() - 1e-3f);

    // O HIT usa a MESMA fonte: tocar no canto CLAMPADO acerta ScaleNE
    // (gizmoDragBegin é o caminho do JNI — encerra o drag de seguida).
    const float nePx = f.doc->viewport().worldToScreenX(points.ne.first);
    const float nePy = f.doc->viewport().worldToScreenY(points.ne.second);
    CHECK(f.doc->gizmoDragBegin(nePx, nePy, nullptr)
          == eng::editor::GizmoHandle::ScaleNE);
    f.doc->gizmoDragEnd();

    // Bounds GRANDE (zoom alto): clamp não interfere — o canto fica MUITO
    // além do mínimo (distância natural > 52dp).
    f.doc->viewport().camera().zoom = 400.f;
    REQUIRE(f.doc->select(entity.value()).ok());
    const auto big = f.doc->selectionBounds(nullptr);
    auto bigPoints = gizmo.scaleHandlePoints(f.doc->viewport(), big);
    const float bigCornerPx =
        f.doc->viewport().worldToScreenX(bigPoints.ne.first)
        - f.doc->viewport().worldToScreenX(big.worldX);
    CHECK(bigCornerPx > minCorner); // distância natural > mínimo
    f.doc->viewport().camera().zoom = 48.f;
}

// =============================================================================
// Event bus da cena + eventos de física (on_hit / triggers)
// =============================================================================

TEST_CASE("p47: eventos de física — on_hit nos dois sentidos com normal oposta",
          "[physics][p47]")
{
    eng::scene::Scene scene;

    auto a = scene.createNode();
    auto b = scene.createNode();
    REQUIRE(scene.isNode(a));
    REQUIRE(scene.isNode(b));

    // Duas esferas sobrepostas (raio 0.5, centros a 0.5 de distância).
    REQUIRE(scene.world().emplace<eng::physics::Collider>(
        a, eng::physics::Collider{}) != nullptr);
    REQUIRE(scene.world().emplace<eng::physics::Collider>(
        b, eng::physics::Collider{}) != nullptr);
    auto* ta = scene.localTransform(a);
    auto* tb = scene.localTransform(b);
    REQUIRE(ta != nullptr);
    REQUIRE(tb != nullptr);
    ta->position = eng::math::Vec3{0.f, 0.f, 0.f};
    tb->position = eng::math::Vec3{0.5f, 0.f, 0.f};

    std::vector<eng::scene::HitEvent> hits;
    auto subscription = scene.events().subscribe<eng::scene::HitEvent>(
        [&](const eng::scene::HitEvent& event) { hits.push_back(event); });

    eng::physics::PhysicsWorld world;
    world.step(scene, 1.f / 60.f);

    // on_hit é publicado nos DOIS sentidos (self/other trocados).
    REQUIRE(hits.size() == 2);
    const bool ab = hits[0].self == a && hits[1].self == b;
    const bool ba = hits[0].self == b && hits[1].self == a;
    CHECK((ab || ba));
    // Normais opostas (mesma linha de contato).
    CHECK(hits[0].nx == Catch::Approx(-hits[1].nx).margin(1e-5f));
    CHECK(hits[0].ny == Catch::Approx(-hits[1].ny).margin(1e-5f));
}

TEST_CASE("p47: eventos de trigger — on_enter único, on_exit ao separar",
          "[physics][p47]")
{
    eng::scene::Scene scene;
    auto a = scene.createNode();
    auto b = scene.createNode();
    eng::physics::Collider triggerA{};
    triggerA.isTrigger = true;
    eng::physics::Collider triggerB{};
    triggerB.isTrigger = true;
    REQUIRE(scene.world().emplace<eng::physics::Collider>(a, triggerA)
            != nullptr);
    REQUIRE(scene.world().emplace<eng::physics::Collider>(b, triggerB)
            != nullptr);
    auto* ta = scene.localTransform(a);
    auto* tb = scene.localTransform(b);
    REQUIRE(ta != nullptr);
    REQUIRE(tb != nullptr);
    ta->position = eng::math::Vec3{0.f, 0.f, 0.f};
    tb->position = eng::math::Vec3{0.2f, 0.f, 0.f}; // sobrepostos

    int entered = 0;
    int exited = 0;
    auto subEnter = scene.events().subscribe<eng::scene::TriggerEvent>(
        [&](const eng::scene::TriggerEvent& event) {
            if (event.entered) {
                ++entered;
            } else {
                ++exited;
            }
        });

    eng::physics::PhysicsWorld world;
    world.step(scene, 1.f / 60.f);
    CHECK(entered == 2); // nos dois sentidos (A→B e B→A)
    CHECK(exited == 0);

    // Continuam sobrepostos: SEM novo on_enter (evento é transição).
    world.step(scene, 1.f / 60.f);
    CHECK(entered == 2);

    // Separa (além dos raios 0.5+0.5): on_exit nos dois sentidos.
    tb->position = eng::math::Vec3{3.f, 0.f, 0.f};
    world.step(scene, 1.f / 60.f);
    CHECK(entered == 2);
    CHECK(exited == 2);
}

TEST_CASE("p47: bus da cena — publish determinístico e contagem de inscritos",
          "[scene][p47]")
{
    eng::scene::Scene scene;
    CHECK(scene.events().subscriberCount<eng::scene::HitEvent>() == 0);

    int count = 0;
    {
        auto sub = scene.events().subscribe<eng::scene::HitEvent>(
            [&](const eng::scene::HitEvent&) { ++count; });
        CHECK(scene.events().subscriberCount<eng::scene::HitEvent>() == 1);
        eng::scene::HitEvent event;
        scene.events().publish(event);
        scene.events().publish(event);
        CHECK(count == 2);
        sub.unsubscribe();
    }
    CHECK(scene.events().subscriberCount<eng::scene::HitEvent>() == 0);
    eng::scene::HitEvent event;
    scene.events().publish(event); // sem inscritos: no-op, sem crash
    CHECK(count == 2);
}

// =============================================================================
// Camera2D: rotação, moldura no editor, persistência
// =============================================================================

TEST_CASE("p47: viewport rotation — round-trip mundo↔tela a 90°",
          "[editor][p47]")
{
    eng::editor::Viewport viewport;
    viewport.setScreenSize(192.f, 96.f);
    auto& camera = viewport.camera();
    camera.posX = 0.f;
    camera.posY = 0.f;
    camera.zoom = 48.f;
    camera.rotation = 3.14159265358979323846f * 0.5f; // 90°

    // Contrato: com a vista a 90°, o +X do MUNDO aparece PARA CIMA.
    const auto [sx, sy] = viewport.worldToScreen(1.f, 0.f);
    CHECK(sx == Catch::Approx(96.f).margin(1e-3f));
    CHECK(sy == Catch::Approx(0.f).margin(1e-3f));

    // Round-trip exato em pontos arbitrários.
    const auto [wx, wy] = viewport.screenToWorld(sx, sy);
    CHECK(wx == Catch::Approx(1.f).margin(1e-3f));
    CHECK(wy == Catch::Approx(0.f).margin(1e-3f));

    // Rotação 0 = caminho reto idêntico ao pré-P4.7.
    camera.rotation = 0.f;
    CHECK(viewport.worldToScreenX(1.f)
          == Catch::Approx(96.f + 48.f).margin(1e-3f));
    CHECK(viewport.worldToScreenY(1.f)
          == Catch::Approx(48.f - 48.f).margin(1e-3f));
}

TEST_CASE("p47: moldura da câmera no editor — vista, rotação e limites",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto cam = f.doc->createEntity("Camera", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "rotationDeg", "90")
                .ok());
    // Limites: o autor VÊ a moldura dos limites no editor.
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "limitsEnabled", "true")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "limitMaxX", "40")
                .ok());

    // O quad da câmera carrega a vista (rotação) + limites para o
    // renderer desenhar as MOLDURAS no viewport (edit mode).
    const auto quads = f.doc->viewport().buildQuads(
        *f.doc->sceneInFocus(), std::nullopt);
    bool found = false;
    for (const auto& quad : quads) {
        if (quad.hasCamera) {
            found = true;
            CHECK(quad.cameraRotation
                  == Catch::Approx(1.5707964f).margin(1e-3f));
            CHECK(quad.cameraLimits);
            CHECK(quad.cameraLimitMaxX == Catch::Approx(40.f));
        }
    }
    CHECK(found);
}

TEST_CASE("p47: CameraData round-trip — campos novos persistem; cena antiga "
          "carrega com defaults",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto cam = f.doc->createEntity("Camera", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "rotationDeg", "90")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "followName", "Player")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "smoothingTime", "0.25")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "limitsEnabled", "true")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "limitMaxY", "30")
                .ok());

    // Round-trip por serialização (save→load em cena NOVA).
    auto snapshot = eng::scene::SceneSerializer::save(*f.doc->sceneInFocus());
    REQUIRE(snapshot.ok());
    eng::scene::Scene reloaded;
    REQUIRE(eng::scene::SceneSerializer::load(reloaded, snapshot.value()).ok());
    const eng::tick::CameraData* data = nullptr;
    reloaded.world().each<eng::tick::CameraData>(
        [&](eng::ecs::Entity, const eng::tick::CameraData& camera) {
            data = &camera;
        });
    REQUIRE(data != nullptr);
    CHECK(data->rotationDeg == Catch::Approx(90.f));
    CHECK(data->followName == "Player");
    CHECK(data->smoothingTime == Catch::Approx(0.25f));
    CHECK(data->limitsEnabled);
    CHECK(data->limitMaxY == Catch::Approx(30.f));

    // Cena PRÉ-P4.7: o MESMO save com os campos NOVOS REMOVIDOS do JSON
    // (strip real — padrão do teste de migration do P4.6). A migration
    // (migrateComponentDataP47 no loadScene) injeta os defaults e o load
    // passa — cenas de versões antigas continuam abrindo.
    REQUIRE(f.doc->saveScene("main.json").ok());
    auto saved = f.fs->readAllText(
        eng::fs::Path{"ContractGame/scenes/main.json"});
    REQUIRE(saved.ok());
    std::string oldScene = saved.value();
    for (const char* key :
         {"rotationDeg", "followName", "deadzoneW", "deadzoneH",
          "smoothingTime", "limitsEnabled", "limitMinX", "limitMinY",
          "limitMaxX", "limitMaxY"}) {
        const std::string needle = std::string("\"") + key + "\":";
        std::size_t at;
        while ((at = oldScene.find(needle)) != std::string::npos) {
            // Todos os campos novos têm campo seguinte (ordem alfabética
            // do dump) — a vírgula pertence ao par removido.
            const std::size_t comma = oldScene.find(',', at);
            REQUIRE(comma != std::string::npos);
            oldScene.erase(at, comma - at + 1);
        }
        CHECK(oldScene.find(needle) == std::string::npos);
    }
    REQUIRE(f.fs->writeAllText(
        eng::fs::Path{"ContractGame/scenes/main.json"}, oldScene));
    REQUIRE(f.doc->loadScene("main.json").ok());
    const eng::tick::CameraData* legacyCamera = nullptr;
    f.doc->sceneInFocus()->world().each<eng::tick::CameraData>(
        [&](eng::ecs::Entity, const eng::tick::CameraData& camera) {
            legacyCamera = &camera;
        });
    REQUIRE(legacyCamera != nullptr);
    CHECK(legacyCamera->rotationDeg == Catch::Approx(0.f));
    CHECK(legacyCamera->followName.empty());
    CHECK_FALSE(legacyCamera->limitsEnabled);
}

TEST_CASE("p47: verbos camera.* — script dirige a câmera ativa",
          "[editor][p47][niscript]")
{
    ContractFixture f;
    f.withProject();
    auto cam = f.doc->createEntity("Camera", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());

    auto actor = f.doc->createEntity("Herói", eng::scene::kNoEntity);
    REQUIRE(actor.ok());
    REQUIRE(f.doc->addComponent(actor.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    const char* source = "up update:\n"
                         "    camera.zoom(64)\n"
                         "    camera.position(2, 1)\n"
                         "    camera.follow(\"Herói\")\n"
                         "stop\n";
    REQUIRE(f.doc
                ->setInspectorField(actor.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);

    // A câmera do CLONE recebeu os verbos (a edição fica intocada).
    eng::tick::CameraData* data = nullptr;
    f.doc->sceneInFocus()->world().each<eng::tick::CameraData>(
        [&](eng::ecs::Entity, eng::tick::CameraData& camera) {
            data = &camera;
        });
    REQUIRE(data != nullptr);
    CHECK(data->zoom == Catch::Approx(64.f));
    CHECK(data->posX == Catch::Approx(2.f));
    CHECK(data->posY == Catch::Approx(1.f));
    CHECK(data->followName == "Herói");
    f.doc->stop();
}

// =============================================================================
// Kinematic_sweep: script ingênuo COLIDE (round 6)
// =============================================================================

TEST_CASE("p47: kinematic_sweep ON — move ingênuo de 6u PARA na parede",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    // Parede estática: box (0.5, 5, 5) em x=3 — face em 2.5.
    auto wall = f.doc->createEntity("Parede", eng::scene::kNoEntity);
    REQUIRE(wall.ok());
    REQUIRE(f.doc->addComponent(wall.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "shape", "Box")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "halfExtents.x", "0.5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "halfExtents.y", "5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "halfExtents.z", "5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::math::Transform",
                                    "position.x", "3")
                .ok());

    // Jogador KINEMATIC (Collider + RigidBody kinematic) + script INGÊNUO:
    // tenta 600 u num tick (o anti-túnel do sweep fatia em substeps).
    auto player = f.doc->createEntity("Jogador", eng::scene::kNoEntity);
    REQUIRE(player.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::physics::RigidBody")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::physics::RigidBody",
                                    "bodyType", "Kinematic")
                .ok());
    const char* source = "up update:\n"
                         "    move(6, 0)\n"
                         "stop\n";
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    // Sweep ON é o DEFAULT (o naive script COLIDE — round 6).
    CHECK(f.doc->kinematicSweep());
    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    const auto fields = f.doc->inspectorFields(player.value(),
                                               "eng::math::Transform");
    float px = 0.f;
    for (const auto& field : fields) {
        if (field.path == "position.x") {
            px = std::stof(field.value);
        }
    }
    INFO("player.x = " << px);
    CHECK(px < 2.55f); // NUNCA do outro lado (face 2.5)
    CHECK(px > 1.5f);  // mas andou (parou NA parede: face − raio 0.5)
    f.doc->stop();
}

TEST_CASE("p47: kinematic_sweep OFF — o mesmo move é teletransporte cru",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto wall = f.doc->createEntity("Parede", eng::scene::kNoEntity);
    REQUIRE(wall.ok());
    REQUIRE(f.doc->addComponent(wall.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "shape", "Box")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::math::Transform",
                                    "position.x", "3")
                .ok());

    auto player = f.doc->createEntity("Jogador", eng::scene::kNoEntity);
    REQUIRE(player.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::physics::RigidBody")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::physics::RigidBody",
                                    "bodyType", "Kinematic")
                .ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    const char* source = "up update:\n"
                         "    move(6, 0)\n"
                         "stop\n";
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    f.doc->setKinematicSweep(false); // semântica pré-P4.7 (cru)
    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    const auto fields = f.doc->inspectorFields(player.value(),
                                               "eng::math::Transform");
    float px = 0.f;
    for (const auto& field : fields) {
        if (field.path == "position.x") {
            px = std::stof(field.value);
        }
    }
    CHECK(px == Catch::Approx(6.f).margin(0.05f)); // ATRAVESSOU (cru)
    f.doc->stop();
}

TEST_CASE("p47: teleport atravessa MESMO com sweep ON (spawn por design)",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    auto wall = f.doc->createEntity("Parede", eng::scene::kNoEntity);
    REQUIRE(wall.ok());
    REQUIRE(f.doc->addComponent(wall.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::math::Transform",
                                    "position.x", "3")
                .ok());

    auto player = f.doc->createEntity("Jogador", eng::scene::kNoEntity);
    REQUIRE(player.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::physics::RigidBody")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::physics::RigidBody",
                                    "bodyType", "Kinematic")
                .ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    const char* source = "up update:\n"
                         "    teleport(6, 0)\n"
                         "stop\n";
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());
    CHECK(f.doc->kinematicSweep()); // sweep ON…

    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    const auto fields = f.doc->inspectorFields(player.value(),
                                               "eng::math::Transform");
    float px = 0.f;
    for (const auto& field : fields) {
        if (field.path == "position.x") {
            px = std::stof(field.value);
        }
    }
    CHECK(px == Catch::Approx(6.f).margin(0.05f)); // …e ATRAVESSOU (cru)
    f.doc->stop();
}

TEST_CASE("p47: physicsKinematicSweep persiste e o ausente volta a ON",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();
    f.doc->setKinematicSweep(false);
    REQUIRE(f.doc->saveScene("main.json").ok());
    auto saved = f.fs->readAllText(
        eng::fs::Path{"ContractGame/scenes/main.json"});
    REQUIRE(saved.ok());
    CHECK(saved.value().find("\"physicsKinematicSweep\":false")
          != std::string::npos);

    // Remove a chave (cena "antiga") → default ON.
    std::string stripped = saved.value();
    const std::string needle = "\"physicsKinematicSweep\":false";
    const auto at = stripped.find(needle);
    REQUIRE(at != std::string::npos);
    // A chave sai ORDENADA do dump (meio do objeto: seguida de vírgula)
    // ou por último (vírgula ANTES dela) — apagar sem deixar vírgula
    // órfã (vírgula pendurada = JSON inválido).
    const bool commaAfter =
        at + needle.size() < stripped.size()
        && stripped[at + needle.size()] == ',';
    const bool commaBefore = at > 0 && stripped[at - 1] == ',';
    stripped.erase(at, needle.size());
    if (commaAfter) {
        stripped.erase(at, 1); // "k1":v,"k2" — apaga a vírgula SEGUIDA
    } else if (commaBefore && at < stripped.size()
               && stripped[at] == '}') {
        stripped.erase(at - 1, 1); // ,"k"} — apaga a vírgula ANTERIOR
    }
    REQUIRE(f.fs->writeAllText(
        eng::fs::Path{"ContractGame/scenes/main.json"}, stripped));
    auto loaded = f.doc->loadScene("main.json");
    INFO("erro: "
         << (loaded.ok() ? std::string{"ok"} : loaded.error().message));
    REQUIRE(loaded.ok());
    CHECK(f.doc->kinematicSweep());
}

// =============================================================================
// Logic LOD: off-screen pula, opt-out roda sempre
// =============================================================================

TEST_CASE("p47: logic LOD — script off-screen pula o update; opt-out roda; "
          "OFF restaura o comportamento",
          "[editor][p47]")
{
    ContractFixture f;
    f.withProject();

    // Dois scripts IDÊNTICOS (movem +1/tick) em x=10000 (FORA da vista —
    // a câmera do editor default fica perto da origem nos testes).
    const char* source = "up update:\n"
                         "    move(1, 0)\n"
                         "stop\n";
    auto normal = f.doc->createEntity("Longe", eng::scene::kNoEntity);
    REQUIRE(normal.ok());
    REQUIRE(f.doc
                ->setInspectorField(normal.value(),
                                    "eng::math::Transform",
                                    "position.x", "10000")
                .ok());
    REQUIRE(f.doc->addComponent(normal.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(normal.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    auto critical = f.doc->createEntity("Crítico", eng::scene::kNoEntity);
    REQUIRE(critical.ok());
    REQUIRE(f.doc
                ->setInspectorField(critical.value(),
                                    "eng::math::Transform",
                                    "position.x", "10000")
                .ok());
    REQUIRE(f.doc->addComponent(critical.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(critical.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());
    // OPT-OUT: gameplay crítico roda SEMPRE.
    REQUIRE(f.doc
                ->setInspectorField(critical.value(),
                                    "eng::editor::NiScriptComponent",
                                    "lodOptOut", "true")
                .ok());

    // LOD ON (o default é OFF — opt-in honesto).
    REQUIRE(f.doc->logicLodEnabled() == false);
    f.doc->setLogicLodEnabled(true);
    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);

    auto xOf = [&](eng::ecs::Entity e) {
        const auto fields = f.doc->inspectorFields(
            e, "eng::math::Transform");
        for (const auto& field : fields) {
            if (field.path == "position.x") {
                return std::stof(field.value);
            }
        }
        return -1.f;
    };
    CHECK(xOf(normal.value()) == Catch::Approx(10000.f).margin(1e-3f));
    CHECK(xOf(critical.value()) == Catch::Approx(10001.f).margin(1e-3f));
    f.doc->stop();

    // LOD OFF (default): os DOIS rodam (semântica pré-P4.7 1:1).
    f.doc->setLogicLodEnabled(false);
    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    CHECK(xOf(normal.value()) == Catch::Approx(10001.f).margin(1e-3f));
    CHECK(xOf(critical.value()) == Catch::Approx(10001.f).margin(1e-3f));
    f.doc->stop();
}
