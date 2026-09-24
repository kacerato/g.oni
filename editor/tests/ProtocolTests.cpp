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

TEST_CASE("protocol: todas as operações que a UI usa respondem sem abortar",
          "[protocol]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Tudo"}, {"template", "platformer"}});
    const auto player = f.idOf("Jogador");
    const auto ground = f.idOf("Chão");

    // Estado e ajustes.
    const Json settings = f.ok({{"op", "settings.get"}});
    CHECK(settings["grid"].is_object());
    CHECK(settings["collisionLayers"].is_array());
    f.ok({{"op", "settings.grid"}, {"visible", false}, {"cell", 2.0}});
    f.ok({{"op", "settings.physicsDt"}, {"dt", 1.0 / 30.0}});
    f.ok({{"op", "layer.add"}, {"name", "Fundo"}});
    f.ok({{"op", "layer.set"}, {"name", "Fundo"}, {"timeScale", 0.5},
          {"update", true}, {"physics", false}, {"render", true}});
    const Json bit = f.ok({{"op", "collision.add"}, {"name", "Inimigos"}});
    f.ok({{"op", "collision.rename"}, {"bit", bit}, {"name", "Vilões"}});
    f.ok({{"op", "host.info"}});
    f.ok({{"op", "project.rename"}, {"name", "Tudo Novo"}});

    // Entidades.
    f.ok({{"op", "entity.select"}, {"id", 0}});
    CHECK(f.state()["selection"] == 0);
    f.ok({{"op", "entity.select"}, {"id", player}});
    f.ok({{"op", "entity.rename"}, {"id", player}, {"name", "Herói"}});
    const Json dup = f.ok({{"op", "entity.duplicate"}, {"id", ground}});
    f.ok({{"op", "entity.reparent"}, {"id", dup}, {"parent", ground}});
    f.ok({{"op", "entity.delete"}, {"id", dup}});
    for (const char* t : {"sprite", "camera", "ground", "physics", "particles",
                          "light", "audio", "empty"}) {
        f.ok({{"op", "entity.create"}, {"template", t}});
    }
    CHECK_FALSE(f.call({{"op", "entity.create"}, {"template", "dragão"}})["ok"]
                    .get<bool>());

    // Transform e inspector.
    const Json t = f.ok({{"op", "transform.get"}, {"id", player}});
    REQUIRE(t["p"].size() == 3);
    f.ok({{"op", "transform.set"}, {"id", player},
          {"p", {1.5, 2.0, 0.0}}, {"r", {0.0, 0.0, 45.0}}, {"s", {2.0, 2.0, 1.0}}});
    const Json t2 = f.ok({{"op", "transform.get"}, {"id", player}});
    CHECK(t2["p"][0].get<double>() == 1.5);
    const Json insp = f.ok({{"op", "inspector"}, {"id", player}});
    bool sawColor = false;
    for (const auto& c : insp["components"]) {
        for (const auto& fld : c["fields"]) {
            if (fld["kind"] == "color") sawColor = true;
            if (fld.contains("options")) CHECK(fld["options"].is_array());
        }
    }
    CHECK(sawColor);
    f.ok({{"op", "component.set"}, {"id", player},
          {"component", "eng::editor::SpriteData"},
          {"path", "tintR,tintG,tintB"}, {"value", "#FF8800"}});
    CHECK_FALSE(f.call({{"op", "component.set"}, {"id", player},
                        {"component", "eng::physics::Collider"},
                        {"path", "radius"}, {"value", "-3"}})["ok"]
                    .get<bool>());
    const Json catalog = f.ok({{"op", "component.catalog"}, {"id", player}});
    CHECK(catalog.is_array());
    const Json added = f.ok({{"op", "component.add"}, {"id", ground},
                             {"name", "eng::editor::AudioSource"}});
    CHECK(added.is_array());
    f.ok({{"op", "component.remove"}, {"id", ground},
          {"name", "eng::editor::AudioSource"}});
    f.ok({{"op", "inspector"}, {"id", 0}});  // entidade inválida: null

    // Ferramentas e histórico.
    f.ok({{"op", "tool.set"}, {"tool", 2}});
    CHECK(f.state()["tool"] == 2);
    f.ok({{"op", "snap.set"}, {"translate", true}, {"rotate", true}});
    f.ok({{"op", "viewport.fit"}});
    f.ok({{"op", "viewport.zoom"}, {"factor", 1.5}, {"x", 10}, {"y", 10}});
    f.ok({{"op", "history.undo"}});
    f.ok({{"op", "history.redo"}});

    // Biblioteca.
    CHECK(f.ok({{"op", "asset.categories"}}).size() >= 5);
    CHECK(f.ok({{"op", "asset.list"}, {"category", "textures"}}).is_array());
    f.ok({{"op", "asset.imageInfo"}, {"name", "nada.png"}});
    f.ok({{"op", "anim.create"}, {"name", "andar"}});
    const Json anims = f.ok({{"op", "anim.list"}});
    REQUIRE(anims.size() == 1);
    const std::string animName = anims[0]["name"].get<std::string>();
    f.ok({{"op", "anim.setMeta"}, {"name", animName}, {"loop", false}, {"fps", 10}});
    CHECK_FALSE(f.ok({{"op", "anim.read"}, {"name", animName}})
                    .get<std::string>()
                    .empty());
    f.ok({{"op", "material.create"}, {"name", "brilho"}});
    const Json mats = f.ok({{"op", "material.list"}});
    REQUIRE(mats.size() == 1);
    const std::string mat = mats[0]["name"].get<std::string>();
    const std::string matJson =
        f.ok({{"op", "material.read"}, {"name", mat}}).get<std::string>();
    f.ok({{"op", "material.write"}, {"name", mat}, {"json", matJson}});
    f.ok({{"op", "asset.rename"}, {"category", "materials"}, {"name", mat},
          {"newName", "luz.mat.json"}});
    f.ok({{"op", "asset.delete"}, {"category", "materials"},
          {"name", "luz.mat.json"}});
    f.ok({{"op", "audio.stop"}});
    CHECK(f.ok({{"op", "audio.playing"}}) == false);

    // Cenas e projeto.
    f.ok({{"op", "scene.save"}, {"path", "fase 2"}});
    const Json scenes = f.ok({{"op", "scene.list"}});
    CHECK(std::find(scenes.begin(), scenes.end(), Json("fase 2.json")) !=
          scenes.end());
    f.ok({{"op", "project.save"}});
    f.ok({{"op", "project.exportZip"}, {"path", "tudo.zip"}});
    const Json imported =
        f.ok({{"op", "project.importZip"}, {"path", "tudo.zip"}, {"name", "Cópia"}});
    CHECK_FALSE(imported.get<std::string>().empty());
    CHECK(f.ok({{"op", "project.list"}}).size() >= 2);

    // Play.
    f.ok({{"op", "play.start"}});
    f.run(10);
    f.ok({{"op", "play.pause"}, {"paused", true}});
    CHECK(f.state()["paused"] == true);
    f.ok({{"op", "play.pause"}, {"paused", false}});
    const Json hud = f.ok({{"op", "play.hud"}});
    CHECK(hud["scripts"].is_object());
    f.ok({{"op", "play.stop"}});
    CHECK(f.state()["mode"] == "edit");
}
