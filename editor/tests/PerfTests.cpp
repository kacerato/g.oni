/// Testes do CÉREBRO de performance: governor (EMA +
/// térmico + histerese), retângulo de vista e CULLING de render.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "eng/editor/PerfGovernor.hpp"
#include "eng/editor/Viewport.hpp"

#include <cmath>

namespace {

using eng::editor::PerfGovernor;
using eng::editor::ThermalLevel;

} // namespace

// =============================================================================
// Governor — desce sob carga, volta quando esfria, NUNCA oscila (round 7)
// =============================================================================

TEST_CASE("p47: governor desce com frames ruins e sobe de volta SEM "
          "oscilar",
          "[perf][p47]")
{
    PerfGovernor g;
    CHECK(g.presetIndex() == 0); // High

    // 30 frames ruins consecutivos → desce UM degrau (Med) — e NÃO mais
    // (a observação recomeça: o efeito do preset precisa de janela).
    for (std::uint32_t i = 0; i < PerfGovernor::kBadFrames; ++i) {
        g.onFrame(30.f, ThermalLevel::None);
    }
    CHECK(g.presetIndex() == 1);

    // Frame bom isolado NÃO sobe (histerese — kGoodFrames > kBadFrames).
    g.onFrame(10.f, ThermalLevel::None);
    CHECK(g.presetIndex() == 1);

    // Jitter no LIMITE (alternando bom/ruim): streaks zera — NUNCA oscila.
    for (std::uint32_t i = 0; i < PerfGovernor::kBadFrames * 4; ++i) {
        g.onFrame(27.f, ThermalLevel::None); // ruim
        g.onFrame(14.f, ThermalLevel::None); // bom (mas um só)
        g.onFrame(27.f, ThermalLevel::None);
        g.onFrame(14.f, ThermalLevel::None);
    }
    CHECK(g.presetIndex() == 1); // ficou EM Med o tempo todo

    // Frames bons CONSECUTIVOS (kGoodFrames + janela do EMA decair) →
    // sobe de volta a High.
    for (std::uint32_t i = 0;
         i < PerfGovernor::kGoodFrames + PerfGovernor::kEmaWindow + 10;
         ++i) {
        g.onFrame(10.f, ThermalLevel::None);
    }
    CHECK(g.presetIndex() == 0);
}

TEST_CASE("p47: governor — térmico severo desce NA HORA e bloqueia a volta",
          "[perf][p47]")
{
    PerfGovernor g;
    // Térmico severo: Low IMEDIATO (um frame basta).
    g.onFrame(16.f, ThermalLevel::Severe);
    CHECK(g.presetIndex() == 2);

    // Frame bom com chip ainda quente (Moderate): NUNCA sobe — voltar
    // com o chip quente é o que causa oscilação.
    for (std::uint32_t i = 0; i < PerfGovernor::kGoodFrames * 2; ++i) {
        g.onFrame(10.f, ThermalLevel::Moderate);
    }
    CHECK(g.presetIndex() == 2);

    // Esfriou (None) + frames bons → sobe um degrau (Low → Med).
    for (std::uint32_t i = 0; i < PerfGovernor::kGoodFrames; ++i) {
        g.onFrame(10.f, ThermalLevel::None);
    }
    CHECK(g.presetIndex() == 1);
}

TEST_CASE("p47: governor — frame inválido não conta; EMA é média móvel",
          "[perf][p47]")
{
    PerfGovernor g;
    g.onFrame(-1.f, ThermalLevel::Unknown); // pausa/surface morta: ignora
    g.onFrame(0.f, ThermalLevel::Unknown);  // idem
    CHECK(g.presetIndex() == 0);
    CHECK(g.badStreak() == 0);
    CHECK(g.goodStreak() == 0);
    CHECK(g.thermal() == ThermalLevel::Unknown);

    g.onFrame(20.f, ThermalLevel::None); // semeia o EMA
    CHECK(g.emaMs() == Catch::Approx(20.f).margin(1e-4f));
    g.onFrame(20.f, ThermalLevel::None);
    CHECK(g.emaMs() == Catch::Approx(20.f).margin(1e-4f)); // converge
}

// =============================================================================
// Culling — vista em mundo + contagem exportada (round 7: 180/200)
// =============================================================================

TEST_CASE("p47: culling de render — 200 entidades, 180 off-screen ⇒ "
          "cull = 180",
          "[perf][p47]")
{
    eng::editor::Viewport vp;
    vp.setScreenSize(800.f, 480.f);
    vp.camera().posX = 0.f;
    vp.camera().posY = 0.f;
    vp.camera().zoom = 48.f;

    eng::scene::Scene scene;
    // 20 entidades NO CAMPO (espaçamento 0.4u — x até −7.6, dentro da
    // meia-largura de 800/48/2 ≈ 8.33u MESMO sem margem).
    for (int i = 0; i < 20; ++i) {
        const auto e = scene.createNode();
        scene.localTransform(e)->position = {
            -0.4f * static_cast<float>(i), 0.f, 0.f};
    }
    // 180 entidades LONGE (x = 10000 + i*5 — fora de QUALQUER margem).
    for (int i = 0; i < 180; ++i) {
        const auto e = scene.createNode();
        scene.localTransform(e)->position = {
            10000.f + 5.f * static_cast<float>(i), 0.f, 0.f};
    }
    REQUIRE(scene.nodeCount() == 200);

    // Sem culling: TODOS os quads (200).
    const auto all = vp.buildQuads(scene, std::nullopt);
    CHECK(all.size() == 200);

    // Com culling: vista ±(8.3+8, 5+8) — pega as 20 do campo, corta as 180.
    auto cull = vp.worldViewRect();
    cull.margin = eng::editor::Viewport::kCullMarginWorld;
    std::uint32_t culled = 0;
    const auto visible = vp.buildQuads(scene, std::nullopt, cull, &culled);
    CHECK(culled == 180);
    CHECK(visible.size() == 20);

    // Rect sem margem nenhuma também corta as 180 (estão a 10000u).
    auto tight = vp.worldViewRect();
    std::uint32_t culledTight = 0;
    (void)vp.buildQuads(scene, std::nullopt, tight, &culledTight);
    CHECK(culledTight == 180);
}

TEST_CASE("p47: culling conserva filhos em vista mesmo com pai fora",
          "[perf][p47]")
{
    eng::editor::Viewport vp;
    vp.setScreenSize(800.f, 480.f);

    eng::scene::Scene scene;
    // Pai LONGE (10000) com filho PERTO da origem (herda transform — o
    // filho em vista desenha: hierarquia nunca poda).
    const auto parent = scene.createNode();
    scene.localTransform(parent)->position = {10000.f, 0.f, 0.f};
    const auto child = scene.createNode();
    REQUIRE(scene.attach(child, parent));
    scene.localTransform(child)->position = {-10000.f, 0.f, 0.f};

    auto cull = vp.worldViewRect();
    std::uint32_t culled = 0;
    const auto visible = vp.buildQuads(scene, std::nullopt, cull, &culled);
    CHECK(culled == 1); // só o pai
    CHECK(visible.size() == 1);
    CHECK(visible[0].entity == child);
}

TEST_CASE("p47: worldViewRect cobre a rotação da vista (c AABB expandido)",
          "[perf][p47]")
{
    eng::editor::Viewport vp;
    vp.setScreenSize(480.f, 480.f);
    vp.camera().zoom = 48.f;
    // Sem rotação: half = 5 unidades em ambos os eixos.
    auto noRot = vp.worldViewRect();
    CHECK(noRot.maxX - noRot.minX == Catch::Approx(10.f).margin(1e-3f));
    // 45°: o AABB da vista cobre os CANTOS — half' = 5·(|cos|+|sin|) ≈ 7.07.
    vp.camera().rotation = 3.14159265358979323846f / 4.f;
    auto rot = vp.worldViewRect();
    CHECK(rot.maxX - rot.minX
          == Catch::Approx(10.f * std::sqrt(2.f)).margin(1e-3f));
}
