#include "eng/tick/Tick.hpp"

/// TickScheduler + ticks concretos — implementação (evolução P0-5; ADR-051).

#include <algorithm>

#include "eng/log/Macros.hpp"

ENG_LOG_CATEGORY("tick");

namespace eng::tick {

// =============================================================================
// TickScheduler
// =============================================================================

eng::core::Result<void> TickScheduler::addSystem(
    std::unique_ptr<TickSystem> system)
{
    if (system == nullptr) {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::InvalidArgument,
            "TickScheduler::addSystem: sistema nulo"});
    }
    if (find(system->name()) != nullptr) {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::AlreadyExists,
            "TickScheduler::addSystem: já existe um sistema '" +
                std::string(system->name()) + "'"});
    }
    systems_.push_back(std::move(system));
    sortSystems();
    return {};
}

void TickScheduler::sortSystems()
{
    // (fase, ordem, inserção): índice atual desempata — std::stable_sort
    // preserva a inserção entre iguais.
    std::stable_sort(
        systems_.begin(), systems_.end(),
        [](const std::unique_ptr<TickSystem>& a,
           const std::unique_ptr<TickSystem>& b) {
            if (a->phase() != b->phase()) {
                return static_cast<std::uint8_t>(a->phase()) <
                       static_cast<std::uint8_t>(b->phase());
            }
            return a->order() < b->order();
        });
}

void TickScheduler::runFrame(eng::scene::Scene& scene, float dt)
{
    for (std::unique_ptr<TickSystem>& system : systems_) {
        system->tick(scene, dt);
    }
}

std::vector<std::string> TickScheduler::systemOrder() const
{
    std::vector<std::string> names;
    names.reserve(systems_.size());
    for (const std::unique_ptr<TickSystem>& system : systems_) {
        names.emplace_back(system->name());
    }
    return names;
}

const TickSystem* TickScheduler::find(const char* name) const
{
    if (name == nullptr) {
        return nullptr;
    }
    for (const std::unique_ptr<TickSystem>& system : systems_) {
        if (std::string_view(system->name()) == std::string_view(name)) {
            return system.get();
        }
    }
    return nullptr;
}

// =============================================================================
// Ticks concretos
// =============================================================================

void PhysicsTick::tick(eng::scene::Scene& scene, float dt)
{
    // Timestep fixo: o dt do frame acumula; passos fixos rodam.
    const auto steps = accumulator_.advance(dt);
    for (std::uint32_t step = 0; step < steps; ++step) {
        world_.step(scene, accumulator_.fixedDt());
    }
}

void AnimationTick::tick(eng::scene::Scene& scene, float dt)
{
    (void)eng::animation::AnimationSystem::update(scene, bank_, dt);
}

void ParticleTick::tick(eng::scene::Scene& scene, float dt)
{
    (void)eng::particles::ParticleSystem::update(scene, dt);
}

}  // namespace eng::tick
