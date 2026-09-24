#include "eng/editor/EditorProtocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "eng/editor/AssetBrowser.hpp"
#include "eng/editor/EditorDocument.hpp"
#include "eng/editor/EditorHost.hpp"
#include "eng/editor/Inspector.hpp"
#include "eng/editor/NiRuntime.hpp"
#include "eng/editor/TextureCache.hpp"
#include "eng/serial/Json.hpp"

namespace eng::editor {

namespace {

using Json = nlohmann::json;
using eng::core::Result;

// --- leitura tolerante de argumentos (sem exceções: tipo errado = default) --

const Json* arg(const Json& req, const char* key)
{
    if (!req.is_object()) {
        return nullptr;
    }
    const auto it = req.find(key);
    return it == req.end() ? nullptr : &*it;
}

std::string str(const Json& req, const char* key, std::string fallback = {})
{
    const Json* v = arg(req, key);
    return (v != nullptr && v->is_string()) ? v->get_ref<const std::string&>()
                                            : fallback;
}

double num(const Json& req, const char* key, double fallback = 0.0)
{
    const Json* v = arg(req, key);
    return (v != nullptr && v->is_number()) ? v->get<double>() : fallback;
}

bool flag(const Json& req, const char* key, bool fallback = false)
{
    const Json* v = arg(req, key);
    return (v != nullptr && v->is_boolean()) ? v->get<bool>() : fallback;
}

bool has(const Json& req, const char* key) { return arg(req, key) != nullptr; }

std::uint64_t u64(const Json& req, const char* key)
{
    const Json* v = arg(req, key);
    if (v == nullptr) {
        return 0;
    }
    if (v->is_number_unsigned()) {
        return v->get<std::uint64_t>();
    }
    if (v->is_number_integer()) {
        const auto i = v->get<std::int64_t>();
        return i < 0 ? 0 : static_cast<std::uint64_t>(i);
    }
    if (v->is_string()) {
        return std::strtoull(v->get_ref<const std::string&>().c_str(), nullptr,
                             10);
    }
    return 0;
}

eng::ecs::Entity entityArg(const Json& req, const char* key = "id")
{
    return EditorDocument::unpackEntity(u64(req, key));
}

bool vec3(const Json& req, const char* key, eng::math::Vec3& out)
{
    const Json* v = arg(req, key);
    if (v == nullptr || !v->is_array() || v->size() != 3) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(*v)[i].is_number()) {
            return false;
        }
    }
    out = eng::math::Vec3{(*v)[0].get<float>(), (*v)[1].get<float>(),
                          (*v)[2].get<float>()};
    return true;
}

// --- respostas --------------------------------------------------------------

std::string dump(const Json& j)
{
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

struct Reply {
    bool ok = true;
    std::string error;
    Json result;
};

Reply ok(Json result = nullptr) { return Reply{true, {}, std::move(result)}; }
Reply fail(std::string message) { return Reply{false, std::move(message), {}}; }

template <typename T>
Reply fromResult(const Result<T>& r)
{
    if (r.isError()) {
        return fail(r.error().message);
    }
    return ok();
}

Json packed(eng::ecs::Entity e)
{
    return EditorDocument::packEntity(e);
}

// --- rótulos amigáveis --------------------------------------------------------

std::string shortName(std::string_view canonical)
{
    static const std::unordered_map<std::string_view, const char*> kNames{
        {"eng::math::Transform", "Transform"},
        {"eng::scene::Name", "Nome"},
        {"eng::scene::LayerMember", "Camada"},
        {"eng::editor::SpriteData", "Sprite"},
        {"eng::physics::RigidBody", "Corpo rígido"},
        {"eng::physics::Collider", "Colisor"},
        {"eng::physics::CharacterBody", "Personagem"},
        {"eng::animation::Animator", "Animador"},
        {"eng::particles::ParticleEmitter", "Partículas"},
        {"eng::editor::NiScriptComponent", "Script"},
        {"eng::tick::CameraData", "Câmera"},
        {"eng::editor::AudioSource", "Som"},
        {"eng::render::Light2D", "Luz 2D"},
        {"eng::editor::TextData", "Texto"},
        {"eng::scene::Template", "Molde"},
    };
    const auto it = kNames.find(canonical);
    if (it != kNames.end()) {
        return it->second;
    }
    const auto pos = canonical.rfind("::");
    return std::string(pos == std::string_view::npos ? canonical
                                                     : canonical.substr(pos + 2));
}

/// Ícone da entidade na hierarquia (o componente mais "visível" vence).
std::string entityKind(const std::vector<std::string>& components)
{
    auto hasC = [&](std::string_view c) {
        return std::find(components.begin(), components.end(), c) !=
               components.end();
    };
    if (hasC("eng::tick::CameraData")) {
        return "camera";
    }
    if (hasC("eng::physics::CharacterBody") ||
        (hasC("eng::editor::NiScriptComponent") &&
         hasC("eng::physics::RigidBody"))) {
        return "character";
    }
    if (hasC("eng::editor::TextData")) {
        return "text";
    }
    if (hasC("eng::render::Light2D")) {
        return "light";
    }
    if (hasC("eng::particles::ParticleEmitter")) {
        return "particles";
    }
    if (hasC("eng::editor::AudioSource")) {
        return "audio";
    }
    if (hasC("eng::editor::SpriteData")) {
        return "sprite";
    }
    if (hasC("eng::editor::NiScriptComponent")) {
        return "script";
    }
    return "empty";
}

/// Kind de UI refinado: alguns inteiros são máscaras de camada.
std::string uiKind(std::string_view component, const Inspector::Field& f)
{
    if (component == "eng::physics::Collider" &&
        (f.path == "layer" || f.path == "mask")) {
        return "bitfield";
    }
    if ((component == "eng::render::Light2D" ||
         component == "eng::scene::LayerMember") &&
        f.path == "layer") {
        return "layer";
    }
    return f.kind.empty() ? "text" : f.kind;
}

// --- modelos --------------------------------------------------------------------

constexpr const char* kPlayerScript =
    "# Jogador de plataforma.\n"
    "# Toque: lado esquerdo anda, lado direito pula.\n"
    "# Teclado: A/D ou setas para andar, Espaço para pular.\n"
    "add &BL\n"
    "\n"
    "var velocidade = 4.0\n"
    "var forcaPulo = 7.5\n"
    "\n"
    "up update:\n"
    "    var me = self()\n"
    "    var vx = 0.0\n"
    "    if action_down(\"left\"):\n"
    "        vx = vx - velocidade\n"
    "    stop\n"
    "    if action_down(\"right\"):\n"
    "        vx = vx + velocidade\n"
    "    stop\n"
    "    me.rigidbody.velocity.x = vx\n"
    "    var vy: float = me.rigidbody.velocity.y\n"
    "    if action_pressed(\"jump\"):\n"
    "        if abs(vy) < 0.05:\n"
    "            me.rigidbody.velocity.y = forcaPulo\n"
    "        stop\n"
    "    stop\n"
    "stop\n";

constexpr const char* kBirdScript =
    "# Pássaro: toque para voar e passe entre os canos.\n"
    "add &BL\n"
    "\n"
    "var forca = 7.0\n"
    "var vivo = true\n"
    "var comecou = false\n"
    "var pontos = 0\n"
    "\n"
    "up update:\n"
    "    var me = self()\n"
    "    if action_pressed(\"tap\"):\n"
    "        if vivo:\n"
    "            if not comecou:\n"
    "                comecou = true\n"
    "                me.rigidbody.useGravity = true\n"
    "                global_set(\"jogando\", 1)\n"
    "                var msg = find(\"Mensagem\")\n"
    "                msg.text.text = \"\"\n"
    "            stop\n"
    "            me.rigidbody.velocity.y = forca\n"
    "            play_sound(\"asa.wav\")\n"
    "        stop\n"
    "        else:\n"
    "            restart()\n"
    "        stop\n"
    "    stop\n"
    "    # Inclina o pássaro conforme sobe ou cai.\n"
    "    var vy: float = me.rigidbody.velocity.y\n"
    "    me.rotation.z = clamp(vy * 5.0, -70.0, 30.0)\n"
    "stop\n"
    "\n"
    "up on_hit:\n"
    "    if vivo:\n"
    "        vivo = false\n"
    "        global_set(\"jogando\", 0)\n"
    "        var msg = find(\"Mensagem\")\n"
    "        msg.text.text = \"FIM!\\nTOQUE PARA JOGAR\"\n"
    "    stop\n"
    "stop\n"
    "\n"
    "up on_enter:\n"
    "    if vivo:\n"
    "        pontos = pontos + 1\n"
    "        var placar = find(\"Placar\")\n"
    "        placar.text.text = str(pontos)\n"
    "    stop\n"
    "stop\n";

constexpr const char* kPipeScript =
    "# Cano: anda para a esquerda enquanto o jogo roda e some fora da tela.\n"
    "var velocidade = 2.4\n"
    "\n"
    "up update:\n"
    "    var me = self()\n"
    "    if global_get(\"jogando\") > 0.5:\n"
    "        me.position.x = me.position.x - velocidade * delta()\n"
    "    stop\n"
    "    if me.position.x < view_left() - 2.0:\n"
    "        despawn(me)\n"
    "    stop\n"
    "stop\n";

constexpr const char* kPipeSpawnerScript =
    "# Gerador: um cano novo a cada intervalo, com o vão em altura aleatória.\n"
    "var intervalo = 1.7\n"
    "var espera = 0.0\n"
    "\n"
    "up update:\n"
    "    if global_get(\"jogando\") > 0.5:\n"
    "        espera = espera - delta()\n"
    "        if espera <= 0.0:\n"
    "            espera = intervalo\n"
    "            var cano = spawn(\"Cano\")\n"
    "            cano.position.x = view_right() + 1.5\n"
    "            cano.position.y = random(-1.5, 2.0)\n"
    "        stop\n"
    "    stop\n"
    "stop\n";

constexpr const char* kBoxSpawnerScript =
    "# Gerador: solta uma caixa a cada toque (e sozinho, de tempos em tempos).\n"
    "add &BL\n"
    "\n"
    "var espera = 0.0\n"
    "\n"
    "up update:\n"
    "    espera = espera - delta()\n"
    "    if action_pressed(\"tap\") or espera <= 0.0:\n"
    "        espera = 0.8\n"
    "        var caixa = spawn(\"Caixa\")\n"
    "        caixa.position.x = random(view_left() + 0.5, view_right() - 0.5)\n"
    "        caixa.position.y = view_top() + 1.0\n"
    "        caixa.rotation.z = random(0.0, 90.0)\n"
    "        var hud = find(\"Contador\")\n"
    "        hud.text.text = \"CAIXAS: \" + str(count(\"Caixa\"))\n"
    "    stop\n"
    "stop\n";

constexpr const char* kBoxScript =
    "# Caixa: some depois de um tempo para a cena não encher.\n"
    "var vida = 0.0\n"
    "\n"
    "up update:\n"
    "    vida = vida + delta()\n"
    "    var me = self()\n"
    "    if vida > 12.0:\n"
    "        despawn(me)\n"
    "    stop\n"
    "stop\n";

class Builder {
public:
    explicit Builder(EditorDocument& doc) : doc_(doc) {}

    Result<void> set(eng::ecs::Entity e, std::string_view comp,
                     std::string_view path, std::string_view value)
    {
        return doc_.setInspectorField(e, comp, path, value);
    }

    Result<void> add(eng::ecs::Entity e, std::string_view comp)
    {
        return doc_.addComponent(e, comp);
    }

    Result<void> place(eng::ecs::Entity e, float x, float y, float sx = 1.f,
                       float sy = 1.f)
    {
        TransformDesc t;
        t.position = eng::math::Vec3{x, y, 0.f};
        t.scale = eng::math::Vec3{sx, sy, 1.f};
        return doc_.setTransform(e, t);
    }

    Result<void> tint(eng::ecs::Entity e, float r, float g, float b)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(r));
        if (auto s = set(e, "eng::editor::SpriteData", "tintR", buf); !s.ok()) {
            return s;
        }
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(g));
        if (auto s = set(e, "eng::editor::SpriteData", "tintG", buf); !s.ok()) {
            return s;
        }
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(b));
        return set(e, "eng::editor::SpriteData", "tintB", buf);
    }

    Result<void> box(eng::ecs::Entity e, float hx, float hy)
    {
        if (auto s = add(e, "eng::physics::Collider"); !s.ok()) {
            return s;
        }
        if (auto s = set(e, "eng::physics::Collider", "shape", "Box"); !s.ok()) {
            return s;
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(hx));
        if (auto s = set(e, "eng::physics::Collider", "halfExtents.x", buf);
            !s.ok()) {
            return s;
        }
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(hy));
        return set(e, "eng::physics::Collider", "halfExtents.y", buf);
    }

    Result<void> text(eng::ecs::Entity e, std::string_view value, float size,
                      bool screen, float sx, float sy)
    {
        const auto* scene = doc_.sceneInFocus();
        const auto comps = scene != nullptr
                               ? Inspector::componentsOf(*scene, e)
                               : std::vector<std::string>{};
        if (std::find(comps.begin(), comps.end(), "eng::editor::TextData") ==
            comps.end()) {
            if (auto r = add(e, "eng::editor::TextData"); !r.ok()) {
                return r;
            }
        }
        char buf[32];
        auto num = [&](float v) {
            std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
            return std::string(buf);
        };
        const std::pair<const char*, std::string> fields[] = {
            {"text", std::string(value)},
            {"size", num(size)},
            {"screenSpace", screen ? "true" : "false"},
            {"screenX", num(sx)},
            {"screenY", num(sy)},
        };
        for (const auto& [path, v] : fields) {
            if (auto r = set(e, "eng::editor::TextData", path, v); !r.ok()) {
                return r;
            }
        }
        return {};
    }

    Result<void> script(eng::ecs::Entity e, const std::string& name,
                        const char* source)
    {
        auto listed = doc_.scriptList();
        const bool exists =
            listed.ok() && std::find(listed.value().begin(), listed.value().end(),
                                     name) != listed.value().end();
        if (!exists) {
            if (auto r = doc_.scriptWrite(name, source); !r.ok()) {
                return r;
            }
        }
        return doc_.scriptAssign(e, name);
    }

    Result<void> body(eng::ecs::Entity e, std::string_view type)
    {
        if (auto s = add(e, "eng::physics::RigidBody"); !s.ok()) {
            return s;
        }
        if (type == "Static") {
            if (auto s = set(e, "eng::physics::RigidBody", "mass", "0"); !s.ok()) {
                return s;
            }
            if (auto s = set(e, "eng::physics::RigidBody", "useGravity", "false");
                !s.ok()) {
                return s;
            }
        }
        return set(e, "eng::physics::RigidBody", "bodyType", type);
    }

private:
    EditorDocument& doc_;
};

#define TRY(expr)                                     \
    do {                                              \
        auto tryResult_ = (expr);                     \
        if (tryResult_.isError()) {                   \
            return makeUnexpected(tryResult_.error()); \
        }                                             \
    } while (false)

using eng::core::makeUnexpected;

/// Cria uma entidade a partir de um modelo. Um passo de desfazer.
Result<eng::ecs::Entity> createFromTemplate(EditorDocument& doc,
                                            std::string_view kind,
                                            std::string_view name,
                                            eng::ecs::Entity parent)
{
    Builder b(doc);
    auto named = [&](const char* fallback) {
        return name.empty() ? std::string(fallback) : std::string(name);
    };
    if (kind == "empty") {
        return doc.createEntity(named("Entidade"), parent);
    }
    auto sprite = [&](const char* fallback) -> Result<eng::ecs::Entity> {
        auto e = doc.createSprite(named(fallback));
        if (e.isError()) {
            return e;
        }
        if (parent != eng::scene::kNoEntity) {
            TRY(doc.reparentEntity(e.value(), parent));
        }
        return e;
    };
    if (kind == "sprite") {
        return sprite("Sprite");
    }
    if (kind == "camera") {
        auto e = doc.createEntity(named("Câmera"), parent);
        if (e.isError()) {
            return e;
        }
        TRY(b.add(e.value(), "eng::tick::CameraData"));
        return e;
    }
    if (kind == "ground") {
        auto e = sprite("Chão");
        if (e.isError()) {
            return e;
        }
        TRY(b.place(e.value(), 0.f, -2.f, 8.f, 1.f));
        TRY(b.tint(e.value(), 0.36f, 0.42f, 0.5f));
        // O colisor escala com o Transform: caixa unitária = tamanho do sprite.
        TRY(b.box(e.value(), 0.5f, 0.5f));
        TRY(b.body(e.value(), "Static"));
        return e;
    }
    if (kind == "physics") {
        auto e = sprite("Caixa");
        if (e.isError()) {
            return e;
        }
        TRY(b.tint(e.value(), 0.95f, 0.72f, 0.36f));
        TRY(b.box(e.value(), 0.5f, 0.5f));
        TRY(b.body(e.value(), "DynamicLite"));
        return e;
    }
    if (kind == "character") {
        auto e = sprite("Jogador");
        if (e.isError()) {
            return e;
        }
        TRY(b.tint(e.value(), 0.54f, 0.71f, 0.97f));
        TRY(b.box(e.value(), 0.5f, 0.5f));
        TRY(b.body(e.value(), "DynamicLite"));
        TRY(b.script(e.value(), "jogador.nis", kPlayerScript));
        return e;
    }
    if (kind == "particles") {
        auto e = doc.createEntity(named("Partículas"), parent);
        if (e.isError()) {
            return e;
        }
        TRY(b.add(e.value(), "eng::particles::ParticleEmitter"));
        return e;
    }
    if (kind == "light") {
        auto e = doc.createEntity(named("Luz"), parent);
        if (e.isError()) {
            return e;
        }
        TRY(b.add(e.value(), "eng::render::Light2D"));
        return e;
    }
    if (kind == "text") {
        auto e = doc.createEntity(named("Texto"), parent);
        if (e.isError()) {
            return e;
        }
        TRY(b.text(e.value(), "TEXTO", 0.6f, false, 0.5f, 0.1f));
        return e;
    }
    if (kind == "audio") {
        auto e = doc.createEntity(named("Som"), parent);
        if (e.isError()) {
            return e;
        }
        TRY(b.add(e.value(), "eng::editor::AudioSource"));
        return e;
    }
    return makeUnexpected(eng::core::Error{eng::core::StatusCode::InvalidArgument,
                                           "modelo desconhecido: " +
                                               std::string(kind)});
}

/// Exemplos prontos oferecidos na tela inicial.
struct ProjectTemplateInfo {
    const char* id;
    const char* title;
    const char* description;
};

constexpr ProjectTemplateInfo kProjectTemplates[] = {
    {"platformer", "Plataforma 2D",
     "Personagem que anda e pula, chão, plataforma e câmera que segue."},
    {"flappy", "Voo",
     "Toque para voar entre canos que surgem sem parar. Placar e recomeço."},
    {"boxes", "Chuva de caixas",
     "Física: cada toque solta uma caixa que cai e empilha."},
    {"empty", "Vazio", "Só uma câmera. Você monta o resto."},
};

eng::project::GameConfig gameFor(float r, float g, float b,
                                 const char* orientation, const char* controls)
{
    eng::project::GameConfig game;
    game.backgroundR = r;
    game.backgroundG = g;
    game.backgroundB = b;
    game.orientation = orientation;
    game.controls = controls;
    return game;
}

/// Popula um projeto recém-criado a partir de um modelo de projeto.
Result<void> applyProjectTemplate(EditorDocument& doc, std::string_view kind)
{
    Builder b(doc);
    auto make = [&](const char* tpl, const char* name,
                    eng::ecs::Entity parent = eng::scene::kNoEntity) {
        return createFromTemplate(doc, tpl, name, parent);
    };
    auto camera = [&](float viewHeight, const char* follow) -> Result<void> {
        auto cam = make("camera", "Câmera");
        if (cam.isError()) {
            return makeUnexpected(cam.error());
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(viewHeight));
        TRY(b.set(cam.value(), "eng::tick::CameraData", "viewHeight", buf));
        if (follow != nullptr) {
            TRY(b.set(cam.value(), "eng::tick::CameraData", "followName", follow));
        }
        return {};
    };
#define MAKE(var, ...)                               \
    auto var = make(__VA_ARGS__);                    \
    if (var.isError()) {                             \
        return makeUnexpected(var.error());          \
    }

    if (kind.empty() || kind == "empty") {
        TRY(camera(10.f, nullptr));
        TRY(doc.setGameConfig(gameFor(0.07f, 0.08f, 0.11f, "auto", "platformer")));
    } else if (kind == "platformer") {
        MAKE(ground, "ground", "Chão");
        TRY(b.place(ground.value(), 0.f, -2.f, 14.f, 1.f));
        TRY(b.tint(ground.value(), 0.30f, 0.52f, 0.36f));
        MAKE(ledge, "ground", "Plataforma");
        TRY(b.place(ledge.value(), 3.5f, 0.2f, 3.f, 0.4f));
        TRY(b.tint(ledge.value(), 0.62f, 0.45f, 0.30f));
        MAKE(player, "character", "Jogador");
        TRY(b.place(player.value(), -2.f, 0.f));
        TRY(camera(9.f, "Jogador"));
        TRY(doc.setGameConfig(gameFor(0.42f, 0.66f, 0.88f, "landscape", "platformer")));
    } else if (kind == "flappy") {
        // Chão.
        MAKE(ground, "ground", "Chão");
        TRY(b.place(ground.value(), 0.f, -5.6f, 60.f, 1.2f));
        TRY(b.tint(ground.value(), 0.42f, 0.70f, 0.30f));
        // Pássaro.
        MAKE(bird, "sprite", "Pássaro");
        TRY(b.place(bird.value(), -1.2f, 0.8f, 0.7f, 0.7f));
        TRY(b.tint(bird.value(), 1.f, 0.82f, 0.22f));
        TRY(b.box(bird.value(), 0.45f, 0.45f));
        TRY(b.body(bird.value(), "DynamicLite"));
        TRY(b.set(bird.value(), "eng::physics::RigidBody", "useGravity", "false"));
        TRY(b.set(bird.value(), "eng::physics::RigidBody", "gravity.y", "-24"));
        TRY(b.script(bird.value(), "passaro.nis", kBirdScript));
        // Molde do cano (par de canos + vão que conta ponto).
        auto pipe = doc.createEntity("Cano", eng::scene::kNoEntity);
        if (pipe.isError()) {
            return makeUnexpected(pipe.error());
        }
        TRY(b.place(pipe.value(), 6.f, 0.f));
        TRY(b.add(pipe.value(), "eng::scene::Template"));
        TRY(b.script(pipe.value(), "cano.nis", kPipeScript));
        constexpr float kGapHalf = 1.6f;
        MAKE(top, "ground", "Cima", pipe.value());
        TRY(b.place(top.value(), 0.f, kGapHalf + 5.f, 1.2f, 10.f));
        TRY(b.tint(top.value(), 0.30f, 0.74f, 0.36f));
        MAKE(bottom, "ground", "Baixo", pipe.value());
        TRY(b.place(bottom.value(), 0.f, -(kGapHalf + 5.f), 1.2f, 10.f));
        TRY(b.tint(bottom.value(), 0.30f, 0.74f, 0.36f));
        auto gap = doc.createEntity("Vão", pipe.value());
        if (gap.isError()) {
            return makeUnexpected(gap.error());
        }
        TRY(b.place(gap.value(), 0.4f, 0.f, 0.2f, 2.f * kGapHalf));
        TRY(b.box(gap.value(), 0.5f, 0.5f));
        TRY(b.set(gap.value(), "eng::physics::Collider", "isTrigger", "true"));
        // Gerador e textos.
        MAKE(spawner, "empty", "Gerador");
        TRY(b.script(spawner.value(), "gerador.nis", kPipeSpawnerScript));
        MAKE(score, "text", "Placar");
        TRY(b.text(score.value(), "0", 1.1f, true, 0.5f, 0.12f));
        MAKE(message, "text", "Mensagem");
        TRY(b.text(message.value(), "TOQUE PARA VOAR", 0.45f, true, 0.5f, 0.36f));
        TRY(camera(12.f, nullptr));
        TRY(doc.setGameConfig(gameFor(0.36f, 0.64f, 0.86f, "portrait", "tap")));
    } else if (kind == "boxes") {
        MAKE(ground, "ground", "Chão");
        TRY(b.place(ground.value(), 0.f, -4.5f, 7.f, 0.8f));
        TRY(b.tint(ground.value(), 0.55f, 0.58f, 0.66f));
        auto box = make("physics", "Caixa");
        if (box.isError()) {
            return makeUnexpected(box.error());
        }
        TRY(b.place(box.value(), 0.f, 8.f, 0.8f, 0.8f));
        TRY(b.add(box.value(), "eng::scene::Template"));
        TRY(b.script(box.value(), "caixa.nis", kBoxScript));
        MAKE(spawner, "empty", "Gerador");
        TRY(b.script(spawner.value(), "gerador.nis", kBoxSpawnerScript));
        MAKE(counter, "text", "Contador");
        TRY(b.text(counter.value(), "TOQUE PARA\nSOLTAR CAIXAS", 0.4f, true, 0.5f, 0.1f));
        TRY(camera(12.f, nullptr));
        TRY(doc.setGameConfig(gameFor(0.10f, 0.11f, 0.16f, "portrait", "tap")));
    } else {
        return makeUnexpected(eng::core::Error{
            eng::core::StatusCode::InvalidArgument,
            "modelo de projeto desconhecido: " + std::string(kind)});
    }
#undef MAKE
    doc.deselect();
    TRY(doc.saveProject());
    return {};
}

#undef TRY

// --- blocos de estado ---------------------------------------------------------------

Json hierarchyJson(const EditorDocument& doc)
{
    Json list = Json::array();
    const auto* scene = doc.sceneInFocus();
    for (const auto& node : doc.hierarchySnapshot()) {
        Json n;
        n["id"] = packed(node.entity);
        n["name"] = node.name;
        n["depth"] = node.depth;
        n["kind"] = scene != nullptr
                        ? entityKind(Inspector::componentsOf(*scene, node.entity))
                        : "empty";
        n["template"] = scene != nullptr && scene->isTemplated(node.entity);
        list.push_back(std::move(n));
    }
    return list;
}

Json inspectorJson(EditorDocument& doc, eng::ecs::Entity entity)
{
    const auto* scene = doc.sceneInFocus();
    if (scene == nullptr || !scene->isNode(entity)) {
        return nullptr;
    }
    Json out;
    out["id"] = packed(entity);
    out["name"] = EditorDocument::nameOf(*scene, entity);
    Json components = Json::array();
    std::unordered_map<std::string, std::string> categories;
    for (const auto& entry : Inspector::catalogEntries()) {
        categories[entry.name] = entry.category.empty() ? "Outros" : entry.category;
    }
    for (const auto& name : Inspector::componentsOf(*scene, entity)) {
        if (name == "eng::scene::Name") {
            continue;  // o nome é editado no cabeçalho
        }
        Json c;
        c["name"] = name;
        c["label"] = shortName(name);
        c["category"] = categories.count(name) ? categories[name] : "Outros";
        c["removable"] = Inspector::isRemovable(name);
        Json fields = Json::array();
        for (const auto& f : doc.inspectorFields(entity, name)) {
            Json jf;
            jf["path"] = f.path;
            jf["type"] = f.typeName;
            jf["value"] = f.value;
            jf["kind"] = uiKind(name, f);
            if (!f.options.empty()) {
                Json opts = Json::array();
                std::size_t start = 0;
                while (start <= f.options.size()) {
                    const auto bar = f.options.find('|', start);
                    const auto end = bar == std::string::npos ? f.options.size() : bar;
                    opts.push_back(f.options.substr(start, end - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                jf["options"] = std::move(opts);
            }
            fields.push_back(std::move(jf));
        }
        c["fields"] = std::move(fields);
        components.push_back(std::move(c));
    }
    out["components"] = std::move(components);
    return out;
}

Json settingsJson(const EditorDocument& doc)
{
    Json out;
    const auto& g = doc.gridConfig();
    out["grid"] = {{"visible", g.visible},
                   {"cell", g.cell},
                   {"majorEvery", g.majorEvery},
                   {"minor", {g.minorR, g.minorG, g.minorB}},
                   {"major", {g.majorR, g.majorG, g.majorB}}};
    out["physicsDt"] = doc.physicsFixedDt();
    out["kinematicSweep"] = doc.kinematicSweep();
    Json layers = Json::array();
    if (auto listed = doc.layerList(); listed.ok()) {
        for (const auto& l : listed.value()) {
            layers.push_back({{"name", l.name},
                              {"timeScale", l.timeScale},
                              {"update", l.update},
                              {"physics", l.physics},
                              {"render", l.render}});
        }
    }
    out["layers"] = std::move(layers);
    Json collision = Json::array();
    for (const auto& c : doc.collisionLayers()) {
        collision.push_back({{"name", c.name}, {"bit", c.bit}});
    }
    out["collisionLayers"] = std::move(collision);
    const auto& game = doc.gameConfig();
    out["game"] = {{"background", {game.backgroundR, game.backgroundG, game.backgroundB}},
                   {"orientation", game.orientation},
                   {"controls", game.controls}};
    return out;
}

Json stringList(const std::vector<std::string>& items)
{
    Json out = Json::array();
    for (const auto& s : items) {
        out.push_back(s);
    }
    return out;
}

} // namespace

EditorProtocol::EditorProtocol(EditorDocument& doc, EditorHost* host)
    : doc_(doc), host_(host)
{
}

std::string EditorProtocol::snapshot(std::uint64_t sinceKey)
{
    // Chave barata: muda a cada chamada e a cada revisão do documento.
    std::uint64_t key = doc_.selectionRevision() * 1000003ull + calls_ * 31ull;
    key ^= (doc_.isPlaying() ? 1ull : 0ull) << 60;
    key ^= (doc_.isPaused() ? 1ull : 0ull) << 59;
    key ^= (doc_.canUndo() ? 1ull : 0ull) << 58;
    key ^= (doc_.canRedo() ? 1ull : 0ull) << 57;
    key ^= (doc_.sceneDirty() ? 1ull : 0ull) << 56;
    key ^= (doc_.projectDirty() ? 1ull : 0ull) << 55;
    key ^= (doc_.hasProject() ? 1ull : 0ull) << 54;
    key ^= static_cast<std::uint64_t>(doc_.tool()) << 50;
    key ^= (doc_.snapTranslate() ? 1ull : 0ull) << 49;
    key ^= (doc_.snapRotate() ? 1ull : 0ull) << 48;
    key ^= (doc_.previewing() ? 1ull : 0ull) << 47;
    if (key == 0) {
        key = 1;
    }
    if (key == sinceKey) {
        return {};
    }
    Json s;
    s["key"] = key;
    if (doc_.hasProject()) {
        const auto& game = doc_.gameConfig();
        s["project"] = {{"name", doc_.projectName()},
                        {"folder", doc_.projectRoot().filename().str()},
                        {"controls", game.controls},
                        {"orientation", game.orientation}};
    } else {
        s["project"] = nullptr;
    }
    s["scene"] = {{"path", doc_.currentScenePath()},
                  {"dirty", doc_.sceneDirty() || doc_.projectDirty()}};
    s["mode"] = doc_.isPlaying() ? "play" : "edit";
    s["paused"] = doc_.isPaused();
    s["previewing"] = doc_.previewing();
    const auto sel = doc_.selection();
    s["selection"] = sel.has_value() ? packed(*sel) : Json(0);
    s["tool"] = static_cast<int>(doc_.tool());
    s["snap"] = {{"translate", doc_.snapTranslate()},
                 {"rotate", doc_.snapRotate()}};
    s["canUndo"] = doc_.canUndo();
    s["canRedo"] = doc_.canRedo();
    s["hierarchy"] = hierarchyJson(doc_);
    return dump(s);
}

std::string EditorProtocol::call(std::string_view requestJson)
{
    ++calls_;
    auto parsed = eng::serial::parseJson(requestJson);
    Reply reply;
    if (parsed.isError()) {
        reply = fail("pedido inválido: " + parsed.error().message);
    } else {
        const Json& req = parsed.value().raw();
        const std::string op = str(req, "op");
        EditorDocument& doc = doc_;
        EditorHost* host = host_;
        auto texturesChanged = [host] {
            if (host != nullptr) {
                host->invalidateTextureCache();
            }
        };
        TextureCache* textures = host != nullptr ? &host->textureCache() : nullptr;

        // ---------------------------------------------------------------- projeto
        if (op == "project.list") {
            auto listed = doc.listProjects();
            reply = listed.ok() ? ok(stringList(listed.value()))
                                : fail(listed.error().message);
        } else if (op == "project.new") {
            auto made = doc.newProject(str(req, "name"));
            if (made.isError()) {
                reply = fail(made.error().message);
            } else {
                texturesChanged();
                auto filled = applyProjectTemplate(doc, str(req, "template", "empty"));
                reply = filled.ok()
                            ? ok(doc.projectRoot().filename().str())
                            : fail(filled.error().message);
            }
        } else if (op == "project.open") {
            auto opened = doc.openProject(eng::fs::Path{str(req, "folder")});
            texturesChanged();
            reply = fromResult(opened);
        } else if (op == "project.ensure") {
            if (host != nullptr) {
                auto ensured = host->ensureStartupProject();
                reply = ensured.ok() ? ok(ensured.value())
                                     : fail(ensured.error().message);
            } else {
                auto ensured = doc.ensureStartupProject();
                reply = ensured.ok() ? ok(ensured.value())
                                     : fail(ensured.error().message);
            }
        } else if (op == "project.save") {
            reply = fromResult(doc.saveProject());
        } else if (op == "project.rename") {
            reply = fromResult(doc.setProjectName(str(req, "name")));
        } else if (op == "project.exportZip") {
            reply = fromResult(doc.exportProjectZip(str(req, "path")));
        } else if (op == "project.importZip") {
            auto imported = doc.importProjectZip(str(req, "path"), str(req, "name"));
            reply = imported.ok() ? ok(imported.value())
                                  : fail(imported.error().message);
        }
        // ---------------------------------------------------------------- cena
        else if (op == "scene.list") {
            auto listed = doc.sceneList();
            reply = listed.ok() ? ok(stringList(listed.value()))
                                : fail(listed.error().message);
        } else if (op == "scene.new") {
            reply = fromResult(doc.newScene());
        } else if (op == "scene.save") {
            std::string path = str(req, "path", doc.currentScenePath());
            if (path.empty()) {
                path = "main.json";
            }
            if (path.size() < 5 || !path.ends_with(".json")) {
                path += ".json";
            }
            reply = fromResult(doc.saveScene(path));
        } else if (op == "scene.load") {
            reply = fromResult(doc.loadScene(str(req, "path")));
        }
        // ---------------------------------------------------------------- entidades
        else if (op == "entity.create") {
            const std::string kind = str(req, "template", "empty");
            doc.beginHistoryGroup("create");
            auto made = createFromTemplate(doc, kind, str(req, "name"),
                                           entityArg(req, "parent"));
            doc.endHistoryGroup();
            if (made.ok()) {
                (void)doc.select(made.value());
                reply = ok(packed(made.value()));
            } else {
                reply = fail(made.error().message);
            }
        } else if (op == "entity.delete") {
            reply = fromResult(doc.deleteEntity(entityArg(req)));
        } else if (op == "entity.rename") {
            reply = fromResult(doc.renameEntity(entityArg(req), str(req, "name")));
        } else if (op == "entity.duplicate") {
            auto dup = doc.duplicateEntity(entityArg(req));
            reply = dup.ok() ? ok(packed(dup.value())) : fail(dup.error().message);
        } else if (op == "entity.reparent") {
            reply = fromResult(
                doc.reparentEntity(entityArg(req), entityArg(req, "parent")));
        } else if (op == "entity.select") {
            if (u64(req, "id") == 0) {
                doc.deselect();
                reply = ok();
            } else {
                reply = fromResult(doc.select(entityArg(req)));
            }
        } else if (op == "transform.get") {
            auto t = doc.transform(entityArg(req));
            if (t.isError()) {
                reply = fail(t.error().message);
            } else {
                const auto& v = t.value();
                reply = ok({{"p", {v.position.x, v.position.y, v.position.z}},
                            {"r",
                             {v.rotationDegrees.x, v.rotationDegrees.y,
                              v.rotationDegrees.z}},
                            {"s", {v.scale.x, v.scale.y, v.scale.z}}});
            }
        } else if (op == "transform.set") {
            auto current = doc.transform(entityArg(req));
            if (current.isError()) {
                reply = fail(current.error().message);
            } else {
                TransformDesc t = current.value();
                (void)vec3(req, "p", t.position);
                (void)vec3(req, "r", t.rotationDegrees);
                (void)vec3(req, "s", t.scale);
                reply = fromResult(doc.setTransform(entityArg(req), t));
            }
        }
        // ---------------------------------------------------------------- inspector
        else if (op == "inspector") {
            reply = ok(inspectorJson(doc, entityArg(req)));
        } else if (op == "component.catalog") {
            Json list = Json::array();
            const auto entity = entityArg(req);
            std::unordered_map<std::string, std::string> deps;
            std::unordered_map<std::string, bool> addable;
            for (const auto& m : doc.addableComponents(entity)) {
                deps[m.name] = m.dependency;
                addable[m.name] = m.addable;
            }
            for (const auto& e : Inspector::catalogEntries()) {
                if (!addable.count(e.name) || !addable[e.name]) {
                    continue;
                }
                list.push_back({{"name", e.name},
                                {"label", shortName(e.name)},
                                {"category", e.category.empty() ? "Outros"
                                                                : e.category},
                                {"alias", e.scriptAlias},
                                {"dependency", deps[e.name]}});
            }
            reply = ok(std::move(list));
        } else if (op == "component.add") {
            doc.beginHistoryGroup("component");
            auto added = doc.addComponentWithDependencies(entityArg(req),
                                                          str(req, "name"));
            doc.endHistoryGroup();
            reply = added.ok() ? ok(stringList(added.value()))
                               : fail(added.error().message);
        } else if (op == "component.remove") {
            reply = fromResult(doc.removeComponent(entityArg(req), str(req, "name")));
        } else if (op == "component.set") {
            reply = fromResult(doc.setInspectorField(entityArg(req),
                                                     str(req, "component"),
                                                     str(req, "path"),
                                                     str(req, "value")));
        }
        // ---------------------------------------------------------------- edição
        else if (op == "tool.set") {
            const int t = static_cast<int>(num(req, "tool"));
            doc.setTool(t == 1   ? EditorTool::Move
                        : t == 2 ? EditorTool::Rotate
                        : t == 3 ? EditorTool::Scale
                                 : EditorTool::Select);
            reply = ok();
        } else if (op == "snap.set") {
            if (has(req, "translate")) {
                doc.setSnapTranslate(flag(req, "translate"));
            }
            if (has(req, "rotate")) {
                doc.setSnapRotate(flag(req, "rotate"));
            }
            reply = ok();
        } else if (op == "viewport.fit") {
            doc.viewportFit(textures);
            reply = ok();
        } else if (op == "viewport.zoom") {
            doc.viewportZoom(static_cast<float>(num(req, "factor", 1.0)),
                             static_cast<float>(num(req, "x")),
                             static_cast<float>(num(req, "y")));
            reply = ok();
        } else if (op == "history.undo") {
            reply = fromResult(doc.undo());
        } else if (op == "history.redo") {
            reply = fromResult(doc.redo());
        }
        // ---------------------------------------------------------------- play
        else if (op == "play.start") {
            reply = fromResult(doc.play());
        } else if (op == "play.stop") {
            doc.stop();
            reply = ok();
        } else if (op == "play.pause") {
            doc.setPaused(flag(req, "paused", true));
            reply = ok();
        } else if (op == "play.hud") {
            const NiScriptStats& st = doc.runtimeScripts().stats();
            Json hud;
            hud["scripts"] = {{"found", st.scriptsFound},
                              {"compiled", st.scriptsCompiled},
                              {"failed", st.scriptsFailed},
                              {"ticks", st.ticks},
                              {"faults", st.faults},
                              {"compileError", st.firstCompileError},
                              {"fault", st.lastFaultMessage}};
            hud["audio"] = host != nullptr ? host->audioStatusLine() : "";
            hud["perf"] = host != nullptr ? host->perfSummaryLine() : "";
            reply = ok(std::move(hud));
        } else if (op == "game.viewport") {
            doc.setGameViewportSize(static_cast<float>(num(req, "w")),
                                    static_cast<float>(num(req, "h")));
            reply = ok();
        }
        // ---------------------------------------------------------------- assets
        else if (op == "asset.categories") {
            reply = ok(stringList(AssetBrowser::categories()));
        } else if (op == "asset.list") {
            auto* browser = doc.assets();
            if (browser == nullptr) {
                reply = fail("nenhum projeto aberto");
            } else {
                auto listed = browser->list(str(req, "category"));
                if (listed.isError()) {
                    reply = fail(listed.error().message);
                } else {
                    Json list = Json::array();
                    for (const auto& e : listed.value()) {
                        list.push_back({{"name", e.name},
                                        {"id", e.id},
                                        {"registered", e.registered},
                                        {"path", e.sourcePath}});
                    }
                    reply = ok(std::move(list));
                }
            }
        } else if (op == "asset.import") {
            const std::string category = str(req, "category");
            auto imported = doc.importAsset(str(req, "temp"), category,
                                            str(req, "name"));
            if (imported.ok() && category == "textures") {
                texturesChanged();
            }
            reply = imported.ok() ? ok(imported.value())
                                  : fail(imported.error().message);
        } else if (op == "asset.rename" || op == "asset.delete" ||
                   op == "asset.move") {
            auto* browser = doc.assets();
            if (browser == nullptr) {
                reply = fail("nenhum projeto aberto");
            } else if (op == "asset.rename") {
                reply = fromResult(browser->rename(str(req, "category"),
                                                   str(req, "name"),
                                                   str(req, "newName")));
            } else if (op == "asset.delete") {
                reply = fromResult(
                    browser->remove(str(req, "category"), str(req, "name")));
            } else {
                reply = fromResult(browser->move(str(req, "from"), str(req, "name"),
                                                 str(req, "to")));
            }
            if (reply.ok) {
                texturesChanged();
            }
        } else if (op == "asset.imageInfo") {
            auto* browser = doc.assets();
            if (browser == nullptr || textures == nullptr) {
                reply = ok(nullptr);
            } else {
                const auto info = textures->imageInfo(*browser, str(req, "name"));
                reply = info.valid ? ok({{"w", info.width},
                                         {"h", info.height},
                                         {"alpha", info.alpha}})
                                   : ok(nullptr);
            }
        } else if (op == "texture.apply") {
            const auto entity = entityArg(req);
            doc.beginHistoryGroup("texture");
            Result<void> applied{};
            const auto* scene = doc.sceneInFocus();
            bool hasSprite = false;
            if (scene != nullptr) {
                const auto comps = Inspector::componentsOf(*scene, entity);
                hasSprite = std::find(comps.begin(), comps.end(),
                                      "eng::editor::SpriteData") != comps.end();
            }
            if (!hasSprite) {
                applied = doc.addComponent(entity, "eng::editor::SpriteData");
            }
            if (applied.ok()) {
                applied = doc.setInspectorField(entity, "eng::editor::SpriteData",
                                                "textureAsset", str(req, "name"));
            }
            doc.endHistoryGroup();
            reply = fromResult(applied);
        }
        // ---------------------------------------------------------------- scripts
        else if (op == "script.list") {
            auto listed = doc.scriptList();
            reply = listed.ok() ? ok(stringList(listed.value()))
                                : fail(listed.error().message);
        } else if (op == "script.read") {
            auto content = doc.scriptRead(str(req, "name"));
            reply = content.ok() ? ok(content.value())
                                 : fail(content.error().message);
        } else if (op == "script.write") {
            reply = fromResult(doc.scriptWrite(str(req, "name"), str(req, "content")));
        } else if (op == "script.create") {
            std::string name = str(req, "name");
            if (!name.ends_with(".nis")) {
                name += ".nis";
            }
            auto made = doc.scriptCreate(name);
            reply = made.ok() ? ok(name) : fail(made.error().message);
        } else if (op == "script.delete") {
            reply = fromResult(doc.scriptDelete(str(req, "name")));
        } else if (op == "script.compile") {
            auto check = doc.scriptCompile(str(req, "source"));
            if (check.isError()) {
                reply = ok({{"ok", false},
                            {"diags",
                             {{{"line", 1}, {"col", 1},
                               {"message", check.error().message}}}}});
            } else {
                Json diags = Json::array();
                for (const auto& d : check.value().diags) {
                    diags.push_back(
                        {{"line", d.line}, {"col", d.col}, {"message", d.message}});
                }
                reply = ok({{"ok", check.value().ok}, {"diags", std::move(diags)}});
            }
        } else if (op == "script.assign") {
            reply = fromResult(doc.scriptAssign(entityArg(req), str(req, "name")));
        }
        // ---------------------------------------------------------------- animação
        else if (op == "anim.list") {
            auto listed = doc.animationList();
            if (listed.isError()) {
                reply = fail(listed.error().message);
            } else {
                Json list = Json::array();
                for (const auto& a : listed.value()) {
                    list.push_back({{"name", a.name},
                                    {"clip", a.clip},
                                    {"duration", a.duration},
                                    {"frames", a.frames},
                                    {"keys", a.keys},
                                    {"loop", a.loop}});
                }
                reply = ok(std::move(list));
            }
        } else if (op == "anim.read") {
            auto content = doc.animationRead(str(req, "name"));
            reply = content.ok() ? ok(content.value())
                                 : fail(content.error().message);
        } else if (op == "anim.write") {
            reply = fromResult(doc.animationWrite(str(req, "name"), str(req, "json")));
        } else if (op == "anim.create") {
            reply = fromResult(doc.animationCreate(str(req, "name")));
        } else if (op == "anim.delete") {
            reply = fromResult(doc.animationDelete(str(req, "name")));
        } else if (op == "anim.assign") {
            reply = fromResult(doc.animationAssign(entityArg(req), str(req, "name")));
        } else if (op == "anim.addFrame") {
            auto when = doc.animationAddFrame(str(req, "name"), str(req, "texture"));
            reply = when.ok() ? ok(when.value()) : fail(when.error().message);
        } else if (op == "anim.setMeta") {
            reply = fromResult(doc.animationSetMeta(
                str(req, "name"), flag(req, "loop", true),
                static_cast<float>(num(req, "fps", 12.0))));
        } else if (op == "anim.addKey") {
            reply = fromResult(doc.animationAddKey(
                str(req, "name"), str(req, "track", "position"),
                static_cast<float>(num(req, "time")),
                static_cast<float>(num(req, "x")), static_cast<float>(num(req, "y")),
                static_cast<float>(num(req, "z"))));
        } else if (op == "anim.keys") {
            auto keys = doc.animationKeyList(str(req, "name"),
                                             str(req, "track", "position"));
            reply = keys.ok() ? ok(keys.value()) : fail(keys.error().message);
        } else if (op == "anim.setKey") {
            reply = fromResult(doc.animationKeySet(
                str(req, "name"), str(req, "track", "position"),
                static_cast<std::size_t>(num(req, "index")),
                static_cast<float>(num(req, "time")),
                static_cast<float>(num(req, "x")), static_cast<float>(num(req, "y")),
                static_cast<float>(num(req, "z"))));
        } else if (op == "anim.deleteKey") {
            reply = fromResult(doc.animationKeyDelete(
                str(req, "name"), str(req, "track", "position"),
                static_cast<std::size_t>(num(req, "index"))));
        } else if (op == "anim.preview") {
            reply = fromResult(doc.previewStart(entityArg(req), str(req, "clip")));
        } else if (op == "anim.previewStop") {
            doc.previewStop();
            reply = ok();
        }
        // ---------------------------------------------------------------- materiais
        else if (op == "material.list") {
            auto listed = doc.materialList();
            if (listed.isError()) {
                reply = fail(listed.error().message);
            } else {
                Json list = Json::array();
                for (const auto& m : listed.value()) {
                    list.push_back({{"name", m.name},
                                    {"shader", m.shader},
                                    {"tint", {m.tintR, m.tintG, m.tintB, m.tintA}}});
                }
                reply = ok(std::move(list));
            }
        } else if (op == "material.read") {
            auto content = doc.materialRead(str(req, "name"));
            reply = content.ok() ? ok(content.value())
                                 : fail(content.error().message);
        } else if (op == "material.write") {
            reply = fromResult(doc.materialWrite(str(req, "name"), str(req, "json")));
        } else if (op == "material.create") {
            reply = fromResult(doc.materialCreate(str(req, "name")));
        } else if (op == "material.delete") {
            reply = fromResult(doc.materialDelete(str(req, "name")));
        }
        // ---------------------------------------------------------------- áudio
        else if (op == "audio.preview") {
            reply = fromResult(doc.audioPreview(str(req, "name")));
        } else if (op == "audio.stop") {
            doc.audioPreviewStop();
            reply = ok();
        } else if (op == "audio.playing") {
            reply = ok(doc.audioPreviewPlaying());
        }
        // ---------------------------------------------------------------- ajustes
        else if (op == "settings.get") {
            reply = ok(settingsJson(doc));
        } else if (op == "settings.grid") {
            eng::project::GridConfig g = doc.gridConfig();
            g.visible = flag(req, "visible", g.visible);
            g.cell = static_cast<float>(num(req, "cell", g.cell));
            g.majorEvery = static_cast<int>(num(req, "majorEvery", g.majorEvery));
            reply = fromResult(doc.setGridConfig(g));
        } else if (op == "settings.game") {
            eng::project::GameConfig game = doc.gameConfig();
            if (const Json* bg = arg(req, "background");
                bg != nullptr && bg->is_array() && bg->size() == 3) {
                float* channels[] = {&game.backgroundR, &game.backgroundG,
                                     &game.backgroundB};
                for (std::size_t i = 0; i < 3; ++i) {
                    if ((*bg)[i].is_number()) {
                        *channels[i] = (*bg)[i].get<float>();
                    }
                }
            }
            game.orientation = str(req, "orientation", game.orientation);
            game.controls = str(req, "controls", game.controls);
            reply = fromResult(doc.setGameConfig(game));
        } else if (op == "project.templates") {
            Json list = Json::array();
            for (const auto& t : kProjectTemplates) {
                list.push_back({{"id", t.id},
                                {"title", t.title},
                                {"description", t.description}});
            }
            reply = ok(std::move(list));
        } else if (op == "settings.physicsDt") {
            reply = fromResult(
                doc.setPhysicsFixedDt(static_cast<float>(num(req, "dt", 1.0 / 60.0))));
        } else if (op == "settings.kinematicSweep") {
            doc.setKinematicSweep(flag(req, "enabled", true));
            reply = ok();
        } else if (op == "layer.add") {
            reply = fromResult(doc.addLayer(str(req, "name")));
        } else if (op == "layer.set") {
            const std::string name = str(req, "name");
            Result<void> r{};
            if (has(req, "timeScale")) {
                r = doc.setLayerTimeScale(name,
                                          static_cast<float>(num(req, "timeScale")));
            }
            if (r.ok() && has(req, "update")) {
                r = doc.setLayerParticipation(name, flag(req, "update", true),
                                              flag(req, "physics", true),
                                              flag(req, "render", true));
            }
            reply = fromResult(r);
        } else if (op == "collision.add") {
            auto bit = doc.addCollisionLayer(str(req, "name"));
            reply = bit.ok() ? ok(bit.value()) : fail(bit.error().message);
        } else if (op == "collision.rename") {
            reply = fromResult(doc.setCollisionLayerName(
                static_cast<std::uint32_t>(num(req, "bit")), str(req, "name")));
        }
        // ---------------------------------------------------------------- host
        else if (op == "host.info") {
            Json info;
            if (host != nullptr) {
                const auto* caps = host->capabilities();
                info["backend"] = caps != nullptr ? caps->backendName : "";
                info["perf"] = host->perfSummaryLine();
                info["audio"] = host->audioStatusLine();
            }
            reply = ok(std::move(info));
        } else if (op == "host.backend") {
            if (host != nullptr) {
                host->setBackend(str(req, "name", "auto").c_str());
            }
            reply = ok();
        } else {
            reply = fail("operação desconhecida: '" + op + "'");
        }
    }

    Json out;
    out["ok"] = reply.ok;
    if (reply.ok) {
        out["result"] = std::move(reply.result);
    } else {
        out["error"] = reply.error;
    }
    return dump(out);
}

} // namespace eng::editor
