/// NiRuntime do editor — implementação (FASE 11; ver NiRuntime.hpp).
///
/// Bindings registrados sobre o CATÁLOGO ÚNICO + reflexão (mesmo
/// mecanismo do Inspector — auditoria D2; nenhum componente hard-coded
/// aqui: a tabela deriva de eng::scene::detail::componentEntries()):
///   position / scale / transform — eng::math::Transform (refletido)
///   rotation — euler↔quat CUSTOM (convenção de graus do script)
///   name — eng::scene::Name
///   <catálogo> — alias canônico E apelido curto (rigidbody, animator...)

#include "eng/editor/NiRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eng/editor/Diagnostics.hpp"
#include "eng/editor/NiScriptComponent.hpp"
#include "eng/log/Macros.hpp"
#include "eng/math/Quat.hpp"
#include "eng/niscript/NiBindings.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/scene/Name.hpp"
#include "eng/scene/SceneSerializer.hpp"
#include "eng/tick/Camera.hpp"

namespace eng::editor {

ENG_LOG_CATEGORY("editor.ni");

NiRuntime::NiRuntime() = default;
NiRuntime::~NiRuntime() { shutdown(); }

// =============================================================================
// Host — serviços do jogo sobre o CLONE
// =============================================================================

struct NiRuntime::HostImpl final : eng::ni::NiHost {
    NiRuntime* runtime = nullptr;
    eng::scene::Scene* scene = nullptr;

    float deltaSeconds() const override { return runtime->delta_; }
    bool actionDown(std::string_view action) const override
    {
        return runtime->queryAction_(action, 0);
    }
    bool actionPressed(std::string_view action) const override
    {
        return runtime->queryAction_(action, 1);
    }
    bool actionReleased(std::string_view action) const override
    {
        return runtime->queryAction_(action, 2);
    }
    eng::ecs::Entity spawn(std::string_view name) override
    {
        const eng::ecs::Entity e = scene->createNode();
        (void)scene->world().emplace<eng::scene::Name>(
            e, eng::scene::Name{std::string(name)});
        return e;
    }
    bool despawn(eng::ecs::Entity entity) override
    {
        return scene->destroyNode(entity);
    }
    eng::ecs::Entity find(std::string_view name) const override
    {
        eng::ecs::Entity found{0xFFFFFFFFu, 0xFFFFFFFFu};
        scene->world().each<eng::scene::Name>(
            [&](eng::ecs::Entity e, const eng::scene::Name& n) {
                if (found.index == 0xFFFFFFFFu && n.value == name) {
                    found = e;
                }
            });
        return found;
    }

    // --- P4.6: movimento de gameplay -----------------------------
    // --- P4.7.0: kinematic_sweep ---------------------------------
    // ON (default): `move` de um KINEMATIC com collider vira varredura
    // (TOI+slide — o script ingênuo COLIDE; parede para e desliza). OFF
    // (ou sem kinematic/collider): translação crua — semântica pré-P4.7.
    // `teleport` é SEMPRE cru, mesmo com sweep ON (válvula de escape do
    // autor — spawn/reposicionamento atravessa por design). A ESCRITA de
    // `position` também resolve (wrap no binding — start()).

    /// Posição de MUNDO da entidade (identidade se obsoleta — o chamador
    /// valida antes de agir; o valor só alimenta deltas de varredura).
    [[nodiscard]] eng::math::Vec3 worldPositionOf(eng::ecs::Entity e) const
    {
        const eng::math::Mat4 world = scene->computeWorldMatrix(e);
        return {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
    }

    /// P4.7.0 B5: resolve a ESCRITA de `position` — se sweep ON e o corpo
    /// é KINEMATIC com collider, varre de `oldWorld` ao destino escrito
    /// (nunca varre DO destino — kinematicSweepMoveFrom) e aplica o delta
    /// de volta ao local. Pais sem rotação/escala: exato (limitação do
    /// moveAndSlide — mesma nota).
    void sweepPositionWrite(eng::ecs::Entity e,
                            const eng::math::Vec3& oldWorld)
    {
        if (!runtime->kinematicSweep_) {
            return;
        }
        const auto* body = scene->world().get<eng::physics::RigidBody>(e);
        if (body == nullptr
            || body->bodyType != eng::physics::BodyType::Kinematic
            || scene->world().get<eng::physics::Collider>(e) == nullptr) {
            return;
        }
        auto* transform = scene->localTransform(e);
        if (transform == nullptr) {
            return;
        }
        const eng::math::Vec3 newWorld = worldPositionOf(e);
        const eng::math::Vec3 resolved = eng::physics::PhysicsWorld::
            kinematicSweepMoveFrom(*scene, e, oldWorld, newWorld - oldWorld);
        if (scene->parentOf(e)
            == eng::ecs::Entity{0xFFFFFFFFu, 0xFFFFFFFFu}) {
            transform->position = resolved;
        } else {
            transform->position =
                transform->position + (resolved - newWorld);
        }
    }

    /// P4.7.0 B5: translação CRUA compartilhada (teleport + fallback do
    /// move quando não varre).
    bool rawTranslate(eng::ecs::Entity self, float dx, float dy)
    {
        auto* transform = scene->localTransform(self);
        if (transform == nullptr) {
            return false;
        }
        transform->position =
            transform->position + eng::math::Vec3{dx, dy, 0.f};
        return true;
    }

    bool translate(eng::ecs::Entity self, float dx, float dy) override
    {
        if (runtime->kinematicSweep_) {
            if (const auto* body =
                    scene->world().get<eng::physics::RigidBody>(self);
                body != nullptr && body->bodyType
                       == eng::physics::BodyType::Kinematic
                && scene->world().get<eng::physics::Collider>(self)
                       != nullptr) {
                // Varredura com deslize (substeps anti-túnel; o mask do
                // próprio corpo decide). move parte do MUNDO atual —
                // nunca do local (pais sem rotação/escala: exato).
                const eng::math::Vec3 worldBefore = worldPositionOf(self);
                const eng::math::Vec3 resolved = eng::physics::PhysicsWorld::
                    kinematicSweepMove(*scene, self,
                                       eng::math::Vec3{dx, dy, 0.f});
                auto* transform = scene->localTransform(self);
                if (transform == nullptr) {
                    return false;
                }
                if (scene->parentOf(self)
                    == eng::ecs::Entity{0xFFFFFFFFu, 0xFFFFFFFFu}) {
                    transform->position = resolved;
                } else {
                    transform->position = transform->position +
                                          (resolved - worldBefore);
                }
                return true;
            }
        }
        return rawTranslate(self, dx, dy);
    }

    bool teleport(eng::ecs::Entity self, float x, float y) override
    {
        // SEMPRE cru — NUNCA varrido (spawn atravessa por design, mesmo
        // com sweep ON; contrato NiBindings.hpp).
        return rawTranslate(self, x, y);
    }

    // --- P4.7.0: wrap da ESCRITA de `position` -------------------
    // O binding refletido escreve CRU; o wrap resolve a varredura por
    // cima (MESMA semântica do move: TOI+slide quando sweep ON — o
    // script que move por position TAMBÉM colide). O estado guarda o
    // binding refletido (delegação) — vivo pelo keepAlive do wrap.
    struct PositionSweepState {
        HostImpl* host = nullptr;
        eng::ni::NiComponentBinding prev;
    };
    /// LEITURA delega ao refletido com o USER dele (o wrapper troca o
    /// user — o ponteiro de função do refletido espera o adapter DELE,
    /// não o PositionSweepState).
    static bool positionSweepGet(void* userData, eng::ecs::Entity e,
                                 std::string_view fieldPath,
                                 eng::ni::NiValue& out, eng::ni::NiFault& fault)
    {
        auto* state = static_cast<PositionSweepState*>(userData);
        return state->prev.get(state->prev.user, e, fieldPath, out, fault);
    }
    static bool positionSweepSet(void* userData, eng::ecs::Entity e,
                                 std::string_view fieldPath,
                                 const eng::ni::NiValue& value,
                                 eng::ni::NiFault& fault)
    {
        auto* state = static_cast<PositionSweepState*>(userData);
        if (!state->host->scene->world().valid(e)) {
            // Entidade nula/obsoleta: o binding refletido faulta com
            // precisão (EntityNull/EntityStale) — nunca varrer às cegas.
            return state->prev.set(state->prev.user, e, fieldPath, value,
                                   fault);
        }
        const eng::math::Vec3 oldWorld = state->host->worldPositionOf(e);
        if (!state->prev.set(state->prev.user, e, fieldPath, value,
                             fault)) {
            return false;
        }
        state->host->sweepPositionWrite(e, oldWorld);
        return true;
    }

    bool moveAndSlide(eng::ecs::Entity self, float dx, float dy,
                      eng::math::Vec3& outPosition) override
    {
        // Preciso SEM CharacterBody → fault do verbo (nunca deslize
        // silencioso — contrato NiBindings.hpp).
        if (scene->world().get<eng::physics::CharacterBody>(self) ==
            nullptr) {
            return false;
        }
        // Mundo ANTES da varredura (o moveAndSlide parte do mundo).
        const eng::math::Mat4 before = scene->computeWorldMatrix(self);
        const eng::math::Vec3 worldBefore{before.at(3, 0), before.at(3, 1),
                                          before.at(3, 2)};
        const eng::math::Vec3 resolved = eng::physics::PhysicsWorld::
            moveAndSlide(*scene, self, eng::math::Vec3{dx, dy, 0.f});
        auto* transform = scene->localTransform(self);
        if (transform == nullptr) {
            return false;
        }
        // Sem pai: local == mundo (aplicação direta). Com pai: aplica o
        // DELTA de mundo ao local — exato para pais sem rotação/escala
        // (limitação v1 documentada; gameplay mobile move na raiz).
        if (scene->parentOf(self) == eng::ecs::Entity{0xFFFFFFFFu,
                                                      0xFFFFFFFFu}) {
            transform->position = resolved;
        } else {
            transform->position = transform->position +
                                  (resolved - worldBefore);
        }
        outPosition = resolved;
        return true;
    }

    // --- P4.7.0: câmera de jogo por script ---------------------
    // PRIMEIRA câmera ativa (mesma resolução do CameraTick — determinística).

    [[nodiscard]] eng::tick::CameraData* activeCameraData() const
    {
        eng::tick::CameraData* found = nullptr;
        scene->world().each<eng::tick::CameraData>(
            [&](eng::ecs::Entity, eng::tick::CameraData& camera) {
                if (found == nullptr && camera.active) {
                    found = &camera;
                }
            });
        return found;
    }

    bool cameraZoom(float pixelsPerUnit) override
    {
        auto* camera = activeCameraData();
        if (camera == nullptr || !(pixelsPerUnit > 0.f)) {
            return false; // sem câmera ativa / zoom inválido
        }
        camera->zoom = pixelsPerUnit;
        return true;
    }

    bool cameraPosition(float x, float y) override
    {
        auto* camera = activeCameraData();
        if (camera == nullptr) {
            return false;
        }
        camera->posX = x;
        camera->posY = y;
        return true;
    }

    bool cameraFollow(std::string_view name) override
    {
        auto* camera = activeCameraData();
        if (camera == nullptr) {
            return false;
        }
        camera->followName = std::string(name);
        return true;
    }
};

bool NiRuntime::queryAction_(std::string_view action, int phase) const
{
    if (actionQuery_ != nullptr) {
        return actionQuery_(action, phase, actionQueryUser_);
    }
    return false;
}

void NiRuntime::setActionQuery(bool (*query)(std::string_view, int,
                                             void*),
                               void* user) noexcept
{
    actionQuery_ = query;
    actionQueryUser_ = user;
}

// =============================================================================
// Ciclo de vida
// =============================================================================

namespace {

/// Euler (graus) ↔ Quat — MESMA convenção de EditorDocument (R =
/// RotY·RotX·RotZ; round-trip testado em EditorTests).
[[nodiscard]] eng::math::Quat quatFromDegrees(
    const eng::math::Vec3& degrees) noexcept
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
    return eng::math::Quat::fromEulerAngles(
        degrees.x * kDegToRad, degrees.y * kDegToRad, degrees.z * kDegToRad);
}

[[nodiscard]] eng::math::Vec3 degreesFromQuat(
    const eng::math::Quat& rotation) noexcept
{
    constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;
    const eng::math::Mat4 m = rotation.toMatrix();
    const float sinPitch = std::clamp(-m.at(2, 1), -1.f, 1.f);
    const float pitch = std::asin(sinPitch);
    float yaw = 0.f;
    float roll = 0.f;
    if (std::abs(std::cos(pitch)) > 1e-4f) {
        yaw = std::atan2(m.at(2, 0), m.at(2, 2));
        roll = std::atan2(m.at(0, 1), m.at(1, 1));
    } else {
        yaw = std::atan2(-m.at(0, 2), m.at(0, 0));
    }
    return eng::math::Vec3{pitch * kRadToDeg, yaw * kRadToDeg,
                           roll * kRadToDeg};
}

/// Binding CUSTOM de rotation (euler em GRAUS — convenção do script,
/// design §3; o genérico refletido só expõe subcampos de Quat).
struct RotationAdapter {
    eng::ecs::World* world = nullptr;
};

bool rotationGet(void* user, eng::ecs::Entity e, std::string_view path,
                 eng::ni::NiValue& out, eng::ni::NiFault& fault)
{
    (void)path; // açúcar "rotation" = campo inteiro
    const auto* self = static_cast<const RotationAdapter*>(user);
    if (e.index == 0xFFFFFFFFu) {
        fault.kind = eng::ni::NiFault::Kind::EntityNull;
        fault.message = "leitura em entidade nula";
        return false;
    }
    if (!self->world->valid(e)) {
        fault.kind = eng::ni::NiFault::Kind::EntityStale;
        fault.message = "entidade obsoleta";
        return false;
    }
    const auto* transform = self->world->get<eng::math::Transform>(e);
    if (transform == nullptr) {
        fault.kind = eng::ni::NiFault::Kind::ComponentMissing;
        fault.message = "Transform ausente na entidade";
        return false;
    }
    const eng::math::Vec3 degrees = degreesFromQuat(transform->rotation);
    out = eng::ni::niVec3(degrees.x, degrees.y, degrees.z);
    return true;
}

bool rotationSet(void* user, eng::ecs::Entity e, std::string_view path,
                 const eng::ni::NiValue& v, eng::ni::NiFault& fault)
{
    (void)path;
    const auto* self = static_cast<const RotationAdapter*>(user);
    if (v.type != eng::ni::NiType::Vec3) {
        fault.kind = eng::ni::NiFault::Kind::Type;
        fault.message = "rotation exige vec3 (graus)";
        return false;
    }
    if (e.index == 0xFFFFFFFFu) {
        fault.kind = eng::ni::NiFault::Kind::EntityNull;
        fault.message = "escrita em entidade nula";
        return false;
    }
    if (!self->world->valid(e)) {
        fault.kind = eng::ni::NiFault::Kind::EntityStale;
        fault.message = "entidade obsoleta";
        return false;
    }
    auto* transform = self->world->get<eng::math::Transform>(e);
    if (transform == nullptr) {
        fault.kind = eng::ni::NiFault::Kind::ComponentMissing;
        fault.message = "Transform ausente na entidade";
        return false;
    }
    transform->rotation = quatFromDegrees(
        eng::math::Vec3{static_cast<float>(v.d[0]),
                        static_cast<float>(v.d[1]),
                        static_cast<float>(v.d[2])});
    return true;
}

/// Fetch de entrada de catálogo (ComponentEntry é type-erased sobre
/// World — get/getMutable por ponteiro de função).
struct CatalogFetch {
    eng::ecs::World* world = nullptr;
    const eng::scene::detail::ComponentEntry* entry = nullptr;
};

const void* catalogFetchC(void* user, eng::ecs::Entity e)
{
    const auto* fetch = static_cast<const CatalogFetch*>(user);
    return fetch->entry->get(*fetch->world, e);
}

void* catalogFetchM(void* user, eng::ecs::Entity e)
{
    const auto* fetch = static_cast<const CatalogFetch*>(user);
    return fetch->entry->getMutable(*fetch->world, e);
}

bool worldValid(void* user, eng::ecs::Entity e)
{
    return static_cast<eng::ecs::World*>(user)->valid(e);
}

bool catalogValid(void* user, eng::ecs::Entity e)
{
    return static_cast<const CatalogFetch*>(user)->world->valid(e);
}

} // namespace

void NiRuntime::start(eng::scene::Scene& runtimeScene)
{
    shutdown();
    scene_ = &runtimeScene;
    natives_.addBaseLibrary();
    natives_.addStandardHost();
    host_ = std::make_unique<HostImpl>();
    host_->runtime = this;
    host_->scene = scene_;

    auto* world = &scene_->world();

    const auto fetchT = [](void* user, eng::ecs::Entity e) -> const void* {
        return static_cast<eng::ecs::World*>(user)
            ->get<eng::math::Transform>(e);
    };
    const auto fetchTm = [](void* user, eng::ecs::Entity e) -> void* {
        return static_cast<eng::ecs::World*>(user)
            ->get<eng::math::Transform>(e);
    };
    (void)eng::ni::niAddReflectionBinding(
        bindings_, "position", "eng::math::Transform", fetchT, fetchTm,
        world, "position", &worldValid);
    // ESCRITA de `position` com varredura — o ÚLTIMO
    // binding "position" vence (contrato NiBindingTable) e o wrap delega
    // ao refletido guardado em prev (vivo pelo keepAlive do wrap).
    if (const auto* reflected = bindings_.find("position");
        reflected != nullptr && reflected->set != nullptr) {
        auto sweepState =
            std::make_shared<HostImpl::PositionSweepState>();
        sweepState->host = host_.get();
        sweepState->prev = *reflected;
        eng::ni::NiComponentBinding sweepBinding;
        sweepBinding.alias = "position";
        sweepBinding.user = sweepState.get();
        sweepBinding.keepAlive = sweepState;
        sweepBinding.get = &HostImpl::positionSweepGet;
        sweepBinding.set = &HostImpl::positionSweepSet;
        bindings_.add(std::move(sweepBinding));
    }
    (void)eng::ni::niAddReflectionBinding(
        bindings_, "scale", "eng::math::Transform", fetchT, fetchTm, world,
        "scale", &worldValid);
    (void)eng::ni::niAddReflectionBinding(
        bindings_, "transform", "eng::math::Transform", fetchT, fetchTm,
        world, "", &worldValid);
    (void)eng::ni::niAddReflectionBinding(
        bindings_, "name", "eng::scene::Name",
        [](void* user, eng::ecs::Entity e) -> const void* {
            return static_cast<eng::ecs::World*>(user)
                ->get<eng::scene::Name>(e);
        },
        [](void* user, eng::ecs::Entity e) -> void* {
            return static_cast<eng::ecs::World*>(user)
                ->get<eng::scene::Name>(e);
        },
        world, "value", &worldValid);

    auto rotationState = std::make_shared<RotationAdapter>();
    rotationState->world = world;
    {
        eng::ni::NiComponentBinding binding;
        binding.alias = "rotation";
        binding.get = &rotationGet;
        binding.set = &rotationSet;
        binding.user = rotationState.get();
        binding.keepAlive = rotationState;
        bindings_.add(std::move(binding));
    }

    for (const auto& [typeName, entry] :
         eng::scene::detail::componentEntries()) {
        auto fetch = std::make_shared<CatalogFetch>();
        fetch->world = world;
        fetch->entry = &entry;
        bindingState_.push_back(fetch);
        (void)eng::ni::niAddReflectionBinding(
            bindings_, typeName, typeName, &catalogFetchC, &catalogFetchM,
            fetch.get(), "", &catalogValid);
        // Apelido do CONTRATO (fonte única — o mesmo
        // registro alimenta Inspector e scripts) e o legado (última
        // parte do nome canônico em minúscula) CONTINUA valendo —
        // scripts de fases anteriores nunca quebram.
        std::string legacy;
        const std::size_t colon = typeName.rfind(':');
        const std::string_view tail =
            colon == std::string::npos
                ? std::string_view(typeName)
                : std::string_view(typeName).substr(colon + 1);
        for (const char c : tail) {
            legacy.push_back(static_cast<char>(
                c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        }
        const std::string& alias = entry.contract.scriptAlias;
        const std::string* const candidates[] = {&alias, &legacy};
        for (const std::string* candidate : candidates) {
            if (candidate->empty() || *candidate == "name"
                || *candidate == "transform" || *candidate == typeName) {
                continue;
            }
            auto fetch2 = std::make_shared<CatalogFetch>();
            fetch2->world = world;
            fetch2->entry = &entry;
            bindingState_.push_back(fetch2);
            (void)eng::ni::niAddReflectionBinding(
                bindings_, *candidate, typeName, &catalogFetchC,
                &catalogFetchM, fetch2.get(), "", &catalogValid);
        }
    }

    // Inscreve os eventos de gameplay no barramento da
    // cena — on_hit (física), on_enter/on_exit (triggers) e
    // on_visible/on_invisible (culling do Bloco 6 publica). As inscrições
    // são RAII e vivem APENAS até o shutdown (nunca sobrevivem à cena).
    subscribeGameEvent<eng::scene::HitEvent>();
    subscribeGameEvent<eng::scene::TriggerEvent>();
    subscribeGameEvent<eng::scene::VisibilityEvent>();

    // Compila + instancia scripts do CLONE (ordem determinística do each).
    // TODOS os resultados vão para stats_ (fonte da UI —
    // toast/painel do editor) E para o diagnóstico persistido (marcos
    // SCRIPT_* — a forense do device passa a mostrar porquê um script
    // "não faz nada"). O silêncio do P3.5 era o defeito D5.
    stats_ = NiScriptStats{};
    const eng::ni::CompileOptions options{&natives_};
    scene_->world().each<eng::editor::NiScriptComponent>(
        [&](eng::ecs::Entity e, const eng::editor::NiScriptComponent& c) {
            if (c.source.empty()) {
                return;
            }
            ++stats_.scriptsFound;
            std::vector<eng::ni::NiDiag> diags;
            auto program = eng::ni::compile(c.source, options, &diags);
            if (!program.ok()) {
                ++stats_.scriptsFailed;
                ENG_ERROR(
                    "ni-script: compilacao falhou na entidade {} ({} "
                    "erro(s))",
                    e.index, diags.size());
                for (const eng::ni::NiDiag& d : diags) {
                    ENG_ERROR("  {}:{} {}", d.line, d.col, d.message);
                }
                if (stats_.firstCompileError.empty() && !diags.empty()) {
                    const eng::ni::NiDiag& d = diags.front();
                    char buf[192];
                    std::snprintf(buf, sizeof buf, "%u:%u %s",
                                  static_cast<unsigned>(d.line),
                                  static_cast<unsigned>(d.col),
                                  d.message.c_str());
                    stats_.firstCompileError = buf;
                    stats_.firstFailedEntity = e.index;
                    diag::mark("SCRIPT_COMPILE", "failed", buf);
                }
                return;
            }
            ++stats_.scriptsCompiled;
            set_.create(std::move(program).value(), e);
        });
    stats_.instances =
        static_cast<std::uint32_t>(set_.size());
    if (stats_.scriptsFound > 0 && stats_.scriptsFailed == 0) {
        diag::mark("SCRIPT_COMPILE", "ok",
                   "todos os scripts compilaram");
    }
    if (stats_.scriptsFound == 0) {
        diag::mark("SCRIPT_COMPILE", "skipped", "nenhum script na cena");
    }

    // @init de TODAS as instâncias (ordem de criação — docs/ni-script/07)
    const eng::ni::NiExecContext::Params p = params();
    for (const auto& instance : set_.asVector()) {
        (void)vm_.run(*instance, "@init", p);
    }
}

void NiRuntime::fireStart()
{
    const eng::ni::NiExecContext::Params p = params();
    for (const auto& instance : set_.asVector()) {
        (void)vm_.run(*instance, "start", p);
    }
}

void NiRuntime::tick(float deltaSeconds)
{
    delta_ = deltaSeconds;
    const eng::ni::NiExecContext::Params p = params();
    for (const auto& instance : set_.asVector()) {
        // LOGIC LOD — filtro (opcional) decide se o
        // script roda neste frame. O callback vê CENA e câmera (o host
        // instala); um script opt-out (lodOptOut do NiScriptComponent)
        // é responsabilidade DO FILTRO (ele tem a cena) — o runtime é
        // burro de propósito: filter false = pula SEM contar tick.
        if (lodFilter_ != nullptr && !lodFilter_(lodFilterUser_,
                                                 instance->self())) {
            continue;
        }
        (void)vm_.run(*instance, "update", p);
        // Contagem VISÍVEL de ticks + faults — o editor
        // mostra "N scripts, T ticks" e o ÚLTIMO fault do runtime; com
        // isto o autor distingue "script compila mas não roda" de
        // "roda e falha no binding".
        ++stats_.ticks;
        if (stats_.firstUpdateTick == 0) {
            stats_.firstUpdateTick = stats_.ticks;
        }
        if (const std::optional<eng::ni::NiFault>& fault =
                instance->lastFault();
            fault.has_value()) {
            ++stats_.faults;
            char buf[192];
            std::snprintf(buf, sizeof buf, "%s @ entidade %u",
                          fault->message.c_str(),
                          static_cast<unsigned>(instance->self().index));
            stats_.lastFaultMessage = buf;
            diag::mark("SCRIPT_FAULT", "runtime", buf);
        }
    }
}

void NiRuntime::shutdown() noexcept
{
    if (!set_.empty() && scene_ != nullptr) {
        const eng::ni::NiExecContext::Params p = params();
        for (const auto& instance : set_.asVector()) {
            (void)vm_.run(*instance, "destroy", p);
        }
    }
    set_.clear();
    bindings_ = eng::ni::NiBindingTable{};
    bindingState_.clear();
    natives_ = eng::ni::NiNativeTable{};
    // Cancela as inscrições de eventos ANTES de soltar a
    // cena (Subscription nunca sobrevive ao bus — ADR-022).
    eventSubscriptions_.clear();
    scene_ = nullptr;
    host_.reset();
}

std::vector<const eng::ni::NiScriptState*> NiRuntime::instances() const
{
    std::vector<const eng::ni::NiScriptState*> result;
    for (const auto& instance : set_.asVector()) {
        result.push_back(instance.get());
    }
    return result;
}

eng::ni::NiExecContext::Params NiRuntime::params() const
{
    eng::ni::NiExecContext::Params p;
    p.natives = &natives_;
    p.host = host_.get();
    p.bindings = &bindings_;
    // const: o VM apenas LÊ o conjunto (propagação de emit — §4); os
    // globais/links das instâncias pertencem a cada NiScriptState.
    p.set = const_cast<eng::ni::NiInstanceSet*>(&set_);
    p.budget = eng::ni::kDefaultBudget;
    return p;
}

// =============================================================================
// Bridge de eventos de gameplay → NI-Script
// =============================================================================

void NiRuntime::runHandlerOn(eng::ecs::Entity self, std::string_view handler)
{
    if (dispatching_) {
        return; // reentrância: evento dentro de handler NÃO re-despacha
    }
    dispatching_ = true;
    struct DepthGuard {
        bool& flag;
        ~DepthGuard() { flag = false; }
    } guard{dispatching_};

    const eng::ni::NiExecContext::Params p = params();
    for (const auto& instance : set_.asVector()) {
        if (instance->self().index != self.index
            || instance->self().generation != self.generation) {
            continue; // handler roda APENAS no self do evento
        }
        (void)vm_.run(*instance, handler, p);
        if (const std::optional<eng::ni::NiFault>& fault =
                instance->lastFault();
            fault.has_value()) {
            ++stats_.faults;
            char buf[192];
            std::snprintf(buf, sizeof buf, "%s @ entidade %u",
                          fault->message.c_str(),
                          static_cast<unsigned>(instance->self().index));
            stats_.lastFaultMessage = buf;
            diag::mark("SCRIPT_FAULT", "runtime", buf);
        }
    }
}

void NiRuntime::dispatchHitEvent(const eng::scene::HitEvent& event)
{
    runHandlerOn(event.self, eng::scene::kEventHit);
}

void NiRuntime::dispatchTriggerEvent(const eng::scene::TriggerEvent& event)
{
    runHandlerOn(event.self, event.entered ? eng::scene::kEventTriggerEnter
                                           : eng::scene::kEventTriggerExit);
}

void NiRuntime::dispatchVisibilityEvent(
    const eng::scene::VisibilityEvent& event)
{
    runHandlerOn(event.entity, event.visible ? eng::scene::kEventVisible
                                             : eng::scene::kEventInvisible);
}

} // namespace eng::editor
