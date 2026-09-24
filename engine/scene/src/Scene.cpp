#include "eng/scene/Scene.hpp"

#include <algorithm>

namespace eng::scene {

eng::ecs::Entity Scene::createNode()
{
    const eng::ecs::Entity node = world_.create();
    (void)world_.emplace<Hierarchy>(node);             // raiz, sem filhos
    (void)world_.emplace<eng::math::Transform>(node);  // identidade
    return node;
}

bool Scene::attach(eng::ecs::Entity child, eng::ecs::Entity parent)
{
    if (!isNode(child) || !isNode(parent) || child == parent) {
        return false;
    }
    // Ciclo: child não pode ser ancestral de parent (subindo a cadeia de
    // parent). Ancestral obsoleto (bypass) encerra a subida — não é ciclo.
    for (eng::ecs::Entity ancestor = parentOf(parent);
         ancestor != kNoEntity;
         ancestor = parentOf(ancestor)) {
        if (ancestor == child) {
            return false;
        }
        if (!world_.valid(ancestor)) {
            break;
        }
    }

    Hierarchy* childHierarchy = hierarchyOf(child);
    if (childHierarchy->parent == parent) {
        return true; // idempotente: já está sob parent
    }
    if (childHierarchy->parent != kNoEntity && world_.valid(childHierarchy->parent)) {
        removeFromChildren(childHierarchy->parent, child);
    }
    childHierarchy->parent = parent;
    hierarchyOf(parent)->children.push_back(child);
    return true;
}

bool Scene::detach(eng::ecs::Entity node)
{
    if (!isNode(node)) {
        return false;
    }
    Hierarchy* hierarchy = hierarchyOf(node);
    if (hierarchy->parent == kNoEntity) {
        return false; // já é raiz
    }
    if (world_.valid(hierarchy->parent)) {
        removeFromChildren(hierarchy->parent, node);
    }
    hierarchy->parent = kNoEntity;
    return true;
}

bool Scene::destroyNode(eng::ecs::Entity node)
{
    if (!isNode(node)) {
        return false; // handle obsoleto: no-op seguro
    }
    const Hierarchy* hierarchy = hierarchyOf(node);
    if (hierarchy->parent != kNoEntity && world_.valid(hierarchy->parent)) {
        removeFromChildren(hierarchy->parent, node); // pai e irmãos sobrevivem
    }

    const std::vector<eng::ecs::Entity> subtree = collectSubtree(node);
    // Links das entidades da subárvore morrem JUNTOS — antes do
    // world_.destroy, para que consultas durante a destruição não vejam
    // pontas mortas.
    for (const eng::ecs::Entity member : subtree) {
        (void)links_.destroyAllFor(member);
    }
    // Folhas primeiro (ordem reversa da coleta por nível): determinístico e
    // mantém os handles válidos o máximo possível durante a destruição.
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
        (void)world_.destroy(*it);
    }
    return true;
}

eng::ecs::Entity Scene::parentOf(eng::ecs::Entity node) const noexcept
{
    const Hierarchy* hierarchy = hierarchyOf(node);
    return hierarchy != nullptr ? hierarchy->parent : kNoEntity;
}

std::size_t Scene::childCount(eng::ecs::Entity parent) const
{
    const Hierarchy* hierarchy = hierarchyOf(parent);
    if (hierarchy == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const eng::ecs::Entity child : hierarchy->children) {
        if (isNode(child)) {
            ++count;
        }
    }
    return count;
}

eng::math::Transform* Scene::localTransform(eng::ecs::Entity node)
{
    return isNode(node) ? world_.get<eng::math::Transform>(node) : nullptr;
}

const eng::math::Transform* Scene::localTransform(eng::ecs::Entity node) const
{
    return isNode(node) ? world_.get<eng::math::Transform>(node) : nullptr;
}

void Scene::updateWorldTransforms()
{
    // Raízes: pai ausente (kNoEntity) ou obsoleto (bypass — tratado como raiz).
    std::vector<eng::ecs::Entity> roots;
    world_.each<Hierarchy>([&](eng::ecs::Entity node, const Hierarchy& hierarchy) {
        if (hierarchy.parent == kNoEntity || !world_.valid(hierarchy.parent)) {
            roots.push_back(node);
        }
    });

    // Descida iterativa (sem recursão): pilha de (nó, mundo do pai).
    std::vector<std::pair<eng::ecs::Entity, eng::math::Mat4>> pending;
    for (const eng::ecs::Entity root : roots) {
        pending.emplace_back(root, eng::math::Mat4::identity());
    }
    while (!pending.empty()) {
        const auto [node, parentWorld] = pending.back();
        pending.pop_back();

        const eng::math::Transform* local = world_.get<eng::math::Transform>(node);
        const eng::math::Mat4 world =
            parentWorld
            * (local != nullptr ? local->toMatrix() : eng::math::Mat4::identity());
        (void)world_.emplace<WorldMatrix>(node, world);

        const Hierarchy* hierarchy = hierarchyOf(node);
        if (hierarchy == nullptr) {
            continue;
        }
        for (const eng::ecs::Entity child : hierarchy->children) {
            if (isNode(child)) {
                pending.emplace_back(child, world);
            }
        }
    }
}

const eng::math::Mat4* Scene::worldMatrix(eng::ecs::Entity node) const
{
    const WorldMatrix* cached = world_.get<WorldMatrix>(node);
    return cached != nullptr ? &cached->matrix : nullptr;
}

eng::math::Mat4 Scene::computeWorldMatrix(eng::ecs::Entity node) const
{
    if (!isNode(node)) {
        return eng::math::Mat4::identity();
    }
    eng::math::Mat4 world = world_.get<eng::math::Transform>(node)->toMatrix();
    for (eng::ecs::Entity ancestor = parentOf(node);
         ancestor != kNoEntity && isNode(ancestor);
         ancestor = parentOf(ancestor)) {
        const eng::math::Transform* local =
            world_.get<eng::math::Transform>(ancestor);
        world = local->toMatrix() * world;
    }
    return world;
}

std::size_t Scene::nodeCount() const noexcept
{
    return world_.componentCount<Hierarchy>();
}

bool Scene::isNode(eng::ecs::Entity node) const noexcept
{
    return world_.valid(node) && world_.has<Hierarchy>(node);
}

Hierarchy* Scene::hierarchyOf(eng::ecs::Entity node) noexcept
{
    return world_.valid(node) ? world_.get<Hierarchy>(node) : nullptr;
}

const Hierarchy* Scene::hierarchyOf(eng::ecs::Entity node) const noexcept
{
    return world_.valid(node) ? world_.get<Hierarchy>(node) : nullptr;
}

void Scene::removeFromChildren(eng::ecs::Entity parent, eng::ecs::Entity child)
{
    Hierarchy* hierarchy = hierarchyOf(parent);
    if (hierarchy == nullptr) {
        return;
    }
    // Apaga o alvo e, de passagem, entradas obsoletas (limpeza oportunista).
    hierarchy->children.erase(
        std::remove_if(hierarchy->children.begin(), hierarchy->children.end(),
                       [&](const eng::ecs::Entity candidate) {
                           return candidate == child || !isNode(candidate);
                       }),
        hierarchy->children.end());
}

std::vector<eng::ecs::Entity> Scene::collectSubtree(eng::ecs::Entity node) const
{
    std::vector<eng::ecs::Entity> subtree;
    subtree.push_back(node);
    for (std::size_t i = 0; i < subtree.size(); ++i) {
        const Hierarchy* hierarchy = hierarchyOf(subtree[i]);
        if (hierarchy == nullptr) {
            continue;
        }
        for (const eng::ecs::Entity child : hierarchy->children) {
            if (isNode(child)) {
                subtree.push_back(child);
            }
        }
    }
    return subtree;
}

// =============================================================================
// Links tipados (evolução P0-5, ADR-051)
// =============================================================================

eng::core::Result<LinkId> Scene::createLink(std::string_view type,
                                            eng::ecs::Entity from,
                                            eng::ecs::Entity to)
{
    using eng::core::Error;
    using eng::core::StatusCode;

    if (!isNode(from) || !isNode(to)) {
        return eng::core::makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "Scene::createLink: ponta do link não é um nó vivo"});
    }
    return links_.create(type, from, to);
}

bool Scene::destroyLink(LinkId id)
{
    return links_.destroy(id);
}

std::size_t Scene::sweepLinks()
{
    return links_.sweep(world_);
}

// =============================================================================
// Camadas (evolução P0-5, ADR-051)
// =============================================================================

eng::core::Result<void> Scene::removeLayer(std::string_view name)
{
    using eng::core::Error;
    using eng::core::StatusCode;

    if (!layers_.has(name)) {
        return eng::core::makeUnexpected(Error{
            StatusCode::NotFound,
            "Scene::removeLayer: camada '" + std::string(name) +
                "' não existe"});
    }
    // Remoção com entidades usando a camada é REJEITADA: sem
    // fallback silencioso para GAME. Varredura dos LayerMember vivos.
    bool inUse = false;
    world_.each<LayerMember>(
        [&](eng::ecs::Entity /*e*/, const LayerMember& member) {
            if (member.layer == name) {
                inUse = true;
            }
        });
    if (inUse) {
        return eng::core::makeUnexpected(Error{
            StatusCode::InvalidState,
            "Scene::removeLayer: camada '" + std::string(name) +
                "' ainda é usada por entidades"});
    }
    return layers_.remove(name);
}

std::string_view Scene::layerOf(eng::ecs::Entity node) const noexcept
{
    const LayerMember* member =
        world_.valid(node) ? world_.get<LayerMember>(node) : nullptr;
    if (member != nullptr) {
        if (const LayerDefinition* definition = layers_.find(member->layer)) {
            return definition->name;
        }
    }
    return LayerRegistry::kGame;  // default (e defensivo p/ órfãos)
}

bool Scene::isTemplated(eng::ecs::Entity node) const noexcept
{
    for (eng::ecs::Entity n = node; n != kNoEntity && world_.valid(n);
         n = parentOf(n)) {
        if (world_.get<Template>(n) != nullptr) {
            return true;
        }
    }
    return false;
}

bool Scene::participatesIn(eng::ecs::Entity node,
                           LayerStage stage) const noexcept
{
    return !isTemplated(node) && layerParticipates(node, stage);
}

bool Scene::layerParticipates(eng::ecs::Entity node,
                              LayerStage stage) const noexcept
{
    const LayerMember* member =
        world_.valid(node) ? world_.get<LayerMember>(node) : nullptr;
    const LayerDefinition* definition =
        member != nullptr ? layers_.find(member->layer) : nullptr;
    if (definition == nullptr) {
        return true;  // sem LayerMember/camada ausente → GAME default
    }
    switch (stage) {
    case LayerStage::Update:
        return definition->participation.update;
    case LayerStage::Physics:
        return definition->participation.physics;
    case LayerStage::Render:
        return definition->participation.render;
    }
    return true;  // inalcançável (todos os estágios acima)
}

float Scene::timeScaleOf(eng::ecs::Entity node) const noexcept
{
    const LayerMember* member =
        world_.valid(node) ? world_.get<LayerMember>(node) : nullptr;
    const LayerDefinition* definition =
        member != nullptr ? layers_.find(member->layer) : nullptr;
    return definition != nullptr ? definition->timeScale : 1.f;
}

} // namespace eng::scene
