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
#include "eng/scene/Name.hpp"

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

namespace {

std::vector<eng::ecs::Entity> entitiesNamed(const eng::scene::Scene& scene,
                                            const std::string& name,
                                            bool includeTemplates = false)
{
    std::vector<eng::ecs::Entity> out;
    scene.world().each<eng::scene::Name>(
        [&](eng::ecs::Entity e, const eng::scene::Name& n) {
            if (n.value == name && (includeTemplates || !scene.isTemplated(e))) {
                out.push_back(e);
            }
        });
    return out;
}

std::string textOf(const eng::scene::Scene& scene, const std::string& name)
{
    const auto found = entitiesNamed(scene, name);
    if (found.empty()) {
        return "<sem entidade>";
    }
    auto v = eng::editor::Inspector::getField(scene, found.front(),
                                              "eng::editor::TextData", "text");
    return v.ok() ? v.value() : "<sem texto>";
}

double numberOf(const eng::scene::Scene& scene, eng::ecs::Entity e,
                const char* comp, const char* path)
{
    auto v = eng::editor::Inspector::getField(scene, e, comp, path);
    return v.ok() ? std::atof(v.value().c_str()) : 0.0;
}

}  // namespace

TEST_CASE("protocol: exemplo Voo é jogável do começo ao recomeço", "[protocol][flappy]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Voo"}, {"template", "flappy"}});
    const Json settings = f.ok({{"op", "settings.get"}});
    CHECK(settings["game"]["controls"] == "tap");
    CHECK(settings["game"]["orientation"] == "portrait");

    // Todos os scripts do exemplo compilam.
    for (const char* script : {"passaro.nis", "cano.nis", "gerador.nis"}) {
        const Json src = f.ok({{"op", "script.read"}, {"name", script}});
        const Json check = f.ok({{"op", "script.compile"}, {"source", src}});
        INFO(script << ": " << check.dump());
        REQUIRE(check["ok"].get<bool>());
    }

    // Tela de celular em retrato.
    f.doc->viewport().setScreenSize(720.f, 1600.f);
    f.doc->setGameViewportSize(720.f, 1600.f);
    f.doc->setScriptSeed(42);
    f.ok({{"op", "play.start"}});
    auto& scene = *f.doc->sceneInFocus();

    // O molde não roda nem aparece: nenhum cano ativo antes de começar.
    f.run(30);
    CHECK(entitiesNamed(scene, "Cano").empty());
    CHECK(entitiesNamed(scene, "Cano", true).size() == 1);
    auto bird = entitiesNamed(scene, "Pássaro").front();
    const double startY = numberOf(scene, bird, "eng::math::Transform", "position.y");
    CHECK(numberOf(scene, bird, "eng::math::Transform", "position.y") == startY);
    CHECK(textOf(scene, "Mensagem") == "TOQUE PARA VOAR");

    auto tap = [&] {
        f.key(eng::input::Key::Space, true);
        f.run(1);
        f.key(eng::input::Key::Space, false);
    };

    // Piloto automático: toca quando o pássaro fica abaixo do vão do próximo
    // cano (ou do centro, se não há cano à frente).
    tap();
    CHECK(textOf(scene, "Mensagem").empty());
    int frames = 0;
    std::size_t maxPipes = 0;
    for (; frames < 60 * 20; ++frames) {
        bird = entitiesNamed(scene, "Pássaro").front();
        const double by = numberOf(scene, bird, "eng::math::Transform", "position.y");
        const double vy = numberOf(scene, bird, "eng::physics::RigidBody", "velocity.y");
        double target = 0.3;
        double nearest = 1e9;
        const auto pipes = entitiesNamed(scene, "Cano");
        maxPipes = std::max(maxPipes, pipes.size());
        for (const auto pipe : pipes) {
            const double px = numberOf(scene, pipe, "eng::math::Transform", "position.x");
            if (px > -1.2 - 0.8 && px < nearest) {
                nearest = px;
                target = numberOf(scene, pipe, "eng::math::Transform", "position.y");
            }
        }
        if (by < target - 0.4 && vy < 0.0) {
            tap();
        } else {
            f.run(1);
        }
        if (textOf(scene, "Mensagem").rfind("FIM", 0) == 0) {
            break;
        }
    }
    const std::string score = textOf(scene, "Placar");
    INFO("placar=" << score << " quadros=" << frames << " canos=" << maxPipes);
    CHECK(maxPipes >= 2);                 // canos surgem pela direita
    CHECK(std::atoi(score.c_str()) >= 3); // passou por vários canos
    const Json hud = f.ok({{"op", "play.hud"}});
    INFO(hud.dump());
    CHECK(hud["scripts"]["faults"] == 0);

    // Canos andam para a esquerda e somem fora da tela (não acumulam).
    CHECK(entitiesNamed(scene, "Cano").size() <= 4);

    // Sem tocar, o pássaro cai e bate: fim de jogo.
    for (int i = 0; i < 60 * 5 && textOf(scene, "Mensagem").rfind("FIM", 0) != 0; ++i) {
        f.run(1);
    }
    REQUIRE(textOf(scene, "Mensagem").rfind("FIM", 0) == 0);
    // Parado: canos não andam mais.
    const auto still = entitiesNamed(scene, "Cano");
    if (!still.empty()) {
        const double x0 = numberOf(scene, still.front(), "eng::math::Transform", "position.x");
        f.run(30);
        CHECK(numberOf(scene, still.front(), "eng::math::Transform", "position.x") == x0);
    }

    // Toque recomeça a fase do zero.
    tap();
    f.run(2);
    auto& fresh = *f.doc->sceneInFocus();
    CHECK(f.doc->isPlaying());
    CHECK(textOf(fresh, "Placar") == "0");
    CHECK(textOf(fresh, "Mensagem") == "TOQUE PARA VOAR");
    CHECK(entitiesNamed(fresh, "Cano").empty());
    f.ok({{"op", "play.stop"}});
}

TEST_CASE("protocol: exemplo Chuva de caixas solta, empilha e limpa caixas",
          "[protocol][boxes]")
{
    Fixture f;
    f.ok({{"op", "project.new"}, {"name", "Caixas"}, {"template", "boxes"}});
    f.doc->viewport().setScreenSize(720.f, 1600.f);
    f.doc->setScriptSeed(7);
    f.ok({{"op", "play.start"}});
    auto& scene = *f.doc->sceneInFocus();
    f.run(60 * 4);
    const auto boxes = entitiesNamed(scene, "Caixa");
    INFO("caixas=" << boxes.size() << " texto=" << textOf(scene, "Contador"));
    CHECK(boxes.size() >= 4);
    CHECK(textOf(scene, "Contador").rfind("CAIXAS: ", 0) == 0);
    // Alguma caixa parou sobre o chão (topo do chão em y = -4.1).
    bool resting = false;
    for (const auto box : boxes) {
        const double y = numberOf(scene, box, "eng::math::Transform", "position.y");
        resting = resting || (y < -3.0 && y > -4.2);
    }
    CHECK(resting);
    const Json hud = f.ok({{"op", "play.hud"}});
    INFO(hud.dump());
    CHECK(hud["scripts"]["faults"] == 0);
    // Tempo de vida: depois de muitos segundos o número fica limitado.
    f.run(60 * 20);
    CHECK(entitiesNamed(scene, "Caixa").size() <= 18);
}
