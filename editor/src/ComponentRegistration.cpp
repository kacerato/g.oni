#include "eng/animation/Animation.hpp"

#include "eng/editor/AudioSource.hpp"
#include "eng/editor/EditorDocument.hpp"
#include "eng/editor/NiRuntime.hpp"
#include "eng/editor/NiScriptComponent.hpp"
#include "eng/editor/SpriteData.hpp"
#include "eng/editor/TextData.hpp"
#include "eng/particles/Particles.hpp"
#include "eng/render/Light2D.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/scene/Layers.hpp"
#include "eng/scene/SceneSerializer.hpp"
#include "eng/tick/Camera.hpp"

/// Registro dos componentes de GAMEPLAY:
/// physics/animation/particles entram no CATÁLOGO ÚNICO do serializer —
/// aparecem no inspector do editor e persistem em cena. O
/// registro vive no CONSUMIDOR (editor) porque engine/scene não pode
/// depender das camadas de gameplay (grafo 00-overview).
///
/// ComponentContract v2: TODO componente declara
/// categoria (grupos do Inspector) + apelido NI-Script (mesma fonte p/
/// scripts e Inspector) e, quando há efeito colateral nativo, HOOKS
/// (onAttach/onDetach/onValidate) — o caso especial "Light2D casa com a
/// camada" migrou do EditorDocument para o hook (mesma semântica do
/// P4.6 Bloco 2, testes legados continuam valendo).

namespace eng::editor {

namespace {

// --- hooks nativos -----------------------------------------

/// onAttach da Light2D: luz nova CASA COM A CAMADA onde vivem os sprites
/// lit da cena (defaults coerentes — P4.6 Bloco 2). Contagem por
/// LayerMember; material vazio = lit; material explícito só conta com
/// shader lit resolvido (cache frio conta como lit); empate = 1ª camada
/// vista — determinístico pela ordem do each.
void light2DAttach(eng::scene::Scene& scene, eng::ecs::Entity entity,
                   const eng::scene::detail::ComponentEntry& /*entry*/,
                   void* hookUser)
{
    auto* document = static_cast<EditorDocument*>(hookUser);
    auto* light = scene.world().get<eng::render::Light2D>(entity);
    if (light == nullptr) {
        return;
    }
    std::vector<std::pair<std::string, std::size_t>> counts;
    scene.world().each<eng::editor::SpriteData>(
        [&](eng::ecs::Entity sprite, const eng::editor::SpriteData& data) {
            const bool lit = document == nullptr
                                 ? true
                                 : document->materialCountsAsLit(
                                       data.materialAsset);
            if (!lit) {
                return; // unlit: luz não afeta — não conta
            }
            std::string layerName = "GAME";
            if (const auto* member =
                    scene.world().get<eng::scene::LayerMember>(sprite)) {
                layerName = member->layer;
            }
            bool found = false;
            for (auto& entry : counts) {
                if (entry.first == layerName) {
                    ++entry.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                counts.emplace_back(layerName, 1u);
            }
        });
    // Vencedor = maior contagem (empate: 1ª camada vista — determinístico).
    const std::pair<std::string, std::size_t>* winner = nullptr;
    for (const auto& entry : counts) {
        if (winner == nullptr || entry.second > winner->second) {
            winner = &entry;
        }
    }
    if (winner != nullptr) {
        light->layer = winner->first;
    }
}

/// onValidate do Collider: geometria não-negativa (raio/halfExtents).
/// Escrita inválida pelo Inspector ROLA-BACK com erro preciso; defaults
/// nunca violam (radius 0.5, halfExtents 0.5).
eng::core::Result<void> colliderValidate(
    eng::scene::Scene& scene, eng::ecs::Entity entity,
    const eng::scene::detail::ComponentEntry& /*entry*/)
{
    const auto* collider = scene.world().get<eng::physics::Collider>(entity);
    if (collider == nullptr) {
        return {}; // sem componente (rollback já removeu) — nada a validar
    }
    if (collider->radius < 0.f) {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::InvalidArgument,
            "radius negativo (" + std::to_string(collider->radius) + ")"});
    }
    if (collider->halfExtents.x < 0.f || collider->halfExtents.y < 0.f ||
        collider->halfExtents.z < 0.f) {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::InvalidArgument,
            "halfExtents negativo"});
    }
    return {};
}

/// onValidate da camada: só aceita camadas que existem na cena (senão a
/// cena salva não abriria mais).
eng::core::Result<void> layerMemberValidate(
    eng::scene::Scene& scene, eng::ecs::Entity entity,
    const eng::scene::detail::ComponentEntry& /*entry*/)
{
    const auto* member = scene.world().get<eng::scene::LayerMember>(entity);
    if (member == nullptr || scene.layers().has(member->layer)) {
        return {};
    }
    return eng::core::makeUnexpected(eng::core::Error{
        eng::core::StatusCode::InvalidArgument,
        "camada '" + member->layer + "' não existe — crie em Ajustes"});
}

const bool goni_editor_components_registered = [] {
    using eng::scene::SceneSerializer;
    using eng::scene::detail::ComponentContract;
    using Contract = eng::scene::detail::ComponentContract;

    // --- Física -----------------------------------------------------------
    {
        Contract c;
        c.category = "Física";
        c.scriptAlias = "rigidbody";
        (void)SceneSerializer::registerComponentType<eng::physics::RigidBody>(
            "eng::physics::RigidBody", std::move(c));
    }
    {
        Contract c;
        c.category = "Física";
        c.scriptAlias = "collider";
        (void)SceneSerializer::registerComponentType<eng::physics::Collider>(
            "eng::physics::Collider", std::move(c), nullptr, nullptr,
            &colliderValidate);
    }
    {
        Contract c;
        c.category = "Física";
        c.scriptAlias = "character";
        c.required = {"eng::physics::Collider"};
        c.conflicts = {"eng::physics::RigidBody"};
        (void)SceneSerializer::registerComponentType<
            eng::physics::CharacterBody>("eng::physics::CharacterBody",
                                         std::move(c));
    }
    // Conflito é BIDIRECIONAL: RigidBody também recusa CharacterBody.
    {
        eng::scene::detail::ComponentContract c;
        c.category = "Física";
        c.scriptAlias = "rigidbody";
        c.conflicts = {"eng::physics::CharacterBody"};
        eng::scene::detail::registerComponentContract(
            "eng::physics::RigidBody", std::move(c));
    }
    {
        Contract c;
        c.category = "Lógica";
        c.scriptAlias = "animator";
        (void)SceneSerializer::registerComponentType<eng::animation::Animator>(
            "eng::animation::Animator", std::move(c));
    }
    {
        Contract c;
        c.category = "FX";
        c.scriptAlias = "particles";
        (void)SceneSerializer::registerComponentType<
            eng::particles::ParticleEmitter>("eng::particles::ParticleEmitter",
                                             std::move(c));
    }
    // Evolução P0-3: sprite com textura real (workflow importar→ver na cena).
    {
        Contract c;
        c.category = "Render";
        c.scriptAlias = "sprite";
        (void)SceneSerializer::registerComponentType<eng::editor::SpriteData>(
            "eng::editor::SpriteData", std::move(c));
    }
    // Texto na cena ou preso à tela (placar, mensagens).
    {
        Contract c;
        c.category = "Render";
        c.scriptAlias = "text";
        (void)SceneSerializer::registerComponentType<eng::editor::TextData>(
            "eng::editor::TextData", std::move(c));
    }
    // Scripts NI-Script anexados a nós
    {
        Contract c;
        c.category = "Lógica";
        c.scriptAlias = "script";
        (void)SceneSerializer::registerComponentType<
            eng::editor::NiScriptComponent>("eng::editor::NiScriptComponent",
                                            std::move(c));
    }
    // Evolução P0-5: câmera de jogo como cidadã da cena +
    // membro de camada (GAME/SUBGAME/nomeadas) — Inspector/persistência/clone.
    // MÚLTIPLAS câmeras são permitidas (Play = 1ª ativa vence,
    // Bloco 4) — sem `single`.
    {
        Contract c;
        c.category = "Câmera";
        c.scriptAlias = "camera";
        (void)SceneSerializer::registerComponentType<eng::tick::CameraData>(
            "eng::tick::CameraData", std::move(c));
    }
    // LayerMember: registro DUPLO (built-in no scene + consumidor aqui) é
    // idempotente por design; o contrato é o MESMO nos dois —
    // a ordem de init estático entre TUs não altera o resultado.
    {
        Contract c;
        c.category = "Lógica";
        c.scriptAlias = "layer";
        (void)SceneSerializer::registerComponentType<eng::scene::LayerMember>(
            "eng::scene::LayerMember", std::move(c), nullptr, nullptr,
            &layerMemberValidate);
    }
    // AudioSource — áudio authorável dirigindo o AudioMixer
    // REAL (AudioTick do documento no Play).
    {
        Contract c;
        c.category = "Áudio";
        c.scriptAlias = "audio";
        (void)SceneSerializer::registerComponentType<eng::editor::AudioSource>(
            "eng::editor::AudioSource", std::move(c));
    }
    // Light2D — luz 2D real alimentando o bloco PerFrame do sprite.lit
    // (renderiza no Edit e no Play; clone via serializacao).
    // O default "casa com a camada" é HOOK nativo.
    {
        Contract c;
        c.category = "FX";
        c.scriptAlias = "light";
        (void)SceneSerializer::registerComponentType<eng::render::Light2D>(
            "eng::render::Light2D", std::move(c), &light2DAttach);
    }
    return true;
}();

}  // namespace

// (Símbolo exported para garantir que o TU entre no link — o registro é
// efeito colateral da inicialização estática; sem isso o linker pode
// descartar o objeto.)
void ensureEditorComponentsRegistered() noexcept
{
    (void)goni_editor_components_registered;
}

}  // namespace eng::editor
