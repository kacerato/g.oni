#include "eng/physics/Physics.hpp"

#include "eng/scene/SceneEvents.hpp"

/// Physics — implementação. Esfera + AABB; semi-implícito.

#include <algorithm>
#include <cmath>

namespace eng::physics {

namespace {

using eng::math::Vec3;

/// AABB do colisor no MUNDO (transform aplicado — box alinhado ao MUNDO
/// nesta fase; rotação de box é extensão documentada).
struct WorldAabb {
    Vec3 center;
    Vec3 halfExtents;
};

struct WorldShape {
    bool isSphere{false};
    Vec3 center;
    float radius{0.f};
    WorldAabb box;
};

[[nodiscard]] WorldShape worldShapeOf(const eng::scene::Scene& scene,
                                     eng::ecs::Entity entity,
                                     const Collider& collider)
{
    WorldShape shape;
    const eng::math::Mat4 world = scene.computeWorldMatrix(entity);
    shape.center = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
    shape.isSphere = collider.shape == ColliderShape::Sphere;
    if (shape.isSphere) {
        // Escala da COLUNA X aproxima o raio (correta para escala
        // uniforme; anisotrópica é limitação documentada — AABB ignora
        // rotação do nó, idem).
        const float sx = std::sqrt(world.at(0, 0) * world.at(0, 0) +
                                  world.at(0, 1) * world.at(0, 1) +
                                  world.at(0, 2) * world.at(0, 2));
        shape.radius = collider.radius * sx;
    } else {
        const float sx = std::sqrt(world.at(0, 0) * world.at(0, 0) +
                                  world.at(0, 1) * world.at(0, 1) +
                                  world.at(0, 2) * world.at(0, 2));
        const float sy = std::sqrt(world.at(1, 0) * world.at(1, 0) +
                                  world.at(1, 1) * world.at(1, 1) +
                                  world.at(1, 2) * world.at(1, 2));
        const float sz = std::sqrt(world.at(2, 0) * world.at(2, 0) +
                                  world.at(2, 1) * world.at(2, 1) +
                                  world.at(2, 2) * world.at(2, 2));
        shape.box.center = shape.center;
        shape.box.halfExtents = {collider.halfExtents.x * sx,
                                collider.halfExtents.y * sy,
                                collider.halfExtents.z * sz};
    }
    return shape;
}

/// Colisão esfera-esfera → profundidade/normal (de B para A).
[[nodiscard]] bool sphereSphere(const WorldShape& a, const WorldShape& b,
                               Vec3& normal, float& depth)
{
    const Vec3 delta = a.center - b.center;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                    delta.z * delta.z);
    const float radiusSum = a.radius + b.radius;
    if (distance >= radiusSum) {
        return false;
    }
    depth = radiusSum - distance;
    normal = distance > 1e-6f
                 ? Vec3{delta.x / distance, delta.y / distance,
                        delta.z / distance}
                 : Vec3{0.f, 1.f, 0.f};
    return true;
}

/// Esfera-AABB: ponto mais próximo do centro do box.
[[nodiscard]] bool sphereAabb(const WorldShape& sphere, const WorldShape& box,
                             bool sphereIsA, Vec3& normal, float& depth)
{
    const WorldAabb& aabb = box.box;
    const Vec3 clamped = {
        std::clamp(sphere.center.x, aabb.center.x - aabb.halfExtents.x,
                   aabb.center.x + aabb.halfExtents.x),
        std::clamp(sphere.center.y, aabb.center.y - aabb.halfExtents.y,
                   aabb.center.y + aabb.halfExtents.y),
        std::clamp(sphere.center.z, aabb.center.z - aabb.halfExtents.z,
                   aabb.center.z + aabb.halfExtents.z)};
    const Vec3 delta = sphere.center - clamped;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                    delta.z * delta.z);
    if (distance >= sphere.radius) {
        return false;
    }
    depth = sphere.radius - distance;
    if (distance > 1e-6f) {
        normal = {delta.x / distance, delta.y / distance,
                  delta.z / distance};
    } else {
        // Centro DENTRO do box: menor eixo para sair.
        const Vec3 local = sphere.center - aabb.center;
        const Vec3 exits = {
            aabb.halfExtents.x - std::abs(local.x),
            aabb.halfExtents.y - std::abs(local.y),
            aabb.halfExtents.z - std::abs(local.z)};
        if (exits.x <= exits.y && exits.x <= exits.z) {
            normal = {local.x >= 0.f ? 1.f : -1.f, 0.f, 0.f};
            depth = sphere.radius + exits.x;
        } else if (exits.y <= exits.z) {
            normal = {0.f, local.y >= 0.f ? 1.f : -1.f, 0.f};
            depth = sphere.radius + exits.y;
        } else {
            normal = {0.f, 0.f, local.z >= 0.f ? 1.f : -1.f};
            depth = sphere.radius + exits.z;
        }
    }
    if (!sphereIsA) {
        normal = {-normal.x, -normal.y, -normal.z}; // normal de B para A
    }
    return true;
}

[[nodiscard]] bool aabbAabb(const WorldShape& a, const WorldShape& b,
                           Vec3& normal, float& depth)
{
    const Vec3 delta = a.box.center - b.box.center;
    const Vec3 overlap = {
        a.box.halfExtents.x + b.box.halfExtents.x - std::abs(delta.x),
        a.box.halfExtents.y + b.box.halfExtents.y - std::abs(delta.y),
        a.box.halfExtents.z + b.box.halfExtents.z - std::abs(delta.z)};
    if (overlap.x <= 0.f || overlap.y <= 0.f || overlap.z <= 0.f) {
        return false;
    }
    // Menor eixo de separação.
    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        depth = overlap.x;
        normal = {delta.x >= 0.f ? 1.f : -1.f, 0.f, 0.f};
    } else if (overlap.y <= overlap.z) {
        depth = overlap.y;
        normal = {0.f, delta.y >= 0.f ? 1.f : -1.f, 0.f};
    } else {
        depth = overlap.z;
        normal = {0.f, 0.f, delta.z >= 0.f ? 1.f : -1.f};
    }
    return true;
}

[[nodiscard]] bool shapesCollide(const WorldShape& a, const WorldShape& b,
                                Vec3& normal, float& depth)
{
    if (a.isSphere && b.isSphere) {
        return sphereSphere(a, b, normal, depth);
    }
    if (a.isSphere && !b.isSphere) {
        return sphereAabb(a, b, true, normal, depth);
    }
    if (!a.isSphere && b.isSphere) {
        // Papéis invertidos (A é box): sphereAabb já devolve a normal no
        // contrato daQUI (de B para A) via sphereIsA=false — NÃO negar de
        // novo (bug da dupla negação pego pelo teste de repouso no chão).
        return sphereAabb(b, a, false, normal, depth);
    }
    return aabbAabb(a, b, normal, depth);
}

/// Ray-sphere; t em [0, maxDistance].
[[nodiscard]] bool raySphere(const Vec3& origin, const Vec3& dir, float maxD,
                            const WorldShape& s, float& tOut)
{
    const Vec3 oc = origin - s.center;
    const float b = oc.x * dir.x + oc.y * dir.y + oc.z * dir.z;
    const float c = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z -
                    s.radius * s.radius;
    const float disc = b * b - c;
    if (disc < 0.f) {
        return false;
    }
    const float t = -b - std::sqrt(disc);
    if (t >= 0.f && t <= maxD) {
        tOut = t;
        return true;
    }
    const float t2 = -b + std::sqrt(disc);
    if (t < 0.f && t2 >= 0.f && t2 <= maxD) { // origem dentro
        tOut = t2;
        return true;
    }
    return false;
}

/// Ray-AABB (slab method).
[[nodiscard]] bool rayAabb(const Vec3& origin, const Vec3& dir, float maxD,
                          const WorldAabb& b, float& tOut)
{
    float tMin = 0.f;
    float tMax = maxD;
    for (int axis = 0; axis < 3; ++axis) {
        const float o = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
        const float d = axis == 0 ? dir.x : axis == 1 ? dir.y : dir.z;
        const float c = axis == 0 ? b.center.x
                       : axis == 1 ? b.center.y
                                  : b.center.z;
        const float h = axis == 0 ? b.halfExtents.x
                        : axis == 1 ? b.halfExtents.y
                                   : b.halfExtents.z;
        if (std::abs(d) < 1e-8f) {
            if (o < c - h || o > c + h) {
                return false;
            }
            continue;
        }
        float t1 = (c - h - o) / d;
        float t2 = (c + h - o) / d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) {
            return false;
        }
    }
    tOut = tMin;
    return true;
}

[[nodiscard]] bool masksOverlap(const Collider& a, const Collider& b)
{
    return (a.layer & b.mask) != 0u && (b.layer & a.mask) != 0u;
}

/// Massa inversa EFETIVA na resolução. Static e
/// Kinematic não são empurrados (inv 0 — empurram os dinâmicos e os
/// impulsos não lhes aplicam); DynamicLite = regra da massa atual.
[[nodiscard]] float effectiveInvMass(const RigidBody* body)
{
    if (body == nullptr) {
        return 0.f;
    }
    if (body->bodyType == BodyType::Static ||
        body->bodyType == BodyType::Kinematic) {
        return 0.f;
    }
    return body->mass > 0.f ? 1.f / body->mass : 0.f;
}

/// Um passo do deslize (mesma matemática do v1 — §7.5): avança ao
/// destino, acha a MAIOR penetração da esfera e projeta para fora ao
/// longo da normal — a componente normal do movimento é absorvida, a
/// tangential desliza. `selfMask`: o mask do PRÓPRIO corpo
/// decide contra quem o deslize acontece (padrão Godot p/ cinemáticos;
/// 0xFFFFFFFF = colide com tudo — default, não muda cenários antigos).
[[nodiscard]] Vec3 sweepSphereOnce(const eng::scene::Scene& scene,
                                   eng::ecs::Entity body, Vec3 position,
                                   Vec3 chunk, float radius,
                                   std::uint32_t selfMask)
{
    const Vec3 target = position + chunk;
    float deepest = 0.f;
    Vec3 pushNormal{0.f, 1.f, 0.f};
    bool collided = false;

    scene.world().each<Collider>([&](eng::ecs::Entity e,
                                     const Collider& collider) {
        if (e == body || collider.isTrigger ||
            !scene.participatesIn(e, eng::scene::LayerStage::Physics)) {
            return;
        }
        if ((selfMask & collider.layer) == 0u) {
            return; // filtragem por mask do corpo (uma direção)
        }
        const WorldShape other = worldShapeOf(scene, e, collider);
        WorldShape self;
        self.isSphere = true;
        self.center = target;
        self.radius = radius;
        Vec3 normal;
        float depth = 0.f;
        if (!shapesCollide(self, other, normal, depth)) {
            return;
        }
        if (depth > deepest) {
            deepest = depth;
            pushNormal = normal;
            collided = true;
        }
    });

    return collided ? target + pushNormal * (deepest * 1.001f + 0.001f)
                    : target;
}

}  // namespace

// =============================================================================
// step
// =============================================================================

void PhysicsWorld::step(eng::scene::Scene& scene, float fixedDt)
{
    contacts_.clear();

    // Par canônico (menor índice primeiro — geração
    // desempata) para o diff de triggers entre passos.
    auto canonicalPair = [](eng::ecs::Entity a,
                            eng::ecs::Entity b) {
        if (a.index < b.index
            || (a.index == b.index && a.generation < b.generation)) {
            return std::make_pair(a, b);
        }
        return std::make_pair(b, a);
    };
    std::vector<std::pair<eng::ecs::Entity, eng::ecs::Entity>>
        currentTriggers;

    // 1) Integração semi-implícita (velocidade → posição).
    //    Camadas (evolução P0-5, ADR-051): corpo em camada sem
    //    participação de física é PULADO — fica estático e fora do
    //    mundo físico (sem resposta, sem trigger, sem raycast).
    scene.world().each<RigidBody>(
        [&](eng::ecs::Entity e, RigidBody& body) {
            if (body.bodyType == BodyType::Static) {
                return; // nunca integra (mesmo com mass > 0)
            }
            if (body.mass <= 0.f) {
                return; // legado pré-P4.6: massa 0 = estático
            }
            if (!scene.participatesIn(e, eng::scene::LayerStage::Physics)) {
                return; // camada sem física
            }
            if (body.bodyType == BodyType::Kinematic) {
                // Cinemático — a velocidade é 100%
                // AUTORADA (script/Inspector): sem gravidade, sem
                // damping. Integra e EMPURRA os dinâmicos na resolução
                // (massa inversa efetiva 0 — ver effectiveInvMass).
                auto* kinematic = scene.localTransform(e);
                if (kinematic != nullptr) {
                    kinematic->position =
                        kinematic->position + body.velocity * fixedDt;
                }
                return;
            }
            if (body.useGravity) {
                body.velocity = body.velocity + body.gravity * fixedDt;
            }
            if (body.linearDamping > 0.f) {
                const float damping =
                    1.f - std::clamp(body.linearDamping * fixedDt, 0.f, 1.f);
                body.velocity = body.velocity * damping;
            }
            auto* transform = scene.localTransform(e);
            if (transform != nullptr) {
                transform->position =
                    transform->position + body.velocity * fixedDt;
            }
        });

    // 2) Broad/narrow: a v1 varria TODOS
    //    os pares (O(n²) — 200 corpos = 20k checagens por passo no C33).
    //    Agora: AABB de cada colisor insere o índice nas células que
    //    cobre (célula ≥ 2× a maior extensão da cena); pares candidatos
    //    saem por bucket, DEDUPLICADOS e ordenados na ordem CANÔNICA do
    //    laço antigo (i<j por índice) — determinismo 1:1 com o v1
    //    (a ordem dos contatos é contrato: HitEvent/trigger diff). O
    //    narrow phase não mudou — só QUEM chega a ele.
    //    Honestidade: o hash é RECONSTRUÍDO por passo com buckets
    //    reutilizados (pooling); incremental por dirty-tracking é
    //    extensão documentada (a reconstrução é O(n) e barata).
    std::vector<eng::ecs::Entity> collidable;
    collidable.reserve(scene.nodeCount());
    scene.world().each<Collider>([&](eng::ecs::Entity e, const Collider&) {
        if (scene.isNode(e) &&
            scene.participatesIn(e, eng::scene::LayerStage::Physics)) {
            collidable.push_back(e);
        }
    });

    // Célula: ≥ 2× a maior meia-extensão (um AABB toca no máximo 4 células
    // por eixo em cena normal — pares dentro do bucket são candidatos).
    float maxExtent = 1.f;
    for (const eng::ecs::Entity e : collidable) {
        const Collider& c = *scene.world().get<Collider>(e);
        const float extent =
            c.shape == ColliderShape::Sphere
                ? c.radius
                : std::max(c.halfExtents.x,
                           std::max(c.halfExtents.y, c.halfExtents.z));
        maxExtent = std::max(maxExtent, extent);
    }
    const float cell = maxExtent * 2.f;
    hashBuckets_.clear();
    for (std::size_t i = 0; i < collidable.size(); ++i) {
        const WorldShape s =
            worldShapeOf(scene, collidable[i], *scene.world().get<Collider>(collidable[i]));
        // AABB da forma (esfera: raio; box: meia-extensão REAL por eixo
        // — box alongado NÃO cabe no raio da esfera circunscrita).
        const Collider& c = *scene.world().get<Collider>(collidable[i]);
        float ex = s.radius, ey = s.radius;
        if (c.shape == ColliderShape::Box) {
            ex = c.halfExtents.x;
            ey = c.halfExtents.y;
        }
        const std::int64_t bx0 = static_cast<std::int64_t>(
            std::floor((s.center.x - ex) / cell));
        const std::int64_t bx1 = static_cast<std::int64_t>(
            std::floor((s.center.x + ex) / cell));
        const std::int64_t by0 = static_cast<std::int64_t>(
            std::floor((s.center.y - ey) / cell));
        const std::int64_t by1 = static_cast<std::int64_t>(
            std::floor((s.center.y + ey) / cell));
        for (std::int64_t cx = bx0; cx <= bx1; ++cx) {
            for (std::int64_t cy = by0; cy <= by1; ++cy) {
                hashBuckets_[static_cast<std::uint64_t>(cx) * 0x100000000ull
                             + static_cast<std::uint64_t>(cy)]
                    .push_back(static_cast<std::uint32_t>(i));
            }
        }
    }
    // Pares candidatos: por bucket, todos os pares dentro dele (i<j por
    // POSIÇÃO no collidable — que segue a ordem de iteração do world,
    // índice crescente); dedupe + ordenação canônica.
    candidatePairs_.clear();
    for (const auto& [key, bucket] : hashBuckets_) {
        for (std::size_t bi = 0; bi < bucket.size(); ++bi) {
            for (std::size_t bj = bi + 1; bj < bucket.size(); ++bj) {
                std::uint32_t i = bucket[bi];
                std::uint32_t j = bucket[bj];
                if (i > j) {
                    std::swap(i, j);
                }
                candidatePairs_.emplace_back(i, j);
            }
        }
    }
    std::sort(candidatePairs_.begin(), candidatePairs_.end());
    candidatePairs_.erase(
        std::unique(candidatePairs_.begin(), candidatePairs_.end()),
        candidatePairs_.end());

    for (const auto& [i, j] : candidatePairs_) {
        const eng::ecs::Entity a = collidable[i];
        const eng::ecs::Entity b = collidable[j];
        const Collider& colliderA = *scene.world().get<Collider>(a);
        const Collider& colliderB = *scene.world().get<Collider>(b);
            if (!masksOverlap(colliderA, colliderB)) {
                continue;
            }

            const WorldShape shapeA = worldShapeOf(scene, a, colliderA);
            const WorldShape shapeB = worldShapeOf(scene, b, colliderB);
            Vec3 normal;
            float depth = 0.f;
            if (!shapesCollide(shapeA, shapeB, normal, depth)) {
                continue;
            }

            const bool trigger = colliderA.isTrigger || colliderB.isTrigger;
            const Vec3 mid = shapeA.center -
                             normal * (depth * 0.5f);
            contacts_.push_back(ContactEvent{
                a, b, normal, mid, depth, trigger});
            if (trigger) {
                // Par de trigger ATIVO neste passo (diff
                // com o passo anterior publica on_enter/on_exit no fim).
                currentTriggers.push_back(canonicalPair(a, b));
                continue; // contato SEM resolução
            }

            // On_hit nos DOIS sentidos (o bridge do
            // NI-Script roda o handler do script cujo self == evento;
            // normal aponta de other PARA self — convenção do doc).
            eng::scene::HitEvent hitAB;
            hitAB.self = a;
            hitAB.other = b;
            hitAB.nx = normal.x;
            hitAB.ny = normal.y;
            scene.events().publish(hitAB);
            eng::scene::HitEvent hitBA;
            hitBA.self = b;
            hitBA.other = a;
            hitBA.nx = -normal.x;
            hitBA.ny = -normal.y;
            scene.events().publish(hitBA);

            // 3) Resolução: projeção posicional proporcional às massas
            //    inversas + impulso escalar ao longo da normal.
            //    InvA/invB via effectiveInvMass — Static/Kinematic
            //    têm massa inversa efetiva 0 (não são empurrados).
            RigidBody* bodyA = scene.world().get<RigidBody>(a);
            RigidBody* bodyB = scene.world().get<RigidBody>(b);
            const float invA = effectiveInvMass(bodyA);
            const float invB = effectiveInvMass(bodyB);
            const float invSum = invA + invB;
            if (invSum <= 0.f) {
                continue; // dois estáticos
            }
            const auto* transformA = scene.localTransform(a);
            const auto* transformB = scene.localTransform(b);
            const Vec3 push = normal * (depth / invSum);
            if (transformA != nullptr && invA > 0.f) {
                scene.localTransform(a)->position =
                    scene.localTransform(a)->position + push * invA;
            }
            if (transformB != nullptr && invB > 0.f) {
                scene.localTransform(b)->position =
                    scene.localTransform(b)->position - push * invB;
            }

            // Impulso (restituição 0 — gameplay mobile): cancela a
            // componente de aproximação ao longo da normal.
            if (bodyA != nullptr && invA > 0.f) {
                const float vn = bodyA->velocity.x * normal.x +
                                 bodyA->velocity.y * normal.y +
                                 bodyA->velocity.z * normal.z;
                if (vn < 0.f) {
                    bodyA->velocity =
                        bodyA->velocity - normal * vn;
                }
            }
            if (bodyB != nullptr && invB > 0.f) {
                const float vn = bodyB->velocity.x * normal.x +
                                 bodyB->velocity.y * normal.y +
                                 bodyB->velocity.z * normal.z;
                if (vn > 0.f) {
                    bodyB->velocity =
                        bodyB->velocity - normal * vn;
                }
        }
    }

    // --- P4.7.0 Bloco 1: on_enter / on_exit (diff de pares de trigger)
    // Nos DOIS sentidos (mesma razão do on_hit). Determinístico: pares
    // canônicos; entrada em ordem de par detectado, saída em ordem do
    // passo anterior.
    for (const auto& pair : currentTriggers) {
        const bool wasOverlapping =
            std::find(triggerPairsPrev_.begin(), triggerPairsPrev_.end(),
                      pair) != triggerPairsPrev_.end();
        if (wasOverlapping) {
            continue;
        }
        eng::scene::TriggerEvent enteredAB;
        enteredAB.self = pair.first;
        enteredAB.other = pair.second;
        enteredAB.entered = true;
        scene.events().publish(enteredAB);
        eng::scene::TriggerEvent enteredBA;
        enteredBA.self = pair.second;
        enteredBA.other = pair.first;
        enteredBA.entered = true;
        scene.events().publish(enteredBA);
    }
    for (const auto& pair : triggerPairsPrev_) {
        const bool stillOverlapping =
            std::find(currentTriggers.begin(), currentTriggers.end(),
                      pair) != currentTriggers.end();
        if (stillOverlapping) {
            continue;
        }
        eng::scene::TriggerEvent exitedAB;
        exitedAB.self = pair.first;
        exitedAB.other = pair.second;
        exitedAB.entered = false;
        scene.events().publish(exitedAB);
        eng::scene::TriggerEvent exitedBA;
        exitedBA.self = pair.second;
        exitedBA.other = pair.first;
        exitedBA.entered = false;
        scene.events().publish(exitedBA);
    }
    triggerPairsPrev_.swap(currentTriggers);
}

// =============================================================================
// raycast
// =============================================================================

eng::core::Result<RaycastHit> PhysicsWorld::raycast(
    const eng::scene::Scene& scene, Vec3 origin, Vec3 direction,
    float maxDistance, std::uint32_t mask)
{
    using eng::core::Error;
    using eng::core::StatusCode;
    using eng::core::makeUnexpected;

    const float len = std::sqrt(direction.x * direction.x +
                                direction.y * direction.y +
                                direction.z * direction.z);
    if (len < 1e-6f) {
        return makeUnexpected(Error{StatusCode::InvalidArgument,
                                    "raycast: direção nula"});
    }
    if (maxDistance <= 0.f) {
        return makeUnexpected(Error{StatusCode::InvalidArgument,
                                    "raycast: distância <= 0"});
    }
    const Vec3 dir = {direction.x / len, direction.y / len,
                      direction.z / len};

    RaycastHit best;
    best.hit = false;
    best.distance = maxDistance;

    scene.world().each<Collider>(
        [&](eng::ecs::Entity e, const Collider& collider) {
            if (!scene.isNode(e) || (collider.layer & mask) == 0u ||
                !scene.participatesIn(e, eng::scene::LayerStage::Physics)) {
                return;
            }
            const WorldShape shape = worldShapeOf(scene, e, collider);
            float t = 0.f;
            bool hit = shape.isSphere
                          ? raySphere(origin, dir, best.distance, shape, t)
                          : rayAabb(origin, dir, best.distance, shape.box, t);
            if (!hit || t >= best.distance) {
                return;
            }
            best.hit = true;
            best.entity = e;
            best.distance = t;
            best.point = origin + dir * t;
            // Normal: aproximação por eixo dominante da saída (documentado:
            // normal exata de superfície é refinamento futuro).
            if (shape.isSphere) {
                const Vec3 delta = best.point - shape.center;
                const float dl = std::sqrt(delta.x * delta.x +
                                           delta.y * delta.y +
                                           delta.z * delta.z);
                best.normal = dl > 1e-6f
                                  ? Vec3{delta.x / dl, delta.y / dl,
                                         delta.z / dl}
                                  : Vec3{0.f, 1.f, 0.f};
            } else {
                const Vec3 local = best.point - shape.box.center;
                const Vec3 ratio = {
                    local.x / std::max(shape.box.halfExtents.x, 1e-6f),
                    local.y / std::max(shape.box.halfExtents.y, 1e-6f),
                    local.z / std::max(shape.box.halfExtents.z, 1e-6f)};
                if (std::abs(ratio.x) >= std::abs(ratio.y) &&
                    std::abs(ratio.x) >= std::abs(ratio.z)) {
                    best.normal = {ratio.x >= 0.f ? 1.f : -1.f, 0.f, 0.f};
                } else if (std::abs(ratio.y) >= std::abs(ratio.z)) {
                    best.normal = {0.f, ratio.y >= 0.f ? 1.f : -1.f, 0.f};
                } else {
                    best.normal = {0.f, 0.f, ratio.z >= 0.f ? 1.f : -1.f};
                }
            }
        });

    if (!best.hit) {
        best.distance = 0.f;
    }
    return best;
}

// =============================================================================
// CharacterBody
// =============================================================================

/// Fatia o
/// motion em substeps de no máximo MEIO raio (anti-túnel, teto 64) e
/// compõe sweepSphereOnce por chunk. Escreve a posição final em
/// `outResolved` (a posição inicial `from` é imutável — chamador mantém).
void sweptSphereMove(const eng::scene::Scene& scene, eng::ecs::Entity body,
                     const Vec3& from, const Vec3& motion, float radius,
                     std::uint32_t selfMask, Vec3& outResolved)
{
    const float len = std::sqrt(motion.x * motion.x + motion.y * motion.y +
                                motion.z * motion.z);
    const float maxStep = std::max(radius * 0.5f, 0.05f);
    const std::uint32_t substeps = static_cast<std::uint32_t>(
        std::min(64.0, std::max(1.0, std::ceil(
            static_cast<double>(len) / static_cast<double>(maxStep)))));
    const Vec3 chunk{motion.x / static_cast<float>(substeps),
                     motion.y / static_cast<float>(substeps),
                     motion.z / static_cast<float>(substeps)};
    outResolved = from;
    for (std::uint32_t step = 0; step < substeps; ++step) {
        outResolved = sweepSphereOnce(scene, body, outResolved, chunk,
                                      radius, selfMask);
    }
}

Vec3 PhysicsWorld::kinematicSweepMove(const eng::scene::Scene& scene,
                                      eng::ecs::Entity body, Vec3 motion)
{
    // Varredura por COLLIDER (sem CharacterBody). O raio
    // da esfera varrida: Sphere = radius; Box = círculo inscrito na
    // meia-extensão MÍNIMA (conservador — para no espaço mais apertado).
    const eng::math::Mat4 world = scene.computeWorldMatrix(body);
    const Vec3 position = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
    return kinematicSweepMoveFrom(scene, body, position, motion);
}

Vec3 PhysicsWorld::kinematicSweepMoveFrom(const eng::scene::Scene& scene,
                                          eng::ecs::Entity body,
                                          const Vec3& from, Vec3 motion)
{
    const Collider* collider = scene.world().get<Collider>(body);
    if (collider == nullptr) {
        return from + motion; // sem collider: nada a varrer (movimento cru)
    }
    float radius = collider->radius;
    if (collider->shape == ColliderShape::Box) {
        radius = std::min(collider->halfExtents.x,
                          std::min(collider->halfExtents.y,
                                   collider->halfExtents.z));
    }
    if (!(radius > 0.f)) {
        return from + motion;
    }
    Vec3 resolved = from;
    sweptSphereMove(scene, body, from, motion, radius, collider->mask,
                    resolved);
    return resolved;
}

Vec3 PhysicsWorld::moveAndSlide(const eng::scene::Scene& scene,
                                eng::ecs::Entity body, Vec3 motion)
{
    using eng::math::Vec3;
    const auto* character = scene.world().get<CharacterBody>(body);
    if (character == nullptr) {
        return motion;
    }

    // Esfera do personagem na posição atual.
    const eng::math::Mat4 world = scene.computeWorldMatrix(body);
    Vec3 position = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
    const float radius = character->radius;

    // O v1 fazia UMA passada (checava só o
    // destino — movimento > raio atravessava paredes finas). Agora o
    // motion é fatiado em substeps de no máximo meio raio; cada substep
    // projeta a penetração (sweepSphereOnce) — parar/deslizar é a
    // composição. Teto de 64 substeps: motion por chamada acima disso
    // É teletransporte por definição (documentado no §07-bindings).
    std::uint32_t selfMask = 0xFFFFFFFFu;
    if (const Collider* selfCollider = scene.world().get<Collider>(body)) {
        selfMask = selfCollider->mask;
    }
    Vec3 resolved = position;
    sweptSphereMove(scene, body, position, motion, radius, selfMask,
                    resolved);

    // Bug C-18 da auditoria final: snapToGround era serializado e nunca
    // aplicado. Semântica: com o movimento (quase) horizontal e chão a até
    // meio raio abaixo, PROJETA a esfera para pousar — personagem desce
    // rampas/degraus sem "flutuar" nos frames de queda.
    if (character->snapToGround) {
        const bool mostlyHorizontal = std::abs(motion.y) <= radius * 0.5f;
        if (mostlyHorizontal && radius > 0.f) {
            // O raio parte do CENTRO: alcance = raio (até a superfície da
            // esfera) + meio raio de folga de snap.
            const float snapDistance = radius * 1.5f;
            const auto snap = raycast(scene, resolved,
                                      Vec3{0.f, -1.f, 0.f}, snapDistance);
            if (snap.ok() && snap.value().hit) {
                const auto& hit = snap.value();
                // Só gruda em superfícies razoavelmente horizontais.
                if (hit.normal.y > 0.5f) {
                    resolved = hit.point +
                               hit.normal * (radius * 1.001f + 0.001f);
                }
            }
        }
    }
    return resolved;
}

// =============================================================================
// TimestepAccumulator
// =============================================================================

std::uint32_t TimestepAccumulator::advance(float frameDt) noexcept
{
    if (frameDt <= 0.f) {
        return 0;
    }
    carry_ += frameDt;
    std::uint32_t steps = 0;
    while (carry_ >= fixedDt_ && steps < 8u) { // clamp anti-espiral
        carry_ -= fixedDt_;
        ++steps;
    }
    if (carry_ >= fixedDt_) {
        carry_ = fixedDt_; // despeja o excesso (frame congelado)
    }
    return steps;
}

}  // namespace eng::physics
