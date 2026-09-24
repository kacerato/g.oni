#include "eng/animation/Animation.hpp"

/// Animation — implementação.

#include <algorithm>
#include <cmath>
#include <utility>

#include <string>

namespace eng::animation {

using eng::math::Quat;
using eng::math::Vec3;

/// Interpolação por tipo (antes de TODO uso — position/scale lerp,
/// rotation SLERP — §7.8).
[[nodiscard]] Vec3 blendValue(const Vec3& a, const Vec3& b, float t) noexcept
{
    return a + (b - a) * t;
}

[[nodiscard]] Quat blendValue(const Quat& a, const Quat& b, float t) noexcept
{
    return a.slerp(b, t);
}

namespace {

/// Amostra uma track ordenada por tempo (busca linear — clips curtos).
template <typename Key>
[[nodiscard]] typename Key::value_type sampleTrack(
    const std::vector<Key>& keys, float time)
{
    if (keys.empty()) {
        return {};
    }
    if (time <= keys.front().time || keys.size() == 1u) {
        return keys.front().value;
    }
    if (time >= keys.back().time) {
        return keys.back().value;
    }
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const Key& a = keys[i];
        const Key& b = keys[i + 1];
        if (time >= a.time && time <= b.time) {
            const float span = b.time - a.time;
            const float t =
                span > 1e-6f ? (time - a.time) / span : 0.f;
            return blendValue(a.value, b.value, t);
        }
    }
    return keys.back().value;
}

}  // namespace

// =============================================================================
// AnimationClip / Bank
// =============================================================================

float AnimationClip::duration() const noexcept
{
    float end = 0.f;
    if (!position.empty()) {
        end = std::max(end, position.back().time);
    }
    if (!rotation.empty()) {
        end = std::max(end, rotation.back().time);
    }
    if (!scale.empty()) {
        end = std::max(end, scale.back().time);
    }
    if (!frames.empty()) {
        // O último frame SEGURA o slot dele (frameHold) — ver header.
        end = std::max(end, frames.back().time + frameHold);
    }
    return end;
}

const SpriteFrameKey* sampleFrame(const AnimationClip& clip,
                                   float time) noexcept
{
    // Amostragem DISCRETA (flipbook): o último key com time <= cursor
    // vale — não há interpolação entre regiões de textura.
    const SpriteFrameKey* active = nullptr;
    for (const SpriteFrameKey& key : clip.frames) {
        if (key.time <= time) {
            active = &key;
        } else {
            break;  // track ordenada por tempo
        }
    }
    return active;
}

void AnimationBank::add(AnimationClip clip)
{
    const std::string key = clip.name;
    clips_.insert_or_assign(std::move(key), std::move(clip));
}

const AnimationClip* AnimationBank::find(std::string_view name) const noexcept
{
    const auto it = clips_.find(std::string(name));
    return it == clips_.end() ? nullptr : &it->second;
}

// =============================================================================
// AnimatorStateMachine
// =============================================================================

void AnimatorStateMachine::transition(const AnimationBank& bank,
                                     std::string_view next,
                                     float blendDuration)
{
    if (animator_.clip == next) {
        return; // já está nele
    }
    if (bank.find(next) == nullptr) {
        return; // clip desconhecido: no-op seguro (documentado)
    }
    if (blendDuration <= 0.f || animator_.clip.empty()) {
        // Troca seca (sem estado anterior válido).
        animator_.clip = std::string(next);
        animator_.time = 0.f;
        animator_.playing = true;
        animator_.previousClip.clear();
        animator_.blendDuration = 0.f;
        animator_.blendRemaining = 0.f;
        return;
    }
    // Bug C-13 da auditoria final: o estado de blend agora vive no
    // COMPONENTE (serializável) e o AnimationSystem o aplica. Uma
    // transição durante um fade em andamento assume o clip CORRENTE
    // como novo "previous" (o fade antigo é descartado — comportamento
    // padrão de engines).
    animator_.previousClip = animator_.clip;
    animator_.previousTime = animator_.time;
    animator_.blendDuration = blendDuration;
    animator_.blendRemaining = blendDuration;
    animator_.clip = std::string(next);
    animator_.time = 0.f;
    animator_.playing = true;
}

AnimatorState AnimatorStateMachine::state() const noexcept
{
    // Vista por valor do estado de blend do componente (pós C-13: a
    // fonte da verdade são os campos do Animator).
    AnimatorState view;
    view.current = animator_.clip;
    view.previous = animator_.previousClip;
    view.previousTime = animator_.previousTime;
    view.blendDuration = animator_.blendDuration;
    view.blendRemaining = animator_.blendRemaining;
    return view;
}

// =============================================================================
// AnimationSystem
// =============================================================================

AnimationSystem::Pose AnimationSystem::sample(const AnimationClip& clip,
                                               float time)
{
    Pose pose;
    pose.position = sampleTrack(clip.position, time);
    pose.rotation = sampleTrack(clip.rotation, time);
    pose.scale = sampleTrack(clip.scale, time);
    if (clip.scale.empty()) {
        pose.scale = {1.f, 1.f, 1.f};
    }
    return pose;
}

AnimationSystem::Pose AnimationSystem::blend(const Pose& a, const Pose& b,
                                             float t)
{
    const float clamped = std::clamp(t, 0.f, 1.f);
    Pose out;
    out.position = blendValue(a.position, b.position, clamped);
    out.rotation = a.rotation.slerp(b.rotation, clamped);
    out.scale = blendValue(a.scale, b.scale, clamped);
    return out;
}

void AnimationSystem::update(eng::scene::Scene& scene,
                             const AnimationBank& bank, float deltaSeconds)
{
    scene.world().each<Animator>([&](eng::ecs::Entity e, Animator& animator) {
        // Camadas (evolução P0-5, ADR-051): sem participação de update o
        // animator CONGELA (não expira blend, não avança); com timeScale
        // o dt é escalado por entidade.
        if (!scene.participatesIn(e, eng::scene::LayerStage::Update)) {
            return;
        }
        const float dt =
            deltaSeconds * scene.timeScaleOf(e);
        const AnimationClip* clip = bank.find(animator.clip);
        if (clip == nullptr || clip->duration() <= 0.f) {
            // sem clip válido: congela (sem crash); blend ativo expira
            // (não há pose nova com que misturar).
            animator.previousClip.clear();
            animator.blendRemaining = 0.f;
            return;
        }

        // Avanço com velocidade — APENAS tocando; pausado
        // mantém o cursor (seek continua aplicando a pose do instante).
        if (animator.playing) {
            animator.time += dt * animator.speed;
            if (animator.loop) {
                animator.time = std::fmod(animator.time, clip->duration());
            } else if (animator.time >= clip->duration()) {
                animator.time = clip->duration();
                animator.playing = false; // fim
            }
        }

        // ---- Cross-fade (bug C-13 da auditoria final) -----------------
        // O clip que SAI continua tocando durante o fade (mesma
        // velocidade, clamp no fim — sem loop no fade); o fator t segue o
        // blendRemaining. previousClip ausente do banco (ou duração zero)
        // encerra o blend na hora (sem misturar com garbage).
        bool blending = !animator.previousClip.empty() &&
                        animator.blendRemaining > 0.f &&
                        animator.blendDuration > 0.f;
        const AnimationClip* previous =
            blending ? bank.find(animator.previousClip) : nullptr;
        if (blending && (previous == nullptr || previous->duration() <= 0.f)) {
            animator.previousClip.clear();
            animator.blendRemaining = 0.f;
            blending = false;
        }
        if (blending && animator.playing) {
            animator.previousTime += dt * animator.speed;
            if (animator.previousTime >= previous->duration()) {
                animator.previousTime = previous->duration();
            }
        }

        Pose pose = sample(*clip, animator.time);
        if (blending) {
            // Consome o dt do fade ANTES do fator: um blend de D segundos
            // completa em exatamente D de tempo acumulado (frame 1 de um
            // fade 0.2s com dt 0.1 já está em t=0.5 — sem frame "perdido"
            // em t=0).
            animator.blendRemaining =
                std::max(0.f, animator.blendRemaining - dt);
            const float t = 1.f - (animator.blendRemaining /
                                   animator.blendDuration);
            pose = blend(sample(*previous, animator.previousTime), pose, t);
            if (animator.blendRemaining == 0.f) {
                animator.previousClip.clear();
            }
        } else {
            animator.previousClip.clear();
            animator.blendRemaining = 0.f;
        }

        auto* transform = scene.localTransform(e);
        if (transform == nullptr) {
            return;
        }
        if (animator.applyPosition) {
            transform->position = pose.position;
        }
        if (animator.applyRotation) {
            transform->rotation = pose.rotation;
        }
        if (animator.applyScale) {
            transform->scale = pose.scale;
        }
    });
}

}  // namespace eng::animation
