#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "eng/scene/Name.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/tick/Camera.hpp"

namespace {

constexpr float kEps = 1e-4f;

Catch::Approx approx(float value)
{
    return Catch::Approx(value).epsilon(kEps).margin(kEps);
}

}  // namespace

// =============================================================================
// resolveActiveCamera
// =============================================================================

TEST_CASE("camera: cena sem câmera resolve vazio", "[tick][camera]")
{
    eng::scene::Scene scene;
    (void)scene.createNode();
    (void)scene.createNode();

    const auto active = eng::tick::resolveActiveCamera(scene);
    CHECK_FALSE(active.found());
    CHECK(eng::tick::activeCameraCount(scene) == 0);
}

TEST_CASE("camera: primeira câmera ativa em ordem de criação vence", "[tick][camera]")
{
    eng::scene::Scene scene;
    const auto first = scene.createNode();
    const auto second = scene.createNode();

    auto* firstCamera = scene.world().emplace<eng::tick::CameraData>(first);
    REQUIRE(firstCamera != nullptr);
    firstCamera->posX = 5.f;
    firstCamera->posY = -3.f;
    firstCamera->zoom = 96.f;

    auto* secondCamera =
        scene.world().emplace<eng::tick::CameraData>(second);
    REQUIRE(secondCamera != nullptr);
    secondCamera->posX = 100.f;

    const auto active = eng::tick::resolveActiveCamera(scene);
    REQUIRE(active.found());
    CHECK(active.entity == first);            // ordem de criação
    CHECK(active.data.posX == approx(5.f));  // a PRIMEIRA vence
    CHECK(active.data.posY == approx(-3.f));
    CHECK(active.data.zoom == approx(96.f));
    CHECK(eng::tick::activeCameraCount(scene) == 2);
}

TEST_CASE("camera: inativas são puladas; active=false desliga sem remover", "[tick][camera]")
{
    eng::scene::Scene scene;
    const auto first = scene.createNode();
    const auto second = scene.createNode();

    // Pool NÃO é estável entre emplaces (dense array realoca — ADR-024):
    // emplace tudo, depois reter ponteiros.
    (void)scene.world().emplace<eng::tick::CameraData>(first);
    (void)scene.world().emplace<eng::tick::CameraData>(second);
    auto* firstCamera = scene.world().get<eng::tick::CameraData>(first);
    auto* secondCamera = scene.world().get<eng::tick::CameraData>(second);
    REQUIRE(firstCamera != nullptr);
    REQUIRE(secondCamera != nullptr);
    firstCamera->posX = 1.f;
    firstCamera->active = false;  // desligada
    secondCamera->posX = 2.f;

    const auto active = eng::tick::resolveActiveCamera(scene);
    REQUIRE(active.found());
    CHECK(active.entity == second);
    CHECK(active.data.posX == approx(2.f));
    CHECK(eng::tick::activeCameraCount(scene) == 1);

    // Religa a primeira → volta a vencer (ordem de criação).
    firstCamera->active = true;
    const auto reactivated = eng::tick::resolveActiveCamera(scene);
    REQUIRE(reactivated.found());
    CHECK(reactivated.entity == first);
}

// =============================================================================
// CameraTickSystem
// =============================================================================

TEST_CASE("camera: CameraTickSystem cacheia a câmera ativa do frame", "[tick][camera]")
{
    eng::scene::Scene scene;
    eng::tick::TickScheduler scheduler;
    REQUIRE(scheduler
                .addSystem(std::make_unique<eng::tick::CameraTickSystem>())
                .ok());
    CHECK(scheduler.systemOrder() ==
          std::vector<std::string>{"CameraTick"});

    // Antes de qualquer frame: vazia.
    const auto* system = scheduler.find("CameraTick");
    REQUIRE(system != nullptr);
    const auto* cameraTick =
        static_cast<const eng::tick::CameraTickSystem*>(system);
    CHECK_FALSE(cameraTick->activeCamera().found());

    // Sem câmera na cena → continua vazia após o frame.
    scheduler.runFrame(scene, 1.f / 60.f);
    CHECK_FALSE(cameraTick->activeCamera().found());

    // Câmera entra na cena → próximo frame cacheia.
    const auto cam = scene.createNode();
    auto* data = scene.world().emplace<eng::tick::CameraData>(cam);
    REQUIRE(data != nullptr);
    data->posX = 7.f;
    data->zoom = 64.f;

    scheduler.runFrame(scene, 1.f / 60.f);
    const auto& active = cameraTick->activeCamera();
    REQUIRE(active.found());
    CHECK(active.entity == cam);
    CHECK(active.data.posX == approx(7.f));
    CHECK(active.data.zoom == approx(64.f));

    // Câmera é desativada → o frame seguinte reflete.
    data->active = false;
    scheduler.runFrame(scene, 1.f / 60.f);
    CHECK_FALSE(cameraTick->activeCamera().found());
}

// =============================================================================
// CameraData: defaults (contrato com o viewport: zoom = pixels/unidade)
// =============================================================================

TEST_CASE("camera: CameraData defaults — origem, zoom 48, ativa", "[tick][camera]")
{
    const eng::tick::CameraData data;
    CHECK(data.posX == approx(0.f));
    CHECK(data.posY == approx(0.f));
    CHECK(data.zoom == approx(48.f));  // idem Viewport::Camera2D default
    CHECK(data.active);
}

// =============================================================================
// Follow / deadzone / smoothing / limites
// =============================================================================

TEST_CASE("p47: camera follow — snap no alvo (smoothing 0)", "[tick][camera][p47]")
{
    eng::scene::Scene scene;
    const auto cam = scene.createNode();
    const auto target = scene.createNode();
    auto* data = scene.world().emplace<eng::tick::CameraData>(cam);
    REQUIRE(data != nullptr);
    data->followName = "Player";
    auto* name = scene.world().emplace<eng::scene::Name>(target);
    REQUIRE(name != nullptr);
    name->value = "Player";
    auto* transform = scene.localTransform(target);
    REQUIRE(transform != nullptr);
    transform->position = eng::math::Vec3{12.f, -7.f, 0.f};

    eng::tick::CameraTickSystem tick;
    tick.tick(scene, 1.f / 60.f);
    REQUIRE(tick.activeCamera().found());
    // Follow SNAP: câmera no centro do alvo (smoothing default 0).
    CHECK(tick.activeCamera().data.posX == approx(12.f));
    CHECK(tick.activeCamera().data.posY == approx(-7.f));
}

TEST_CASE("p47: camera deadzone — dentro não move, fora anda o excesso",
          "[tick][camera][p47]")
{
    eng::scene::Scene scene;
    const auto cam = scene.createNode();
    const auto target = scene.createNode();
    auto* data = scene.world().emplace<eng::tick::CameraData>(cam);
    REQUIRE(data != nullptr);
    data->followName = "Player";
    data->deadzoneW = 4.f;
    data->deadzoneH = 4.f;
    auto* name = scene.world().emplace<eng::scene::Name>(target);
    REQUIRE(name != nullptr);
    name->value = "Player";
    auto* transform = scene.localTransform(target);
    REQUIRE(transform != nullptr);

    eng::tick::CameraTickSystem tick;
    tick.tick(scene, 1.f / 60.f); // alvo em (0,0): câmera inicia em (0,0)

    // Alvo anda 1.5 — dentro da zona morta (±2): câmera NÃO segue.
    transform->position = eng::math::Vec3{1.5f, 0.f, 0.f};
    tick.tick(scene, 1.f / 60.f);
    CHECK(tick.activeCamera().data.posX == approx(0.f));

    // Alvo anda para (3.5, 0) — excesso de 1.5 além de +2: câmera vai a 1.5.
    transform->position = eng::math::Vec3{3.5f, 0.f, 0.f};
    tick.tick(scene, 1.f / 60.f);
    CHECK(tick.activeCamera().data.posX == approx(1.5f));
}

TEST_CASE("p47: camera smoothing — aproximação exponencial com dt",
          "[tick][camera][p47]")
{
    eng::scene::Scene scene;
    const auto cam = scene.createNode();
    const auto target = scene.createNode();
    auto* data = scene.world().emplace<eng::tick::CameraData>(cam);
    REQUIRE(data != nullptr);
    data->followName = "Player";
    data->smoothingTime = 0.1f; // constante de tempo 100 ms
    auto* name = scene.world().emplace<eng::scene::Name>(target);
    REQUIRE(name != nullptr);
    name->value = "Player";
    auto* transform = scene.localTransform(target);
    REQUIRE(transform != nullptr);
    transform->position = eng::math::Vec3{10.f, 0.f, 0.f};

    eng::tick::CameraTickSystem tick;
    // 1º tick: snap para o desejado (estado de suavização inicia no alvo).
    tick.tick(scene, 1.f / 60.f);
    CHECK(tick.activeCamera().data.posX == approx(10.f));

    // Alvo salta 10 → 20: a câmera anda PARCIALMENTE (exponencial).
    transform->position = eng::math::Vec3{20.f, 0.f, 0.f};
    tick.tick(scene, 1.f / 60.f);
    const float first = tick.activeCamera().data.posX;
    CHECK(first > 10.f);
    CHECK(first < 20.f);
    // E continua se aproximando a cada tick (monótona).
    tick.tick(scene, 1.f / 60.f);
    const float second = tick.activeCamera().data.posX;
    CHECK(second > first);
    CHECK(second < 20.f);
}

TEST_CASE("p47: camera limits — visível pós-zoom clampado ao retângulo",
          "[tick][camera][p47]")
{
    eng::scene::Scene scene;
    const auto cam = scene.createNode();
    auto* data = scene.world().emplace<eng::tick::CameraData>(cam);
    REQUIRE(data != nullptr);
    data->followName = "Player";
    data->limitsEnabled = true;
    // Mundo 0..40 × 0..30.
    data->limitMinX = 0.f;
    data->limitMinY = 0.f;
    data->limitMaxX = 40.f;
    data->limitMaxY = 30.f;

    eng::tick::CameraTickSystem tick;
    // Vista 480×240 px com zoom 48 → visível 10×5 unidades
    // (meias-extensões 5×2.5) — clamp em [5,35]×[2.5,27.5].
    tick.setViewSize(480.f, 240.f);

    // Alvo FORA do mundo (100, 100): câmera bate no canto (35, 27.5).
    const auto player = scene.createNode();
    auto* name = scene.world().emplace<eng::scene::Name>(player);
    REQUIRE(name != nullptr);
    name->value = "Player";
    auto* transform = scene.localTransform(player);
    REQUIRE(transform != nullptr);
    transform->position = eng::math::Vec3{100.f, 100.f, 0.f};

    tick.tick(scene, 1.f / 60.f);
    CHECK(tick.activeCamera().data.posX == approx(35.f));
    CHECK(tick.activeCamera().data.posY == approx(27.5f));
}
