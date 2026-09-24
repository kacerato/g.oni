#pragma once

/// eng::animation — clips, animator e máquina de estados.
///
/// - Clips com keyframes TRS (position lerp, rotation SLERP — §7.8);
/// - Animator é COMPONENTE: play/pause/stop/loop/speed/seek;
/// - Estados + transições com cross-fade linear;
/// - SKELETAL: a hierarquia de nós da cena É a preparação (pose =
///   transforms de nós); skinning/mesh é EXTENSÃO FUTURA documentada —
///   ainda não há mesh renderer no engine.
/// - Sem RHI/Android; aplica TRS no Transform do próprio nó.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eng/math/Quat.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::animation {

// =============================================================================
// Keyframes
// =============================================================================

template <typename T>
struct Keyframe {
    using value_type = T;
    float time{0.f};
    T value{};
};

using PositionKey = Keyframe<eng::math::Vec3>;
using RotationKey = Keyframe<eng::math::Quat>;
using ScaleKey = Keyframe<eng::math::Vec3>;

/// Key de FRAME de sprite: a MEMA track de keyframes
/// da clip — o valor é a região do sprite que passa a valer a partir de
/// `time` (flipbook 2D). Dados PUROS (string + floats): a engine não
/// conhece o componente visual; o CONSUMIDOR (editor/runtime host)
/// aplica ao sprite da entidade.
struct SpriteFrameKey {
    float time{0.f};
    std::string textureAsset{};  ///< nome do asset de textura
    float u0{0.f}, v0{0.f}, u1{1.f}, v1{1.f};  ///< região UV
};

/// Um clip de animação de TRANSFORM + frames de sprite.
struct AnimationClip {
    std::string name;
    std::vector<PositionKey> position;
    std::vector<RotationKey> rotation;
    std::vector<ScaleKey> scale;
    /// Track de frames (flipbook): amostragem discreta — o último key
    /// com time <= cursor vale. APLICADA pelo consumidor (editor), não
    /// pelo AnimationSystem (a engine não conhece SpriteData).
    std::vector<SpriteFrameKey> frames;
    /// Quanto tempo o ÚLTIMO frame SEGURA
    /// (o slot 1/fps). A duração do clip estende frames.back().time +
    /// frameHold — sem isto o loop voltaria EXATAMENTE no último frame e
    /// ele nunca seria exibido (fmod no ponto de corte).
    float frameHold{0.f};

    /// Duração = último keyframe de qualquer track.
    [[nodiscard]] float duration() const noexcept;
};

/// Frame ativo da clip no instante `time` (nullptr quando a track de
/// frames está vazia ou o cursor está antes do primeiro key).
[[nodiscard]] const SpriteFrameKey* sampleFrame(
    const AnimationClip& clip, float time) noexcept;

/// Biblioteca de clips por nome (banco do runtime — §9: API C++).
class AnimationBank final {
public:
    void add(AnimationClip clip);
    [[nodiscard]] const AnimationClip* find(std::string_view name) const
        noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return clips_.size(); }

private:
    std::unordered_map<std::string, AnimationClip> clips_;
};

// =============================================================================
// Animator — componente ECS, refletido/serializável
// =============================================================================

struct Animator {
    std::string clip{"idle"};   ///< clip corrente (por nome do banco)
    float time{0.f};            ///< cursor de playback (segundos)
    /// Cross-fade ATIVO (bug C-13 da auditoria final: o estado de blend
    /// vivia FORA do componente e o AnimationSystem nunca o aplicava —
    /// a transição "estalava"). `previousClip` vazio = sem blend ativo;
    /// o Sistema consome esses campos e limpa previousClip ao fim do fade.
    std::string previousClip{};
    float previousTime{0.f};    ///< cursor do clip que sai (durante o fade)
    float blendDuration{0.f};
    float blendRemaining{0.f};
    float speed{1.f};          ///< 0.5 = metade, 2 = dobro
    bool loop{true};
    bool playing{false};
    /// Aplica a track de FRAMES ao sprite da entidade (quando houver
    /// sprite E track de frames no clip). TRS continua sob apply*.
    bool applySprite{true};
    bool applyPosition{true};
    bool applyRotation{true};
    bool applyScale{true};
};

ENG_REFLECT_BEGIN(eng::animation::Animator)
    ENG_REFLECT_FIELD(clip)
    ENG_REFLECT_FIELD(time)
    ENG_REFLECT_FIELD(previousClip)
    ENG_REFLECT_FIELD(previousTime)
    ENG_REFLECT_FIELD(blendDuration)
    ENG_REFLECT_FIELD(blendRemaining)
    ENG_REFLECT_FIELD(speed)
    ENG_REFLECT_FIELD(loop)
    ENG_REFLECT_FIELD(playing)
    ENG_REFLECT_FIELD(applySprite)
    ENG_REFLECT_FIELD(applyPosition)
    ENG_REFLECT_FIELD(applyRotation)
    ENG_REFLECT_FIELD(applyScale)
ENG_REFLECT_END()

// =============================================================================
// Estados e transições
// =============================================================================

struct AnimationTransition {
    std::string from;
    std::string to;
    float blendDuration{0.15f};
};

/// Leitura do estado de cross-fade do Animator (vista por valor — a
/// fonte da verdade são os campos previousClip/blend* do COMPONENTE).
struct AnimatorState {
    std::string current;
    std::string previous;
    float blendRemaining{0.f};
    float blendDuration{0.f};
    float previousTime{0.f};
};

/// Máquina de estados mínima: Idle→Run→Jump→Attack são NOMES —
/// as REGRAS ficam no gameplay (C++/script — §9); a engine fornece a
/// transição com cross-fade.
///
/// Pós C-13 (auditoria final): o estado de blend vive no COMPONENTE
/// Animator (serializável) e é APLICADO pelo AnimationSystem::update —
/// a máquina é o gatilho (`transition`) + vista (`state`). O avanço por
/// frame é responsabilidade do Sistema (um único condutor por frame:
/// não chame a máquina E o sistema para o mesmo animator no mesmo tick).
class AnimatorStateMachine final {
public:
    explicit AnimatorStateMachine(Animator& animator) noexcept
        : animator_(animator)
    {
    }

    /// Dispara uma transição para o estado/clip `next` (trocando o clip do
    /// Animator e iniciando o cross-fade com o anterior). Transição para o
    /// estado atual: no-op. Clip desconhecido: no-op seguro.
    void transition(const AnimationBank& bank, std::string_view next,
                    float blendDuration = 0.15f);

    /// Vista do estado de blend (lida do componente).
    [[nodiscard]] AnimatorState state() const noexcept;

private:
    Animator& animator_;
};

// =============================================================================
// Sistema — aplica TRS interpolado
// =============================================================================

class AnimationSystem final {
public:
    AnimationSystem() = delete;

    /// Avança TODOS os Animators da cena e aplica o TRS interpolado nos
    /// Transforms locais dos nós.
    static void update(eng::scene::Scene& scene,
                       const AnimationBank& bank, float deltaSeconds);

    /// Amostra um clip num instante (TRS out; tracks ausentes ficam com o
    /// valor default e o bit de aplicação desligado pelo chamador).
    struct Pose {
        eng::math::Vec3 position{};
        eng::math::Quat rotation{};
        eng::math::Vec3 scale{1.f, 1.f, 1.f};
    };
    [[nodiscard]] static Pose sample(const AnimationClip& clip, float time);

    /// Blend linear de poses (position/scale lerp; rotation slerp).
    [[nodiscard]] static Pose blend(const Pose& a, const Pose& b, float t);
};

}  // namespace eng::animation
