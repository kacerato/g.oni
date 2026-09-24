#include "eng/particles/Particles.hpp"

/// Particles — implementação. CPU determinístico.

#include <algorithm>
#include <cmath>

namespace eng::particles {

namespace {

using eng::math::Vec3;

/// Direção de spawn: LEQUE PLANAR de `spread` graus ao redor da direção
/// (rotação em um plano pela sequência van der Corput — não um cone 3D
/// completo; limitação documentada, "cone" era drift de comentário da
/// auditoria final);
/// o ângulo é escolhido DETERMINÍSTICAMENTE pelo índice (sequência de van
/// der Corput bit-reversal — uniforme sem RNG; testes reprodutíveis).
[[nodiscard]] Vec3 spawnDirection(const ParticleEmitter& emitter,
                                  std::uint32_t index)
{
    const float len = std::sqrt(emitter.direction.x * emitter.direction.x +
                               emitter.direction.y * emitter.direction.y +
                               emitter.direction.z * emitter.direction.z);
    Vec3 dir = {0.f, 1.f, 0.f};
    if (len > 1e-6f) {
        dir = {emitter.direction.x / len, emitter.direction.y / len,
               emitter.direction.z / len};
    }
    if (!emitter.randomSpread || emitter.spread <= 0.f) {
        return dir;
    }

    // Bit-reversal determinístico [0..1).
    float fraction = 0.f;
    {
        std::uint32_t bits = index + 1u;
        float scale = 0.5f;
        while (bits != 0u) {
            if ((bits & 1u) != 0u) {
                fraction += scale;
            }
            bits >>= 1;
            scale *= 0.5f;
        }
    }
    const float coneAngle =
        (fraction * 2.f - 1.f) * emitter.spread * (3.14159265f / 180.f);

    // Rotaciona a direção ao redor do eixo perpendicular (plano XZ→dir):
    // construção simples com base ortonormal.
    const Vec3 up = std::abs(dir.y) < 0.99f ? Vec3{0.f, 1.f, 0.f}
                                            : Vec3{1.f, 0.f, 0.f};
    const Vec3 right = {
        dir.y * up.z - dir.z * up.y,
        dir.z * up.x - dir.x * up.z,
        dir.x * up.y - dir.y * up.x,
    };
    const float rl = std::sqrt(right.x * right.x + right.y * right.y +
                              right.z * right.z);
    const Vec3 rhat = rl > 1e-6f
                          ? Vec3{right.x / rl, right.y / rl, right.z / rl}
                          : Vec3{1.f, 0.f, 0.f};
    const float cosA = std::cos(coneAngle);
    const float sinA = std::sin(coneAngle);
    return {
        dir.x * cosA + rhat.x * sinA,
        dir.y * cosA + rhat.y * sinA,
        dir.z * cosA + rhat.z * sinA,
    };
}

void spawnParticle(eng::scene::Scene& scene, eng::ecs::Entity entity,
                   const ParticleEmitter& emitter, ParticlePool& pool)
{
    if (pool.particles.size() >= emitter.maxParticles) {
        return; // pool cheia: descarta (limites móveis §10)
    }
    const eng::math::Mat4 world = scene.computeWorldMatrix(entity);
    Particle particle;
    particle.position = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
    particle.velocity =
        spawnDirection(emitter, pool.spawnIndex++) * emitter.speed;
    particle.age = 0.f;
    particle.lifetime = emitter.lifetime;
    particle.size = emitter.size;
    particle.rotation = 0.f;
    pool.particles.push_back(particle);
}

}  // namespace

void ParticleSystem::update(eng::scene::Scene& scene, float deltaSeconds)
{
    if (deltaSeconds <= 0.f) {
        return;
    }

    scene.world().each<ParticleEmitter>(
        [&](eng::ecs::Entity e, const ParticleEmitter& emitter) {
            // Camadas (evolução P0-5, ADR-051): sem update a emissão
            // congela inteira (integração, morte e spawn); timeScale
            // escala o dt do emissor.
            if (!scene.participatesIn(e, eng::scene::LayerStage::Update)) {
                return;
            }
            const float dt = deltaSeconds * scene.timeScaleOf(e);
            ParticlePool* pool = scene.world().get<ParticlePool>(e);
            if (pool == nullptr) {
                // Pool nasce junto do primeiro update (runtime only).
                pool = scene.world().emplace<ParticlePool>(e, ParticlePool{});
                if (pool == nullptr) {
                    return;
                }
            }

            // 1) Integração + vida das EXISTENTES (spawn vem DEPOIS: uma
            //    partícula nascida NESTE update não envelhece o dt que a
            //    gerou — bug pego pelo teste de determinismo).
            for (auto& particle : pool->particles) {
                particle.age += dt;
                particle.velocity =
                    particle.velocity + emitter.gravity * dt;
                particle.position =
                    particle.position + particle.velocity * dt;
                particle.rotation += emitter.rotationSpeed * dt;
            }

            // 2) Morte.
            const auto dead = [](const Particle& particle) {
                return particle.age >= particle.lifetime;
            };
            pool->particles.erase(
                std::remove_if(pool->particles.begin(),
                               pool->particles.end(), dead),
                pool->particles.end());

            // 3) Spawn por acumulador (taxa média — independente do dt).
            if (emitter.playing && emitter.rate > 0.f) {
                pool->spawnAccumulator += emitter.rate * dt;
                while (pool->spawnAccumulator >= 1.f) {
                    pool->spawnAccumulator -= 1.f;
                    spawnParticle(scene, e, emitter, *pool);
                }
            }
        });
}

std::size_t ParticleSystem::aliveCount(
    const eng::scene::Scene& scene) noexcept
{
    std::size_t total = 0;
    scene.world().each<ParticlePool>(
        [&](eng::ecs::Entity, const ParticlePool& pool) {
            total += pool.particles.size();
        });
    return total;
}

void ParticleSystem::burst(eng::scene::Scene& scene, eng::ecs::Entity emitter,
                           std::uint32_t count)
{
    const ParticleEmitter* config = scene.world().get<ParticleEmitter>(emitter);
    if (config == nullptr) {
        return;
    }
    ParticlePool* pool = scene.world().get<ParticlePool>(emitter);
    if (pool == nullptr) {
        pool = scene.world().emplace<ParticlePool>(emitter, ParticlePool{});
    }
    if (pool == nullptr) {
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        spawnParticle(scene, emitter, *config, *pool);
    }
}

}  // namespace eng::particles
