/// Testes do EditorProtocol — a porta única da UI (snapshot + call JSON).
///
/// Cobrem o fluxo do usuário de ponta a ponta sem Android: criar projeto a
/// partir de modelo, criar entidades por modelo, desfazer, scripts ligados a
/// arquivo e o jogo de plataforma rodando com controles.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "eng/editor/EditorDocument.hpp"
#include "eng/editor/EditorProtocol.hpp"
#include "eng/editor/Inspector.hpp"
#include "eng/fs/MemoryFileSystem.hpp"
#include "eng/input/Input.hpp"

namespace {

using Json = nlohmann::json;
using eng::editor::EditorDocument;
using eng::editor::EditorProtocol;

struct Fixture {
    eng::fs::MemoryFileSystem fs;
    std::unique_ptr<EditorDocument> doc;
    std::unique_ptr<EditorProtocol> proto;

    Fixture()
    {
        auto created = EditorDocument::create(fs, eng::fs::Path{"."});
        REQUIRE(created.ok());
        doc = std::move(created.value());
        proto = std::make_unique<EditorProtocol>(*doc);
    }

    Json call(const Json& req)
    {
        const std::string text = proto->call(req.dump());
        Json out = Json::parse(text, nullptr, false);
        REQUIRE_FALSE(out.is_discarded());
        return out;
    }

    Json ok(const Json& req)
    {
        Json out = call(req);
        INFO(req.dump() << " -> " << out.dump());
        REQUIRE(out["ok"].get<bool>());
        return out["result"];
    }

    Json state()
    {
        Json s = Json::parse(proto->snapshot(0), nullptr, false);
        REQUIRE(s.is_object());
        return s;
    }

    std::uint64_t idOf(const std::string& name)
    {
        const Json s = state();
        for (const auto& n : s["hierarchy"]) {
            if (n["name"] == name) {
                return n["id"].get<std::uint64_t>();
            }
        }
        FAIL("entidade não encontrada: " << name);
        return 0;
    }

    double field(std::uint64_t id, const char* comp, const char* path)
    {
        auto v = eng::editor::Inspector::getField(
            *doc->sceneInFocus(), EditorDocument::unpackEntity(id), comp, path);
        REQUIRE(v.ok());
        return std::atof(v.value().c_str());
    }

    void key(eng::input::Key k, bool down)
    {
        eng::input::InputEvent e;
        e.device = eng::input::DeviceKind::Keyboard;
        e.key = k;
        e.keyDown = down;
        doc->runtimeInput().queueEvent(e);
    }

    void run(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            doc->tick(1.f / 60.f);
        }
    }
};

} // namespace

TEST_CASE("protocol: pedidos inválidos devolvem erro, nunca crash", "[protocol]")
{
    Fixture f;
    Json bad = Json::parse(f.proto->call("{nao é json"), nullptr, false);
    CHECK_FALSE(bad["ok"].get<bool>());
    Json unknown = f.call({{"op", "nada.disso"}});
    CHECK_FALSE(unknown["ok"].get<bool>());
    CHECK(unknown["error"].get<std::string>().find("nada.disso") !=
          std::string::npos);
    // Sem projeto: erro com mensagem, não abort.
    Json noProject = f.call({{"op", "script.list"}});
    CHECK_FALSE(noProject["ok"].get<bool>());
}

TEST_CASE("protocol: snapshot só muda quando o estado muda", "[protocol]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Snap"}});
    const Json s = f.state();
    const auto key = s["key"].get<std::uint64_t>();
    CHECK(f.proto->snapshot(key).empty());
    f.ok({{"op", "entity.create"}, {"template", "sprite"}});
    CHECK_FALSE(f.proto->snapshot(key).empty());
}

TEST_CASE("protocol: modelo de entidade vira um único passo de desfazer",
          "[protocol]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Undo"}});
    const auto before = f.state()["hierarchy"].size();
    const Json id = f.ok({{"op", "entity.create"}, {"template", "physics"}});
    const Json s = f.state();
    CHECK(s["hierarchy"].size() == before + 1);
    CHECK(s["selection"] == id);

    const Json insp = f.ok({{"op", "inspector"}, {"id", id}});
    std::vector<std::string> comps;
    for (const auto& c : insp["components"]) {
        comps.push_back(c["name"].get<std::string>());
    }
    CHECK(std::find(comps.begin(), comps.end(), "eng::physics::Collider") !=
          comps.end());
    CHECK(std::find(comps.begin(), comps.end(), "eng::physics::RigidBody") !=
          comps.end());

    f.ok({{"op", "history.undo"}});
    CHECK(f.state()["hierarchy"].size() == before);
}

TEST_CASE("protocol: script ligado a arquivo acompanha as edições", "[protocol]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Link"}});
    const Json id = f.ok({{"op", "entity.create"}, {"template", "empty"}});
    const Json name = f.ok({{"op", "script.create"}, {"name", "mover"}});
    CHECK(name == "mover.nis");
    f.ok({{"op", "script.assign"}, {"id", id}, {"name", "mover.nis"}});
    const std::string source = "up update:\n    teleport(1.0, 2.0)\nstop\n";
    f.ok({{"op", "script.write"}, {"name", "mover.nis"}, {"content", source}});

    auto v = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(),
        EditorDocument::unpackEntity(id.get<std::uint64_t>()),
        "eng::editor::NiScriptComponent", "source");
    REQUIRE(v.ok());
    CHECK(v.value() == source);

    const Json diags = f.ok({{"op", "script.compile"}, {"source", "up x:\n"}});
    CHECK_FALSE(diags["ok"].get<bool>());
    CHECK(diags["diags"].size() >= 1);
}

TEST_CASE("protocol: modelo Plataforma 2D é jogável (cai, anda e pula)",
          "[protocol]")
{
    Fixture f;
    const Json folder =
        f.ok({{"op", "project.new"}, {"name", "Plataforma"}, {"template", "platformer"}});
    CHECK(folder == "Plataforma");
    const Json scripts = f.ok({{"op", "script.list"}});
    REQUIRE(scripts.size() == 1);
    CHECK(scripts[0] == "jogador.nis");
    const Json compiled = f.ok({{"op", "script.compile"},
                                {"source", f.ok({{"op", "script.read"},
                                                 {"name", "jogador.nis"}})}});
    INFO(compiled.dump());
    REQUIRE(compiled["ok"].get<bool>());

    const auto player = f.idOf("Jogador");
    const double startX = f.field(player, "eng::math::Transform", "position.x");

    f.ok({{"op", "play.start"}});
    f.run(120);  // cai e assenta no chão (topo em y = -1.5)
    const double restY = f.field(player, "eng::math::Transform", "position.y");
    INFO("y parado = " << restY);
    CHECK(restY > -1.2);
    CHECK(restY < -0.8);

    f.key(eng::input::Key::Right, true);
    f.run(30);
    f.key(eng::input::Key::Right, false);
    f.run(1);
    const double walkedX = f.field(player, "eng::math::Transform", "position.x");
    INFO("x = " << walkedX);
    CHECK(walkedX > startX + 1.0);

    f.key(eng::input::Key::Space, true);
    f.run(1);
    f.key(eng::input::Key::Space, false);
    double jumpY = restY;
    for (int i = 0; i < 60; ++i) {
        f.run(1);
        jumpY = std::max(jumpY,
                         f.field(player, "eng::math::Transform", "position.y"));
    }
    INFO("y no pulo = " << jumpY);
    CHECK(jumpY > restY + 1.5);

    const Json hud = f.ok({{"op", "play.hud"}});
    CHECK(hud["scripts"]["compiled"] == 1);
    INFO(hud.dump());
    CHECK(hud["scripts"]["faults"] == 0);

    f.ok({{"op", "play.stop"}});
    CHECK(f.field(player, "eng::math::Transform", "position.x") == startX);
}

TEST_CASE("protocol: projeto salvo reabre com a cena", "[protocol]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Volta"}, {"template", "platformer"}});
    const auto count = f.state()["hierarchy"].size();
    f.ok({{"op", "project.new"}, {"name", "Outro"}});
    f.ok({{"op", "project.open"}, {"folder", "Volta"}});
    CHECK(f.state()["hierarchy"].size() == count);
    const Json scenes = f.ok({{"op", "scene.list"}});
    CHECK(scenes.size() >= 1);
}
