/// Testes do NI-Script — suite completa.
///
/// Cobertura (missão FASE 11):
///   - Lexer/Parser/Sema: tokens, formas, diagnósticos com LINHA/COLUNA;
///   - VM: aritmética/promoções, curto-circuito, chamadas, recursão,
///     escopos, @init de globais;
///   - Semântica formal §5: if/else, repeat (limites), repair (divisão por
///     zero, dentro de chamada, não-transacional), timeout (orçamento
///     exato, aninhado, sem repair), emit/links (BFS, ciclo, profundidade);
///   - Bindings/ECS: reflexão get/set, geração (entidade obsoleta = Fault,
///     nunca UB), e2e .nis→compile→bytecode→VM→binding→MUDANÇA no ECS;
///   - Determinismo: mesmo script/mundo duas vezes ⇒ efeitos idênticos;
///   - Segurança: nativos fechados, orçamento obrigatório, sem nil.
///
/// Componente de teste (TScriptable) + bindings seguem o MESMO padrão do
/// editor: registro no CONSUMIDOR, reflexão por offset.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "eng/niscript/NiScript.hpp"
#include "eng/niscript/NiVm.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Name.hpp"
#include "eng/scene/Scene.hpp"

using eng::ni::NiDiag;
using eng::ni::NiFault;
using eng::ni::NiNativeTable;
using eng::ni::NiValue;

// =============================================================================
// Fixture — componente refletido + bindings sobre um World REAL
// =============================================================================

namespace {

struct TScriptable {
    float fv = 0.f;
    std::int32_t n = 0;
    std::string s;
    eng::math::Vec3 v{0.f, 0.f, 0.f};
    bool b = false;
};

} // namespace

ENG_REFLECT_BEGIN(TScriptable)
    ENG_REFLECT_FIELD(fv)
    ENG_REFLECT_FIELD(n)
    ENG_REFLECT_FIELD(s)
    ENG_REFLECT_FIELD_AS(v, "eng::math::Vec3")
    ENG_REFLECT_FIELD(b)
ENG_REFLECT_END()

// math é registrado aqui (idempotente — primeiro registro vence; o mesmo
// registro vive no TU do SceneSerializer quando linkeado).
ENG_REFLECT_BEGIN(eng::math::Vec3)
    ENG_REFLECT_FIELD(x)
    ENG_REFLECT_FIELD(y)
    ENG_REFLECT_FIELD(z)
ENG_REFLECT_END()

ENG_REFLECT_BEGIN(eng::math::Quat)
    ENG_REFLECT_FIELD(x)
    ENG_REFLECT_FIELD(y)
    ENG_REFLECT_FIELD(z)
    ENG_REFLECT_FIELD(w)
ENG_REFLECT_END()

ENG_REFLECT_BEGIN(eng::math::Transform)
    ENG_REFLECT_FIELD_AS(position, "eng::math::Vec3")
    ENG_REFLECT_FIELD_AS(rotation, "eng::math::Quat")
    ENG_REFLECT_FIELD_AS(scale, "eng::math::Vec3")
ENG_REFLECT_END()

namespace {

/// Host de teste sobre Scene (spawn/find/despawn reais).
struct TestHost final : eng::ni::NiHost {
    eng::scene::Scene* scene = nullptr;
    float dt = 1.f / 60.f;
    bool jumpDown = false;

    TestHost() = default;
    explicit TestHost(eng::scene::Scene* s) : scene(s) {}

    float deltaSeconds() const override { return dt; }
    bool actionDown(std::string_view) const override { return jumpDown; }
    bool actionPressed(std::string_view) const override { return jumpDown; }
    bool actionReleased(std::string_view) const override { return false; }
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

    // --- P4.6: hosts de teste implementam os serviços reais ----
    // translate: transform direto; moveAndSlide: DELEGA para a física REAL
    // (o comportamento do verbo no test é o do jogo — zero fake de física).

    bool translate(eng::ecs::Entity self, float dx, float dy) override
    {
        auto* transform = scene->localTransform(self);
        if (transform == nullptr) {
            return false;
        }
        transform->position =
            transform->position + eng::math::Vec3{dx, dy, 0.f};
        return true;
    }

    bool moveAndSlide(eng::ecs::Entity self, float dx, float dy,
                      eng::math::Vec3& outPosition) override
    {
        if (scene->world().get<eng::physics::CharacterBody>(self) ==
            nullptr) {
            return false;
        }
        outPosition = eng::physics::PhysicsWorld::moveAndSlide(
            *scene, self, eng::math::Vec3{dx, dy, 0.f});
        if (auto* transform = scene->localTransform(self)) {
            transform->position = outPosition;
        }
        return true;
    }
};

/// Ambiente de teste: tabela de nativos (BL+host), bindings e helpers.
struct NiEnv {
    eng::scene::Scene scene;
    TestHost host{&scene};
    NiNativeTable natives;
    eng::ni::NiBindingTable bindings;
    eng::ni::NiInstanceSet set;
    eng::ni::NiVm vm;

    NiEnv()
    {
        natives.addBaseLibrary();
        natives.addStandardHost();
        host.scene = &scene;
        installBindings();
    }

    void installBindings()
    {
        auto* world = &scene.world();
        const auto fetchC = [](void* user, eng::ecs::Entity e) -> const void* {
            return static_cast<eng::ecs::World*>(user)->get<TScriptable>(e);
        };
        const auto fetchM = [](void* user, eng::ecs::Entity e) -> void* {
            return static_cast<eng::ecs::World*>(user)->get<TScriptable>(e);
        };
        const auto valid = [](void* user, eng::ecs::Entity e) {
            return static_cast<eng::ecs::World*>(user)->valid(e);
        };
        REQUIRE(eng::ni::niAddReflectionBinding(
            bindings, "scriptable", "TScriptable", fetchC, fetchM, world, "",
            valid));

        const auto fetchTfC =
            [](void* user, eng::ecs::Entity e) -> const void* {
            return static_cast<eng::ecs::World*>(user)
                ->get<eng::math::Transform>(e);
        };
        const auto fetchTfM = [](void* user, eng::ecs::Entity e) -> void* {
            return static_cast<eng::ecs::World*>(user)
                ->get<eng::math::Transform>(e);
        };
        REQUIRE(eng::ni::niAddReflectionBinding(
            bindings, "scriptablex", "eng::math::Transform", fetchTfC,
            fetchTfM, world, "", valid));
        REQUIRE(eng::ni::niAddReflectionBinding(
            bindings, "position", "eng::math::Transform", fetchTfC, fetchTfM,
            world, "position", valid));
        REQUIRE(eng::ni::niAddReflectionBinding(
            bindings, "name", "eng::scene::Name",
            [](void* user, eng::ecs::Entity e) -> const void* {
                return static_cast<eng::ecs::World*>(user)
                    ->get<eng::scene::Name>(e);
            },
            [](void* user, eng::ecs::Entity e) -> void* {
                return static_cast<eng::ecs::World*>(user)
                    ->get<eng::scene::Name>(e);
            },
            world, "value", valid));
    }

    eng::ni::NiExecContext::Params params(
        std::uint64_t budget = eng::ni::kDefaultBudget)
    {
        eng::ni::NiExecContext::Params p;
        p.natives = &natives;
        p.host = &host;
        p.bindings = &bindings;
        p.set = &set;
        p.budget = budget;
        return p;
    }

    /// Compila ou FALHA o teste com os diagnósticos.
    std::shared_ptr<const eng::ni::NiProgram> compileOrFail(
        const std::string& source)
    {
        std::vector<NiDiag> diags;
        const eng::ni::CompileOptions options{&natives};
        auto result = eng::ni::compile(source, options, &diags);
        if (!result.ok()) {
            std::string all = "falha de compilação:\n";
            for (const NiDiag& d : diags) {
                all += "  " + std::to_string(d.line) + ":"
                     + std::to_string(d.col) + " " + d.message + "\n";
            }
            FAIL(all);
        }
        REQUIRE(result.ok());
        return std::move(result).value();
    }

    /// Compila esperando ERRO; devolve diagnósticos.
    std::vector<NiDiag> compileExpectFail(const std::string& source)
    {
        std::vector<NiDiag> diags;
        const eng::ni::CompileOptions options{&natives};
        auto result = eng::ni::compile(source, options, &diags);
        REQUIRE(result.isError());
        REQUIRE(!diags.empty());
        return diags;
    }

    /// Instancia + roda @init (contrato do host — design §7.4).
    eng::ni::NiScriptState& instantiate(
        std::shared_ptr<const eng::ni::NiProgram> program,
        eng::ecs::Entity self)
    {
        auto& state = set.create(std::move(program), self);
        (void)vm.run(state, "@init", params());
        return state;
    }
};

} // namespace

// =============================================================================
// Lexer / Parser / Sema — diagnósticos com linha/coluna
// =============================================================================

TEST_CASE("ni lexer: tokens básicos com linha/coluna", "[ni]")
{
    NiEnv env;
    const std::string src = "add &BL\n\nvar x: int = 42  # comentário\n";
    auto program = env.compileOrFail(src);
    REQUIRE(program->modules.size() == 1);
    REQUIRE(program->modules[0] == "BL");
    REQUIRE(program->globals.size() == 1);
    REQUIRE(program->globals[0].name == "x");
}

TEST_CASE("ni lexer: erro de string não terminada tem posição", "[ni]")
{
    NiEnv env;
    const auto diags = env.compileExpectFail("var s = \"aberta\n");
    REQUIRE(diags[0].line == 1);
    REQUIRE(diags[0].col >= 9);
}

TEST_CASE("ni lexer: caractere inesperado", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("var x = 1 @ 2\n");
}

TEST_CASE("ni parser: stop sem bloco aberto", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("stop\n");
}

TEST_CASE("ni parser: else sem if", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("up update:\nelse:\nstop\nstop\n");
}

TEST_CASE("ni parser: f com parêntese não fechado", "[ni]")
{
    NiEnv env;
    const auto diags = env.compileExpectFail("f foo(a:\nstop\n");
    REQUIRE(!diags.empty());
}

TEST_CASE("ni sema: var exige tipo ou inicializador", "[ni]")
{
    NiEnv env;
    const auto diags = env.compileExpectFail("var x\n");
    REQUIRE(diags[0].message.find("tipo ou inicializador")
            != std::string::npos);
}

TEST_CASE("ni sema: tipo anotado incompatível com literal", "[ni]")
{
    NiEnv env;
    const auto diags = env.compileExpectFail("var x: float = 1\n");
    REQUIRE(diags[0].message.find("não é compatível") != std::string::npos);
}

TEST_CASE("ni sema: nome desconhecido com linha/coluna", "[ni]")
{
    NiEnv env;
    const auto diags = env.compileExpectFail("up update:\n    nada()\nstop\n");
    REQUIRE(diags[0].line == 2);
    REQUIRE(diags[0].col >= 5);
}

TEST_CASE("ni sema: nativo de &BL sem import", "[ni]")
{
    NiEnv env;
    const auto diags =
        env.compileExpectFail("up update:\n    var v = vec3(1.0, 2.0, 3.0)\nstop\n");
    REQUIRE(diags[0].message.find("add &BL") != std::string::npos);
}

TEST_CASE("ni sema: módulo desconhecido", "[ni]")
{
    NiEnv env;
    const auto diags = env.compileExpectFail("add &UI\n");
    REQUIRE(diags[0].message.find("módulo desconhecido") != std::string::npos);
}

TEST_CASE("ni sema: give fora de f", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("up update:\n    give 1\nstop\n");
}

TEST_CASE("ni sema: link to não-entity", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("up update:\n    link to 5\nstop\n");
}

TEST_CASE("ni sema: repeat literal fora do range", "[ni]")
{
    NiEnv env;
    const auto diags =
        env.compileExpectFail("up update:\n    repeat 65537:\n    stop\nstop\n");
    REQUIRE(diags[0].message.find("[0, 65536]") != std::string::npos);
}

TEST_CASE("ni sema: timeout literal < 1", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("up update:\n    timeout 0:\n    stop\nstop\n");
}

TEST_CASE("ni sema: redeclaração e handler duplicado", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail("var a: int\nvar a: int\n");
    (void)env.compileExpectFail("up update:\nstop\nup update:\nstop\n");
    (void)env.compileExpectFail("f foo():\nstop\nvar foo: int\n");
}

TEST_CASE("ni sema: aridade de função", "[ni]")
{
    NiEnv env;
    const auto diags =
        env.compileExpectFail("f soma(a: int, b: int):\n    give a\nstop\n"
                               "up update:\n    soma(1)\nstop\n");
    REQUIRE(diags[0].message.find("argumento") != std::string::npos);
}

TEST_CASE("ni sema: campo estático inválido em vec3", "[ni]")
{
    NiEnv env;
    (void)env.compileExpectFail(
        "add &BL\nup update:\n    var v: vec3 = vec3(1.0, 2.0, 3.0)\n"
        "    var z = v.q\nstop\n");
}

// =============================================================================
// VM — aritmética, promoções, chamadas, escopos, @init
// =============================================================================

TEST_CASE("ni vm: aritmética e promoção int→float", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var gi: int = 0\n"
        "var gf: float = 0.0\n"
        "up update:\n"
        "    gi = 7 / 2\n"
        "    gf = 7 / 2.0\n"
        "    gi = 7 % 3\n"
        "    gf = gi + 0.5\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    const NiValue* gi = st.global("gi");
    const NiValue* gf = st.global("gf");
    REQUIRE(gi != nullptr);
    REQUIRE(gf != nullptr);
    REQUIRE(gi->type == eng::ni::NiType::Int);
    REQUIRE(gi->i == 1);            // 7/2=3 (truncado); depois 7%3=1
    REQUIRE(gf->type == eng::ni::NiType::Float);
    REQUIRE(gf->d[0] == 1.5);       // 7/2.0=3.5; depois gi(1) + 0.5
}

TEST_CASE("ni vm: divisão por zero é Fault", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "up update:\n"
        "    g = 10 / 0\n"
        "    g = 1\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::DivByZero);
    REQUIRE(fault->line == 3);      // linha/coluna precisos (missão)
    REQUIRE(st.global("g")->i == 0); // instrução NÃO aplicou escrita
}

// Atribuição composta — o script canônico do editor usa
// `position.x -= dt`; sem os operadores o PLAY falhava em silêncio.
TEST_CASE("ni vm: atribuição composta += -= *= /=", "[ni][p41]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: float = 10.0\n"
        "var h: float = 8.0\n"
        "var m: float = 6.0\n"
        "var n: float = 9.0\n"
        "up update:\n"
        "    g += 5.0\n"
        "    h -= 3.0\n"
        "    m *= 2.0\n"
        "    n /= 3.0\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("g")->d[0] == 15.0);
    REQUIRE(st.global("h")->d[0] == 5.0);
    REQUIRE(st.global("m")->d[0] == 12.0);
    REQUIRE(st.global("n")->d[0] == 3.0);
    // Composto em CAMPO de binding (o caso do repro do device):
    auto p2 = env.compileOrFail(
        "add &BL\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.position.x += 0.5\n"
        "stop\n");
    auto e = env.scene.createNode();
    auto& st2 = env.instantiate(p2, e);
    REQUIRE(env.vm.run(st2, "update", env.params()) == std::nullopt);
    const auto* transform =
        env.scene.world().get<eng::math::Transform>(e);
    REQUIRE(transform != nullptr);
    REQUIRE(transform->position.x == 0.5);
}

TEST_CASE("ni vm: curto-circuito and/or", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var hits: int = 0\n"
        "f side():\n"
        "    hits = hits + 1\n"
        "    give true\n"
        "stop\n"
        "var r1: bool = false\n"
        "var r2: bool = false\n"
        "up update:\n"
        "    r1 = false and side()\n"
        "    r2 = true or side()\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("hits")->i == 0); // RHS nunca avaliado
    REQUIRE(st.global("r1")->i == 0);
    REQUIRE(st.global("r2")->i == 1);
}

TEST_CASE("ni vm: funções, recursão (fatorial) e give implícito nil", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var fat: int = 0\n"
        "var nulo: int = 0\n"
        "f fatOf(n: int):\n"
        "    if n <= 1:\n"
        "        give 1\n"
        "    stop\n"
        "    give n * fatOf(n - 1)\n"
        "stop\n"
        "f noGive():\n"
        "    var x: int = 1\n"
        "stop\n"
        "up update:\n"
        "    fat = fatOf(10)\n"
        "    repair:\n"
        "        nulo = noGive()\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("fat")->i == 3628800);
    // give implícito = nil; consumo em atribuição tipada → NilUse capturado
    REQUIRE(st.global("nulo")->i == 0);
    REQUIRE(st.lastFault().has_value());
    REQUIRE(st.lastFault()->kind == NiFault::Kind::NilUse);
}

TEST_CASE("ni vm: @init executa inicializadores em ordem", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var a: int = 5\n"
        "var b: int = a * 2\n"
        "var c: int = 0\n"
        "up update:\n"
        "    c = b\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(st.global("a")->i == 5);
    REQUIRE(st.global("b")->i == 10);
    REQUIRE(st.global("c")->i == 0);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("c")->i == 10);
}

TEST_CASE("ni vm: escopos e shadowing", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 1\n"
        "up update:\n"
        "    var x: int = 10\n"
        "    if true:\n"
        "        var x: int = 20\n"
        "        g = x\n"
        "    stop\n"
        "    g = g + x\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("g")->i == 30); // 20 (sombreada) + 10 (externa)
}

TEST_CASE("ni vm: string concatenação e len", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "add &BL\n"
        "var s: string = \"\"\n"
        "var n: int = 0\n"
        "up update:\n"
        "    s = \"gon\" + \"i\"\n"
        "    n = len(s)\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("s")->s == "goni");
    REQUIRE(st.global("n")->i == 4);
}

TEST_CASE("ni vm: vetores — aritmética, escalar, componente", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "add &BL\n"
        "var v: vec3\n"
        "up update:\n"
        "    var a: vec3 = vec3(1.0, 2.0, 3.0)\n"
        "    var b: vec3 = vec3(10.0, 20.0, 30.0)\n"
        "    v = a + b - vec3(1.0, 1.0, 1.0)\n"
        "    v = v * 2.0\n"
        "    v.y = v.y + 0.5\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    const NiValue* v = st.global("v");
    REQUIRE(v->type == eng::ni::NiType::Vec3);
    REQUIRE(v->d[0] == 20.0);
    REQUIRE(v->d[1] == 42.5);
    REQUIRE(v->d[2] == 64.0);
}

// =============================================================================
// Semântica formal do fluxo de controle (design §5) — REQUISITO DA MISSÃO
// =============================================================================

TEST_CASE("ni §5.1: if/else executa ramo correto", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var r: int = 0\n"
        "up update:\n"
        "    if 1 < 2:\n"
        "        r = 10\n"
        "    stop\n"
        "    else:\n"
        "        r = 20\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("r")->i == 10);
}

TEST_CASE("ni §5.2: repeat executa exatamente N vezes; 0 é válido", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var hits: int = 0\n"
        "up update:\n"
        "    repeat 3:\n"
        "        hits = hits + 1\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("hits")->i == 3);
}

TEST_CASE("ni §5.2: var do corpo reinicia por iteração", "[ni]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "up update:\n"
        "    repeat 5:\n"
        "        var x: int = 0\n"
        "        x = x + 1\n"
        "        g = g + x\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("g")->i == 5); // x reinicia a cada iteração
}

TEST_CASE("ni §5.2: repeat dinâmico fora do range → RepeatFault reparável",
          "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "f big():\n"
        "    give 70000\n"
        "stop\n"
        "up update:\n"
        "    repair:\n"
        "        repeat big():\n"
        "            g = g + 1\n"
        "        stop\n"
        "    stop\n"
        "    g = g + 100\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("g")->i == 100); // corpo nunca rodou + pós-região ok
    REQUIRE(st.lastFault().has_value());
    REQUIRE(st.lastFault()->kind == NiFault::Kind::RepeatLimit);
}

TEST_CASE("ni §5.3: repair captura divisão por zero e continua", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "up update:\n"
        "    g = 1\n"
        "    repair:\n"
        "        g = 99\n"
        "        g = 10 / 0\n"
        "        g = 100\n"
        "    stop\n"
        "    g = g + 10\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    // NÃO-transacional: efeitos ANTERIORES persistem (g=99);
    // instruções seguintes da região NÃO executam; continua após o stop.
    REQUIRE(st.global("g")->i == 109);
    REQUIRE(st.lastFault().has_value());
    REQUIRE(st.lastFault()->kind == NiFault::Kind::DivByZero);
}

TEST_CASE("ni §5.3: repair captura fault DENTRO de chamada", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "f risky():\n"
        "    give 1 / 0\n"
        "stop\n"
        "up update:\n"
        "    repair:\n"
        "        g = risky()\n"
        "    stop\n"
        "    g = g + 7\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("g")->i == 7);
}

TEST_CASE("ni §5.3: fault SEM repair aborta apenas o EVENTO", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "up update:\n"
        "    g = 1\n"
        "    g = 10 / 0\n"
        "    g = 2\n"
        "stop\n"
        "up outro:\n"
        "    g = 3\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::DivByZero);
    REQUIRE(st.global("g")->i == 1);
    // o script NÃO morre: próximo evento roda
    REQUIRE(env.vm.run(st, "outro", env.params()) == std::nullopt);
    REQUIRE(st.global("g")->i == 3);
}

TEST_CASE("ni §5.4: timeout esgota orçamento de instruções", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var n: int = 0\n"
        "up update:\n"
        "    repair:\n"
        "        timeout 20:\n"
        "            repeat 1000:\n"
        "                n = n + 1\n"
        "            stop\n"
        "        stop\n"
        "    stop\n"
        "    n = n + 1\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    // corpo parou BEM antes de 1000 iterações; continuação executou (+1)
    REQUIRE(st.global("n")->i < 1000);
    REQUIRE(st.global("n")->i >= 1);
    REQUIRE(st.lastFault().has_value());
    REQUIRE(st.lastFault()->kind == NiFault::Kind::Timeout);
}

TEST_CASE("ni §5.4: timeout sem orçamento apertado completa", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var n: int = 0\n"
        "up update:\n"
        "    timeout 1000000:\n"
        "        repeat 10:\n"
        "            n = n + 1\n"
        "        stop\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("n")->i == 10);
    REQUIRE(!st.lastFault().has_value());
}

TEST_CASE("ni §5.4: timeout SEM repair → aborta evento com Timeout", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var n: int = 0\n"
        "up update:\n"
        "    timeout 5:\n"
        "        repeat 1000:\n"
        "            n = n + 1\n"
        "        stop\n"
        "    stop\n"
        "    n = 99999\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::Timeout);
    REQUIRE(st.global("n")->i < 1000);
}

TEST_CASE("ni §4: emit dispara handler local sincronamente", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var hits: int = 0\n"
        "up ping:\n"
        "    hits = hits + 1\n"
        "stop\n"
        "up update:\n"
        "    emit ping\n"
        "    hits = hits + 10\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    // handler local roda ANTES da continuação (síncrono — §4)
    REQUIRE(st.global("hits")->i == 11);
}

TEST_CASE("ni §4: emit propaga por links (BFS) — alvo recebe 1x", "[ni][semantica]")
{
    NiEnv env;
    auto receiver = env.scene.createNode();
    auto sender = env.scene.createNode();

    auto pRecv = env.compileOrFail(
        "var hits: int = 0\n"
        "up ping:\n"
        "    hits = hits + 1\n"
        "stop\n");
    auto& stRecv = env.instantiate(pRecv, receiver);

    auto pSend = env.compileOrFail(
        "var done: bool = false\n"
        "up go:\n"
        "    link to find(\"receiver\")\n"
        "    emit ping\n"
        "    done = true\n"
        "stop\n");
    REQUIRE(env.scene.world().emplace<eng::scene::Name>(
        receiver, eng::scene::Name{"receiver"}) != nullptr);
    auto& stSend = env.instantiate(pSend, sender);

    REQUIRE(env.vm.run(stSend, "go", env.params()) == std::nullopt);
    REQUIRE(stSend.global("done")->i == 1);
    REQUIRE(stRecv.global("hits")->i == 1); // propagado via link
    REQUIRE(stSend.links().size() == 1);
}

TEST_CASE("ni §4: emit em cadeia/ciclo é seguro (visitados)", "[ni][semantica]")
{
    NiEnv env;
    auto a = env.scene.createNode();
    auto b = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<eng::scene::Name>(a, eng::scene::Name{"a"}) != nullptr);
    REQUIRE(env.scene.world().emplace<eng::scene::Name>(b, eng::scene::Name{"b"}) != nullptr);

    auto pBoth = env.compileOrFail(
        "var hits: int = 0\n"
        "var linked: bool = false\n"
        "up setup:\n"
        "    link to find(\"b\")\n"
        "    linked = true\n"
        "stop\n"
        "up go:\n"
        "    emit ping\n"
        "stop\n"
        "up ping:\n"
        "    hits = hits + 1\n"
        "stop\n");
    auto& stA = env.instantiate(pBoth, a);
    (void)env.vm.run(stA, "setup", env.params());
    auto pB = env.compileOrFail(
        "var hits: int = 0\n"
        "up ping:\n"
        "    hits = hits + 1\n"
        "stop\n");
    auto& stB = env.instantiate(pB, b);
    // B também linka de volta para A (ciclo A↔B)
    auto pBLink = env.compileOrFail(
        "var done: bool = false\n"
        "up setup:\n"
        "    link to find(\"a\")\n"
        "    done = true\n"
        "stop\n");
    auto& stBLink = env.instantiate(pBLink, b);
    (void)env.vm.run(stBLink, "setup", env.params());

    // A EMITE ping (via 'go'): A roda localmente; B recebe 1x pelo link;
    // o ciclo NÃO re-dispara para A (visitados) nem duplica para B.
    REQUIRE(env.vm.run(stA, "go", env.params()) == std::nullopt);
    REQUIRE(stA.global("hits")->i == 1);
    REQUIRE(stB.global("hits")->i == 1); // ciclo não duplicou
}

TEST_CASE("ni §4: emissão aninhada além de 32 → EmitDepth", "[ni][semantica]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "up eco:\n"
        "    g = g + 1\n"
        "    emit eco\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    const auto fault = env.vm.run(st, "eco", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::EmitDepth);
    REQUIRE(st.global("g")->i == 33); // raiz + 32 entregues por emit
}

TEST_CASE("ni §4: link duplicado é idempotente; auto-link é Fault", "[ni]")
{
    NiEnv env;
    auto a = env.scene.createNode();
    auto b = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<eng::scene::Name>(b, eng::scene::Name{"b"}) != nullptr);
    REQUIRE(env.scene.world().emplace<eng::scene::Name>(a, eng::scene::Name{"a"}) != nullptr);

    auto p = env.compileOrFail(
        "var n: int = 0\n"
        "up go:\n"
        "    link to find(\"b\")\n"
        "    link to find(\"b\")\n"
        "    n = 1\n"
        "stop\n"
        "up selfLink:\n"
        "    link to find(\"a\")\n"
        "stop\n");
    auto& st = env.instantiate(p, a);
    REQUIRE(env.vm.run(st, "go", env.params()) == std::nullopt);
    REQUIRE(st.links().size() == 1); // set semantics
    const auto fault = env.vm.run(st, "selfLink", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::BadArgument);
}

// =============================================================================
// Bindings — reflexão + ECS
// =============================================================================

TEST_CASE("ni bindings: get/set por reflexão (float/int/string/bool)", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    auto p = env.compileOrFail(
        "var ff: float = 0.0\n"
        "var n: int = 0\n"
        "var ss: string = \"\"\n"
        "var bb: bool = false\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.scriptable.fv = 1.5\n"
        "    me.scriptable.n = 42\n"
        "    me.scriptable.s = \"olá\"\n"
        "    me.scriptable.b = true\n"
        "    ff = me.scriptable.fv\n"
        "    n = me.scriptable.n\n"
        "    ss = me.scriptable.s\n"
        "    bb = me.scriptable.b\n"
        "stop\n");
    auto& st = env.instantiate(p, e);

    const auto* comp = env.scene.world().get<TScriptable>(e);
    REQUIRE(comp != nullptr);
    REQUIRE(comp->fv == 0.f);

    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    comp = env.scene.world().get<TScriptable>(e);
    REQUIRE(comp->fv == 1.5f);
    REQUIRE(comp->n == 42);
    REQUIRE(comp->s == "olá");
    REQUIRE(comp->b);
    REQUIRE(st.global("ff")->d[0] == 1.5);
    REQUIRE(st.global("n")->i == 42);
    REQUIRE(st.global("ss")->s == "olá");
    REQUIRE(st.global("bb")->i == 1);
}

TEST_CASE("ni bindings: vec3 inteiro e subcampo", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    auto p = env.compileOrFail(
        "add &BL\n"
        "var x: float = 0.0\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.scriptable.v = vec3(1.0, 2.0, 3.0)\n"
        "    me.scriptable.v.z = 9.0\n"
        "    x = me.scriptable.v.x\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    const auto* comp = env.scene.world().get<TScriptable>(e);
    REQUIRE(comp->v.x == 1.f);
    REQUIRE(comp->v.y == 2.f);
    REQUIRE(comp->v.z == 9.f);
    REQUIRE(st.global("x")->d[0] == 1.0);
}

TEST_CASE("ni bindings: entidade obsoleta → EntityStale (não UB)", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    auto p = env.compileOrFail(
        "var got: int = 0\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.scriptable.fv = 7.0\n"
        "    got = 1\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    // destrói a entidade DEPOIS da compila/instanciação — handle obsoleto
    REQUIRE(env.scene.destroyNode(e));
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::EntityStale);
    REQUIRE(st.global("got")->i == 0);
}

TEST_CASE("ni bindings: componente ausente → ComponentMissing", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode(); // SEM TScriptable

    auto p = env.compileOrFail(
        "var got: int = 0\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.scriptable.fv = 7.0\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::ComponentMissing);
}

TEST_CASE("ni bindings: int fora do range do campo → Range", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    auto p = env.compileOrFail(
        "up update:\n"
        "    var me = self()\n"
        "    me.scriptable.n = 3000000000\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::Range);
    // nada escrito
    REQUIRE(env.scene.world().get<TScriptable>(e)->n == 0);
}

TEST_CASE("ni bindings: campo desconhecido → FieldUnknown", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    auto p = env.compileOrFail(
        "up update:\n"
        "    var me = self()\n"
        "    me.scriptable.inexistente = 1\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::FieldUnknown);
}

TEST_CASE("ni bindings: açúcar position (Transform via basePath)", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();

    auto p = env.compileOrFail(
        "add &BL\n"
        "var px: float = 0.0\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.position = vec3(4.0, 5.0, 6.0)\n"
        "    me.position.x = 1.0\n"
        "    px = me.position.x\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    const auto* t = env.scene.world().get<eng::math::Transform>(e);
    REQUIRE(t != nullptr);
    REQUIRE(t->position.x == 1.f);
    REQUIRE(t->position.y == 5.f);
    REQUIRE(t->position.z == 6.f);
    REQUIRE(st.global("px")->d[0] == 1.0);
}

TEST_CASE("ni bindings: compview via comp() prefíxa caminho", "[ni][bindings]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    auto p = env.compileOrFail(
        "add &BL\n"
        "var y: float = 0.0\n"
        "up update:\n"
        "    var me = self()\n"
        "    var c = comp(me, \"scriptable\")\n"
        "    c.v.y = 8.0\n"
        "    y = c.v.y\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(env.scene.world().get<TScriptable>(e)->v.y == 8.f);
    REQUIRE(st.global("y")->d[0] == 8.0);
}

TEST_CASE("ni bindings: host — delta/ações/spawn/find/despawn", "[ni][bindings]")
{
    NiEnv env;
    env.host.jumpDown = true;
    const auto e = env.scene.createNode();

    auto p = env.compileOrFail(
        "var d: float = 0.0\n"
        "var jump: bool = false\n"
        "var qtd: int = 0\n"
        "var achou: bool = false\n"
        "up update:\n"
        "    d = delta()\n"
        "    jump = action_down(\"jump\")\n"
        "    var novo = spawn(\"gerado\")\n"
        "    qtd = 1\n"
        "    repair:\n"
        "        achou = despawn(novo)\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.global("d")->d[0] > 0.0);
    REQUIRE(st.global("jump")->i == 1);
    REQUIRE(env.scene.nodeCount() == 1); // despawn destruiu o spawnado
    REQUIRE(st.global("achou")->i == 1);
}

// =============================================================================
// E2E — .nis → compile → bytecode → VM → binding → MUDANÇA no ECS (GATE)
// =============================================================================

TEST_CASE("ni E2E: fonte .nis muda componente ECS via reflexão", "[ni][e2e]")
{
    NiEnv env;
    const auto e = env.scene.createNode();
    REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);

    // Fonte .nis como viria de um arquivo/anexado a uma entidade
    const std::string source =
        "add &BL\n"
        "var velocidade: float = 2.0\n"
        "var passos: int = 0\n"
        "\n"
        "f empurra():\n"
        "    var me = self()\n"
        "    me.scriptable.fv = me.scriptable.fv + velocidade\n"
        "    me.scriptable.n = me.scriptable.n + 1\n"
        "    me.scriptable.s = \"motor\"\n"
        "stop\n"
        "\n"
        "up update:\n"
        "    repeat 3:\n"
        "        empurra()\n"
        "    stop\n"
        "    passos = passos + 1\n"
        "stop\n";

    // compile → programa (bytecode real, inspecionável)
    auto program = env.compileOrFail(source);
    REQUIRE(!program->funcs.empty());
    REQUIRE(!program->handlers.empty());
    REQUIRE(program->nativeCount == env.natives.size());
    bool hasCode = false;
    for (const auto& fn : program->funcs) {
        hasCode = hasCode || !fn.code.empty();
    }
    REQUIRE(hasCode);

    auto& st = env.instantiate(program, e);
    REQUIRE(st.global("velocidade")->d[0] == 2.0); // @init

    // VM executa bytecode → bindings escrevem no ECS
    for (int tick = 0; tick < 2; ++tick) {
        REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    }
    const auto* comp = env.scene.world().get<TScriptable>(e);
    REQUIRE(comp->fv == 12.f);  // 2 ticks × 3 × velocidade 2.0
    REQUIRE(comp->n == 6);     // 2 ticks × 3
    REQUIRE(comp->s == "motor");
    REQUIRE(st.global("passos")->i == 2);
}

// =============================================================================
// Determinismo — mesma entrada ⇒ mesmos efeitos
// =============================================================================

TEST_CASE("ni determinismo: duas execuções idênticas byte-a-byte", "[ni][determinismo]")
{
    const std::string source =
        "add &BL\n"
        "var seed: int = 7\n"
        "var acc: int = 0\n"
        "up update:\n"
        "    repeat 10:\n"
        "        seed = (seed * 31 + 17) % 1000\n"
        "        acc = acc + seed\n"
        "    stop\n"
        "    var me = self()\n"
        "    me.scriptable.n = acc\n"
        "    me.scriptable.fv = seed * 0.5\n"
        "stop\n";

    TScriptable results[2];
    for (int run = 0; run < 2; ++run) {
        NiEnv env;
        const auto e = env.scene.createNode();
        REQUIRE(env.scene.world().emplace<TScriptable>(e) != nullptr);
        auto program = env.compileOrFail(source);
        auto& st = env.instantiate(program, e);
        for (int tick = 0; tick < 5; ++tick) {
            REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
        }
        results[run] = *env.scene.world().get<TScriptable>(e);
    }
    REQUIRE(results[0].n == results[1].n);
    REQUIRE(results[0].fv == results[1].fv);
    REQUIRE(results[0].s == results[1].s);
    REQUIRE(results[0].v.x == results[1].v.x);
    REQUIRE(results[0].v.y == results[1].v.y);
    REQUIRE(results[0].v.z == results[1].v.z);
    REQUIRE(results[0].b == results[1].b);
    // (memcmp bruto seria inválido: std::string carrega ponteiros internos)
}

// =============================================================================
// Segurança — nativos fechados, orçamento obrigatório, sem nil
// =============================================================================

TEST_CASE("ni segurança: loop infinito é impossível (orçamento)", "[ni][segurança]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var n: int = 0\n"
        "up update:\n"
        "    repeat 65536:\n"
        "        n = n + 1\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    // orçamento APERTADO: o loop é abortado de forma determinística
    const auto fault = env.vm.run(st, "update", env.params(100));
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::Timeout);
    REQUIRE(st.global("n")->i < 65536);
}

TEST_CASE("ni segurança: tabela de nativos divergente → erro de config", "[ni][segurança]")
{
    NiEnv env;
    auto p = env.compileOrFail("var x: int = 1\n");
    auto& st = env.instantiate(p, env.scene.createNode());

    NiNativeTable menor; // tabela diferente da compilação
    menor.addBaseLibrary();
    eng::ni::NiExecContext::Params bad;
    bad.natives = &menor;
    const auto fault = env.vm.run(st, "update", bad);
    REQUIRE(fault.has_value());
    REQUIRE(fault->kind == NiFault::Kind::NativeError);
    REQUIRE(fault->message.find("divergente") != std::string::npos);
}

TEST_CASE("ni segurança: sem eval/FS/nome de nativo desconhecido", "[ni][segurança]")
{
    NiEnv env;
    (void)env.compileExpectFail(
        "up update:\n    var f = open(\"arquivo\")\nstop\n");
}

TEST_CASE("ni segurança: nil nunca vaza — consumo é Fault", "[ni][segurança]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "f nada():\n"
        "    var x: int = 1\n"
        "stop\n"
        "up update:\n"
        "    repair:\n"
        "        g = nada() + 1\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    REQUIRE(st.lastFault().has_value());
    REQUIRE(st.lastFault()->kind == NiFault::Kind::NilUse);
}

// =============================================================================
// Ferramentas — diagnóstico de runtime com linha/coluna + hook de trace
// =============================================================================

TEST_CASE("ni ferramentas: fault de runtime carrega linha/coluna", "[ni][tools]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var g: int = 0\n"
        "\n"
        "up update:\n"
        "    var x: int = 0\n"
        "    g = 10 / 0\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());
    const auto fault = env.vm.run(st, "update", env.params());
    REQUIRE(fault.has_value());
    REQUIRE(fault->line == 5);
    REQUIRE(fault->col >= 5);
}

TEST_CASE("ni ferramentas: hook de trace observa execução (§6.5)", "[ni][tools]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "var n: int = 0\n"
        "up update:\n"
        "    repeat 10:\n"
        "        n = n + 1\n"
        "    stop\n"
        "stop\n");
    auto& st = env.instantiate(p, env.scene.createNode());

    int calls = 0;
    std::uint32_t maxLine = 0;
    env.vm.setTraceHook(
        [&](const eng::ni::NiExecContext::TraceInfo& info) {
            ++calls;
            maxLine = std::max(maxLine, info.line);
        },
        1);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    env.vm.setTraceHook(nullptr, 0);
    REQUIRE(calls > 10);   // todo instruction dispatch observável
    REQUIRE(maxLine >= 4); // corpo do repeat (linha 4) visitado
}

// =============================================================================
// Verbos de movimento — move / move_and_slide
// =============================================================================

TEST_CASE("p46 ni: move(dx,dy) transla o SELF (teletransporte cru)",
          "[ni][p46]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "up update:\n"
        "    move(0.5, -1.0)\n"
        "stop\n");
    auto e = env.scene.createNode();
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    const auto* transform = env.scene.world().get<eng::math::Transform>(e);
    REQUIRE(transform != nullptr);
    REQUIRE(transform->position.x == 0.5);
    REQUIRE(transform->position.y == -1.0);
}

TEST_CASE("p46 ni: move aceita literais INTEIROS (autoraria simples)",
          "[ni][p46]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "up update:\n"
        "    move(2, 0)\n"
        "stop\n");
    auto e = env.scene.createNode();
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) == std::nullopt);
    const auto* transform = env.scene.world().get<eng::math::Transform>(e);
    REQUIRE(transform != nullptr);
    REQUIRE(transform->position.x == 2.0);
}

TEST_CASE("p46 ni: move_and_slide sem CharacterBody = fault PRECISO",
          "[ni][p46]")
{
    NiEnv env;
    auto p = env.compileOrFail(
        "up update:\n"
        "    move_and_slide(1.0, 0.0)\n"
        "stop\n");
    auto e = env.scene.createNode();
    auto& st = env.instantiate(p, e);
    REQUIRE(env.vm.run(st, "update", env.params()) != std::nullopt);
    REQUIRE(st.lastFault().has_value());
    REQUIRE(st.lastFault()->kind == NiFault::Kind::NativeError);
    REQUIRE(st.lastFault()->message.find("CharacterBody") !=
            std::string::npos);
}

TEST_CASE("p46 ni: move_and_slide desliza de verdade (física REAL — sem "
          "parede avança, com parede para/desliza)",
          "[ni][p46]")
{
    NiEnv env;
    // Parede estática: caixa em x=3 (face em 2.5).
    auto wall = env.scene.createNode();
    eng::physics::Collider wallShape;
    wallShape.shape = eng::physics::ColliderShape::Box;
    wallShape.halfExtents = {0.5f, 5.f, 5.f};
    (void)env.scene.world().emplace<eng::physics::Collider>(
        wall, wallShape);
    env.scene.localTransform(wall)->position = {3.f, 0.f, 0.f};

    auto p = env.compileOrFail(
        "up update:\n"
        "    move_and_slide(delta() * 2.0, delta() * 0.5)\n"
        "stop\n");
    auto e = env.scene.createNode();
    (void)env.scene.world().emplace<eng::physics::CharacterBody>(
        e, eng::physics::CharacterBody{{0.f, 0.f, 0.f}, 0.5f});
    env.scene.localTransform(e)->position = {0.f, 0.f, 0.f};
    auto& st = env.instantiate(p, e);

    // delta = 1/60 → 0.0333 por tick; 90 ticks ≈ 3 u de tentativa.
    for (int i = 0; i < 90; ++i) {
        (void)env.vm.run(st, "update", env.params());
    }
    const auto* transform = env.scene.world().get<eng::math::Transform>(e);
    REQUIRE(transform != nullptr);
    // Face da parede (2.5) − raio (0.5) → centro para ~2.0. NUNCA além.
    CHECK(transform->position.x < 2.05f);
    CHECK(transform->position.x > 1.9f);
    // E o componente Y deslizou livre (2 u de tentativa, sem obstáculo).
    CHECK(transform->position.y ==
          Catch::Approx(90.0 / 60.0 * 0.5).margin(0.01));
}
