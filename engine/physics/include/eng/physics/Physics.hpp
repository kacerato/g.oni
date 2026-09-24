#pragma once

/// eng::physics — física mínima correta sobre o ECS.
///
/// Primitivas: esfera + AABB.
/// Integração semi-implícita de Euler; mass == 0 → corpo ESTÁTICO;
/// resolução por projeção posicional + impulso escalar (sem rotação de
/// corpo — sem inércia angular nesta fase, documentado).
///
/// TIMESTEP: `step(scene, fixedDt)` com dt FIXO — o chamador
/// acumula o dt do frame e dá N passos (ver TimestepAccumulator).
///
/// Nada aqui conhece RHI/Android.

#include <cstdint>
#include <unordered_map>
#include <span>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/ecs/Ecs.hpp"
#include "eng/math/Mat4.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::physics {

// =============================================================================
// Componentes — refletidos p/ inspector/serialização
// =============================================================================

/// Tipo de corpo. Enum de NAMESPACE (ADR-021 — nested
/// quebra o traço canônico do reflect, ver ColliderShape).
///   Static      — NUNCA integra (mesmo com mass > 0 autorado);
///                 massa inversa efetiva 0 na resolução.
///   Kinematic   — integra SOMENTE a velocidade AUTORADA (script/);
///                 sem gravidade/damping; empurra dinâmicos, não é
///                 empurrado (massa inversa efetiva 0).
///   DynamicLite — comportamento integral pré-P4.6 (gravidade +
///                 damping + impulso arcade). DEFAULT: cenas antigas
///                 migram 1:1 (mass == 0 → Static, senão DynamicLite).
enum class BodyType : std::uint8_t {
    Static = 0,
    Kinematic = 1,
    DynamicLite = 2,
};

struct RigidBody {
    float mass{1.f};              ///< 0 = estático (colisor fixo)
    eng::math::Vec3 velocity{0.f, 0.f, 0.f};
    eng::math::Vec3 gravity{0.f, -9.81f, 0.f};
    bool useGravity{true};
    float linearDamping{0.f};     ///< 0..1 por segundo
    /// P4.6. ÚLTIMO campo: agregados
    /// posicionais existentes (testes/fixtures) continuam compilando.
    BodyType bodyType{BodyType::DynamicLite};
};

/// Forma do colisor (enum de namespace — o reflect cobre enums de
/// namespace; nested quebra o traço de nome canônico, ADR-021).
enum class ColliderShape : std::uint8_t { Sphere = 0, Box = 1 };

struct Collider {
    ColliderShape shape{ColliderShape::Sphere};
    float radius{0.5f};                 ///< Sphere
    eng::math::Vec3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box (AABB local)
    std::uint32_t layer{1};             ///< bit(s) próprio(s)
    std::uint32_t mask{0xFFFFFFFFu};    ///< com quem colide
    bool isTrigger{false};              ///< contato SEM resolução
};

/// Controle de personagem mobile: esfera que MOVE E DESLIZA
/// contra estáticos — física de gameplay, não simulação completa.
struct CharacterBody {
    eng::math::Vec3 velocity{0.f, 0.f, 0.f};
    float radius{0.5f};
    bool snapToGround{false}; ///< projeta para o chão (remediação C-18:
                              ///< movimento horizontal + chão a meio raio)
};

// Registro reflect (nomes estáveis — ADR-021/033).
ENG_REFLECT_BEGIN(eng::physics::RigidBody)
    ENG_REFLECT_FIELD(mass)
    ENG_REFLECT_FIELD_AS(velocity, "eng::math::Vec3")
    ENG_REFLECT_FIELD_AS(gravity, "eng::math::Vec3")
    ENG_REFLECT_FIELD(useGravity)
    ENG_REFLECT_FIELD(linearDamping)
    ENG_REFLECT_FIELD_AS(bodyType, "eng::physics::BodyType")
ENG_REFLECT_END()

// Tipo de corpo refletido (Inspector = enum dropdown).
ENG_REFLECT_ENUM_BEGIN(eng::physics::BodyType)
    ENG_REFLECT_ENUM_VALUE(Static)
    ENG_REFLECT_ENUM_VALUE(Kinematic)
    ENG_REFLECT_ENUM_VALUE(DynamicLite)
ENG_REFLECT_ENUM_END()

// BUG FIX (evolução P0-6): era ENG_REFLECT_BEGIN (STRUCT) — o enum era
// registrado como struct sem propriedades: o campo `shape` do Collider
// NUNCA apareceu no Inspector nem foi serializado (recursão em struct
// vazia = silêncio). O macro correto registra kind=Enum + subjacente.
ENG_REFLECT_ENUM_BEGIN(eng::physics::ColliderShape)
    ENG_REFLECT_ENUM_VALUE(Sphere)
    ENG_REFLECT_ENUM_VALUE(Box)
ENG_REFLECT_ENUM_END()

ENG_REFLECT_BEGIN(eng::physics::Collider)
    ENG_REFLECT_FIELD_AS(shape, "eng::physics::ColliderShape")
    ENG_REFLECT_FIELD(radius)
    ENG_REFLECT_FIELD_AS(halfExtents, "eng::math::Vec3")
    ENG_REFLECT_FIELD(layer)
    ENG_REFLECT_FIELD(mask)
    ENG_REFLECT_FIELD(isTrigger)
ENG_REFLECT_END()

ENG_REFLECT_BEGIN(eng::physics::CharacterBody)
    ENG_REFLECT_FIELD_AS(velocity, "eng::math::Vec3")
    ENG_REFLECT_FIELD(radius)
    ENG_REFLECT_FIELD(snapToGround)
ENG_REFLECT_END()

// =============================================================================
// Contatos
// =============================================================================

struct ContactEvent {
    eng::ecs::Entity entityA{};
    eng::ecs::Entity entityB{};
    eng::math::Vec3 normal{0.f, 1.f, 0.f}; ///< de B para A
    eng::math::Vec3 point{0.f, 0.f, 0.f};
    float depth{0.f};
    bool isTrigger{false};
};

// =============================================================================
// Raycast
// =============================================================================

struct RaycastHit {
    bool hit{false};
    eng::ecs::Entity entity{};
    eng::math::Vec3 point{0.f, 0.f, 0.f};
    eng::math::Vec3 normal{0.f, 0.f, 0.f};
    float distance{0.f};
};

// =============================================================================
// PhysicsWorld — sistema sobre a cena
// =============================================================================

class PhysicsWorld final {
public:
    /// Um passo FIXO: integra → detecta → resolve (não-re triggers).
    /// `fixedDt` deve ser constante entre chamadas (ex.: 1/60).
    void step(eng::scene::Scene& scene, float fixedDt);

    /// Raio contra TODOS os colisores (o MAIS PRÓXIMO vence). Direção
    /// normalizada exigida (erro preciso caso contrário).
    [[nodiscard]] static eng::core::Result<RaycastHit> raycast(
        const eng::scene::Scene& scene, eng::math::Vec3 origin,
        eng::math::Vec3 direction, float maxDistance,
        std::uint32_t mask = 0xFFFFFFFFu);

    /// Movimento + deslize do CharacterBody contra estáticos/dinâmicos
    /// (retorna a posição FINAL em mundo — o chamador aplica ao
    /// transform; `motion` é o deslocamento proposto, NÃO consumido de
    /// CharacterBody::velocity; com snapToGround, projeta ao chão).
    [[nodiscard]] static eng::math::Vec3 moveAndSlide(
        const eng::scene::Scene& scene, eng::ecs::Entity body,
        eng::math::Vec3 motion);

    /// Varredura de KINEMATIC por COLLIDER (sem
    /// CharacterBody). Mesma matemática de substeps anti-túnel do
    /// moveAndSlide (chunk ≤ meio raio, teto 64), raio da esfera do
    /// Collider do próprio corpo (Sphere = radius; Box = círculo
    /// inscrito na meia-extensão mínima) e o mask do próprio corpo
    /// decidindo contra quem desliza. Retorna a posição FINAL em mundo.
    /// Parte da posição de MUNDO ATUAL do corpo (`motion` = delta
    /// proposto — semântica do `move` do script).
    [[nodiscard]] static eng::math::Vec3 kinematicSweepMove(
        const eng::scene::Scene& scene, eng::ecs::Entity body,
        eng::math::Vec3 motion);

    /// Variante com ORIGEM explícita — varre de `from`
    /// (posição de mundo ANTES da escrita) por `motion`. Necessária para
    /// a escrita de `position` (o write já moveu o corpo; a varredura
    /// tem de voltar à origem — nunca varrer DO destino).
    [[nodiscard]] static eng::math::Vec3 kinematicSweepMoveFrom(
        const eng::scene::Scene& scene, eng::ecs::Entity body,
        const eng::math::Vec3& from, eng::math::Vec3 motion);

    [[nodiscard]] std::span<const ContactEvent> contacts() const noexcept
    {
        return contacts_;
    }
    [[nodiscard]] std::size_t contactCount() const noexcept
    {
        return contacts_.size();
    }

    void clearContacts() { contacts_.clear(); }

private:
    std::vector<ContactEvent> contacts_;

    /// Pares de trigger sobrepostos NO ÚLTIMO passo
    /// (canônicos menor-índice primeiro) — diff publica on_enter/on_exit
    /// (TriggerEvent) no barramento da cena. Vazio = nenhum par antes.
    std::vector<std::pair<eng::ecs::Entity, eng::ecs::Entity>>
        triggerPairsPrev_;

    /// Broad phase SPATIAL HASH — células (chave = cx<<32
    /// | cy) → índices de collidable. Reconstruído por passo (buckets
    /// reusam capacidade do map — pooling); pares candidatos ordenados na
    /// ordem canônica do laço O(n²) antigo (determinismo 1:1).
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>
        hashBuckets_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> candidatePairs_;
};

/// Acumulador de timestep fixo.
class TimestepAccumulator final {
public:
    explicit TimestepAccumulator(float fixedDt = 1.f / 60.f) noexcept
        : fixedDt_(fixedDt)
    {
    }

    /// Alimenta o dt do frame; devolve QUANTOS passos fixos executar.
    std::uint32_t advance(float frameDt) noexcept;

    [[nodiscard]] float fixedDt() const noexcept { return fixedDt_; }
    void setFixedDt(float fixedDt) noexcept
    {
        fixedDt_ = fixedDt > 0.f ? fixedDt : 1.f / 60.f;
    }
    [[nodiscard]] float carry() const noexcept { return carry_; }
    void reset() noexcept { carry_ = 0.f; }

private:
    float fixedDt_;
    float carry_{0.f};
};

}  // namespace eng::physics
