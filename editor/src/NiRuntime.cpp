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
#include "eng/editor/SceneClone.hpp"
#include "eng/scene/Layers.hpp"
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
    /// Cria uma cópia ativa da entidade chamada `name` (normalmente um
    /// Molde). Sem entidade com esse nome, nasce um nó vazio. Os scripts
    /// da cópia começam a rodar no fim do tick corrente.
    eng::ecs::Entity spawn(std::string_view name) override
    {
        eng::ecs::Entity source = eng::scene::kNoEntity;
        // Prefere um Molde com o nome; senão, qualquer entidade viva.
        scene->world().each<eng::scene::Name>(
            [&](eng::ecs::Entity e, const eng::scene::Name& n) {
                if (n.value != name) {
                    return;
                }
                if (source == eng::scene::kNoEntity ||
                    (scene->world().get<eng::scene::Template>(e) != nullptr &&
                     scene->world().get<eng::scene::Template>(source) == nullptr)) {
                    source = e;
                }
            });
        eng::ecs::Entity e = eng::scene::kNoEntity;
        if (source != eng::scene::kNoEntity) {
            e = cloneSubtree(*scene, source, name, /*activate=*/true);
        } else {
            e = scene->createNode();
            (void)scene->world().emplace<eng::scene::Name>(
                e, eng::scene::Name{std::string(name)});
        }
        runtime->pendingSpawn_.push_back(e);
        return e;
    }
    /// Destruição adiada para o fim do tick: o script que se destrói
    /// termina o handler com o `self` ainda válido.
    bool despawn(eng::ecs::Entity entity) override
    {
        if (!scene->isNode(entity)) {
            return false;
        }
        auto& pending = runtime->pendingDespawn_;
        if (std::find(pending.begin(), pending.end(), entity) == pending.end()) {
            pending.push_back(entity);
        }
        return true;
    }

    // --- nativos do runtime (API de jogo) -----------------------------------

    [[nodiscard]] static HostImpl* self(eng::ni::NiExecContext& ctx,
                                        eng::ni::NiFault& fault, const char* who)
    {
        auto* host = static_cast<HostImpl*>(ctx.host());
        if (host == nullptr) {
            fault.kind = eng::ni::NiFault::Kind::NativeError;
            fault.message = std::string(who) + ": sem jogo em execução";
        }
        return host;
    }
    [[nodiscard]] static bool number(const eng::ni::NiValue& v, double& out)
    {
        if (v.type == eng::ni::NiType::Float) {
            out = v.d[0];
            return true;
        }
        if (v.type == eng::ni::NiType::Int || v.type == eng::ni::NiType::Bool) {
            out = static_cast<double>(v.i);
            return true;
        }
        return false;
    }
    static bool typeError(eng::ni::NiFault& fault, const char* message)
    {
        fault.kind = eng::ni::NiFault::Kind::Type;
        fault.message = message;
        return false;
    }
    [[nodiscard]] double nextRandom()
    {
        // xorshift64*: rápido e determinístico dada a semente.
        std::uint64_t& x = runtime->rng_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        const std::uint64_t r = x * 0x2545F4914F6CDD1Dull;
        return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
    }

    static bool fnRandom(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                         std::uint16_t argc, eng::ni::NiValue& out,
                         eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "random");
        if (h == nullptr) {
            return false;
        }
        double lo = 0.0;
        double hi = 1.0;
        if (argc == 2 && (!number(args[0], lo) || !number(args[1], hi))) {
            return typeError(fault, "random(min, max) exige números");
        }
        out = eng::ni::niFloat(lo + (hi - lo) * h->nextRandom());
        return true;
    }
    static bool fnRandomInt(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                            std::uint16_t, eng::ni::NiValue& out,
                            eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "random_int");
        if (h == nullptr) {
            return false;
        }
        double lo = 0.0;
        double hi = 0.0;
        if (!number(args[0], lo) || !number(args[1], hi)) {
            return typeError(fault, "random_int(min, max) exige números");
        }
        const auto a = static_cast<std::int64_t>(std::floor(std::min(lo, hi)));
        const auto b = static_cast<std::int64_t>(std::floor(std::max(lo, hi)));
        const auto span = static_cast<double>(b - a + 1);
        out = eng::ni::niInt(a + static_cast<std::int64_t>(h->nextRandom() * span));
        return true;
    }
    static bool fnGlobalSet(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                            std::uint16_t, eng::ni::NiValue& out,
                            eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "global_set");
        if (h == nullptr) {
            return false;
        }
        double v = 0.0;
        if (args[0].type != eng::ni::NiType::String || !number(args[1], v)) {
            return typeError(fault, "global_set(nome, número)");
        }
        h->runtime->globals_[args[0].s] = v;
        out = eng::ni::niBool(true);
        return true;
    }
    static bool fnGlobalGet(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                            std::uint16_t, eng::ni::NiValue& out,
                            eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "global_get");
        if (h == nullptr) {
            return false;
        }
        if (args[0].type != eng::ni::NiType::String) {
            return typeError(fault, "global_get(nome) exige string");
        }
        const auto it = h->runtime->globals_.find(args[0].s);
        out = eng::ni::niFloat(it == h->runtime->globals_.end() ? 0.0 : it->second);
        return true;
    }
    static bool fnRestart(eng::ni::NiExecContext& ctx, const eng::ni::NiValue*,
                          std::uint16_t, eng::ni::NiValue& out,
                          eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "restart");
        if (h == nullptr) {
            return false;
        }
        h->runtime->restartRequested_ = true;
        out = eng::ni::niBool(true);
        return true;
    }
    static bool fnTime(eng::ni::NiExecContext& ctx, const eng::ni::NiValue*,
                       std::uint16_t, eng::ni::NiValue& out, eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "time");
        if (h == nullptr) {
            return false;
        }
        out = eng::ni::niFloat(h->runtime->time_);
        return true;
    }
    template <int Side>
    static bool fnView(eng::ni::NiExecContext& ctx, const eng::ni::NiValue*,
                       std::uint16_t, eng::ni::NiValue& out, eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "view");
        if (h == nullptr) {
            return false;
        }
        const NiRuntime& r = *h->runtime;
        const float v = Side == 0   ? r.viewLeft_
                        : Side == 1 ? r.viewRight_
                        : Side == 2 ? r.viewBottom_
                                    : r.viewTop_;
        out = eng::ni::niFloat(v);
        return true;
    }
    static bool fnPlaySound(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                            std::uint16_t argc, eng::ni::NiValue& out,
                            eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "play_sound");
        if (h == nullptr) {
            return false;
        }
        double volume = 1.0;
        if (args[0].type != eng::ni::NiType::String ||
            (argc == 2 && !number(args[1], volume))) {
            return typeError(fault, "play_sound(\"som.wav\", volume?)");
        }
        const NiRuntime& r = *h->runtime;
        const bool played = r.soundPlayer_ != nullptr &&
                            r.soundPlayer_(r.soundUser_, args[0].s,
                                           static_cast<float>(volume));
        out = eng::ni::niBool(played);
        return true;
    }
    static bool fnCount(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                        std::uint16_t, eng::ni::NiValue& out, eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "count");
        if (h == nullptr) {
            return false;
        }
        if (args[0].type != eng::ni::NiType::String) {
            return typeError(fault, "count(nome) exige string");
        }
        std::int64_t n = 0;
        const auto& pending = h->runtime->pendingDespawn_;
        h->scene->world().each<eng::scene::Name>(
            [&](eng::ecs::Entity e, const eng::scene::Name& name) {
                if (name.value == args[0].s && !h->scene->isTemplated(e) &&
                    std::find(pending.begin(), pending.end(), e) == pending.end()) {
                    ++n;
                }
            });
        out = eng::ni::niInt(n);
        return true;
    }
    static bool fnLog(eng::ni::NiExecContext& ctx, const eng::ni::NiValue* args,
                      std::uint16_t, eng::ni::NiValue& out, eng::ni::NiFault& fault)
    {
        HostImpl* h = self(ctx, fault, "log");
        if (h == nullptr) {
            return false;
        }
        const eng::ni::NiValue& v = args[0];
        std::string text = "<valor>";
        if (v.type == eng::ni::NiType::String) {
            text = v.s;
        } else if (v.type == eng::ni::NiType::Int) {
            text = std::to_string(v.i);
        } else if (v.type == eng::ni::NiType::Float) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%g", v.d[0]);
            text = buf;
        } else if (v.type == eng::ni::NiType::Bool) {
            text = v.i != 0 ? "true" : "false";
        }
        ENG_INFO("[script] {}", text);
        h->runtime->stats_.lastLog = text;
        out = eng::ni::niBool(true);
        return true;
    }
    /// Primeira entidade ATIVA com o nome (moldes só se não houver outra).
    eng::ecs::Entity find(std::string_view name) const override
    {
        eng::ecs::Entity found{0xFFFFFFFFu, 0xFFFFFFFFu};
        eng::ecs::Entity templated{0xFFFFFFFFu, 0xFFFFFFFFu};
        scene->world().each<eng::scene::Name>(
            [&](eng::ecs::Entity e, const eng::scene::Name& n) {
                if (n.value != name) {
                    return;
                }
                if (scene->isTemplated(e)) {
                    if (templated.index == 0xFFFFFFFFu) {
                        templated = e;
                    }
                } else if (found.index == 0xFFFFFFFFu) {
                    found = e;
                }
            });
        return found.index != 0xFFFFFFFFu ? found : templated;
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
    if (path == "x" || path == "y" || path == "z") {
        out = eng::ni::niFloat(path == "x" ? degrees.x
                               : path == "y" ? degrees.y
                                             : degrees.z);
        return true;
    }
    out = eng::ni::niVec3(degrees.x, degrees.y, degrees.z);
    return true;
}

bool rotationSet(void* user, eng::ecs::Entity e, std::string_view path,
                 const eng::ni::NiValue& v, eng::ni::NiFault& fault)
{
    const auto* self = static_cast<const RotationAdapter*>(user);
    // `e.rotation.z = 30.0` troca só um eixo (graus).
    const bool axis = path == "x" || path == "y" || path == "z";
    const bool numeric =
        v.type == eng::ni::NiType::Float || v.type == eng::ni::NiType::Int;
    if (axis ? !numeric : v.type != eng::ni::NiType::Vec3) {
        fault.kind = eng::ni::NiFault::Kind::Type;
        fault.message = axis ? "rotation.x/y/z exige número (graus)"
                             : "rotation exige vec3 (graus)";
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
    if (axis) {
        eng::math::Vec3 degrees = degreesFromQuat(transform->rotation);
        const float value = v.type == eng::ni::NiType::Float
                                ? static_cast<float>(v.d[0])
                                : static_cast<float>(v.i);
        (path == "x" ? degrees.x : path == "y" ? degrees.y : degrees.z) = value;
        transform->rotation = quatFromDegrees(degrees);
        return true;
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
    registerNatives(natives_);
    pendingSpawn_.clear();
    pendingDespawn_.clear();
    programCache_.clear();
    globals_.clear();
    time_ = 0.f;
    restartRequested_ = false;
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
            if (scene_->isTemplated(e)) {
                // Moldes não rodam; a fonte é validada para o autor ver o
                // erro já no Play, e as cópias reusam o programa.
                (void)programFor(c.source);
                return;
            }
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
            programCache_[c.source] = program.value();
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
    time_ += deltaSeconds;
    // Pedidos feitos fora do tick (eventos da física) entram agora.
    flushPending();
    const eng::ni::NiExecContext::Params p = params();
    // Índice (não range-for): scripts podem pedir spawn durante o laço;
    // as instâncias novas só entram no flush do fim.
    const std::size_t count = set_.size();
    for (std::size_t i = 0; i < count && i < set_.size(); ++i) {
        const auto& instance = set_.asVector()[i];
        const eng::ecs::Entity owner = instance->self();
        if (!scene_->isNode(owner) ||
            !scene_->participatesIn(owner, eng::scene::LayerStage::Update)) {
            continue;
        }
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
    flushPending();
}

std::shared_ptr<const eng::ni::NiProgram> NiRuntime::programFor(
    const std::string& source)
{
    const auto it = programCache_.find(source);
    if (it != programCache_.end()) {
        return it->second;
    }
    const eng::ni::CompileOptions options{&natives_};
    std::vector<eng::ni::NiDiag> diags;
    auto program = eng::ni::compile(source, options, &diags);
    std::shared_ptr<const eng::ni::NiProgram> result;
    if (program.ok()) {
        result = program.value();
    } else if (stats_.firstCompileError.empty() && !diags.empty()) {
        char buf[192];
        std::snprintf(buf, sizeof buf, "%u:%u %s",
                      static_cast<unsigned>(diags.front().line),
                      static_cast<unsigned>(diags.front().col),
                      diags.front().message.c_str());
        stats_.firstCompileError = buf;
    }
    programCache_[source] = result;  // falha também é lembrada (nullptr)
    return result;
}

void NiRuntime::instantiateScripts(eng::ecs::Entity root)
{
    std::vector<eng::ecs::Entity> nodes{root};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        scene_->eachChild(nodes[i],
                          [&](eng::ecs::Entity child) { nodes.push_back(child); });
    }
    const eng::ni::NiExecContext::Params p = params();
    for (const eng::ecs::Entity e : nodes) {
        const auto* c = scene_->world().get<eng::editor::NiScriptComponent>(e);
        if (c == nullptr || c->source.empty() || scene_->isTemplated(e)) {
            continue;
        }
        auto program = programFor(c->source);
        if (program == nullptr) {
            ++stats_.scriptsFailed;
            continue;
        }
        eng::ni::NiScriptState& state = set_.create(std::move(program), e);
        (void)vm_.run(state, "@init", p);
        (void)vm_.run(state, "start", p);
    }
    stats_.instances = static_cast<std::uint32_t>(set_.size());
}

void NiRuntime::flushPending()
{
    if (scene_ == nullptr) {
        return;
    }
    // Um spawn pode acontecer dentro de start() de outro recém-criado:
    // repete até esvaziar (limitado para nunca travar o frame).
    for (int round = 0; round < 8 && !pendingSpawn_.empty(); ++round) {
        std::vector<eng::ecs::Entity> spawned;
        spawned.swap(pendingSpawn_);
        for (const eng::ecs::Entity e : spawned) {
            if (scene_->isNode(e)) {
                instantiateScripts(e);
            }
        }
    }
    if (!pendingDespawn_.empty()) {
        std::vector<eng::ecs::Entity> doomed;
        doomed.swap(pendingDespawn_);
        const eng::ni::NiExecContext::Params p = params();
        for (const eng::ecs::Entity e : doomed) {
            for (eng::ni::NiScriptState* state : set_.instancesOf(e)) {
                (void)vm_.run(*state, "destroy", p);
            }
            (void)scene_->destroyNode(e);
        }
        (void)set_.removeIf(
            [](void* user, eng::ecs::Entity self) {
                return !static_cast<eng::scene::Scene*>(user)->isNode(self);
            },
            scene_);
        stats_.instances = static_cast<std::uint32_t>(set_.size());
    }
}

void NiRuntime::registerNatives(eng::ni::NiNativeTable& natives_)
{
    using eng::ni::NiType;
    natives_.addBaseLibrary();
    natives_.addStandardHost();
    (void)natives_.add("random", 0, 2, &HostImpl::fnRandom, NiType::Float);
    (void)natives_.add("random_int", 2, &HostImpl::fnRandomInt, NiType::Int);
    (void)natives_.add("global_set", 2, &HostImpl::fnGlobalSet, NiType::Bool);
    (void)natives_.add("global_get", 1, &HostImpl::fnGlobalGet, NiType::Float);
    (void)natives_.add("restart", 0, &HostImpl::fnRestart, NiType::Bool);
    (void)natives_.add("time", 0, &HostImpl::fnTime, NiType::Float);
    (void)natives_.add("view_left", 0, &HostImpl::fnView<0>, NiType::Float);
    (void)natives_.add("view_right", 0, &HostImpl::fnView<1>, NiType::Float);
    (void)natives_.add("view_bottom", 0, &HostImpl::fnView<2>, NiType::Float);
    (void)natives_.add("view_top", 0, &HostImpl::fnView<3>, NiType::Float);
    (void)natives_.add("play_sound", 1, 2, &HostImpl::fnPlaySound, NiType::Bool);
    (void)natives_.add("count", 1, &HostImpl::fnCount, NiType::Int);
    (void)natives_.add("log", 1, &HostImpl::fnLog, NiType::Bool);
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
    pendingSpawn_.clear();
    pendingDespawn_.clear();
    programCache_.clear();
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
