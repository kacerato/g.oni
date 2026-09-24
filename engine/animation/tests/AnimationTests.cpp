/// Testes de eng::animation.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <catch2/catch_approx.hpp>

#include "eng/animation/Animation.hpp"

namespace {

using namespace eng::animation;
using Catch::Approx;

AnimationClip makeClip()
{
    AnimationClip clip;
    clip.name = "test";
    clip.position = {{0.f, {0.f, 0.f, 0.f}}, {1.f, {10.f, 0.f, 0.f}}};
    clip.rotation = {{0.f, eng::math::Quat{0.f, 0.f, 0.f, 1.f}},
                     {1.f, eng::math::Quat::fromEulerAngles(
                                0.f, 0.f, 3.14159265f / 2.f)}};
    clip.scale = {{0.f, {1.f, 1.f, 1.f}}, {1.f, {2.f, 2.f, 2.f}}};
    return clip;
}

}  // namespace

TEST_CASE("animation: clip duração e amostragem interpola", "[animation]")
{
    const auto clip = makeClip();
    CHECK(clip.duration() == Approx(1.f).margin(1e-5f));

    auto mid = AnimationSystem::sample(clip, 0.5f);
    CHECK(mid.position.x == Approx(5.f).margin(1e-4f));
    CHECK(mid.scale.x == Approx(1.5f).margin(1e-4f));
    // SLERP de 90° no meio = 45°.
    CHECK(mid.rotation.w == Approx(std::cos(3.14159265f / 8.f)).margin(1e-3f));

    // Bordas.
    CHECK(AnimationSystem::sample(clip, -1.f).position.x == Approx(0.f));
    CHECK(AnimationSystem::sample(clip, 5.f).position.x == Approx(10.f));
}

TEST_CASE("animation: playback avança, speed e loop", "[animation]")
{
    eng::scene::Scene scene;
    const auto e = scene.createNode();
    Animator animator;
    animator.clip = "test";
    animator.playing = true;
    animator.loop = true;
    (void)scene.world().emplace<Animator>(e, animator);

    AnimationBank bank;
    bank.add(makeClip());

    AnimationSystem::update(scene, bank, 0.25f);
    const auto* after = scene.world().get<Animator>(e);
    REQUIRE(after != nullptr);
    CHECK(after->time == Approx(0.25f).margin(1e-5f));
    CHECK(scene.localTransform(e)->position.x == Approx(2.5f).margin(1e-3f));

    // Speed 2× avança o dobro.
    scene.world().get<Animator>(e)->speed = 2.f;
    AnimationSystem::update(scene, bank, 0.25f);
    CHECK(after->time == Approx(0.75f).margin(1e-4f));
    CHECK(scene.localTransform(e)->position.x == Approx(7.5f).margin(1e-3f));

    // Loop: passa de 1.0 → volta ao início.
    AnimationSystem::update(scene, bank, 0.25f); // t = 1.25 → 0.25
    CHECK(after->time == Approx(0.25f).margin(1e-4f));
    CHECK(after->playing);
}

TEST_CASE("animation: sem loop o clip TERMINA (stop natural)", "[animation]")
{
    eng::scene::Scene scene;
    const auto e = scene.createNode();
    Animator animator;
    animator.clip = "test";
    animator.playing = true;
    animator.loop = false;
    (void)scene.world().emplace<Animator>(e, animator);

    AnimationBank bank;
    bank.add(makeClip());

    AnimationSystem::update(scene, bank, 2.f); // além do fim
    const auto* after = scene.world().get<Animator>(e);
    REQUIRE(after != nullptr);
    CHECK_FALSE(after->playing);
    CHECK(after->time == Approx(1.f).margin(1e-5f)); // seek no fim
    CHECK(scene.localTransform(e)->position.x == Approx(10.f).margin(1e-3f));
}

TEST_CASE("animation: pause congela; seek aplica no instante", "[animation]")
{
    eng::scene::Scene scene;
    const auto e = scene.createNode();
    Animator animator;
    animator.clip = "test";
    animator.playing = false; // pausado
    (void)scene.world().emplace<Animator>(e, animator);
    AnimationBank bank;
    bank.add(makeClip());

    AnimationSystem::update(scene, bank, 5.f); // pausado: nada muda
    CHECK(scene.localTransform(e)->position.x == Approx(0.f));

    // Seek manual + update aplicado.
    auto* anim = scene.world().get<Animator>(e);
    anim->time = 0.75f;
    AnimationSystem::update(scene, bank, 0.f);
    CHECK(scene.localTransform(e)->position.x == Approx(7.5f).margin(1e-3f));
}

TEST_CASE("animation: flags de aplicação respeitadas", "[animation]")
{
    eng::scene::Scene scene;
    const auto e = scene.createNode();
    Animator animator;
    animator.clip = "test";
    animator.playing = true;
    animator.applyPosition = false;
    animator.applyScale = false;
    (void)scene.world().emplace<Animator>(e, animator);
    scene.localTransform(e)->position = {99.f, 0.f, 0.f};
    AnimationBank bank;
    bank.add(makeClip());

    AnimationSystem::update(scene, bank, 0.5f);
    // Position/scale INALTERADOS; rotation aplicada.
    CHECK(scene.localTransform(e)->position.x == Approx(99.f));
    CHECK(scene.localTransform(e)->scale.x == Approx(1.f));
    CHECK(scene.localTransform(e)->rotation.w ==
          Approx(std::cos(3.14159265f / 8.f)).margin(1e-3f));
}

TEST_CASE("animation: máquina de estados com cross-fade", "[animation]")
{
    eng::scene::Scene scene;
    const auto e = scene.createNode();
    Animator animator;
    animator.clip = "idle";
    animator.playing = true;
    (void)scene.world().emplace<Animator>(e, animator);

    AnimationBank bank;
    AnimationClip idle;
    idle.name = "idle";
    idle.position = {{0.f, {0.f, 0.f, 0.f}}, {1.f, {1.f, 0.f, 0.f}}};
    bank.add(idle);
    AnimationClip run;
    run.name = "run";
    run.position = {{0.f, {0.f, 5.f, 0.f}}, {1.f, {1.f, 5.f, 0.f}}};
    bank.add(run);

    AnimatorStateMachine machine(*scene.world().get<Animator>(e));
    machine.transition(bank, "run", 0.2f);

    CHECK(machine.state().current == "run");
    CHECK(machine.state().previous == "idle");
    CHECK(machine.state().blendRemaining == Approx(0.2f));

    // Bug C-13 da auditoria final: o cross-fade agora é APLICADO ao nó
    // pelo AnimationSystem (antes o estado existia mas a transição
    // "estalava" — o teste antigo verificava apenas a utilidade blend()).
    // Metade do fade (0.1s de 0.2s): pose = idle@0.1s (y=0) misturada
    // com run@0.1s (y=5) em t=0.5 → y=2.5.
    AnimationSystem::update(scene, bank, 0.1f);
    CHECK(scene.localTransform(e)->position.y == Approx(2.5f).margin(1e-4f));
    CHECK(machine.state().previous == "idle");

    // Fim do fade: pose integral do run (y=5) e previous limpo.
    AnimationSystem::update(scene, bank, 0.1f);
    CHECK(scene.localTransform(e)->position.y == Approx(5.f).margin(1e-4f));
    CHECK(machine.state().blendRemaining == Approx(0.f));
    CHECK(machine.state().previous.empty());

    // Transição para o MESMO estado: no-op.
    machine.transition(bank, "run");
    CHECK(machine.state().current == "run");
    // Estado desconhecido: no-op seguro.
    machine.transition(bank, "nao-existe");
    CHECK(machine.state().current == "run");
}
