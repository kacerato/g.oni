/// Testes de eng::particles.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "eng/particles/Particles.hpp"

namespace {

using namespace eng::particles;
using Catch::Approx;

struct SceneFixture {
    eng::scene::Scene scene;

    eng::ecs::Entity addEmitter(const ParticleEmitter& emitter =
                                   ParticleEmitter{}) {
        const auto e = scene.createNode();
        (void)scene.world().emplace<ParticleEmitter>(e, emitter);
        scene.localTransform(e)->position = {0.f, 0.f, 0.f};
        return e;
    }
};

}  // namespace

TEST_CASE("particles: emitter spawna por rate com acumulador", "[particles]")
{
    SceneFixture f;
    ParticleEmitter config;
    config.rate = 60.f;   // 60/s
    config.lifetime = 10.f;
    const auto e = f.addEmitter(config);

    // 0.5s de uma vez: 30 partículas (acumulador lida com dt qualquer).
    (void)e;
    ParticleSystem::update(f.scene, 0.5f);
    CHECK(ParticleSystem::aliveCount(f.scene) == 30u);

    // Passos menores com o MESMO tempo total: MESMA contagem (média).
    SceneFixture g;
    (void)g.addEmitter(config);
    for (int i = 0; i < 30; ++i) {
        ParticleSystem::update(g.scene, 1.f / 60.f);
    }
    CHECK(ParticleSystem::aliveCount(g.scene) == 30u);
}

TEST_CASE("particles: lifetime mata; update integra", "[particles]")
{
    SceneFixture f;
    ParticleEmitter config;
    config.rate = 10.f;
    config.lifetime = 0.5f;
    config.speed = 1.f;
    config.direction = {0.f, 1.f, 0.f};
    config.gravity = {0.f, 0.f, 0.f};
    const auto e = f.addEmitter(config);

    // 0.2s: 2 spawnadas — e NÃO envelhecem o dt que as gerou (idade 0).
    ParticleSystem::update(f.scene, 0.2f);
    REQUIRE(ParticleSystem::aliveCount(f.scene) == 2u);

    // +0.4s: as 2 antigas têm 0.4 (ainda < 0.5), +4 novas → 6 vivas.
    ParticleSystem::update(f.scene, 0.4f);
    CHECK(ParticleSystem::aliveCount(f.scene) == 6u);

    // +0.2s: as 2 antigas chegam a 0.6 → MORREM; +2 novas.
    ParticleSystem::update(f.scene, 0.2f);
    CHECK(ParticleSystem::aliveCount(f.scene) == 6u);

    // Integração: a partícula de idade 0.4 subiu 0.4m (1 m/s, sem g).
    const auto* pool = f.scene.world().get<ParticlePool>(e);
    REQUIRE(pool != nullptr);
    REQUIRE(pool->particles.size() == 6u);
    // As sobreviventes de 0.2s subiram 0.2m (1 m/s, sem gravidade);
    // as de 0.4s MORRERAM (0.6 ≥ 0.5) — foram removidas.
    bool hasRisen = false;
    for (const auto& particle : pool->particles) {
        if (particle.age > 0.15f) {
            hasRisen = particle.position.y > 0.15f;
        }
    }
    CHECK(hasRisen);
}

TEST_CASE("particles: gravidade puxa as partículas", "[particles]")
{
    SceneFixture f;
    ParticleEmitter config;
    config.rate = 0.f; // sem spawn automático — burst manual
    config.lifetime = 10.f;
    config.speed = 0.f;
    config.gravity = {0.f, -10.f, 0.f};
    const auto e = f.addEmitter(config);

    ParticleSystem::burst(f.scene, e, 1);
    REQUIRE(ParticleSystem::aliveCount(f.scene) == 1u);

    ParticleSystem::update(f.scene, 1.f);
    const auto* pool = f.scene.world().get<ParticlePool>(e);
    REQUIRE(pool != nullptr);
    // Euler semi-implícito (mesma convenção da física): v=-10, p=v*dt.
    CHECK(pool->particles[0].velocity.y == Approx(-10.f).margin(1e-3f));
    CHECK(pool->particles[0].position.y == Approx(-10.f).margin(1e-2f));
}

TEST_CASE("particles: burst imediato e pool cheia limita", "[particles]")
{
    SceneFixture f;
    ParticleEmitter config;
    config.rate = 0.f;
    config.lifetime = 100.f;
    config.maxParticles = 5;
    const auto e = f.addEmitter(config);

    ParticleSystem::burst(f.scene, e, 10);
    CHECK(ParticleSystem::aliveCount(f.scene) == 5u); // limitada

    // Rate alto também respeita o limite.
    ParticleEmitter fast;
    fast.rate = 1000.f;
    fast.lifetime = 100.f;
    fast.maxParticles = 5;
    const auto e2 = f.addEmitter(fast);
    ParticleSystem::update(f.scene, 1.f);
    const auto* pool = f.scene.world().get<ParticlePool>(e2);
    REQUIRE(pool != nullptr);
    CHECK(pool->particles.size() <= 5u);
}

TEST_CASE("particles: emissor parado não spawna", "[particles]")
{
    SceneFixture f;
    ParticleEmitter config;
    config.rate = 100.f;
    config.playing = false;
    const auto e = f.addEmitter(config);
    (void)e;
    ParticleSystem::update(f.scene, 1.f);
    CHECK(ParticleSystem::aliveCount(f.scene) == 0u);
}

TEST_CASE("particles: determinismo — mesma sequência, mesmo estado",
          "[particles]")
{
    const auto run = [](std::vector<float> dts) {
        SceneFixture f;
        ParticleEmitter config;
        config.rate = 25.f;
        config.lifetime = 0.3f;
        config.speed = 3.f;
        config.gravity = {0.f, -2.f, 0.f};
        config.randomSpread = true;
        const auto e = f.addEmitter(config);
        for (float dt : dts) {
            ParticleSystem::update(f.scene, dt);
        }
        const auto* pool = f.scene.world().get<ParticlePool>(e);
        std::vector<std::pair<float, float>> snapshot;
        if (pool != nullptr) {
            for (const auto& particle : pool->particles) {
                snapshot.emplace_back(particle.position.x,
                                     particle.position.y);
            }
        }
        return snapshot;
    };

    // (a) MESMA sequência de dt → snapshot BIT-A-BIT idêntico.
    const auto a = run({0.1f, 0.1f, 0.1f});
    const auto a2 = run({0.1f, 0.1f, 0.1f});
    REQUIRE(a.size() == a2.size());
    REQUIRE_FALSE(a.empty());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].first == a2[i].first);
        CHECK(a[i].second == a2[i].second);
    }

    // (b) Fatias DIFERENTES do mesmo tempo total: a CONTAGEM (taxa média)
    // coincide; posições não são invariantes (nascimento intra-chunk).
    const auto b = run({0.05f, 0.15f, 0.1f});
    REQUIRE(a.size() == b.size());
}
