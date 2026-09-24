#pragma once

/// eng::tick — arquitetura de Tick: fases, agendador determinístico e os
/// ticks concretos de gameplay (evolução P0-5; ADR-051).
///
/// Taxonomia = composição sobre o ECS:
///   Um "tipo de Tick" no G.ONI é o PAR (componente que declara, sistema
///   que executa), agendado por um TickScheduler com fases canônicas:
///
///   | Fase        | Responsabilidade                    | Ticks           |
///   |-------------|-------------------------------------|-----------------|
///   | PreUpdate   | estado do mundo assenta             | PhysicsTick     |
///   | Update      | lógica de jogo                      | AnimationTick, |
///   |             |                                     | ParticleTick    |
///   | PostUpdate  | pós-lógica (reservada)              | —               |
///   | PreRender   | câmera ativa/ordenação visual       | CameraTick      |
///
///   - `TickSystem` é INTERFACE (composição), não árvore de herança de
///     gameplay. Adicionar um tick novo não toca os existentes.
///   - Ordem determinística: (fase, order() dentro da fase, inserção).
///   - O `ScriptTick` (NI-Script) fica no EDITOR — depende de NiRuntime,
///     que é camada de composição (mesmo padrão do catálogo de
///     componentes, ADR-043).
///   - dt é o dt do FRAME; sistemas per-entidade escalam pela camada via
///     `Scene::timeScaleOf`.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "eng/animation/Animation.hpp"
#include "eng/core/Result.hpp"
#include "eng/particles/Particles.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::tick {

/// Fases canônicas do frame — ordem fixa de execução.
enum class Phase : std::uint8_t {
    PreUpdate = 0,
    Update,
    PostUpdate,
    PreRender,
};
constexpr std::size_t kPhaseCount = 4;

/// Um sistema de tick.
class TickSystem {
public:
    virtual ~TickSystem() = default;
    TickSystem(const TickSystem&) = delete;
    TickSystem& operator=(const TickSystem&) = delete;

    /// Nome estável (diagnóstico/testes; duplicatas no scheduler são
    /// rejeitadas).
    [[nodiscard]] virtual const char* name() const = 0;

    /// Fase de execução no frame.
    [[nodiscard]] virtual Phase phase() const = 0;

    /// Ordem DENTRO da fase (menor primeiro; empate → ordem de inserção).
    [[nodiscard]] virtual int order() const { return 0; }

    /// Um passo do frame. `dt` é o dt do frame (escalas por camada são
    /// responsabilidade do sistema que opera por entidade).
    virtual void tick(eng::scene::Scene& scene, float dt) = 0;

protected:
    TickSystem() = default;
};

/// Agendador determinístico: sistemas ordenados por (fase, ordem,
/// inserção). Um `runFrame` executa TODAS as fases em ordem.
class TickScheduler final {
public:
    TickScheduler() = default;
    ~TickScheduler() = default;
    TickScheduler(const TickScheduler&) = delete;
    TickScheduler& operator=(const TickScheduler&) = delete;

    /// Adiciona um sistema. Erro: nome duplicado. A ordenação interna é
    /// recalculada — `systemOrder()` reflete a ordem de execução.
    [[nodiscard]] eng::core::Result<void> addSystem(
        std::unique_ptr<TickSystem> system);

    /// Um frame completo: todas as fases em ordem, sistemas em ordem.
    void runFrame(eng::scene::Scene& scene, float dt);

    /// Nomes dos sistemas na ordem de execução (teste/diagnóstico).
    [[nodiscard]] std::vector<std::string> systemOrder() const;

    /// Sistema por nome (nullptr se ausente).
    [[nodiscard]] const TickSystem* find(const char* name) const;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return systems_.size();
    }

    void clear() noexcept { systems_.clear(); }

private:
    void sortSystems();

    std::vector<std::unique_ptr<TickSystem>> systems_;
};

// =============================================================================
// Ticks concretos — wrappers finos dos sistemas de gameplay que JÁ
// existem. O valor é a ORDEM DECLARADA e o ponto de extensão
// único para o frame do jogo (editor hoje, runtime de bundles depois).
// =============================================================================

/// Física com timestep fixo: o dt do frame acumula; passos
/// de tamanho fixo rodam a física. Corpos em camadas sem participação de
/// física são pulados dentro do próprio PhysicsWorld.
class PhysicsTick final : public TickSystem {
public:
    PhysicsTick(eng::physics::PhysicsWorld& world,
                eng::physics::TimestepAccumulator& accumulator) noexcept
        : world_(world), accumulator_(accumulator)
    {
    }

    [[nodiscard]] const char* name() const override { return "PhysicsTick"; }
    [[nodiscard]] Phase phase() const override { return Phase::PreUpdate; }
    void tick(eng::scene::Scene& scene, float dt) override;

private:
    eng::physics::PhysicsWorld& world_;
    eng::physics::TimestepAccumulator& accumulator_;
};

/// Animação TRS: dt do frame escalado POR ENTIDADE pela
/// camada (`Scene::timeScaleOf` — ADR-051).
class AnimationTick final : public TickSystem {
public:
    explicit AnimationTick(eng::animation::AnimationBank& bank) noexcept
        : bank_(bank)
    {
    }

    [[nodiscard]] const char* name() const override { return "AnimationTick"; }
    [[nodiscard]] Phase phase() const override { return Phase::Update; }
    [[nodiscard]] int order() const override { return 10; }
    void tick(eng::scene::Scene& scene, float dt) override;

private:
    eng::animation::AnimationBank& bank_;
};

/// Partículas CPU: idem animação — dt escalado por
/// entidade pela camada.
class ParticleTick final : public TickSystem {
public:
    [[nodiscard]] const char* name() const override { return "ParticleTick"; }
    [[nodiscard]] Phase phase() const override { return Phase::Update; }
    [[nodiscard]] int order() const override { return 20; }
    void tick(eng::scene::Scene& scene, float dt) override;
};

}  // namespace eng::tick
