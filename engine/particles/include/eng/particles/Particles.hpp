#pragma once

/// eng::particles — emissor CPU com simulação determinística (FASE 10,
/// missão §7.12/§7.13).
///
/// - CPU v1: a decisão é REGISTRADA — GPU particles exigem
///   compute/texturas que o RHI ainda não tem; a estrutura (pool por
///   emissor) já suporta a migração.
/// - Spawn por ACUMULADOR de rate (determinístico dado o dt).
/// - Partículas vivem em `ParticlePool` (componente de RUNTIME — não é
///   serializado; o estado é derivado do emitter a cada play).
/// - O HOST renderiza como quads pos+cor (mesma draw-list do editor).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "eng/math/Vec3.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::particles {

// =============================================================================
// Emitter — componente refletido/serializável
// =============================================================================

struct ParticleEmitter {
    float rate{20.f};            ///< partículas por segundo
    float lifetime{1.f};         ///< segundos por partícula
    float speed{2.f};            ///< magnitude da velocidade inicial
    eng::math::Vec3 direction{0.f, 1.f, 0.f}; ///< direção (normalizada na simulação)
    float spread{30.f};          ///< ângulo de abertura em GRAUS (0 = feixe)
    float size{0.08f};           ///< tamanho (unidades de mundo)
    float rotationSpeed{0.f};    ///< radianos/s
    eng::math::Vec3 gravity{0.f, -1.f, 0.f};
    bool playing{true};
    std::uint32_t maxParticles{512};
    /// Semear a direção? v1: spread determinístico por ÍNDICE (sem RNG —
    /// §7.14 "deterministic test quando possível").
    bool randomSpread{true};
};

ENG_REFLECT_BEGIN(eng::particles::ParticleEmitter)
    ENG_REFLECT_FIELD(rate)
    ENG_REFLECT_FIELD(lifetime)
    ENG_REFLECT_FIELD(speed)
    ENG_REFLECT_FIELD_AS(direction, "eng::math::Vec3")
    ENG_REFLECT_FIELD(spread)
    ENG_REFLECT_FIELD(size)
    ENG_REFLECT_FIELD(rotationSpeed)
    ENG_REFLECT_FIELD_AS(gravity, "eng::math::Vec3")
    ENG_REFLECT_FIELD(playing)
    ENG_REFLECT_FIELD(maxParticles)
    ENG_REFLECT_FIELD(randomSpread)
ENG_REFLECT_END()

// =============================================================================
// Estado de runtime (NÃO serializado — pool do emissor)
// =============================================================================

struct Particle {
    eng::math::Vec3 position;
    eng::math::Vec3 velocity;
    float age{0.f};
    float lifetime{1.f};
    float size{0.08f};
    float rotation{0.f};
};

/// Pool anexada à MESMA entidade do emitter (runtime only).
struct ParticlePool {
    std::vector<Particle> particles;
    float spawnAccumulator{0.f};
    std::uint32_t spawnIndex{0}; ///< sequência determinística (spread)
};

// =============================================================================
// Sistema
// =============================================================================

class ParticleSystem final {
public:
    ParticleSystem() = delete;

    /// Um update (dt do frame — partículas não exigem timestep fixo;
    /// spawn por acumulador mantém a taxa média independente do dt).
    static void update(eng::scene::Scene& scene, float deltaSeconds);

    /// Total de partículas VIVAS na cena (prova de conteúdo/testes).
    [[nodiscard]] static std::size_t aliveCount(
        const eng::scene::Scene& scene) noexcept;

    /// Spawn imediato de `count` partículas (bursts — API de gameplay).
    static void burst(eng::scene::Scene& scene, eng::ecs::Entity emitter,
                      std::uint32_t count);
};

}  // namespace eng::particles
