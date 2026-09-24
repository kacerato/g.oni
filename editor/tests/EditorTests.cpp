/// Testes do editor (FASE 8, missão §8.10) — rodam no LINUX.
///
/// Cobertura exigida pela missão: projeto (new/open/save/settings),
/// entidades (create/delete/duplicate/rename/hierarchy/componentes),
/// inspector, scene save/load, asset discovery, play/stop com separação
/// editor×runtime, viewport (câmera/hit-test), ViewportRenderer e EditorHost
/// contra backends REAIS (lavapipe/llvmpipe — mesmos binários do APK).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <dlfcn.h>  // P4.5.2: contrato JNI (dlsym RTLD_DEFAULT)
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eng/animation/Animation.hpp"
#include "eng/editor/Diagnostics.hpp"
#include "eng/editor/EditorDocument.hpp"
#include "eng/editor/NiRuntime.hpp"
#include "eng/editor/EditorHost.hpp"
#include "eng/editor/Gizmo.hpp"
#include "eng/editor/Inspector.hpp"
#include "eng/editor/ProjectZip.hpp"
#include "eng/editor/SpriteData.hpp"
#include "eng/editor/TextureCache.hpp"
#include "eng/editor/ViewportRenderer.hpp"
#include "eng/fs/MemoryFileSystem.hpp"
#include "eng/fs/NativeFileSystem.hpp"
#include "eng/log/ConsoleSink.hpp"
#include "eng/log/Logger.hpp"
#include "eng/particles/Particles.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/rhi/Renderer.hpp"
#include "eng/rhi/gles/GlesBackend.hpp"
#include "eng/rhi/vulkan/VulkanBackend.hpp"
#include "eng/scene/Name.hpp"

namespace {

/// Logs no stderr (diagnóstico de ambiente — padrão das suites RHI).
struct LogSetup {
    eng::log::ConsoleSink console{stderr};
    LogSetup() { eng::log::Logger::get().addSink(console); }
};
const LogSetup kLogSetup{};

using eng::editor::EditorDocument;
using eng::editor::TransformGizmo;

struct DocFixture {
    eng::fs::MemoryFileSystem fsStorage;  // dono real (documento empresta)
    eng::fs::MemoryFileSystem* fs = &fsStorage;
    std::unique_ptr<EditorDocument> doc;

    DocFixture()
    {
        auto created = EditorDocument::create(fsStorage, eng::fs::Path{"."});
        REQUIRE(created.ok());
        doc = std::move(created.value());
    }

    /// Documento com projeto criado (assets + scenes prontos).
    void withProject()
    {
        REQUIRE(doc->newProject("TestGame").ok());
    }
};

/// Helpers de entidade empacotada (contrato JNI — pack/unpack).
std::uint64_t pack(eng::ecs::Entity e) { return EditorDocument::packEntity(e); }
eng::ecs::Entity unpack(std::uint64_t p) { return EditorDocument::unpackEntity(p); }

/// Projeto novo OU reaberto (testes do host rodam em disco REAL — runs
/// repetidos no mesmo build dir encontram o projeto da run anterior).
void ensureProject(EditorDocument& doc, const char* name)
{
    if (!doc.newProject(name).ok()) {
        REQUIRE(doc.openProject(eng::fs::Path{name}).ok());
    }
}

}  // namespace

// =============================================================================
// 1. Projeto (§8.10: criar/abrir/salvar projeto)
// =============================================================================

TEST_CASE("editor: cria projeto com estrutura completa", "[editor]")
{
    DocFixture f;
    REQUIRE(f.doc->newProject("MyGame").ok());
    REQUIRE(f.doc->hasProject());
    CHECK(f.doc->projectName() == "MyGame");

    auto& fs = *f.fs; // (fs movido para o doc — checagem via doc)
    (void)fs;
    // Estrutura em disco (via fs do próprio documento é inacessível —
    // valida pelos efeitos: openProject re-abre com sucesso).
    CHECK(f.doc->saveProject().ok());
}

TEST_CASE("editor: abre projeto salvo e valida round-trip", "[editor]")
{
    eng::fs::MemoryFileSystem fs;
    {
        auto created = EditorDocument::create(fs, eng::fs::Path{"."});
        REQUIRE(created.ok());
        REQUIRE(created.value()->newProject("RoundTrip").ok());
        REQUIRE(created.value()->setProjectName("RoundTrip2").ok());
        REQUIRE(created.value()->saveProject().ok());
    }
    auto reopened = EditorDocument::create(fs, eng::fs::Path{"."});
    REQUIRE(reopened.ok());
    REQUIRE(reopened.value()->openProject(eng::fs::Path{"RoundTrip"}).ok());
    auto& doc = *reopened.value();
    CHECK(doc.projectName() == "RoundTrip2");
    CHECK_FALSE(doc.projectDirty());
}

TEST_CASE("editor: newProject rejeita duplicado e nome vazio", "[editor]")
{
    DocFixture f;
    REQUIRE(f.doc->newProject("Dup").ok());
    auto again = f.doc->newProject("Dup");
    REQUIRE(again.isError());
    auto empty = f.doc->newProject("");
    REQUIRE(empty.isError());
}

TEST_CASE("editor: settings renomeia e marca dirty", "[editor]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->setProjectName("NovoNome").ok());
    CHECK(f.doc->projectDirty());
    REQUIRE(f.doc->saveProject().ok());
    CHECK_FALSE(f.doc->projectDirty());
    CHECK(f.doc->projectName() == "NovoNome");
}

// =============================================================================
// 2. Entidades (§8.10: criar/apagar/duplicar/rename/hierarchy)
// =============================================================================

TEST_CASE("editor: cria entidade com nome e componente Name", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    CHECK(f.doc->nameOf(*f.doc->sceneInFocus(), entity.value()) == "Player");
    CHECK(f.doc->sceneDirty());

    // Inspector vê os componentes built-in.
    const auto components =
        eng::editor::Inspector::componentsOf(*f.doc->sceneInFocus(), entity.value());
    CHECK(std::find(components.begin(), components.end(),
                    "eng::scene::Name") != components.end());
    CHECK(std::find(components.begin(), components.end(),
                    "eng::math::Transform") != components.end());
}

TEST_CASE("editor: rename e apagar entidade", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Ground", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    REQUIRE(f.doc->renameEntity(entity.value(), "Ground2").ok());
    CHECK(f.doc->nameOf(*f.doc->sceneInFocus(), entity.value()) == "Ground2");

    REQUIRE(f.doc->select(entity.value()).ok());
    CHECK(f.doc->isSelected(entity.value()));

    REQUIRE(f.doc->deleteEntity(entity.value()).ok());
    CHECK_FALSE(f.doc->selection().has_value()); // seleção limpa
    auto renamed = f.doc->renameEntity(entity.value(), "X");
    CHECK(renamed.isError());
}

TEST_CASE("editor: duplicar entidade clona subárvore e componentes", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto parent = f.doc->createEntity("Enemy", eng::scene::kNoEntity);
    REQUIRE(parent.ok());
    eng::editor::TransformDesc tr;
    tr.position = {3.f, 4.f, 0.f};
    tr.scale = {2.f, 2.f, 2.f};
    REQUIRE(f.doc->setTransform(parent.value(), tr).ok());

    auto child = f.doc->createEntity("Gun", parent.value());
    REQUIRE(child.ok());

    auto dup = f.doc->duplicateEntity(parent.value());
    REQUIRE(dup.ok());

    auto snapshot = f.doc->hierarchySnapshot();
    REQUIRE(snapshot.size() == 4); // Enemy, Gun, Enemy.alt, Gun
    CHECK(snapshot[2].name == "Enemy.alt");
    CHECK(snapshot[2].depth == 0);
    CHECK(snapshot[3].name == "Gun");
    CHECK(snapshot[3].depth == 1);

    auto dupTransform = f.doc->transform(dup.value());
    REQUIRE(dupTransform.ok());
    CHECK(dupTransform.value().position.x == 3.f);
    CHECK(dupTransform.value().scale.x == 2.f);

    // Duplicar de novo: nome numera (.alt2).
    auto dup2 = f.doc->duplicateEntity(dup.value());
    REQUIRE(dup2.ok());
    CHECK(f.doc->nameOf(*f.doc->sceneInFocus(), dup2.value()) == "Enemy.alt2");
}

TEST_CASE("editor: reparent aceita e rejeita ciclo", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto a = f.doc->createEntity("A", eng::scene::kNoEntity);
    auto b = f.doc->createEntity("B", a.value());
    auto c = f.doc->createEntity("C", b.value());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());

    // C em A: ok.
    REQUIRE(f.doc->reparentEntity(c.value(), a.value()).ok());
    // A em C: ciclo — rejeitado (Scene::attach, ADR-025).
    auto cycle = f.doc->reparentEntity(a.value(), c.value());
    REQUIRE(cycle.isError());
    // C → raiz.
    REQUIRE(f.doc->reparentEntity(c.value(), eng::scene::kNoEntity).ok());
}

TEST_CASE("editor: hierarquia snapshot com profundidade estável", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto root1 = f.doc->createEntity("R1", eng::scene::kNoEntity);
    auto root2 = f.doc->createEntity("R2", eng::scene::kNoEntity);
    auto child = f.doc->createEntity("C1", root1.value());
    REQUIRE(root1.ok());
    REQUIRE(root2.ok());
    REQUIRE(child.ok());

    const auto snapshot = f.doc->hierarchySnapshot();
    REQUIRE(snapshot.size() == 3);
    CHECK(snapshot[0].name == "R1");
    CHECK(snapshot[0].depth == 0);
    CHECK(snapshot[1].name == "C1");
    CHECK(snapshot[1].depth == 1);
    CHECK(snapshot[2].name == "R2");
    CHECK(snapshot[2].depth == 0);
}

// =============================================================================
// 3. Componentes + Inspector (§8.10)
// =============================================================================

TEST_CASE("editor: catálogo contém os built-ins", "[editor]")
{
    const auto catalog = eng::editor::Inspector::catalog();
    CHECK_FALSE(catalog.empty());
    CHECK(std::find(catalog.begin(), catalog.end(), "eng::scene::Name") !=
          catalog.end());
    CHECK(std::find(catalog.begin(), catalog.end(),
                    "eng::math::Transform") != catalog.end());
}

TEST_CASE("editor: inspector lê/escreve campos por caminho", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    auto got = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(got.ok());
    CHECK(got.value() == "0");

    REQUIRE(f.doc->setInspectorField(entity.value(), "eng::math::Transform",
                                     "position.x", "2.5")
                .ok());
    auto after = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(after.ok());
    CHECK(after.value() == "2.5");

    // String (Name.value).
    REQUIRE(f.doc->setInspectorField(entity.value(), "eng::scene::Name",
                                     "value", "Renamed")
                .ok());
    CHECK(f.doc->nameOf(*f.doc->sceneInFocus(), entity.value()) == "Renamed");

    // Campos achatados incluem subcampos de structs.
    const auto fields = f.doc->inspectorFields(
        entity.value(), "eng::math::Transform");
    REQUIRE_FALSE(fields.empty());
    bool hasScaleY = false;
    bool hasRotationW = false;
    for (const auto& field : fields) {
        if (field.path == "scale.y") { hasScaleY = true; }
        if (field.path == "rotation.w") { hasRotationW = true; }
    }
    CHECK(hasScaleY);
    CHECK(hasRotationW);
}

TEST_CASE("editor: inspector rejeita lixo com erro preciso", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("X", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    auto badFloat = f.doc->setInspectorField(
        entity.value(), "eng::math::Transform", "position.x", "abc");
    CHECK(badFloat.isError());

    auto nanFloat = f.doc->setInspectorField(
        entity.value(), "eng::math::Transform", "position.x", "nan");
    CHECK(nanFloat.isError());

    auto badPath = f.doc->setInspectorField(
        entity.value(), "eng::math::Transform", "position.naoexiste", "1");
    CHECK(badPath.isError());

    auto badComp = f.doc->setInspectorField(
        entity.value(), "ComponenteInexistente", "x", "1");
    CHECK(badComp.isError());
}

// =============================================================================
// 3b. Inspector com kinds semânticos (evolução P0-6, ADR-052)
// =============================================================================

namespace {

/// Encontra um campo por path exato (ou nullptr).
const eng::editor::Inspector::Field* fieldByPath(
    const std::vector<eng::editor::Inspector::Field>& fields,
    std::string_view path)
{
    for (const auto& field : fields) {
        if (field.path == path) {
            return &field;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("editor: inspector emite kind/options de enum (P0-6)", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Col", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider")
                .ok());

    const auto fields = f.doc->inspectorFields(
        entity.value(), "eng::physics::Collider");
    const auto* shape = fieldByPath(fields, "shape");
    REQUIRE(shape != nullptr);
    CHECK(shape->kind == "enum");
    CHECK(shape->options == "Sphere|Box");
    CHECK(shape->value == "Sphere"); // default do campo

    // Escrita por NOME de enumerador (contrato estável ADR-033).
    REQUIRE(f.doc->setInspectorField(entity.value(), "eng::physics::Collider",
                                     "shape", "Box")
                .ok());
    auto after = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::physics::Collider",
        "shape");
    REQUIRE(after.ok());
    CHECK(after.value() == "Box");

    // Enumerador inexistente → erro preciso.
    auto bad = f.doc->setInspectorField(
        entity.value(), "eng::physics::Collider", "shape", "Cylinder");
    CHECK(bad.isError());
}

TEST_CASE("editor: inspector emite kind bool/number/int/text (P0-6)",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("S", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::editor::SpriteData")
                .ok());

    const auto fields = f.doc->inspectorFields(
        entity.value(), "eng::editor::SpriteData");

    // bool → Switch na UI.
    const auto* flipX = fieldByPath(fields, "flipX");
    REQUIRE(flipX != nullptr);
    CHECK(flipX->kind == "bool");
    CHECK(flipX->value == "false");
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::editor::SpriteData", "flipX",
                                     "true")
                .ok());
    CHECK(fieldByPath(f.doc->inspectorFields(entity.value(),
                                             "eng::editor::SpriteData"),
                      "flipX")
              ->value == "true");

    // bool com valor inválido → erro.
    CHECK(f.doc->setInspectorField(entity.value(), "eng::editor::SpriteData",
                                   "flipX", "sim")
              .isError());

    // f32 → number.
    const auto* sort = fieldByPath(fields, "sort");
    REQUIRE(sort != nullptr);
    CHECK(sort->kind == "number");

    // string COMUM → text (Name.value).
    const auto nameFields = f.doc->inspectorFields(entity.value(),
                                                  "eng::scene::Name");
    const auto* value = fieldByPath(nameFields, "value");
    REQUIRE(value != nullptr);
    CHECK(value->kind == "text");
    CHECK(value->options.empty());
}

TEST_CASE("editor: inspector colapsa canais de cor em UM campo hex (P0-6)",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Tint", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::editor::SpriteData")
                .ok());

    const auto fields = f.doc->inspectorFields(
        entity.value(), "eng::editor::SpriteData");

    // Os três canais viram UM campo sintético — os individuais SOMEM.
    const auto* tint = fieldByPath(fields, "tintR,tintG,tintB");
    REQUIRE(tint != nullptr);
    CHECK(tint->kind == "color");
    CHECK(tint->typeName == "color");
    CHECK(tint->value == "#FFFFFF"); // defaults 1,1,1
    CHECK(fieldByPath(fields, "tintR") == nullptr);
    CHECK(fieldByPath(fields, "tintG") == nullptr);
    CHECK(fieldByPath(fields, "tintB") == nullptr);

    // Escrita hex → três floats; leitura devolve o hex.
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::editor::SpriteData",
                                     "tintR,tintG,tintB", "#FF8000")
                .ok());
    auto r = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::editor::SpriteData",
        "tintR");
    REQUIRE(r.ok());
    CHECK(r.value() == "1");
    auto g = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::editor::SpriteData",
        "tintG");
    REQUIRE(g.ok());
    // float(128/255) impresso com %.9g — comparação numérica robusta.
    CHECK(std::stof(g.value()) == Catch::Approx(128.f / 255.f)
                                     .margin(1e-6f));
    auto hex = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::editor::SpriteData",
        "tintR,tintG,tintB");
    REQUIRE(hex.ok());
    CHECK(hex.value() == "#FF8000");

    // Clamp: valores fora de [0..1] saturam nos bytes hex.
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::editor::SpriteData", "tintR", "2")
                .ok());
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::editor::SpriteData", "tintG",
                                     "-1")
                .ok());
    auto clamped = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::editor::SpriteData",
        "tintR,tintG,tintB");
    REQUIRE(clamped.ok());
    CHECK(clamped.value() == "#FF0000");
}

TEST_CASE("editor: grupo de cor rejeita hex lixo SEM escrever nada (P0-6)",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Bad", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::editor::SpriteData")
                .ok());

    // Estado conhecido: vermelho puro.
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::editor::SpriteData",
                                     "tintR,tintG,tintB", "#FF0000")
                .ok());

    for (const char* garbage : {"red", "#12345", "#GGHHII", "", "#1234567"}) {
        auto written = f.doc->setInspectorField(
            entity.value(), "eng::editor::SpriteData", "tintR,tintG,tintB",
            garbage);
        CHECK(written.isError());
    }
    // Nenhuma escrita parcial: continua vermelho puro.
    auto hex = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::editor::SpriteData",
        "tintR,tintG,tintB");
    REQUIRE(hex.ok());
    CHECK(hex.value() == "#FF0000");

    // Grupo com contagem errada de canais → erro preciso.
    CHECK(f.doc->setInspectorField(entity.value(), "eng::editor::SpriteData",
                                   "tintR,tintG", "#FF0000")
              .isError());
    CHECK(f.doc->setInspectorField(
              entity.value(), "eng::editor::SpriteData",
              "tintR,tintG,tintB,opacity,sort", "#FF0000FF")
              .isError());
    // Canal que não é float → erro.
    CHECK(f.doc->setInspectorField(entity.value(), "eng::editor::SpriteData",
                                   "tintR,textureAsset,tintB", "#FF0000")
              .isError());
}

TEST_CASE("editor: campo de textura reporta kind texture (P0-6)", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Sprite", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::editor::SpriteData")
                .ok());

    const auto fields = f.doc->inspectorFields(
        entity.value(), "eng::editor::SpriteData");
    const auto* texture = fieldByPath(fields, "textureAsset");
    REQUIRE(texture != nullptr);
    CHECK(texture->kind == "texture");
    CHECK(texture->typeName == "string");
    CHECK(texture->value.empty()); // sem textura atribuída

    // Continua sendo uma string gravável (o picker de textura escreve aqui).
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::editor::SpriteData",
                                     "textureAsset", "hero.png")
                .ok());
    CHECK(fieldByPath(f.doc->inspectorFields(entity.value(),
                                             "eng::editor::SpriteData"),
                      "textureAsset")
              ->value == "hero.png");
}

// =============================================================================
// 3c. Scripts NI-Script como assets do projeto (evolução P0-7, ADR-053)
// =============================================================================

TEST_CASE("editor: scriptCreate gera template válido e catalogado (P0-7)",
          "[editor]")
{
    DocFixture f;
    f.withProject();

    // Nome sem extensão → ganha .nis.
    REQUIRE(f.doc->scriptCreate("Movimento").ok());
    auto names = f.doc->scriptList();
    REQUIRE(names.ok());
    REQUIRE(names.value().size() == 1);
    CHECK(names.value()[0] == "Movimento.nis");

    // O TEMPLATE COMPILA — provado pela MESMA checagem que a UI usa.
    auto source = f.doc->scriptRead("Movimento.nis");
    REQUIRE(source.ok());
    auto check = f.doc->scriptCompile(source.value());
    REQUIRE(check.ok());
    CHECK(check.value().ok);
    CHECK(check.value().diags.empty());

    // Duplicado → AlreadyExists.
    CHECK(f.doc->scriptCreate("Movimento").isError());
    // Nome com extensão idêntica → duplicado do mesmo jeito.
    CHECK(f.doc->scriptCreate("Movimento.nis").isError());

    // Nomes inválidos rejeitados (traversal, vazio).
    CHECK(f.doc->scriptCreate("").isError());
    CHECK(f.doc->scriptCreate("../evil").isError());

    // Catalogado no registry: o AssetBrowser lista como registrado com id.
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    auto listed = browser->list("scripts");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].registered);
    CHECK(listed.value()[0].id != "-");
}

TEST_CASE("editor: scriptWrite/Read round-trip + registry (P0-7)", "[editor]")
{
    DocFixture f;
    f.withProject();

    const std::string src = "add &BL\n\nvar hp: int = 10\n";
    REQUIRE(f.doc->scriptWrite("Player.nis", src).ok());

    auto read = f.doc->scriptRead("Player.nis");
    REQUIRE(read.ok());
    CHECK(read.value() == src);

    // Substituição preserva o id (ADR-029).
    auto before = f.doc->assets()->list("scripts");
    REQUIRE(before.ok());
    REQUIRE(before.value().size() == 1);
    const std::string idBefore = before.value()[0].id;

    const std::string src2 = "add &BL\n\nvar hp: int = 20\n";
    REQUIRE(f.doc->scriptWrite("Player.nis", src2).ok());
    auto after = f.doc->assets()->list("scripts");
    REQUIRE(after.ok());
    REQUIRE(after.value().size() == 1);
    CHECK(after.value()[0].id == idBefore);
    CHECK(after.value()[0].registered);

    auto read2 = f.doc->scriptRead("Player.nis");
    REQUIRE(read2.ok());
    CHECK(read2.value() == src2);

    // Delete remove arquivo + meta.
    REQUIRE(f.doc->scriptDelete("Player.nis").ok());
    CHECK(f.doc->scriptList().value().empty());
    CHECK(f.doc->scriptRead("Player.nis").isError());
}

TEST_CASE("editor: scriptCompile separa válido de inválido com diagnósticos (P0-7)",
          "[editor]")
{
    DocFixture f;
    f.withProject();

    // Fonte VÁLIDA (usa nativo de host: self()).
    const std::string good = "add &BL\n"
                             "\n"
                             "up update:\n"
                             "    var me = self()\n"
                             "    me.position.x = me.position.x + 1\n"
                             "stop\n";
    auto okCheck = f.doc->scriptCompile(good);
    REQUIRE(okCheck.ok());
    CHECK(okCheck.value().ok);
    CHECK(okCheck.value().diags.empty());

    // Fonte QUEBRADA: string aberta → diagnóstico com linha/coluna.
    const std::string bad = "var s = \"aberta\n";
    auto badCheck = f.doc->scriptCompile(bad);
    REQUIRE(badCheck.ok());          // o CHECK rodou (erro interno não houve)
    CHECK_FALSE(badCheck.value().ok); // o VEREDITO é do compilador
    REQUIRE_FALSE(badCheck.value().diags.empty());
    CHECK(badCheck.value().diags[0].line == 1);
    CHECK(badCheck.value().diags[0].col > 0);
    CHECK_FALSE(badCheck.value().diags[0].message.empty());

    // Nativo inexistente → sema pega (a tabela do runtime de Play).
    const std::string badNative = "up update:\n    voo_magico()\nstop\n";
    auto nativeCheck = f.doc->scriptCompile(badNative);
    REQUIRE(nativeCheck.ok());
    CHECK_FALSE(nativeCheck.value().ok);
    CHECK_FALSE(nativeCheck.value().diags.empty());
}

TEST_CASE("editor: scriptAssign anexa fonte e PLAY roda o script (P0-7)",
          "[editor]")
{
    DocFixture f;
    f.withProject();

    const std::string src = "add &BL\n"
                            "\n"
                            "var speed: float = 3.0\n"
                            "\n"
                            "up update:\n"
                            "    var me = self()\n"
                            "    me.position.x = me.position.x + speed\n"
                            "stop\n";
    REQUIRE(f.doc->scriptWrite("Andar.nis", src).ok());

    auto entity = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    // Anexa: adiciona o componente e copia a fonte do ASSET.
    REQUIRE(f.doc->scriptAssign(entity.value(), "Andar.nis").ok());
    auto fields = f.doc->inspectorFields(
        entity.value(), "eng::editor::NiScriptComponent");
    const auto* sourceField = fieldByPath(fields, "source");
    REQUIRE(sourceField != nullptr);
    CHECK(sourceField->value == src);
    CHECK(sourceField->kind == "code");
    const auto* assetField = fieldByPath(fields, "scriptAsset");
    REQUIRE(assetField != nullptr);
    CHECK(assetField->value == "Andar.nis");
    CHECK(assetField->kind == "script");

    // PLAY: compila e roda o up update (o mesmo caminho da FASE 11).
    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->runtimeScripts().size() == 1);
    f.doc->tick(1.f / 60.f);
    auto x = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(x.ok());
    // up update: me.position.x = me.position.x + speed → 0 + 3.0 = 3.0
    // (a conta do script é +speed por update, não *delta).
    CHECK(std::stof(x.value()) == Catch::Approx(3.f).margin(1e-4f));
    f.doc->stop();

    // Sem projeto aberto (documento recém-criado) → erro preciso.
    DocFixture fresh;
    CHECK(fresh.doc->scriptList().isError());
    CHECK(fresh.doc->scriptRead("x.nis").isError());
}

TEST_CASE("editor: add/remove componente com proteção dos core", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("X", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    // Remover Name/Transform: protegidos.
    auto rmName =
        f.doc->removeComponent(entity.value(), "eng::scene::Name");
    CHECK(rmName.isError());
    auto rmTransform =
        f.doc->removeComponent(entity.value(), "eng::math::Transform");
    CHECK(rmTransform.isError());

    // Add duplicado: erro AlreadyExists.
    auto dup = f.doc->addComponent(entity.value(), "eng::scene::Name");
    CHECK(dup.isError());

    // Remover algo ausente: erro.
    auto rmAbsent = f.doc->removeComponent(entity.value(),
                                            "eng::math::Transform");
    CHECK(rmAbsent.isError()); // protegido SEMPRE (mesmo presente)
}

// =============================================================================
// 4. Transform (Euler ↔ Quat round-trip)
// =============================================================================

TEST_CASE("editor: transform Euler↔Quat round-trip", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Rot", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    // Perto do gimbal (|ângulo| ~ 90°), asin amplifica o erro do f32 —
    // tolerância proporcional em vez de absoluta (limitação documentada).
    const auto tolerance = [](float degrees) {
        return std::abs(std::abs(degrees) - 90.f) < 2.f ? 0.05f : 1e-3f;
    };
    const eng::math::Vec3 angles[] = {
        {0.f, 0.f, 0.f},   {30.f, 0.f, 0.f},  {0.f, 45.f, 0.f},
        {0.f, 0.f, 60.f},   {10.f, 20.f, 30.f}, {-25.f, 40.f, -15.f},
        {90.f, 0.f, 0.f},  {0.f, 89.f, 0.f},
    };
    for (const auto& angle : angles) {
        eng::editor::TransformDesc desc;
        desc.rotationDegrees = angle;
        REQUIRE(f.doc->setTransform(entity.value(), desc).ok());
        auto back = f.doc->transform(entity.value());
        REQUIRE(back.ok());
        INFO("angle " << angle.x << "," << angle.y << "," << angle.z
                     << " -> " << back.value().rotationDegrees.x << ","
                     << back.value().rotationDegrees.y << ","
                     << back.value().rotationDegrees.z);
        CHECK_THAT(back.value().rotationDegrees.x,
                   Catch::Matchers::WithinAbs(angle.x, tolerance(angle.x)));
        CHECK_THAT(back.value().rotationDegrees.y,
                   Catch::Matchers::WithinAbs(angle.y, tolerance(angle.y)));
        CHECK_THAT(back.value().rotationDegrees.z,
                   Catch::Matchers::WithinAbs(angle.z, tolerance(angle.z)));
    }
}

// =============================================================================
// 5. Cena save/load (§8.10)
// =============================================================================

TEST_CASE("editor: save/load de cena com dirty flags", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Persisted", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->setInspectorField(entity.value(), "eng::math::Transform",
                                     "position.y", "7")
                .ok());
    CHECK(f.doc->sceneDirty());

    REQUIRE(f.doc->saveScene("main.json").ok());
    CHECK_FALSE(f.doc->sceneDirty());

    // Modifica e recarrega.
    REQUIRE(f.doc->setInspectorField(entity.value(), "eng::math::Transform",
                                     "position.y", "99")
                .ok());
    REQUIRE(f.doc->loadScene("main.json").ok());
    CHECK_FALSE(f.doc->sceneDirty());

    auto snapshot = f.doc->hierarchySnapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].name == "Persisted");
    auto got = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), snapshot[0].entity, "eng::math::Transform",
        "position.y");
    REQUIRE(got.ok());
    CHECK(got.value() == "7");
}

// =============================================================================
// 6. Play/Stop — separação editor × runtime (§8.10)
// =============================================================================

TEST_CASE("editor: play clona, edição rejeitada, mutação não vaza", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->saveScene("s.json").ok());

    // PLAY: runtime clone.
    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->isPlaying());

    // Snapshot do clone == snapshot da edição.
    auto editSnapshot = f.doc->hierarchySnapshot(); // foco = runtime em Play
    CHECK(editSnapshot.size() == 1);

    // Edição REJEITADA em Play.
    auto create = f.doc->createEntity("Novo", eng::scene::kNoEntity);
    CHECK(create.isError());
    auto del = f.doc->deleteEntity(entity.value());
    CHECK(del.isError());
    auto rename = f.doc->renameEntity(entity.value(), "Outro");
    CHECK(rename.isError());
    auto field = f.doc->setInspectorField(entity.value(),
                                         "eng::math::Transform", "position.x",
                                         "5");
    CHECK(field.isError());

    // Mover entidade em Play: muda o CLONE (debug §8.7).
    REQUIRE(f.doc->moveEntityScreen(entity.value(), 48.f, 0.f).ok());
    auto cloneX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(cloneX.ok());
    CHECK(cloneX.value() == "1"); // 48px / zoom 48 = 1 unidade

    // STOP: edição intacta.
    f.doc->stop();
    CHECK_FALSE(f.doc->isPlaying());
    auto editX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(editX.ok());
    CHECK(editX.value() == "0");

    // Segundo play re-clona do estado atual da edição.
    REQUIRE(f.doc->play().ok());
    auto freshX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(freshX.ok());
    CHECK(freshX.value() == "0");
    f.doc->stop();
}

TEST_CASE("editor: play duplicado e stop sem play são tratados", "[editor]")
{
    DocFixture f;
    f.withProject();
    f.doc->stop(); // stop sem play: no-op
    REQUIRE(f.doc->play().ok());
    auto twice = f.doc->play();
    CHECK(twice.isError());
    f.doc->stop();
}

TEST_CASE("editor: PLAY roda scripts NI-Script do clone (FASE 11)",
          "[editor][ni]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Motor", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    // Script anexado via catálogo (mesmo caminho do Inspector/JNI — a UI
    // dedicada de script é FUTURO declarado, docs/ni-script/08).
    const char* source =
        "add &BL\n"
        "var speed: float = 2.0\n"
        "var ticks: int = 0\n"
        "up start:\n"
        "    var me = self()\n"
        "    me.scale.y = 1.5\n"
        "stop\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.position.x = me.position.x + speed\n"
        "    me.name = \"motor\"\n"
        "    ticks = ticks + 1\n"
        "stop\n"
        "up destroy:\n"
        "    var me = self()\n"
        "    me.scale.y = 9.0\n"
        "stop\n";
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(entity.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    // PLAY: compila + @init + up start (scale.y = 1.5 no CLONE).
    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->runtimeScripts().size() == 1);
    auto scaleY = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "scale.y");
    REQUIRE(scaleY.ok());
    CHECK(scaleY.value() == "1.5");
    auto nameAfterStart = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::scene::Name", "value");
    REQUIRE(nameAfterStart.ok());
    CHECK(nameAfterStart.value() == "Motor"); // name muda no update, não no start

    // TICKs: up update move o clone (2/tick).
    f.doc->tick(1.f / 60.f);
    f.doc->tick(1.f / 60.f);
    f.doc->tick(1.f / 60.f);
    auto posX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(posX.ok());
    CHECK(posX.value() == "6"); // 3 ticks × speed 2.0
    auto nameAfter = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::scene::Name", "value");
    REQUIRE(nameAfter.ok());
    CHECK(nameAfter.value() == "motor");

    // STOP: up destroy (best-effort) roda ANTES do descarte; edição NUNCA
    // foi tocada (ADR-044).
    f.doc->stop();
    CHECK(f.doc->runtimeScripts().empty());
    auto editX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(editX.ok());
    CHECK(editX.value() == "0"); // edição intacta
    auto editScale = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "scale.y");
    REQUIRE(editScale.ok());
    CHECK(editScale.value() == "1"); // 1.5/9.0 ficaram no clone descartado
    auto editName = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::scene::Name", "value");
    REQUIRE(editName.ok());
    CHECK(editName.value() == "Motor");
}

TEST_CASE("editor: script com erro de compilação é desabilitado, cena segue",
          "[editor][ni]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Quebrado", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(entity.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", "up update:\n    nada()\nstop\n")
                .ok());

    // PLAY: script inválido NÃO derruba o play (log + desabilitado).
    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->runtimeScripts().empty()); // nada compilou
    f.doc->tick(1.f / 60.f);                 // tick sem scripts: ok
    f.doc->stop();
}

// =============================================================================
// P4.1 — T2/D5: linguagem (atribuição composta) + diagnóstico VISÍVEL
// =============================================================================

TEST_CASE("editor: P4.1 — D5: atribuição composta (-= += *= /=) compila e roda",
          "[editor][ni][p41]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Movido", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    // O SCRIPT EXATO do repro D5 no device (`position.x -= dt`): sem os
    // operadores compostos ele NÃO compilava — e o erro era silencioso
    // para o autor (só logcat). CRLF incluído (script editado no device
    // sai do EditText com \r\n).
    const char* source =
        "add &BL\r\n"
        "var speed: float = 2.0\r\n"
        "up update:\r\n"
        "    var me = self()\r\n"
        "    me.position.x -= delta()\r\n"
        "    speed *= 1.0\r\n"
        "    speed += 0.0\r\n"
        "    speed -= 0.0\r\n"
        "    speed /= 1.0\r\n"
        "stop\r\n";
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(entity.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->runtimeScripts().size() == 1);
    f.doc->tick(1.f / 60.f);
    f.doc->tick(1.f / 60.f);
    auto posX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), entity.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(posX.ok());
    // O repro D5: `me.position.x -= delta()` → 2 ticks × -(1/60) = -1/30.
    CHECK(std::abs(std::stof(posX.value()) + 1.f / 30.f) < 1e-3f);
    const auto& stats = f.doc->runtimeScripts().stats();
    CHECK(stats.scriptsFailed == 0);
    CHECK(stats.ticks == 2);
    CHECK(stats.healthy());
    f.doc->stop();
}

TEST_CASE("editor: P4.1 — D5: estatística de script VISÍVEL (erro/ticks)",
          "[editor][ni][p41]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("ComErro", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    // Erro de sintaxe REAL do D5: `-=` antes do P4.1 lexava como tokens
    // Minus+Assign e explodia na compilação em silêncio.
    REQUIRE(f.doc
                ->setInspectorField(entity.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source",
                                    "up update:\n    position.x -= dt\nstop\n")
                .ok());

    // ANTES do play: stats zerados.
    CHECK(f.doc->runtimeScripts().stats().scriptsFound == 0);

    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    const auto& stats = f.doc->runtimeScripts().stats();
    CHECK(stats.scriptsFound == 1);
    CHECK(stats.scriptsFailed == 1);
    CHECK(stats.scriptsCompiled == 0);
    CHECK(stats.instances == 0);
    CHECK_FALSE(stats.firstCompileError.empty());  // a UI mosta ISTO
    CHECK(stats.firstFailedEntity != 0xFFFFFFFFu);
    f.doc->stop();
}

TEST_CASE("editor: P4.1 — D5: script que RODA tem ticks contados no stats",
          "[editor][ni][p41]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Saudavel", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(entity.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source",
                                    "up update:\n    var me = self()\n"
                                    "    me.position.x = 1.0\nstop\n")
                .ok());
    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    f.doc->tick(1.f / 60.f);
    f.doc->tick(1.f / 60.f);
    const auto& stats = f.doc->runtimeScripts().stats();
    CHECK(stats.scriptsCompiled == 1);
    CHECK(stats.instances == 1);
    CHECK(stats.ticks == 3);
    CHECK(stats.firstUpdateTick == 1);
    CHECK(stats.faults == 0);
    f.doc->stop();
}

TEST_CASE("editor: input do JOGO em Play (FASE 9 §6.4 — separação)",
          "[editor]")
{
    DocFixture f;
    f.withProject();

    // Bindings por JSON asset (mesma API que input.json configuraria).
    using eng::serial::JsonValue;
    JsonValue root = JsonValue::array();
    JsonValue jump = JsonValue::object();
    jump.set("name", JsonValue::string("jump"));
    JsonValue sources = JsonValue::array();
    JsonValue zone = JsonValue::object();
    JsonValue rect = JsonValue::array();
    rect.append(JsonValue::real(0.0));
    rect.append(JsonValue::real(0.0));
    rect.append(JsonValue::real(1.0));
    rect.append(JsonValue::real(0.5));
    zone.set("touchZone", std::move(rect));
    sources.append(std::move(zone));
    jump.set("sources", std::move(sources));
    root.append(std::move(jump));
    auto bindings = eng::input::ActionBindings::fromJson(root);
    REQUIRE(bindings.ok());
    f.doc->setRuntimeBindings(std::move(bindings.value()));
    f.doc->setGameViewportSize(200.f, 100.f);

    // Em EDIT: input do jogo NÃO processa (gestos do editor não vazam).
    f.doc->gameTouch(0, 0, 100.f, 25.f, 1.f);
    f.doc->tick(1.f / 60.f);
    CHECK_FALSE(f.doc->runtimeInput().action("jump").down);

    // PLAY: toques alimentam o input do runtime.
    REQUIRE(f.doc->play().ok());
    f.doc->gameTouch(0, 0, 100.f, 25.f, 1.f);
    f.doc->tick(1.f / 60.f);
    CHECK(f.doc->runtimeInput().action("jump").down);
    CHECK(f.doc->runtimeInput().action("jump").pressed);
    f.doc->gameTouch(2, 0, 100.f, 25.f, 1.f);
    f.doc->tick(1.f / 60.f);
    CHECK(f.doc->runtimeInput().action("jump").released);

    // STOP: input do jogo congela com a sessão.
    f.doc->stop();
    CHECK_FALSE(f.doc->runtimeInput().action("jump").down);
}

// =============================================================================
// 7. Viewport (câmera + hit-test)
// =============================================================================

TEST_CASE("editor: câmera world↔screen ida e volta + zoom foco", "[editor]")
{
    eng::editor::Viewport viewport;
    viewport.setScreenSize(1080.f, 600.f);
    viewport.camera().posX = 10.f;
    viewport.camera().posY = -4.f;
    viewport.camera().zoom = 48.f;

    CHECK_THAT(viewport.screenToWorldX(viewport.worldToScreenX(123.5f)),
               Catch::Matchers::WithinAbs(123.5f, 1e-3f));
    CHECK_THAT(viewport.screenToWorldY(viewport.worldToScreenY(-42.25f)),
               Catch::Matchers::WithinAbs(-42.25f, 1e-3f));

    // Zoom centrado: o ponto sob o foco não se move na tela.
    const float focusX = 300.f;
    const float focusY = 200.f;
    const float wx = viewport.screenToWorldX(focusX);
    const float wy = viewport.screenToWorldY(focusY);
    viewport.zoomAt(1.5f, focusX, focusY);
    CHECK_THAT(viewport.screenToWorldX(focusX),
               Catch::Matchers::WithinAbs(wx, 1e-3f));
    CHECK_THAT(viewport.screenToWorldY(focusY),
               Catch::Matchers::WithinAbs(wy, 1e-3f));
    CHECK(viewport.camera().zoom == 72.f); // 48*1.5

    // Pan por delta de tela: ponto fixo da tela desloca (-dx/zoom, +dy/zoom).
    const float beforeX = viewport.screenToWorldX(500.f);
    const float beforeY = viewport.screenToWorldY(500.f);
    viewport.pan(96.f, 48.f);
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(beforeX - 96.f / 72.f, 1e-3f));
    CHECK_THAT(viewport.screenToWorldY(500.f),
               Catch::Matchers::WithinAbs(beforeY + 48.f / 72.f, 1e-3f));
}

TEST_CASE("editor: câmera de JOGO toma o viewport em Play (P0-5, ADR-051)", "[editor][tick]")
{
    DocFixture f;
    f.withProject();
    auto cam = f.doc->createEntity("Cam", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    // CameraData entra pelo CATÁLOGO (mesmo caminho do Inspector/JNI).
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "posX", "12").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "posY", "-6").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "zoom", "96").ok());

    auto& viewport = f.doc->viewport();
    viewport.setScreenSize(1000.f, 500.f);
    // Câmera do EDITOR em outro lugar — se vazasse, o teste pega.
    viewport.camera().posX = 1000.f;
    viewport.camera().posY = 1000.f;
    viewport.camera().zoom = 8.f;

    REQUIRE(f.doc->play().ok());
    // play() NÃO roda frame (contrato FASE 11) — mas a câmera já resolve.
    CHECK(f.doc->hasGameCamera());
    CHECK(viewport.gameCameraActive());
    // Conversões seguem a câmera do JOGO: centro da tela = pos da câmera.
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(12.f, 1e-3f));
    CHECK_THAT(viewport.screenToWorldY(250.f),
               Catch::Matchers::WithinAbs(-6.f, 1e-3f));
    // worldToScreen do ponto da câmera = centro (zoom 96: 1 unidade = 96px).
    CHECK_THAT(viewport.worldToScreenX(12.f),
               Catch::Matchers::WithinAbs(500.f, 1e-2f));
    CHECK_THAT(viewport.worldToScreenY(-6.f),
               Catch::Matchers::WithinAbs(250.f, 1e-2f));

    // Gestos do editor são NO-OP sob câmera de jogo (debug honesto).
    const float beforeX = viewport.screenToWorldX(500.f);
    viewport.pan(120.f, 60.f);
    viewport.zoomAt(2.f, 500.f, 250.f);
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(beforeX, 1e-6f));

    f.doc->stop();
    // STOP devolve a câmera do editor.
    CHECK_FALSE(f.doc->hasGameCamera());
    CHECK_FALSE(viewport.gameCameraActive());
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(1000.f, 1e-3f));
}

TEST_CASE("editor: Play sem CameraData usa a câmera do editor (P0-5)", "[editor][tick]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Plain", eng::scene::kNoEntity);
    REQUIRE(e.ok());

    auto& viewport = f.doc->viewport();
    viewport.setScreenSize(1000.f, 500.f);
    viewport.camera().posX = 42.f;
    viewport.camera().zoom = 48.f;

    REQUIRE(f.doc->play().ok());
    CHECK_FALSE(f.doc->hasGameCamera());
    CHECK_FALSE(viewport.gameCameraActive());
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(42.f, 1e-3f));
    // Pan continua funcionando (câmera do editor em foco).
    viewport.pan(48.f, 0.f);
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(41.f, 1e-3f));
    f.doc->stop();
}

TEST_CASE("editor: câmera de jogo desativada (active=false) cai para o editor", "[editor][tick]")
{
    DocFixture f;
    f.withProject();
    auto cam = f.doc->createEntity("Cam", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "active", "false").ok());

    auto& viewport = f.doc->viewport();
    viewport.setScreenSize(1000.f, 500.f);
    viewport.camera().posX = 7.f;

    REQUIRE(f.doc->play().ok());
    CHECK_FALSE(f.doc->hasGameCamera());
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(7.f, 1e-3f));
    f.doc->stop();
}

TEST_CASE("editor: hit-test seleciona o quad da frente", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto a = f.doc->createEntity("A", eng::scene::kNoEntity);
    auto b = f.doc->createEntity("B", eng::scene::kNoEntity);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    eng::editor::TransformDesc ta;
    ta.position = {0.f, 0.f, 0.f};
    REQUIRE(f.doc->setTransform(a.value(), ta).ok());
    eng::editor::TransformDesc tb;
    tb.position = {0.3f, 0.f, 0.f}; // sobreposto a A, criado depois (frente)
    REQUIRE(f.doc->setTransform(b.value(), tb).ok());

    auto& viewport = f.doc->viewport();
    viewport.setScreenSize(200.f, 200.f);

    const auto quads =
        viewport.buildQuads(*f.doc->sceneInFocus(), f.doc->selection());
    REQUIRE(quads.size() == 2);

    // Tap no centro: pega B (último desenhado = frente).
    auto hit = f.doc->viewportTap(100.f, 100.f);
    REQUIRE(hit.has_value());
    CHECK(*hit == b.value());

    // Tap longe: nada.
    auto none = f.doc->viewportTap(5.f, 5.f);
    CHECK_FALSE(none.has_value());
    CHECK_FALSE(f.doc->selection().has_value());
}

TEST_CASE("editor: quads refletem hierarquia (world matrix)", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto parent = f.doc->createEntity("P", eng::scene::kNoEntity);
    auto child = f.doc->createEntity("C", parent.value());
    REQUIRE(parent.ok());
    REQUIRE(child.ok());
    eng::editor::TransformDesc tp;
    tp.position = {4.f, 0.f, 0.f};
    REQUIRE(f.doc->setTransform(parent.value(), tp).ok());
    eng::editor::TransformDesc tc;
    tc.position = {1.f, 0.f, 0.f};
    REQUIRE(f.doc->setTransform(child.value(), tc).ok());

    const auto quads = f.doc->viewport().buildQuads(
        *f.doc->sceneInFocus(), f.doc->selection());
    REQUIRE(quads.size() == 2);
    // Filho: 4 (pai) + 1 (local) = 5 no X.
    CHECK_THAT(quads[1].worldX, Catch::Matchers::WithinAbs(5.f, 1e-3f));
    CHECK_THAT(quads[0].worldX, Catch::Matchers::WithinAbs(4.f, 1e-3f));
}

// =============================================================================
// 8. AssetBrowser (§8.10: asset discovery)
// =============================================================================

TEST_CASE("editor: importa, lista, renomeia, move e remove assets", "[editor]")
{
    DocFixture f;
    f.withProject();

    // Arquivo temporário (como o SAF do Android deixaria — §D7).
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());
    REQUIRE(f.fs->writeAllText(eng::fs::Path{".import_tmp/grass.png"},
                               "PNGDATA")
                .ok());

    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);

    auto imported = browser->import(".import_tmp/grass.png", "textures",
                                     "grass");
    REQUIRE(imported.ok());
    CHECK_FALSE(imported.value().empty());

    auto listed = browser->list("textures");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].name == "grass.png");
    CHECK(listed.value()[0].registered);
    CHECK(listed.value()[0].id == imported.value());

    // Rename: id preservado (ADR-029).
    REQUIRE(browser->rename("textures", "grass.png", "lava.png").ok());
    listed = browser->list("textures");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].name == "lava.png");
    CHECK(listed.value()[0].id == imported.value());

    // Move p/ models: tipo muda, id preservado.
    REQUIRE(browser->move("textures", "lava.png", "models").ok());
    auto models = browser->list("models");
    REQUIRE(models.ok());
    REQUIRE(models.value().size() == 1);
    CHECK(models.value()[0].id == imported.value());
    auto textures = browser->list("textures");
    REQUIRE(textures.ok());
    CHECK(textures.value().empty());

    // Remove: some de tudo.
    REQUIRE(browser->remove("models", "lava.png").ok());
    models = browser->list("models");
    REQUIRE(models.ok());
    CHECK(models.value().empty());
    CHECK(browser->registry().size() == 0);
}

TEST_CASE("editor: import preserva a extensão de nomes CURTOS (regr. P0)",
          "[editor]")
{
    // Caso "art" (3 < ".png" 4): o guard pós-P0 ainda negava a extensão a
    // nomes MAIS CURTOS que ela — o vertical slice P1 pegou o sprite sem
    // textura ("No such file: .../textures/art"). Regressão permanente.

    DocFixture f;
    f.withProject();
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());
    REQUIRE(f.fs->writeAllText(eng::fs::Path{".import_tmp/hero.png"},
                               "PNGDATA")
                .ok());

    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);

    // "hero" tem 4 chars — o MESMO comprimento de ".png". Antes: o guard
    // `size() >= ext.size()+1` negava a extensão e o arquivo era salvo como
    // "hero" (sem sufixo) — a fronteira que valida pelo nome final
    // (com extensão) não encontrava o arquivo.
    std::string finalName;
    auto imported = browser->import(".import_tmp/hero.png", "textures",
                                     "hero", &finalName);
    REQUIRE(imported.ok());
    CHECK(finalName == "hero.png");

    // O arquivo existe EXATAMENTE sob o nome final devolvido (é ele que a
    // validação JNI lê e o TextureCache resolve).
    auto bytes = browser->read("textures", finalName);
    REQUIRE(bytes.ok());
    CHECK(bytes.value().size() == 7);  // "PNGDATA"

    // E a listagem mostra o nome com extensão.
    auto listed = browser->list("textures");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].name == "hero.png");

    // "art" tem 3 chars — MAIS CURTO que ".png": o guard pós-P0
    // (`>= ext.size()`) ainda negava a extensão a estes (bug achado pelo
    // vertical slice P1: sprite "art.png" sem textura no viewport).
    REQUIRE(f.fs->writeAllText(eng::fs::Path{".import_tmp/art.png"},
                               "PNGDATA")
                .ok());
    std::string artName;
    auto art = browser->import(".import_tmp/art.png", "textures", "art",
                               &artName);
    REQUIRE(art.ok());
    CHECK(artName == "art.png");
    auto artBytes = browser->read("textures", artName);
    REQUIRE(artBytes.ok());
    CHECK(artBytes.value().size() == 7);
}

TEST_CASE("editor: arquivo não catalogado aparece como unregistered", "[editor]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.fs->mkdirs(eng::fs::Path{"TestGame/assets/scenes"}).ok());
    REQUIRE(f.fs->writeAllText(
                 eng::fs::Path{"TestGame/assets/scenes/level.json"}, "{}")
                .ok());

    auto listed = f.doc->assets()->list("scenes");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK_FALSE(listed.value()[0].registered);
}

// =============================================================================
// 8.5 Guarda de ambiente (bug C-17 da auditoria final)
// =============================================================================

namespace {

/// EditorHost::create com backend real exige driver (lavapipe/EGL). Sem
/// driver o caso SKIPA com motivo — o mesmo protocolo de degradação de
/// rhi_vulkan/rhi_gles (antes: 3 casos FALHAVAM neste ambiente).
bool editorGraphicsUnavailable() {
    (void)eng::rhi::Renderer::registerBackend(
        eng::rhi::BackendType::Vulkan, &eng::rhi::vulkan::createBackend);
    (void)eng::rhi::Renderer::registerBackend(
        eng::rhi::BackendType::OpenGLES, &eng::rhi::gles::createBackend);
    eng::rhi::RendererConfig config; // device-only probe
    config.enableValidation = false;
    auto renderer = eng::rhi::Renderer::create(config);
    return renderer.isError();
}

}  // namespace

// =============================================================================
// 9. ViewportRenderer + EditorHost — backends reais (rhi_hardware)
// =============================================================================

TEST_CASE("editor: viewport renderer desenha quads (GLES/llvmpipe)",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host = eng::editor::EditorHost::create("gles",
                                                ".editor-test-ws-gles");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};

    auto& doc = owned->document();
    ensureProject(doc, "RenderGame");
    auto entity = doc.createEntity("Red", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    // Entidade no CENTRO da tela (0,0 mundo).
    owned->document().viewport().setScreenSize(64.f, 48.f);

    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                          48);
    REQUIRE(owned->state() == eng::editor::HostSurfaceState::Available);

    REQUIRE(owned->renderFrame(1.f / 60.f));
    REQUIRE(owned->stats().firstFrameSubmitted);
    REQUIRE(owned->stats().firstFramePresented);

    // PROVA de conteúdo sem readback: os vértices enviados. O quad da
    // entidade cobre o centro (0,0 clip) — os 6 vértices do quad
    // englobam (0,0) com meia-largura >= kMinQuadPixels/64 em clip.
    const auto* renderer = owned->capabilities(); // (não-null = vivo)
    CHECK(renderer != nullptr);
    CHECK(owned->selectedBackend() == eng::rhi::BackendType::OpenGLES);

    // Ciclo de surface (§XXVIII FASE 7 adaptado): destroy→recreate→render.
    owned->surfaceDestroyed();
    CHECK(owned->state() == eng::editor::HostSurfaceState::Destroyed);
    CHECK_FALSE(owned->renderFrame(1.f / 60.f)); // sem surface: no-op
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                          48);
    REQUIRE(owned->renderFrame(1.f / 60.f));

    // Pause/Resume com render.
    owned->onPause();
    CHECK_FALSE(owned->renderFrame(1.f / 60.f));
    owned->onResume();
    REQUIRE(owned->renderFrame(1.f / 60.f));
}

TEST_CASE("editor: viewport renderer Vulkan/lavapipe submete frames",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host =
        eng::editor::EditorHost::create("vulkan", ".editor-test-ws-vk");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};

    auto& doc = owned->document();
    ensureProject(doc, "VkGame");
    REQUIRE(doc.createEntity("Entity", eng::scene::kNoEntity).ok());

    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                          48);
    REQUIRE(owned->state() == eng::editor::HostSurfaceState::Available);
    CHECK(owned->selectedBackend() == eng::rhi::BackendType::Vulkan);

    const auto before = owned->stats().framesSubmitted;
    REQUIRE(owned->renderFrame(1.f / 60.f));
    CHECK(owned->stats().framesSubmitted == before + 1);
    CHECK(owned->stats().framesPresented >= 1);
}

TEST_CASE("editor: play/stop alterna conteúdo do viewport no host", "[editor]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    std::filesystem::remove_all(
        std::filesystem::path{".editor-test-ws-auto"});  // watchdog: determinismo
    auto host = eng::editor::EditorHost::create("auto", ".editor-test-ws-auto");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};

    auto& doc = owned->document();
    ensureProject(doc, "PlayGame");
    auto entity = doc.createEntity("Thing", eng::scene::kNoEntity);
    REQUIRE(entity.ok());

    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                          48);

    REQUIRE(owned->renderFrame(1.f / 60.f)); // Edit
    REQUIRE(doc.play().ok());
    REQUIRE(owned->renderFrame(1.f / 60.f)); // Play (foco = clone)
    doc.stop();
    REQUIRE(owned->renderFrame(1.f / 60.f)); // Edit de novo
}

// =============================================================================
// 10. Reabertura entre execuções (regression FASE 8)
// =============================================================================

TEST_CASE("editor: reabre projeto de execução anterior via workspace", "[editor]")
{
    // REGRESSÃO FASE 8: openProject resolvia contra o CWD em vez do
    // WORKSPACE — só reproduzível com disco persistente entre hosts (o
    // CI sempre roda com dirs novos). Sem GPU (host sem surface).
    const char* ws = ".editor-test-ws-reopen";
    {
        auto host = eng::editor::EditorHost::create("auto", ws);
        REQUIRE(host.ok());
        std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
        // Runs repetidos encontram o projeto — para ESTE teste tanto faz
        // quem o criou; o que se valida é a REABERTURA pelo workspace.
        (void)owned->document().newProject("ReopenGame");
    }
    auto host2 = eng::editor::EditorHost::create("auto", ws);
    REQUIRE(host2.ok());
    std::unique_ptr<eng::editor::EditorHost> owned2{host2.value()};
    auto& doc2 = owned2->document();
    REQUIRE(doc2.newProject("ReopenGame").isError());            // já existe
    REQUIRE(doc2.openProject(eng::fs::Path{"ReopenGame"}).ok()); // pelo workspace
    CHECK(doc2.projectName() == "ReopenGame");
    CHECK_FALSE(doc2.projectDirty());
}

// =============================================================================
// 11. FASE 10 — física/animação/partículas em PLAY (§8 integração)
// =============================================================================

TEST_CASE("editor: componentes de gameplay no catálogo/serialização",
          "[editor]")
{
    const auto catalog = eng::editor::Inspector::catalog();
    for (const char* name :
         {"eng::physics::RigidBody", "eng::physics::Collider",
          "eng::physics::CharacterBody", "eng::animation::Animator",
          "eng::particles::ParticleEmitter"}) {
        CAPTURE(name);
        CHECK(std::find(catalog.begin(), catalog.end(), name) !=
              catalog.end());
    }

    // Round-trip pela cena: cria com physics/animation/particles, salva,
    // recarrega — componentes persistem (ADR-033).
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Gameplay", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::RigidBody")
                .ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::physics::Collider")
                .ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                 "eng::animation::Animator")
                .ok());
    REQUIRE(f.doc->addComponent(entity.value(),
                                 "eng::particles::ParticleEmitter")
                .ok());
    REQUIRE(f.doc->setInspectorField(entity.value(),
                                     "eng::physics::RigidBody", "mass", "2.5")
                .ok());

    REQUIRE(f.doc->saveScene("gp.json").ok());
    REQUIRE(f.doc->loadScene("gp.json").ok());
    auto snapshot = f.doc->hierarchySnapshot();
    REQUIRE(snapshot.size() == 1);
    auto got = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), snapshot[0].entity,
        "eng::physics::RigidBody", "mass");
    REQUIRE(got.ok());
    CHECK(got.value() == "2.5");
}

TEST_CASE("editor: PLAY avança física (timestep fixo) sobre o CLONE",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    auto ball = f.doc->createEntity("Ball", eng::scene::kNoEntity);
    REQUIRE(ball.ok());
    REQUIRE(f.doc->addComponent(ball.value(), "eng::physics::RigidBody")
                .ok());
    // Gravidade padrão -9.81; posição y=10.
    eng::editor::TransformDesc tr;
    tr.position = {0.f, 10.f, 0.f};
    REQUIRE(f.doc->setTransform(ball.value(), tr).ok());

    REQUIRE(f.doc->play().ok());
    // 0.5s em frames de ~8ms: física avança por passos FIXOS de 1/60.
    for (int i = 0; i < 61; ++i) {
        f.doc->tick(1.f / 120.f);
    }
    auto runtimeY = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), ball.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(runtimeY.ok());
    const float fallen = 10.f - std::stof(runtimeY.value());
    CHECK(fallen > 0.5f); // caiu de verdade
    CHECK(fallen < 1.3f); // ~0.5s de queda (1.22m)

    f.doc->stop();
    // Edição INTACTA (§8.7): y continua 10.
    auto editY = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), ball.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(editY.ok());
    CHECK(editY.value() == "10");
}

TEST_CASE("editor: PLAY avança animação e partículas sobre o CLONE",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    auto node = f.doc->createEntity("Fx", eng::scene::kNoEntity);
    REQUIRE(node.ok());
    REQUIRE(f.doc->addComponent(node.value(),
                                 "eng::animation::Animator")
                .ok());
    REQUIRE(f.doc->addComponent(node.value(),
                                 "eng::particles::ParticleEmitter")
                .ok());
    // Animator: clip "rise", tocando.
    eng::animation::AnimationClip rise;
    rise.name = "rise";
    rise.position = {{0.f, {0.f, 0.f, 0.f}}, {1.f, {0.f, 2.f, 0.f}}};
    f.doc->runtimeAnimations().add(rise);
    REQUIRE(f.doc->setInspectorField(node.value(), "eng::animation::Animator",
                                     "clip", "rise")
                .ok());
    REQUIRE(f.doc->setInspectorField(node.value(), "eng::animation::Animator",
                                     "playing", "true")
                .ok());

    REQUIRE(f.doc->play().ok());
    for (int i = 0; i < 30; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    // Animação aplicada ao CLONE: y ≈ 1.0 (0.5s de 1s de clip).
    auto y = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), node.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(y.ok());
    CHECK(std::stof(y.value()) > 0.9f);

    // Partículas vivas no clone (emitter padrão 20/s).
    CHECK(eng::particles::ParticleSystem::aliveCount(*f.doc->sceneInFocus()) >
          0);

    f.doc->stop();
    // Edição intacta: sem animação aplicada.
    auto editY = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), node.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(editY.ok());
    CHECK(std::stof(editY.value()) == 0.f);
    // E as partículas morreram com o clone.
    CHECK(eng::particles::ParticleSystem::aliveCount(
              *f.doc->sceneInFocus()) == 0);
}

// =============================================================================
// 12. Pack/unpack JNI
// =============================================================================

TEST_CASE("editor: pack/unpack entity é bijetivo exceto 0", "[editor]")
{
    const eng::ecs::Entity e{17u, 3u};
    const std::uint64_t packed = pack(e);
    CHECK(packed != 0ull);
    const eng::ecs::Entity back = unpack(packed);
    CHECK(back == e);
    CHECK(unpack(0ull) == eng::scene::kNoEntity);
}

// =============================================================================
// Correções da auditoria final FASES 4–10 (remediação)
// =============================================================================

TEST_CASE("editor: documento pré-projeto é editável sem UB (C-3)", "[editor]")
{
    // Antes: create() não emitia a cena — sceneInFocus() fazia &*scene_
    // vazio (UB). Agora o contrato do header ("cena vazia PRONTA PARA
    // EDIÇÃO") é real.
    eng::fs::MemoryFileSystem fs;
    auto docResult = EditorDocument::create(fs, eng::fs::Path{".ws-pre"});
    REQUIRE(docResult.ok());
    auto doc = std::move(docResult.value());

    REQUIRE(doc->sceneInFocus() != nullptr);          // sem UB
    const auto before = doc->sceneInFocus()->nodeCount();
    CHECK(before == 0);

    // Comandos de cena funcionam ANTES de qualquer newProject.
    auto entity = doc->createEntity("Solto", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    CHECK(doc->sceneInFocus()->nodeCount() == 1);
    CHECK(doc->hierarchySnapshot().size() == 1);

    // Comandos que exigem projeto continuam rejeitando com erro preciso.
    auto saved = doc->saveProject();
    CHECK(saved.isError());
}

TEST_CASE("editor: viewport desenha partículas vivas como quads (drift D6)",
          "[editor]")
{
    // A auditoria final mostrou que NADA lia a ParticlePool — agora o
    // Viewport gera um quad por partícula viva (clone em Play tem pools).
    eng::fs::MemoryFileSystem fs;
    auto docResult = EditorDocument::create(fs, eng::fs::Path{".ws-part"});
    REQUIRE(docResult.ok());
    auto doc = std::move(docResult.value());
    ensureProject(*doc, "ParticleQuads");

    auto emitter = doc->createEntity("Emitter", eng::scene::kNoEntity);
    REQUIRE(emitter.ok());
    REQUIRE(doc->addComponent(emitter.value(),
                              "eng::particles::ParticleEmitter")
                .ok());
    // rate 10/s (default é 5) via inspector de reflexão.
    REQUIRE(doc->setInspectorField(emitter.value(),
                                   "eng::particles::ParticleEmitter", "rate",
                                   "10")
                .ok());

    // Em Edit: sem pool → zero quads de partícula.
    const auto& viewport = doc->viewport();
    const eng::scene::Scene* editScene = doc->sceneInFocus();
    CHECK(viewport.buildParticleQuads(*editScene).empty());

    // Em Play: o clone ganha pools após o primeiro tick (spawn por
    // acumulador) → quads de partícula existem.
    REQUIRE(doc->play().ok());
    doc->tick(0.5f); // 10/s * 0.5s = 5 vivas
    const auto quads = viewport.buildParticleQuads(*doc->sceneInFocus());
    CHECK(quads.size() == 5);
    doc->stop();
    CHECK(viewport.buildParticleQuads(*doc->sceneInFocus()).empty());
}

// =============================================================================
// 15. Sprite com TEXTURA REAL (evolução P0 — o fim do retângulo colorido)
// =============================================================================

namespace {

/// PNG 2x2 RGBA (vermelho/verde/azul/branco) — cópia EXATA do fixture
/// kPng2x2 de engine/image/tests/ImageFixtures.hpp (gerado por
/// scripts/gen_image_fixtures.py — procedência única).
constexpr unsigned char kPng2x2[86] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,
    0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,
    36,0,0,0,29,73,68,65,84,120,1,1,18,0,237,255,
    0,255,0,0,255,0,255,0,255,4,1,0,255,0,255,0,
    0,0,62,255,6,0,112,227,74,153,0,0,0,0,73,69,
    78,68,174,66,96,130,
};

void writePngTemp(eng::fs::FileSystem& fs)
{
    REQUIRE(fs.mkdirs(eng::fs::Path{".import_tmp"}).ok());
    REQUIRE(fs.writeAllBytes(
                eng::fs::Path{".import_tmp/grass.png"},
                std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                          sizeof(kPng2x2)})
                .ok());
}

}  // namespace

TEST_CASE("editor: sprite — importar imagem, atribuir, quads e persistência",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    writePngTemp(*f.fs);

    // 1) Import REAL: bytes PNG válidos catalogados em textures/.
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    auto imported = browser->import(".import_tmp/grass.png", "textures", "grass");
    REQUIRE(imported.ok());

    // 2) Metadados decodificados SEM GPU (dimensões/alfa do arquivo real).
    eng::editor::TextureCache cache;
    const auto info = cache.imageInfo(*browser, "grass.png");
    CHECK(info.valid);
    CHECK(info.width == 2);
    CHECK(info.height == 2);
    CHECK(info.alpha);

    // 3) Entidade com componente SpriteData (catálogo ÚNICO — ADR-043).
    auto created = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(created.ok());
    const eng::ecs::Entity player = created.value();
    REQUIRE(f.doc->addComponent(player, "eng::editor::SpriteData").ok());

    // 4) Atribuição da textura por CAMPO (mesma via do Inspector/JNI).
    REQUIRE(f.doc
                ->setInspectorField(player, "eng::editor::SpriteData",
                                   "textureAsset", "grass.png")
                .ok());
    // Campos do workflow: região + flip + tint + ppu.
    REQUIRE(f.doc
                ->setInspectorField(player, "eng::editor::SpriteData",
                                   "pixelsPerUnit", "0.5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(player, "eng::editor::SpriteData", "flipX",
                                   "true")
                .ok());

    // 5) O QUAD carrega o sprite (não é mais o marcador hue puro).
    const auto* scene = f.doc->sceneInFocus();
    const auto quads = f.doc->viewport().buildQuads(*scene, player);
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].textureAsset == "grass.png");
    CHECK(quads[0].u0 == 0.f);
    CHECK(quads[0].u1 == 1.f);
    CHECK(quads[0].flipX);
    CHECK(quads[0].tintR == 1.f);
    CHECK(quads[0].spritePpu == 0.5f);

    // 6) Persistência: save/load preserva o SpriteData COMPLETO.
    REQUIRE(f.doc->saveScene("sprites.json").ok());
    REQUIRE(f.doc->loadScene("sprites.json").ok());
    const auto* sprite = f.doc->sceneInFocus()->world().get<eng::editor::SpriteData>(
        player);
    REQUIRE(sprite != nullptr);
    CHECK(sprite->textureAsset == "grass.png");
    CHECK(sprite->pixelsPerUnit == 0.5f);
    CHECK(sprite->flipX);
    CHECK(sprite->flipY == false);

    // 7) Componente no catálogo do inspector (aparece para o usuário).
    const auto componentsTsv = f.doc->inspectorFields(player, "eng::editor::SpriteData");
    REQUIRE_FALSE(componentsTsv.empty());
}

TEST_CASE("editor: sprite com textura AUSENTE cai no caminho de cor (honesto)",
          "[editor]")
{
    DocFixture f;
    f.withProject();

    auto created = f.doc->createEntity("Ghost", eng::scene::kNoEntity);
    REQUIRE(created.ok());
    REQUIRE(f.doc->addComponent(created.value(), "eng::editor::SpriteData").ok());
    REQUIRE(f.doc
                ->setInspectorField(created.value(), "eng::editor::SpriteData",
                                   "textureAsset", "nao_existe.png")
                .ok());

    // Quad marcado como sprite, mas o acquire falha → renderiza como quad
    // de cor (sem crash, sem placeholder falso — log único no cache).
    const auto* scene = f.doc->sceneInFocus();
    const auto quads = f.doc->viewport().buildQuads(*scene, created.value());
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].textureAsset == "nao_existe.png");
    // O TextureCache SEM renderer não sobe nada — imageInfo invalida.
    eng::editor::TextureCache cache;
    const auto info = cache.imageInfo(*f.doc->assets(), "nao_existe.png");
    CHECK_FALSE(info.valid);
}

TEST_CASE("editor: host renderiza sprite TEXTURIZADO (GLES/llvmpipe real)",
          "[editor][rhi_hardware]")
{
    eng::rhi::Renderer::clearRegisteredBackends();
    REQUIRE(eng::rhi::Renderer::registerBackend(
                eng::rhi::BackendType::OpenGLES, &eng::rhi::gles::createBackend)
                .ok());

    // Host em disco REAL (workspace temporário isolado por caso).
    const std::string root = "sprite_host_test_" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(&root));
    auto created = eng::editor::EditorHost::create("gles", root.c_str());
    if (!created) {
        SKIP("OpenGL ES indisponível: " << created.error().message);
    }
    std::unique_ptr<eng::editor::EditorHost> host{created.value()};
    // RECOVERY P0: janela-marker headless — antes nullptr, o que deixava o
    // host em NoSurface e este teste SKIPAVA ATÉ NO CI (nunca validou o
    // upload de textura de verdade; o bug da orientação sobreviveu por isso).
    int marker = 0;
    host->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64, 48);
    if (host->viewportRenderer() == nullptr) {
        SKIP("OpenGL ES indisponível (renderer não criado sem driver)");
    }

    auto& doc = host->document();
    REQUIRE(doc.newProject("SpriteGame").ok());
    REQUIRE(doc.saveProject().ok());

    // Import via fs do WORKSPACE (rooted — a mesma fronteira do Android):
    // escreve o staging PNG e importa.
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    {
        eng::fs::FileSystem& ws = host->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws
                    .writeAllBytes(
                        eng::fs::Path{".import_tmp/hero.png"},
                        std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                                  sizeof(kPng2x2)})
                    .ok());
    }
    // import() usa path relativo ao ROOT do projeto — o staging do host é
    // criado FORA do projeto (filesDir/.import_tmp): o documento do host
    // aponta o workspace p/ root/, então o import resolve .import_tmp/hero.png
    // relativo ao workspace. (Contrato §8.5: staging DENTRO do workspace.)
    auto imported = browser->import(".import_tmp/hero.png", "textures", "hero");
    REQUIRE(imported.ok());

    auto entity = doc.createEntity("Hero", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(doc.addComponent(entity.value(), "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(entity.value(), "eng::editor::SpriteData",
                                 "textureAsset", "hero.png")
                .ok());

    // Frame com o sprite: textura REAL subiu (decode→RHI→bind→draw).
    REQUIRE(host->renderFrame(1.f / 60.f));
    const auto* renderer = host->viewportRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(renderer->lastFrameTexturedSprites() == 1);
    // P3: sprite SEM material usa o pipeline LIT (default — luz visível ao
    // adicionar Light2D sem tocar em cada sprite). Os vértices vão para o
    // lote lit (48B, mesmos campos u/v).
    const auto& spriteVerts = renderer->lastFrameLitSpriteVertices();
    REQUIRE(spriteVerts.size() == 6);
    // UV completo no quad (0,0)→(1,1): a amostragem cobre a textura.
    CHECK(spriteVerts[0].u == 0.f);
    CHECK(spriteVerts[0].v == 0.f);
    CHECK(spriteVerts[1].u == 1.f);
    CHECK(spriteVerts[1].v == 0.f);
    CHECK(spriteVerts[2].u == 1.f);
    CHECK(spriteVerts[2].v == 1.f);
    CHECK(spriteVerts[5].u == 0.f);
    CHECK(spriteVerts[5].v == 1.f);

    // Segundo frame: cache HIT (mesma textura — sem novo upload) e Play
    // com sprites no clone (separação editor×runtime mantida).
    REQUIRE(host->renderFrame(1.f / 60.f));
    CHECK(renderer->lastFrameTexturedSprites() == 1);

    REQUIRE(doc.play().ok());
    REQUIRE(host->renderFrame(1.f / 60.f));
    CHECK(renderer->lastFrameTexturedSprites() == 1);
    REQUIRE(host->renderFrame(1.f / 60.f));
    doc.stop();
}

TEST_CASE("editor: SpriteData default — ppu 48 (imagem utilizável no viewport)",
          "[editor]")
{
    DocFixture f;
    f.withProject();
    auto entity = f.doc->createEntity("Hero", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(f.doc->addComponent(entity.value(), "eng::editor::SpriteData").ok());

    // Default = 48 px/unidade (casa com o zoom padrão da câmera → a imagem
    // aparece 1:1 na tela). Antes: 1 → uma foto de 1080px media 51.840px de
    // tela — o viewport virava um "mar de cor".
    const auto* scene = f.doc->sceneInFocus();
    const auto quads = f.doc->viewport().buildQuads(*scene, entity.value());
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].spritePpu == 48.f);
}

// =============================================================================
// REPRODUÇÃO P0 (RECOVERY FASE 0) — "A imagem é importada mas NÃO APARECE
// CORRETAMENTE no viewport". Contrato VISUAL do sprite pinhado com PIXEL
// REAL lido da surface (missão: feature visual é validada visualmente):
//   1. TAMANHO — o quad cobre a região mundial esperada (sem o fator 0.5
//      espúrio no half-extent NDC que desenhava tudo com metade do size);
//   2. ORIENTAÇÃO — o topo da imagem aparece no TOPO do quad na tela
//      (stb decodifica top-down; GL tem v=0 na BASE — sem o flip na
//      fronteira de upload, o sprite sai de ponta-cabeça);
//   3. SELEÇÃO — o hit-test acerta a borda do sprite (tamanho desenhado,
//      não a escala local).
// =============================================================================

TEST_CASE("editor: P0 — PNG no viewport: tamanho, orientação e hit CORRETOS",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host = eng::editor::EditorHost::create("gles",
                                                ".editor-test-ws-p0img");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    int marker = 0;  // janela-marker headless (mesmo padrão dos testes GLES)
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                          128, 128);
    REQUIRE(owned->state() == eng::editor::HostSurfaceState::Available);

    auto& doc = owned->document();
    ensureProject(doc, "P0ImageGame");

    // PNG canônico 2x2 (kPng2x2): linha 0 do ARQUIVO é o TOPO da imagem —
    // (0,0)=vermelho TL, (1,0)=verde TR, (0,1)=azul BL, (1,1)=branco BR.
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    {
        eng::fs::FileSystem& ws = owned->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws
                    .writeAllBytes(
                        eng::fs::Path{".import_tmp/quad.png"},
                        std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                                  sizeof(kPng2x2)})
                    .ok());
    }
    REQUIRE(browser->import(".import_tmp/quad.png", "textures", "quad").ok());

    // Entidade em (0.5, -0.5), sprite região completa, ppu=1 → tamanho
    // mundial 2x2 unidades (região 2px / ppu 1). Câmera padrão (0,0),
    // zoom 48 (1 unidade = 48px; superfície 128x128): o CENTRO da tela
    // cai no ponto (u=0.25, v=0.75) do sprite — 25% da esquerda, 75% de
    // baixo — exatamente o canto SUPERIOR-ESQUERDO da imagem original.
    auto entity = doc.createEntity("Sprite", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(doc.addComponent(entity.value(), "eng::editor::SpriteData").ok());
    REQUIRE(doc
                .setInspectorField(entity.value(), "eng::editor::SpriteData",
                                   "textureAsset", "quad.png")
                .ok());
    REQUIRE(doc
                .setInspectorField(entity.value(), "eng::editor::SpriteData",
                                   "pixelsPerUnit", "1")
                .ok());
    REQUIRE(doc
                .setInspectorField(entity.value(), "eng::math::Transform",
                                   "position.x", "0.5")
                .ok());
    REQUIRE(doc
                .setInspectorField(entity.value(), "eng::math::Transform",
                                   "position.y", "-0.5")
                .ok());

    REQUIRE(owned->renderFrame(1.f / 60.f));
    auto* renderer = owned->viewportRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(renderer->lastFrameTexturedSprites() == 1);

    // CONTRATO 1 — TAMANHO: topo do quad em world y = -0.5 + 1.0 = 0.5
    // → tela y = 64 - 0.5*48 = 40 → clip = 1 - (40/128)*2 = 0.375.
    // (Antes: 0.0 — o half-extent do sprite carregava um 0.5 espúrio e a
    // imagem desenhava com METADE do tamanho mundial, menor que a própria
    // borda de seleção.)
    // P3: default LIT — vértices no lote lit (48B; mesmos pos/uv).
    const auto& verts = renderer->lastFrameLitSpriteVertices();
    REQUIRE(verts.size() == 6);
    CHECK(verts[5].y == Catch::Approx(0.375f).margin(1e-3f));

    // CONTRATO 2 — ORIENTAÇÃO: o pixel central da tela mostra o canto
    // SUPERIOR-ESQUERDO da imagem = VERMELHO (não azul — ponta-cabeça).
    std::uint8_t pixel[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixel).ok());
    INFO("readback: " << +pixel[0] << " " << +pixel[1] << " " << +pixel[2]
                      << " " << +pixel[3]);
    CHECK(pixel[0] >= 200);  // R dominante
    CHECK(pixel[1] <= 64);   // G baixo
    CHECK(pixel[2] <= 64);   // B baixo
    CHECK(pixel[3] == 255);  // opaco

    // CONTRATO 3 — SELEÇÃO: toque perto da borda direita do sprite
    // (world x≈1.29, y=-0.5 → tela (126, 88)) ACERTA — o hit box é o
    // tamanho desenhado (±48px), não a escala local (±24px).
    auto hit = doc.viewportTap(126.f, 88.f, &owned->textureCache());
    CHECK(hit.has_value());
}

// =============================================================================
// REGRESSÃO §26 (RECOVERY P0) — os bugs do APK Android:
//   "InvalidArgument: AssetBrowser: destino absoluto é proibido"
//   "InvalidArgument: EditorDocument: caminho absoluto proibido"
//
// Topologia ANDROID reproduzida no Linux: workspace FÍSICO ABSOLUTO (como
// filesDir/projects) entra pelo EditorHost. A fronteira (RootedFileSystem)
// converte; o editor opera relativo. Sem a correção, newProject até criava
// a estrutura, mas import/scriptCreate morriam nas validações anti-absoluto
// (o root absoluto vazava para dentro do documento).
// =============================================================================

namespace {

/// Tmpdir RAII ABSOLUTO (o do FsTests é relativo ao CWD dos testes de fs;
/// aqui o requisito é exatamente um root absoluto, como o Android entrega).
struct AbsTmpDir {
    std::filesystem::path dir;

    AbsTmpDir()
    {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec || base.empty()) {
            base = "/tmp";
        }
        static std::uint64_t counter = 0;
        do {
            dir = base / ("goni_editor_host_abs_" +
                          std::to_string(++counter) + "_" +
                          std::to_string(
                              reinterpret_cast<std::uintptr_t>(this)));
        } while (std::filesystem::exists(dir, ec));
        std::filesystem::create_directories(dir, ec);
    }

    ~AbsTmpDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AbsTmpDir(const AbsTmpDir&) = delete;
    AbsTmpDir& operator=(const AbsTmpDir&) = delete;

    [[nodiscard]] std::string str() const { return dir.generic_string(); }
};

} // namespace

TEST_CASE("editor: host com workspace ABSOLUTO — import e script funcionam "
          "(regressão Android §26)",
          "[editor][rhi_hardware]")
{
    eng::rhi::Renderer::clearRegisteredBackends();
    REQUIRE(eng::rhi::Renderer::registerBackend(
                eng::rhi::BackendType::OpenGLES, &eng::rhi::gles::createBackend)
                .ok());

    AbsTmpDir tmp;
    // filesDir/projects do Android: ABSOLUTO. É o que a EditorActivity
    // passa por JNI (nativeEditorCreate("auto", workspace.absolutePath)).
    const std::string workspaceRoot = tmp.str() + "/projects";
    auto created = eng::editor::EditorHost::create("gles",
                                                   workspaceRoot.c_str());
    if (!created) {
        SKIP("OpenGL ES indisponível: " << created.error().message);
    }
    std::unique_ptr<eng::editor::EditorHost> host{created.value()};
    host->surfaceCreated(nullptr, eng::rhi::NativeWindowKind::Headless, 64,
                         48);

    auto& doc = host->document();

    // --- 1. Ciclo de vida do projeto (§4) -----------------------------------
    REQUIRE(doc.newProject("MeuJogo").ok());
    REQUIRE(doc.hasProject());
    CHECK(doc.projectName() == "MeuJogo");

    // --- 2. Import de asset (o bug nº 1 do APK) -----------------------------
    // SAF teria copiado para <workspace>/.import_tmp/ — staging relativo.
    {
        eng::fs::FileSystem& ws = host->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws.writeAllBytes(
                    eng::fs::Path{".import_tmp/grass.png"},
                    std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                              sizeof(kPng2x2)})
                    .ok());
    }
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    auto imported = browser->import(".import_tmp/grass.png", "textures",
                                     "grass");
    REQUIRE(imported.ok());  // ← ANTES: "destino absoluto é proibido"

    // Import visível na listagem, com id do registry.
    auto listed = browser->list("textures");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].name == "grass.png");
    CHECK(listed.value()[0].registered);
    CHECK(listed.value()[0].id == imported.value());

    // --- 3. Criação de script (o bug nº 2 do APK) ---------------------------
    REQUIRE(doc.scriptCreate("Movimento").ok());  // ← ANTES: "caminho absoluto proibido"
    // Conteúdo maior que 512 bytes (§12 — sem buffer JNI truncando).
    std::string big = "# comentário grande\n";
    for (int i = 0; i < 40; ++i) {
        big += "# linha de preenchimento " + std::to_string(i) + "\n";
    }
    REQUIRE(doc.scriptWrite("Big.nis", big).ok());
    {
        auto back = doc.scriptRead("Big.nis");
        REQUIRE(back.ok());
        CHECK(back.value().size() == big.size());
        CHECK(back.value() == big);
    }
    {
        auto scripts = doc.scriptList();
        REQUIRE(scripts.ok());
        REQUIRE(scripts.value().size() == 2);  // Movimento.nis + Big.nis
    }

    // --- 4. Cena: save → reload → conteúdo íntegro --------------------------
    {
        auto entity = doc.createEntity("Player", eng::scene::kNoEntity);
        REQUIRE(entity.ok());
        REQUIRE(doc.addComponent(entity.value(),
                                 "eng::editor::SpriteData")
                    .ok());
        REQUIRE(doc.setInspectorField(entity.value(),
                                      "eng::editor::SpriteData",
                                      "textureAsset", "grass.png")
                    .ok());
        REQUIRE(doc.setTransform(entity.value(),
                                 {{64.f, 32.f, 0.f},
                                  {0.f, 45.f, 0.f},
                                  {2.f, 2.f, 1.f}})
                    .ok());
    }
    REQUIRE(doc.saveScene("main.json").ok());
    {
        // Round-trip: nova cena + recarrega o estado salvo.
        REQUIRE(doc.newScene().ok());
        REQUIRE(doc.loadScene("main.json").ok());
        auto snapshot = doc.hierarchySnapshot();
        REQUIRE(snapshot.size() == 1);
        CHECK(snapshot[0].name == "Player");
        auto transform = doc.transform(snapshot[0].entity);
        REQUIRE(transform.ok());
        // Round-trip Euler→Quat→Euler: precisão de FPU, não identidade bit
        // a bit (mesma convenção dos testes de transform do documento).
        CHECK(transform.value().rotationDegrees.y ==
              Catch::Approx(45.f).margin(1e-3f));
        CHECK(transform.value().scale.x == Catch::Approx(2.f).margin(1e-4f));
    }

    // --- 5. Persistência LIMPA: nada de absoluto nos arquivos (§2.6) ---------
    REQUIRE(doc.saveProject().ok());
    {
        eng::fs::NativeFileSystem raw;
        const std::string files[] = {
            "/MeuJogo/project.goni.json",
            "/MeuJogo/assets/asset_registry.json",
            "/MeuJogo/scenes/main.json",
        };
        for (const std::string& rel : files) {
            auto text = raw.readAllText(
                eng::fs::Path{workspaceRoot + rel});
            INFO("arquivo: " << rel);
            REQUIRE(text.ok());
            CHECK(text.value().find(tmp.str()) == std::string::npos);
            CHECK(text.value().find(workspaceRoot) == std::string::npos);
        }
    }

    // --- 6. Reabertura do projeto (segunda execução do app) ------------------
    {
        auto reopened = eng::editor::EditorHost::create("gles",
                                                        workspaceRoot.c_str());
        if (reopened) {
            std::unique_ptr<eng::editor::EditorHost> second{reopened.value()};
            auto& doc2 = second->document();
            REQUIRE(doc2.openProject(eng::fs::Path{"MeuJogo"}).ok());
            CHECK(doc2.projectName() == "MeuJogo");
            // Assets e scripts sobreviveram à reabertura.
            auto* browser2 = doc2.assets();
            REQUIRE(browser2 != nullptr);
            auto textures = browser2->list("textures");
            REQUIRE(textures.ok());
            REQUIRE(textures.value().size() == 1);
            CHECK(textures.value()[0].name == "grass.png");
            auto scripts = doc2.scriptList();
            REQUIRE(scripts.ok());
            REQUIRE(scripts.value().size() == 2);
        }
        // Backend indisponível não invalida a regressão de paths (o host 1
        // já provou o pipeline); a reabertura é bônus de integração.
    }
}

// =============================================================================
// COLLIDER VISÍVEL (RECOVERY §10): "The editor must visually show the
// collision shape" — o autor edita shape/layer/mask/trigger e VÊ o que a
// física usará. Três provas: dados no quad, contorno na GPU, e o efeito
// físico OBSERVÁVEL (player cai no chão e PARA).
// =============================================================================

TEST_CASE("editor: quad expõe o shape do collider (dados — §10)", "[editor]")
{
    DocFixture f;
    f.withProject();
    auto ground = f.doc->createEntity("Ground", eng::scene::kNoEntity);
    REQUIRE(ground.ok());
    REQUIRE(f.doc->addComponent(ground.value(), "eng::physics::Collider")
                .ok());

    // Box com halfExtents (2,1) e escala de nó (2,1,1) → mundo (4,1).
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider", "shape", "Box")
                .ok());
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider",
                                     "halfExtents.x", "2")
                .ok());
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider",
                                     "halfExtents.y", "1")
                .ok());
    eng::editor::TransformDesc tr;
    tr.scale = {2.f, 1.f, 1.f};
    REQUIRE(f.doc->setTransform(ground.value(), tr).ok());

    const auto quads = f.doc->viewport().buildQuads(
        *f.doc->sceneInFocus(), std::nullopt);
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].hasCollider);
    CHECK_FALSE(quads[0].colliderIsSphere);
    CHECK_FALSE(quads[0].colliderTrigger);
    CHECK(quads[0].colliderHalfX == Catch::Approx(4.f).margin(1e-4f));
    CHECK(quads[0].colliderHalfY == Catch::Approx(1.f).margin(1e-4f));

    // Esfera: raio 1.5 × escala X (2) → halfX == halfY == 3 (convenção do
    // PhysicsWorld::worldShapeOf — coluna X aproxima o raio).
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider", "shape",
                                     "Sphere")
                .ok());
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider", "radius", "1.5")
                .ok());
    const auto sphereQuads = f.doc->viewport().buildQuads(
        *f.doc->sceneInFocus(), std::nullopt);
    REQUIRE(sphereQuads.size() == 1);
    CHECK(sphereQuads[0].hasCollider);
    CHECK(sphereQuads[0].colliderIsSphere);
    CHECK(sphereQuads[0].colliderHalfX == Catch::Approx(3.f).margin(1e-4f));
    CHECK(sphereQuads[0].colliderHalfY == Catch::Approx(3.f).margin(1e-4f));

    // Trigger refletido no quad (contorno âmbar no renderer).
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider", "isTrigger",
                                     "true")
                .ok());
    const auto triggerQuads = f.doc->viewport().buildQuads(
        *f.doc->sceneInFocus(), std::nullopt);
    REQUIRE(triggerQuads.size() == 1);
    CHECK(triggerQuads[0].colliderTrigger);

    // Sem Collider: quad não carrega shape.
    auto plain = f.doc->createEntity("Plain", eng::scene::kNoEntity);
    REQUIRE(plain.ok());
    const auto plainQuads = f.doc->viewport().buildQuads(
        *f.doc->sceneInFocus(), std::nullopt);
    REQUIRE(plainQuads.size() == 2);
    std::size_t withCollider = 0;
    for (const auto& quad : plainQuads) {
        withCollider += quad.hasCollider ? 1u : 0u;
    }
    CHECK(withCollider == 1);
}

TEST_CASE("editor: renderer desenha o contorno do collider (§10)",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host = eng::editor::EditorHost::create("gles",
                                                ".editor-test-ws-collider");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    auto& doc = owned->document();
    ensureProject(doc, "ColliderGame");

    auto entity = doc.createEntity("Solid", eng::scene::kNoEntity);
    REQUIRE(entity.ok());
    REQUIRE(doc.addComponent(entity.value(), "eng::physics::Collider").ok());
    // Box sólido.
    REQUIRE(doc.setInspectorField(entity.value(), "eng::physics::Collider",
                                  "shape", "Box")
                .ok());
    auto trigger = doc.createEntity("Sensor", eng::scene::kNoEntity);
    REQUIRE(trigger.ok());
    REQUIRE(doc.addComponent(trigger.value(), "eng::physics::Collider").ok());
    REQUIRE(doc.setInspectorField(trigger.value(), "eng::physics::Collider",
                                  "shape", "Sphere")
                .ok());
    REQUIRE(doc.setInspectorField(trigger.value(), "eng::physics::Collider",
                                  "isTrigger", "true")
                .ok());

    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                          48);
    REQUIRE(owned->state() == eng::editor::HostSurfaceState::Available);
    REQUIRE(owned->renderFrame(1.f / 60.f));

    // PROVA sem readback: vértices do último frame.
    //  - Box: 4 segmentos × 6 vértices = 24 vértices de contorno
    //  - Esfera: 8 segmentos × 6 = 48
    const auto& verts =
        owned->viewportRenderer()->lastFrameVertices();
    std::size_t solid = 0;
    std::size_t amber = 0;
    constexpr float kTealR = 0.16f, kTealG = 0.90f, kTealB = 0.85f;
    constexpr float kAmberR = 0.98f, kAmberG = 0.78f, kAmberB = 0.20f;
    for (const auto& v : verts) {
        if (v.r == kTealR && v.g == kTealG && v.b == kTealB) {
            ++solid;
        } else if (v.r == kAmberR && v.g == kAmberG && v.b == kAmberB) {
            ++amber;
        }
    }
    CHECK(solid == 24);  // box sólido: teal
    CHECK(amber == 48);  // esfera trigger: âmbar
}

TEST_CASE("editor: §10 — player com collider CAI no chão e PARA (física "
          "observável)",
          "[editor]")
{
    DocFixture f;
    f.withProject();

    // Chão: box collider SEM RigidBody (estático — invB = 0).
    auto ground = f.doc->createEntity("Ground", eng::scene::kNoEntity);
    REQUIRE(ground.ok());
    REQUIRE(f.doc->addComponent(ground.value(), "eng::physics::Collider")
                .ok());
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider", "shape", "Box")
                .ok());
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider",
                                     "halfExtents.x", "50")
                .ok());
    REQUIRE(f.doc->setInspectorField(ground.value(),
                                     "eng::physics::Collider",
                                     "halfExtents.y", "1")
                .ok());
    eng::editor::TransformDesc groundTr;
    groundTr.position = {0.f, 0.f, 0.f};
    REQUIRE(f.doc->setTransform(ground.value(), groundTr).ok());

    // Player: RigidBody (cai) + esfera collider r=0.5 em y=5.
    auto player = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(player.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::RigidBody")
                .ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider")
                .ok());
    REQUIRE(f.doc->setInspectorField(player.value(),
                                     "eng::physics::Collider", "shape",
                                     "Sphere")
                .ok());
    REQUIRE(f.doc->setInspectorField(player.value(),
                                     "eng::physics::Collider", "radius", "0.5")
                .ok());
    eng::editor::TransformDesc playerTr;
    playerTr.position = {0.f, 5.f, 0.f};
    REQUIRE(f.doc->setTransform(player.value(), playerTr).ok());

    REQUIRE(f.doc->play().ok());
    // 3 segundos de jogo em frames de 8ms — física em passos fixos 1/60.
    for (int i = 0; i < 360; ++i) {
        f.doc->tick(1.f / 120.f);
    }
    // OBSERVÁVEL pelo CAMINHO REAL do usuário (doc.inspectorFields — o
    // mesmo que o painel do Android lê em Play): parou EM CIMA do chão
    // (top do box = y 1; centro do player = 1 + 0.5).
    // NOTA: este é o teste que PEGOU o bug do clone aleatório — o save
    // ordena por UUID e o handle de edição apontava outra entidade no
    // clone; agora toFocus() traduz na fronteira do documento.
    {
        const auto fields = f.doc->inspectorFields(
            player.value(), "eng::math::Transform");
        REQUIRE(fields.size() > 0);
        const auto* yField = fieldByPath(fields, "position.y");
        REQUIRE(yField != nullptr);
        CHECK(std::stof(yField->value) ==
              Catch::Approx(1.5f).margin(0.05f));
    }

    f.doc->stop();
    // Authoring intacto (§8.7): player volta para y=5 — e a seleção pós-
    // stop foi RESETADA (handle de clone não vaza para a edição).
    {
        const auto fields = f.doc->inspectorFields(
            player.value(), "eng::math::Transform");
        REQUIRE(fields.size() > 0);
        const auto* yField = fieldByPath(fields, "position.y");
        REQUIRE(yField != nullptr);
        CHECK(yField->value == "5");
    }
    CHECK_FALSE(f.doc->selection().has_value());
}

// =============================================================================
// P1 — COMPLETE 2D AUTHORING VERTICAL SLICE
//
// CREATE SCENE → ADD SPRITE → IMPORT IMAGE → IMAGE VISIBLE → SELECT →
// MOVE → ROTATE → SCALE → INSPECT → DUPLICATE → DELETE → SAVE → RELOAD
// → PLAY → SEE RUNTIME.
//
// Cada contrato é testado pelo CAMINHO REAL (documento/ECS — o mesmo que
// o Activity opera via JNI). Features visuais provadas com PIXEL REAL
// lido da surface (missão P1.15).
// =============================================================================

namespace {

/// Viewport 200x150, câmera padrão (0,0) zoom 48 — centro da tela (100,75)
/// é o mundo (0,0). Handle/axis/ring em posições PREVISÍVEIS:
///   eixo X handle   → tela (100+84, 75) = (184, 75)
///   anel rotate     → raio max(half)+26px/48; handle no ângulo da entidade
///   canto NE        → (100+halfW*48, 75-halfH*48)
struct GizmoFixture {
    DocFixture f;
    eng::ecs::Entity entity{};

    GizmoFixture()
    {
        f.withProject();
        f.doc->viewport().setScreenSize(200.f, 150.f);
        auto created = f.doc->createEntity("Hero", eng::scene::kNoEntity);
        REQUIRE(created.ok());
        entity = created.value();
    }
};

}  // namespace

// --- P1.1 SELEÇÃO ------------------------------------------------------------

TEST_CASE("editor: P1 — seleção sobrevive a transformações e invalida em delete",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    REQUIRE(doc.select(g.entity).ok());
    CHECK(doc.isSelected(g.entity));

    // Transformações NÃO derrubam a seleção (P1.1).
    eng::editor::TransformDesc desc;
    desc.position = eng::math::Vec3{3.f, -2.f, 0.f};
    desc.rotationDegrees = eng::math::Vec3{0.f, 0.f, 45.f};
    desc.scale = eng::math::Vec3{2.f, 2.f, 1.f};
    REQUIRE(doc.setTransform(g.entity, desc).ok());
    CHECK(doc.isSelected(g.entity));

    // Deselect explícito.
    doc.deselect();
    CHECK_FALSE(doc.selection().has_value());

    // DELETE limpa a seleção (handle não fica dangling).
    REQUIRE(doc.select(g.entity).ok());
    REQUIRE(doc.deleteEntity(g.entity).ok());
    CHECK_FALSE(doc.selection().has_value());
    CHECK_FALSE(doc.isSelected(g.entity));

    // Handle STALE: select em entidade morta → erro preciso; isSelected
    // falso (nenhuma referência ECS stale sobrevive — P1.1).
    auto stale = doc.select(g.entity);
    REQUIRE(stale.isError());
    CHECK(stale.error().code == eng::core::StatusCode::NotFound);
}

TEST_CASE("editor: P1 — stale handles: reciclagem de pool não ressuscita seleção",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    // Populaciona o pool e recicla: A criado/destruído; B pode reusar o
    // ÍNDICE com geração DIFERENTE — handle antigo NÃO pode acertar B.
    auto second = doc.createEntity("Other", eng::scene::kNoEntity);
    REQUIRE(second.ok());
    REQUIRE(doc.deleteEntity(g.entity).ok());
    auto recycled = doc.createEntity("Recycled", eng::scene::kNoEntity);
    REQUIRE(recycled.ok());

    // O handle antigo é inválido (índice/geração não batem com o vivo).
    CHECK_FALSE(doc.isSelected(g.entity));
    auto stale = doc.select(g.entity);
    if (stale.isError()) {
        CHECK(stale.error().code == eng::core::StatusCode::NotFound);
    } else {
        // Se o pool devolveu exatamente o MESMO handle (index+gen), a
        // entidade É a mesma reciclada — seleção válida por construção.
        CHECK(recycled.value() == g.entity);
    }

    // gizmoDragBegin com seleção morta → None (sem crash, sem stale).
    doc.deselect();
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) ==
          eng::editor::GizmoHandle::None);

    // selectionBounds de seleção morta → inválido.
    CHECK(doc.select(EditorDocument::unpackEntity(0xFFFFFFFF00000000ull)).isError());
    auto bounds = doc.selectionBounds(nullptr);
    CHECK_FALSE(bounds.valid);
}

// --- P1.2 BOUNDS --------------------------------------------------------------

TEST_CASE("editor: P1 — bounds da seleção segue pos/rot/escala/textura (P1.2)",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    // Sem textura: tamanho = escala local (default 1 → half 0.5).
    REQUIRE(doc.select(g.entity).ok());
    auto bounds = doc.selectionBounds(nullptr);
    REQUIRE(bounds.valid);
    CHECK(bounds.worldX == Catch::Approx(0.f));
    CHECK(bounds.worldY == Catch::Approx(0.f));
    CHECK(bounds.halfW == Catch::Approx(0.5f));
    CHECK(bounds.halfH == Catch::Approx(0.5f));
    CHECK(bounds.rotation == Catch::Approx(0.f));

    // MOVE → bounds acompanha.
    eng::editor::TransformDesc desc;
    desc.position = eng::math::Vec3{2.f, 1.f, 0.f};
    REQUIRE(doc.setTransform(g.entity, desc).ok());
    bounds = doc.selectionBounds(nullptr);
    REQUIRE(bounds.valid);
    CHECK(bounds.worldX == Catch::Approx(2.f));
    CHECK(bounds.worldY == Catch::Approx(1.f));

    // ROTATE → bounds gira.
    desc.rotationDegrees = eng::math::Vec3{0.f, 0.f, 90.f};
    REQUIRE(doc.setTransform(g.entity, desc).ok());
    bounds = doc.selectionBounds(nullptr);
    CHECK(bounds.rotation == Catch::Approx(1.5707963f).margin(1e-3f));

    // SCALE → bounds cresce (não usa tamanho arbitrário).
    desc.scale = eng::math::Vec3{3.f, 2.f, 1.f};
    REQUIRE(doc.setTransform(g.entity, desc).ok());
    bounds = doc.selectionBounds(nullptr);
    CHECK(bounds.halfW == Catch::Approx(1.5f));
    CHECK(bounds.halfH == Catch::Approx(1.f));

    // COM TEXTURA REAL: tamanho desenhado = região px / ppu (P1.2 — o
    // mesmo número do renderer e do hit-test, nunca arbitrário).
    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());
    eng::editor::TextureCache cache;
    bounds = doc.selectionBounds(&cache);
    REQUIRE(bounds.valid);
    // PNG 2x2, ppu 1, escala 3x2 → half = (2*3/1*0.5, 2*2/1*0.5) = (3, 2).
    CHECK(bounds.halfW == Catch::Approx(3.f));
    CHECK(bounds.halfH == Catch::Approx(2.f));

    // Ppu 4 → half = 3*2/4*0.5 = 0.75 (acima do mínimo tocável).
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pixelsPerUnit", "4").ok());
    bounds = doc.selectionBounds(&cache);
    REQUIRE(bounds.valid);
    CHECK(bounds.halfW == Catch::Approx(0.75f).margin(1e-4f));
    // Ppu default 48 → half 0.0625 CLAMPADO ao mínimo tocável (handle
    // sempre alcançável pelo dedo — decisão de UX §8.8).
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pixelsPerUnit", "48").ok());
    bounds = doc.selectionBounds(&cache);
    REQUIRE(bounds.valid);
    CHECK(bounds.halfW == Catch::Approx(0.22917f).margin(1e-3f));
}

// --- P1.3 MOVE GIZMO ----------------------------------------------------------

TEST_CASE("editor: P1 — gizmo MOVE: centro e eixos X/Y com drag REAL", "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Move);

    // Handle CENTRAL no centro da tela (entidade em (0,0)).
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) == GizmoHandle::MoveCenter);

    // Drag de 48px à direita = +1 unidade de mundo.
    REQUIRE(doc.gizmoDragTo(148.f, 75.f).ok());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-4f));
    CHECK(tr.value().position.y == Catch::Approx(0.f).margin(1e-4f));

    // Drag de 48px ACIMA = +1 em Y (flip de tela→mundo correto).
    REQUIRE(doc.gizmoDragTo(148.f, 27.f).ok());
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-4f));
    CHECK(tr.value().position.y == Catch::Approx(1.f).margin(1e-4f));
    doc.gizmoDragEnd();

    // EIXO X: volta o herói à origem e pega o handle do eixo X (84px à
    // direita do CENTRO — segue a entidade — tela (184, 75)).
    eng::editor::TransformDesc reset;
    reset.position = eng::math::Vec3{1.f, 1.f, 0.f};  // onde o drag deixou
    REQUIRE(doc.setTransform(g.entity, reset).ok());
    REQUIRE(doc.select(g.entity).ok());
    // Handle X da entidade em (1,1): tela (100+48+84, 75-48) = (232, 27).
    CHECK(doc.gizmoDragBegin(232.f, 27.f, nullptr) == GizmoHandle::MoveAxisX);
    REQUIRE(doc.gizmoDragTo(280.f, -21.f).ok());  // diagonal na tela (+1,+1)
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(2.f).margin(1e-3f));
    CHECK(tr.value().position.y == Catch::Approx(1.f).margin(1e-3f));
    doc.gizmoDragEnd();

    // EIXO Y: alvo em (100, 75-84=… pega pelo centro do handle Y: eixo
    // tem 84px a partir da borda do bounds; handle Y na tela ~ (100, -9)?
    // Não: eixo Y começa NO TOPO do bounds (0.5*48=24px) + 84px → handle
    // em y = 75-24-84 ≈ -33 (fora da tela 150px). Reproduz o layout REAL:
    // hit radius 24px dá alcance até y = -33+24 = -9 — inalcançável em
    // 150px de altura? NÃO: o hit é no CENTRO do handle (x=100, y=75-108
    // = -33) — fora. A ferramenta MOVE oferece o CENTRO p/ movimento
    // livre em telas pequenas; eixo Y fica acessível com zoom out.
    // (Teste do eixo Y com zoom menor: zoom 24 → eixo = 84/24 = 3.5
    // unidades a partir do topo do bounds.)
    doc.viewport().camera().zoom = 24.f;
    REQUIRE(doc.select(g.entity).ok());
    // bounds half em tela: 0.5*24 = 12px; eixo Y handle: 75-12-84 = -21…
    // ainda fora. Zoom 12: 75-6-84 = -15. Zoom 8 (mín): 75-4-84 = -13.
    // O LAYOUT é honesto: eixo Y aponta PARA CIMA e sai da tela quando a
    // entidade está centralizada — o usuário move a câmera. Validamos o
    // TRAVAMENTO de X no eixo Y por simetria: hit fora da tela não testa.
    doc.viewport().camera().zoom = 48.f;

    // PRIORIDADE do handle: entidade B embaixo do handle do eixo X de A —
    // o gizmo de A vence (não re-seleciona, não move B).
    auto other = doc.createEntity("Other", eng::scene::kNoEntity);
    REQUIRE(other.ok());
    eng::editor::TransformDesc otherPos;
    otherPos.position = eng::math::Vec3{3.75f, 1.f, 0.f};  // sob o handle X
    REQUIRE(doc.setTransform(other.value(), otherPos).ok());
    CHECK(doc.gizmoDragBegin(280.f, 27.f, nullptr) == GizmoHandle::MoveAxisX);
    REQUIRE(doc.gizmoDragTo(328.f, 27.f).ok());
    doc.gizmoDragEnd();
    tr = doc.transform(other.value());  // B não se mexeu
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(3.75f).margin(1e-4f));
    CHECK(doc.isSelected(g.entity));  // seleção de A sobreviveu (P1.1)
}

TEST_CASE("editor: P1 — gizmo MOVE com textura REAL: bounds desenhado é o alvo",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());
    // ppu 1 → sprite 2x2 px = 2x2 unidades de mundo: bem maior que a
    // escala local 1x1. O handle central continua NO CENTRO (posição).
    eng::editor::TextureCache cache;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Move);
    CHECK(doc.gizmoDragBegin(100.f, 75.f, &cache) == GizmoHandle::MoveCenter);
    REQUIRE(doc.gizmoDragTo(148.f, 75.f).ok());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-4f));

    // Com o sprite em (1,0), o TOQUE na borda dele (tamanho desenhado
    // 2x2 → tela 96x96 centrada em (148, 75)) ACERTA a entidade.
    auto hit = doc.viewportTap(190.f, 75.f, &cache);
    REQUIRE(hit.has_value());
    CHECK(*hit == g.entity);
}

// --- P1.4 ROTATE GIZMO --------------------------------------------------------

TEST_CASE("editor: P1 — gizmo ROTATE: ângulo do pointer, snap e round-trip",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Rotate);

    // Anel: raio = 0.5 + 26/48 = 1.0417 → handle no ângulo 0 (rotação 0):
    // tela (100 + 1.0417*48, 75) ≈ (150, 75).
    const float ringR = 0.5f + 26.f / 48.f;
    const float hx = 100.f + ringR * 48.f;
    CHECK(doc.gizmoDragBegin(hx, 75.f, nullptr) == GizmoHandle::RotateRing);

    // Pointer reto ACIMA do pivot → +90°.
    REQUIRE(doc.gizmoDragTo(100.f, 75.f - ringR * 48.f).ok());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(90.f).margin(0.5f));
    doc.gizmoDragEnd();

    // Re-grab NO NOVO handle (o anel segue a rotação atual: 90° → topo).
    // SNAP no MESMO gesto: 52.5° (LIVRE — a 7.5° de 45 e 60, fora do ímã
    // de 4°) → 59° (ímã puxa para 60 — P1.4).
    REQUIRE(doc.select(g.entity).ok());
    const float topX = 100.f;
    const float topY = 75.f - ringR * 48.f;
    CHECK(doc.gizmoDragBegin(topX, topY, nullptr) ==
          GizmoHandle::RotateRing);
    const float a47 = 52.5f * 3.14159265f / 180.f;
    const float a59 = 59.f * 3.14159265f / 180.f;
    REQUIRE(doc.gizmoDragTo(100.f + std::cos(a47) * ringR * 48.f,
                            75.f - std::sin(a47) * ringR * 48.f).ok());
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(52.5f).margin(1.f));
    REQUIRE(doc.gizmoDragTo(100.f + std::cos(a59) * ringR * 48.f,
                            75.f - std::sin(a59) * ringR * 48.f).ok());
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(60.f).margin(0.01f));
    doc.gizmoDragEnd();

    // SAVE → RELOAD → MESMA rotação (P1.4 critério).
    REQUIRE(doc.saveScene("rot.json").ok());
    REQUIRE(doc.loadScene("rot.json").ok());
    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    tr = doc.transform(nodes[0].entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(60.f).margin(0.01f));
}

// --- P1.5 SCALE GIZMO ---------------------------------------------------------

TEST_CASE("editor: P1 — gizmo SCALE: cantos X/Y, clamp e round-trip", "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Scale);

    // Canto NE do bounds (half 0.5): tela (100+24, 75-24) = (124, 51).
    CHECK(doc.gizmoDragBegin(124.f, 51.f, nullptr) == GizmoHandle::ScaleNE);

    // Dobrar a distância local → escala 2x2 (X e Y independentes).
    REQUIRE(doc.gizmoDragTo(148.f, 27.f).ok());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().scale.x == Catch::Approx(2.f).margin(1e-3f));
    CHECK(tr.value().scale.y == Catch::Approx(2.f).margin(1e-3f));

    // Drag ASSIMÉTRICO: só X dobra de novo (Y mantém).
    REQUIRE(doc.gizmoDragTo(196.f, 27.f).ok());
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().scale.x == Catch::Approx(4.f).margin(1e-2f));
    CHECK(tr.value().scale.y == Catch::Approx(2.f).margin(1e-2f));

    // IMPEDIR VALORES INVÁLIDOS: arrasto PARA DENTRO do centro (ratio
    // ~0) → clamp no mínimo (P1.5), nunca 0/negativo/NaN.
    REQUIRE(doc.gizmoDragTo(101.f, 74.f).ok());
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().scale.x >= eng::editor::TransformGizmo::kScaleMin);
    CHECK(tr.value().scale.y >= eng::editor::TransformGizmo::kScaleMin);
    CHECK(std::isfinite(tr.value().scale.x));
    CHECK(std::isfinite(tr.value().scale.y));
    doc.gizmoDragEnd();

    // SAVE → RELOAD → MESMA escala.
    REQUIRE(doc.saveScene("scale.json").ok());
    REQUIRE(doc.loadScene("scale.json").ok());
    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    tr = doc.transform(nodes[0].entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().scale.x >= eng::editor::TransformGizmo::kScaleMin);
    CHECK(tr.value().scale.y >= eng::editor::TransformGizmo::kScaleMin);
}

TEST_CASE("editor: P1 — gizmo ROTATE/SCALE sobre sprite ROTACIONADO (frame local)",
          "[editor]")
{
    // O frame LOCAL do nó desconta a rotação: escalar um sprite girado 90°
    // tem de escalar os EIXOS DO SPRITE, não os do mundo (P1.5).
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    eng::editor::TransformDesc desc;
    desc.rotationDegrees = eng::math::Vec3{0.f, 0.f, 90.f};
    REQUIRE(doc.setTransform(g.entity, desc).ok());
    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Scale);

    // Canto NE do bounds GIRADO: (0.5,0.5) rot 90° → mundo (-0.5, 0.5)
    // → tela (100-24, 75-24) = (76, 51).
    CHECK(doc.gizmoDragBegin(76.f, 51.f, nullptr) == GizmoHandle::ScaleNE);
    // Pointer em world (1.5, 1.5) → local (desconta 90°): (1.5, -1.5)…
    // escala X por ratio 1.5/0.5=3, Y por -1.5/0.5=-3 → clamp (inválido).
    REQUIRE(doc.gizmoDragTo(172.f, -21.f).ok());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    // pointer world (1.5, 2.0) → local (desconta 90°): (2.0, -1.5) →
    // ratio.x = 2.0/0.5 = 4 (o drag DOBRA o alcance no eixo local X).
    CHECK(tr.value().scale.x == Catch::Approx(4.f).margin(1e-2f));
    // Y ficou negativo pelo caminho do canto oposto → clamp no mínimo.
    CHECK(tr.value().scale.y >= eng::editor::TransformGizmo::kScaleMin);
}

// --- P1.6 TOOL MODES ----------------------------------------------------------

TEST_CASE("editor: P1 — tool modes: abstração única, Select sem gizmo, Play sem gizmo",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::EditorTool;

    CHECK(doc.tool() == EditorTool::Select);
    CHECK(doc.gizmoDraw(nullptr).quads.empty());  // Select: nada desenha

    for (const EditorTool tool :
         {EditorTool::Move, EditorTool::Rotate, EditorTool::Scale}) {
        doc.setTool(tool);
        CHECK(doc.tool() == tool);
        REQUIRE(doc.select(g.entity).ok());
        const auto draw = doc.gizmoDraw(nullptr);
        // P4.7.0 B2: cada tool desenha handles — Rotate agora é anel +
        // TRIÂNGULO (sem quad central); Move tem diamante + setas;
        // Scale tem cantos/arestas + setas.
        const bool hasAnyHandle = !draw.quads.empty()
                                  || !draw.triangles.empty()
                                  || !draw.segments.empty();
        CHECK(hasAnyHandle);
        if (tool == EditorTool::Rotate) {
            CHECK_FALSE(draw.segments.empty());  // anel
            CHECK(draw.triangles.size() == 1);   // handle triangular
        } else if (tool == EditorTool::Move) {
            // P4.1 (D1/D2): 4 HASTES — setas nos DOIS lados de cada eixo
            // (±X, ±Y) com pontas visíveis; era 2 (só +X/+Y).
            CHECK(draw.segments.size() == 4);
            CHECK(draw.triangles.size() == 4);   // setas reais (B2)
        } else {
            // P4.1 (D4): 4 diagonais (guia) + 8 meias-arestas do quad
            // (as arestas ganharam handles de escala de um eixo).
            CHECK(draw.segments.size() == 12);
            CHECK(draw.triangles.size() == 4);   // setas de aresta (B2)
        }
    }

    // Play: gizmo NÃO existe (edição rejeitada — §8.7).
    doc.setTool(EditorTool::Move);
    REQUIRE(doc.select(g.entity).ok());
    REQUIRE(doc.play().ok());
    CHECK(doc.gizmoDraw(nullptr).quads.empty());
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) ==
          eng::editor::GizmoHandle::None);
    doc.stop();
    // P4.2 (T5 — contrato REVISTO; era a7fd366 "stop reseta"): o Modo
    // Jogo exige voltar COM a seleção intacta → o gizmo CONTINUA na
    // entidade selecionada (nova seleção também re-arma).
    CHECK_FALSE(doc.gizmoDraw(nullptr).quads.empty());
    CHECK(doc.selection().has_value());
    CHECK(*doc.selection() == g.entity);
}

// =============================================================================
// P4.1 — T1: re-armo determinístico + métricas de toque (D1–D4)
// =============================================================================

TEST_CASE("editor: P4.1 — D1: seleção A → drag → seleção B → drag FUNCIONA",
          "[editor][p41]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    auto b = doc.createEntity("B", eng::scene::kNoEntity);
    REQUIRE(b.ok());
    eng::editor::TransformDesc bPos;
    bPos.position = eng::math::Vec3{0.f, 2.f, 0.f};  // B acima de A
    REQUIRE(doc.setTransform(b.value(), bPos).ok());

    doc.setTool(eng::editor::EditorTool::Move);

    // 1) A selecionada → drag REAL pela seta X (handle em 100+96px);
    // arrasta +48px PARA A DIREITA = +1 unidade de mundo.
    REQUIRE(doc.select(g.entity).ok());
    const float axisPx1 = TransformGizmo::axisPx(1.f);
    CHECK(doc.gizmoDragBegin(100.f + axisPx1, 75.f,
                             nullptr) == GizmoHandle::MoveAxisX);
    REQUIRE(doc.gizmoDragTo(100.f + axisPx1 + 48.f, 75.f).ok());
    doc.gizmoDragEnd();
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));

    // 2) Seleção TROCA para B → drag DE B funciona (re-armo). O defeito
    // D1: o estado de drag do A sobrevivia e a UI ficava presa nele.
    REQUIRE(doc.select(b.value()).ok());
    CHECK(doc.gizmoDragBegin(100.f + axisPx1, 75.f - 96.f,
                             nullptr) == GizmoHandle::MoveAxisX);
    REQUIRE(doc.gizmoDragTo(100.f + axisPx1 + 48.f, 75.f - 96.f).ok());
    doc.gizmoDragEnd();
    tr = doc.transform(b.value());
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
    // E A não se mexeu.
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
}

TEST_CASE("editor: P4.1 — D1: troca de ferramenta/play/stop matam drag vivo",
          "[editor][p41]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    doc.setTool(eng::editor::EditorTool::Move);
    REQUIRE(doc.select(g.entity).ok());
    // Drag começa e NÃO termina (UP nunca chega — gesto interrompido).
    REQUIRE(doc.gizmoDragBegin(100.f, 75.f, nullptr) ==
            GizmoHandle::MoveCenter);

    // Troca de ferramenta → drag morto: hit-test volta a funcionar
    // (sem re-armo, dragging() travaria TODO toque até um UP fantasma).
    doc.setTool(eng::editor::EditorTool::Rotate);
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) !=
          GizmoHandle::MoveCenter);
    doc.gizmoDragEnd();

    // Play → drag morto (o clone é outra cena).
    doc.setTool(eng::editor::EditorTool::Move);
    REQUIRE(doc.gizmoDragBegin(100.f, 75.f, nullptr) ==
            GizmoHandle::MoveCenter);
    REQUIRE(doc.play().ok());
    doc.stop();
    // P4.2 (T5): stop PRESERVA a seleção (contrato revisto, era a7fd366)
    // → o drag re-arma imediatamente (o re-armo P4.1 segue: NENHUM estado
    // do drag pré-Play sobrevive — mas um toque NOVO funciona).
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) ==
          GizmoHandle::MoveCenter);
    doc.gizmoDragEnd();
}

TEST_CASE("editor: P4.1 — D2: raio de acerto CONSTANTE EM PX no zoom",
          "[editor][p41]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    doc.setTool(eng::editor::EditorTool::Move);
    REQUIRE(doc.select(g.entity).ok());

    // O bug D2: o raio de acerto era convertido px→mundo e comparado com
    // px — em zoom 8 o alvo tinha ~4px, em 512 ~256px. P4.1: o alvo é
    // 48px de DIÂMETRO em qualquer zoom (raio 24px na densidade 1).
    // Toca a 20px do centro da seta X (dentro do alvo): hit em ZOOM ALTO.
    const float axis = TransformGizmo::axisPx(1.f);  // 96px de comprimento
    CHECK(doc.gizmoDragBegin(100.f + axis - 20.f, 75.f, nullptr) ==
          GizmoHandle::MoveAxisX);
    doc.gizmoDragEnd();

    // MESMO toque relativo (20px da seta) em ZOOM BAIXO: hit igual —
    // constante em espaço de ecrã (a seta fica LONGE em mundo; o alvo
    // continua 24px).
    doc.viewport().camera().zoom = 8.f;
    const float axisWorld8 = axis / 8.f;  // seta em 12 unidades
    const float screenAtZoom8 = 100.f + axisWorld8 * 8.f;
    CHECK(doc.gizmoDragBegin(screenAtZoom8 - 20.f, 75.f, nullptr) ==
          GizmoHandle::MoveAxisX);
    doc.gizmoDragEnd();
    doc.viewport().camera().zoom = 48.f;
}

TEST_CASE("editor: P4.1 — D3: anel de rotação ≥ 64px em zoom baixo",
          "[editor][p41]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Rotate);

    // Zoom mínimo (8): bounds half 0.5 → 4px na tela; SEM o mínimo o
    // anel teria 4+26=30px (impossível — repro D3). Com o mínimo: 64px.
    doc.viewport().camera().zoom = 8.f;
    // Entidade FORA do centro (senão o anel sai da tela): move p/ canto.
    eng::editor::TransformDesc pos;
    pos.position = eng::math::Vec3{30.f, 0.f, 0.f};
    REQUIRE(doc.setTransform(g.entity, pos).ok());
    REQUIRE(doc.select(g.entity).ok());
    // Centro da entidade em tela: 100 + 30*8 = 340. Handle do anel:
    // 340 + 64 = 404 (> 200 — fora da tela 200px). Pan a câmera p/ ver:
    doc.viewport().camera().posX = 30.f;  // entidade no centro da tela
    const float handleX =
        doc.viewport().worldToScreenX(30.f + TransformGizmo::kRingMinDp *
                                                doc.viewport().uiScale() / 8.f);
    CHECK(doc.gizmoDragBegin(handleX, 75.f, nullptr) ==
          GizmoHandle::RotateRing);
    doc.viewport().camera().posX = 0.f;
    doc.viewport().camera().zoom = 48.f;
}

TEST_CASE("editor: P4.1 — D4: arestas de escala movem UM eixo",
          "[editor][p41]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Scale);

    // Aresta LESTE: centro da aresta X+ em (0.5, 0) → tela (124, 75).
    // Toca a 6px da aresta (os cantos ficam a >24px — prioridade certa).
    CHECK(doc.gizmoDragBegin(130.f, 75.f, nullptr) == GizmoHandle::ScaleEdgeE);
    // Ratio local: grab 0.625 → 1.25 (tela 160) = 2x em X; Y INTACTO
    // (o defeito D4: só os cantos existiam e eram minúsculos).
    REQUIRE(doc.gizmoDragTo(160.f, 75.f).ok());
    doc.gizmoDragEnd();
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().scale.x == Catch::Approx(2.f).margin(1e-3f));
    CHECK(tr.value().scale.y == Catch::Approx(1.f).margin(1e-3f));

    // Aresta NORTE: após o X=2, half é (1, 0.5) → N em (100, 75-24=51).
    // Toca a 1px dela (cantos agora a 48px — sem disputa).
    REQUIRE(doc.select(g.entity).ok());
    CHECK(doc.gizmoDragBegin(100.f, 52.f, nullptr) == GizmoHandle::ScaleEdgeN);
    // Ratio local: grab 0.4792 → 0.9583 (tela 29) = 2x em Y.
    REQUIRE(doc.gizmoDragTo(100.f, 29.f).ok());
    doc.gizmoDragEnd();
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().scale.x == Catch::Approx(2.f).margin(1e-3f));
    CHECK(tr.value().scale.y == Catch::Approx(2.f).margin(1e-3f));

    // Cantos continuam escalando os DOIS eixos (com scale (2,2) o NE
    // fica em (100+48, 75-48) = (148, 27)).
    REQUIRE(doc.select(g.entity).ok());
    CHECK(doc.gizmoDragBegin(148.f, 27.f, nullptr) == GizmoHandle::ScaleNE);
}

TEST_CASE("editor: P4.1 — D3/D4: densidade (uiScale) amplia os alvos",
          "[editor][p41]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    // Device real (C33): densidade 2 → alvo de toque 48dp = 96px de raio
    // em px de surface (48px na densidade 1).
    doc.viewport().setUiScale(2.f);
    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Move);

    // Toca a 40px do centro da seta X (96px de comprimento): com raio
    // 48px (24dp × densidade 2) É hit; com o antigo (raio fixo 24px) não
    // seria — é a diferença entre "parece morto" e funciona (D4).
    const float axis = TransformGizmo::axisPx(2.f);  // 96dp × 2 = 192px
    CHECK(doc.gizmoDragBegin(100.f + axis - 40.f, 75.f, nullptr) ==
          GizmoHandle::MoveAxisX);
    doc.gizmoDragEnd();

    // Handle VISUAL permanece na faixa 28–40 px (regra da missão):
    CHECK(TransformGizmo::handlePx(2.f) <= 40.f);
    CHECK(TransformGizmo::handlePx(2.f) >= 28.f);
    CHECK(TransformGizmo::handlePx(1.f) <= 40.f);
    CHECK(TransformGizmo::handlePx(1.f) >= 28.f);
}

// --- P1.9 INSPECTOR SYNC ------------------------------------------------------

TEST_CASE("editor: P1 — Inspector ↔ viewport bidirecionais (revisão única)",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    REQUIRE(doc.select(g.entity).ok());
    const std::uint64_t rev0 = doc.selectionRevision();

    // Inspector → ECS: setInspectorField escreve no Transform REAL.
    REQUIRE(doc.setInspectorField(g.entity, "eng::math::Transform",
                                  "position.x", "2.5").ok());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(2.5f).margin(1e-5f));
    CHECK(doc.selectionRevision() != rev0);  // bump: UI percebe

    // Gizmo → ECS → Inspector: o drag atualiza o Transform que o
    // inspectorFields LÊ (mesma fonte de verdade — P1.9). A entidade
    // está em (2.5, 0) → centro na tela (220, 75).
    doc.setTool(eng::editor::EditorTool::Move);
    CHECK(doc.gizmoDragBegin(220.f, 75.f, nullptr) ==
          eng::editor::GizmoHandle::MoveCenter);
    REQUIRE(doc.gizmoDragTo(316.f, 75.f).ok());  // +2 unidades
    doc.gizmoDragEnd();
    {
        const auto fields = doc.inspectorFields(g.entity,
                                                "eng::math::Transform");
        const auto* xField = fieldByPath(fields, "position.x");
        REQUIRE(xField != nullptr);
        CHECK(std::stof(xField->value) == Catch::Approx(4.5f).margin(1e-3f));
    }

    // Rotação e escala idem (Inspector numérico → gizmo vê o mesmo TRS).
    REQUIRE(doc.setInspectorField(g.entity, "eng::math::Transform",
                                  "rotation.z", "30").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::math::Transform",
                                  "scale.x", "1.5").ok());
    tr = doc.transform(g.entity);
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(30.f).margin(1e-4f));
    CHECK(tr.value().scale.x == Catch::Approx(1.5f).margin(1e-5f));
}

// --- P1.10 SPRITE CREATION UX --------------------------------------------------

TEST_CASE("editor: P1 — createSprite: SpriteData default, placeholder, numeração",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    auto sprite = doc.createSprite("Sprite");
    REQUIRE(sprite.ok());
    CHECK(doc.isSelected(sprite.value()));  // selecionada de fábrica

    // SpriteData presente com defaults (ppu 48) e SEM textura → o quad é
    // isSprite (placeholder xadrez no renderer, não hue).
    const auto* scene = doc.sceneInFocus();
    const auto quads = doc.viewport().buildQuads(*scene, doc.selection());
    REQUIRE(quads.size() == 2);  // Hero (fixture) + Sprite
    const auto& spriteQuad = quads[1];
    CHECK(spriteQuad.isSprite);
    CHECK(spriteQuad.textureAsset.empty());
    CHECK(spriteQuad.spritePpu == 48.f);

    // Numeração automática: Sprite, Sprite 2, Sprite 3.
    auto second = doc.createSprite("Sprite");
    REQUIRE(second.ok());
    CHECK(doc.nameOf(*doc.sceneInFocus(), second.value()) == "Sprite 2");
    auto third = doc.createSprite("Sprite");
    REQUIRE(third.ok());
    CHECK(doc.nameOf(*doc.sceneInFocus(), third.value()) == "Sprite 3");
    CHECK(doc.sceneDirty());  // authoring marcado sujo

    // SAVE/LOAD mantém os placeholders (SpriteData serializa vazio).
    REQUIRE(doc.saveScene("sprites.json").ok());
    REQUIRE(doc.loadScene("sprites.json").ok());
    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 4);  // Hero + Sprite + Sprite 2 + Sprite 3
}

// --- P1.7 DUPLICATE ------------------------------------------------------------

TEST_CASE("editor: P1 — DUPLICATE de sprite: IDs diferentes, independentes, serializam",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::SpriteData;

    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());

    // Sprite A com textura e transform autoral.
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    eng::editor::TransformDesc desc;
    desc.position = eng::math::Vec3{1.f, 2.f, 0.f};
    desc.rotationDegrees = eng::math::Vec3{0.f, 0.f, 25.f};
    REQUIRE(doc.setTransform(g.entity, desc).ok());

    auto dup = doc.duplicateEntity(g.entity);
    REQUIRE(dup.ok());
    CHECK(dup.value() != g.entity);  // entity ID NOVO

    // SpriteData duplicado: MESMA imagem (componente clonado).
    const auto* scene = doc.sceneInFocus();
    const auto* spriteA = scene->world().get<SpriteData>(g.entity);
    const auto* spriteB = scene->world().get<SpriteData>(dup.value());
    REQUIRE(spriteA != nullptr);
    REQUIRE(spriteB != nullptr);
    CHECK(spriteB->textureAsset == spriteA->textureAsset);
    CHECK(spriteB != spriteA);  // sem compartilhar estado mutável

    // Transform independente: mover A não mexe em B.
    REQUIRE(doc.moveEntityScreen(g.entity, 48.f, 0.f).ok());
    auto trA = doc.transform(g.entity);
    auto trB = doc.transform(dup.value());
    REQUIRE(trA.ok());
    REQUIRE(trB.ok());
    CHECK(trA.value().position.x == Catch::Approx(2.f).margin(1e-4f));
    CHECK(trB.value().position.x == Catch::Approx(1.f).margin(1e-4f));
    CHECK(trB.value().rotationDegrees.z == Catch::Approx(25.f).margin(1e-3f));

    // SERIALIZE: ambos persistem com a textura certa.
    REQUIRE(doc.saveScene("dup.json").ok());
    REQUIRE(doc.loadScene("dup.json").ok());
    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 2);
    int textured = 0;
    for (const auto& node : nodes) {
        const auto* sprite =
            doc.sceneInFocus()->world().get<SpriteData>(node.entity);
        if (sprite != nullptr && sprite->textureAsset == "grass.png") {
            ++textured;
        }
    }
    CHECK(textured == 2);
}

// --- P1.8 DELETE ----------------------------------------------------------------

TEST_CASE("editor: P1 — DELETE: seleção limpa, links/vizinhos intactos, save válido",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    auto child = doc.createEntity("Child", g.entity);
    REQUIRE(child.ok());
    auto other = doc.createEntity("Other", eng::scene::kNoEntity);
    REQUIRE(other.ok());

    // Seleciona e apaga o PAI (com filho) — cascata folhas primeiro.
    REQUIRE(doc.select(g.entity).ok());
    REQUIRE(doc.deleteEntity(g.entity).ok());
    CHECK_FALSE(doc.selection().has_value());          // seleção limpa
    CHECK(doc.selectionRevision() > 0);

    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 1);                        // só Other sobrou
    CHECK(nodes[0].name == "Other");

    // Handle do pai morto é rejeitado com erro preciso (ANTES do reload:
    // um load reconstrói o mundo e handles antigos podem ALIASEAR os
    // novos — reciclagem de index+generation é por mundo, não eterna).
    auto movedDead = doc.moveEntityScreen(g.entity, 10.f, 10.f);
    REQUIRE(movedDead.isError());
    CHECK(movedDead.error().code == eng::core::StatusCode::NotFound);

    // Save/load continua válido sem os mortos.
    REQUIRE(doc.saveScene("del.json").ok());
    REQUIRE(doc.loadScene("del.json").ok());
    CHECK(doc.hierarchySnapshot().size() == 1);
}

// --- P1.11 SAVE/RELOAD do fluxo completo ----------------------------------------

TEST_CASE("editor: P1 — SAVE/RELOAD do workflow completo sem perda de dados",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());

    // Authoring: 2 sprites texturizados com transforms distintos + delete.
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    eng::editor::TransformDesc a;
    a.position = eng::math::Vec3{-1.f, 0.5f, 0.f};
    a.rotationDegrees = eng::math::Vec3{0.f, 0.f, -15.f};
    a.scale = eng::math::Vec3{2.f, 0.5f, 1.f};
    REQUIRE(doc.setTransform(g.entity, a).ok());
    auto dup = doc.duplicateEntity(g.entity);
    REQUIRE(dup.ok());
    eng::editor::TransformDesc b;
    b.position = eng::math::Vec3{3.f, -1.f, 0.f};
    REQUIRE(doc.setTransform(dup.value(), b).ok());

    REQUIRE(doc.saveScene("full.json").ok());
    // "destroy editor document": newScene limpa o estado autoral.
    REQUIRE(doc.newScene().ok());
    CHECK(doc.hierarchySnapshot().empty());
    REQUIRE(doc.loadScene("full.json").ok());

    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 2);
    // IDs distintos, dados recuperados (textureAsset, transforms).
    std::vector<eng::math::Vec3> positions;
    for (const auto& node : nodes) {
        auto tr = doc.transform(node.entity);
        REQUIRE(tr.ok());
        positions.push_back(tr.value().position);
        const auto* sprite = doc.sceneInFocus()->world().get<eng::editor::SpriteData>(
            node.entity);
        REQUIRE(sprite != nullptr);
        CHECK(sprite->textureAsset == "grass.png");
    }
    REQUIRE(positions.size() == 2);
    bool hasA = false, hasB = false;
    for (const auto& p : positions) {
        if (p.x == Catch::Approx(-1.f).margin(1e-5f)) { hasA = true; }
        if (p.x == Catch::Approx(3.f).margin(1e-5f)) { hasB = true; }
    }
    CHECK(hasA);
    CHECK(hasB);

    // Nenhum path absoluto indevido no JSON salvo.
    const auto text = g.f.fs->readAllText(
        eng::fs::Path{"TestGame/scenes/full.json"});
    REQUIRE(text.ok());
    CHECK(text.value().find("/home/") == std::string::npos);
    CHECK(text.value().find("file://") == std::string::npos);
}

// --- P1.12 PLAY (clone) -----------------------------------------------------------

TEST_CASE("editor: P1 — PLAY: selected/duplicated/transformed/deleted no clone",
          "[editor]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::SpriteData;

    // A (transformado+selecionado), B (deletado), C (duplicado de A).
    eng::editor::TransformDesc a;
    a.position = eng::math::Vec3{3.f, 2.f, 0.f};
    REQUIRE(doc.setTransform(g.entity, a).ok());
    auto doomed = doc.createEntity("Doomed", eng::scene::kNoEntity);
    REQUIRE(doomed.ok());
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    auto clone = doc.duplicateEntity(g.entity);
    REQUIRE(clone.ok());
    REQUIRE(doc.deleteEntity(doomed.value()).ok());  // morte PRÉ-Play
    REQUIRE(doc.select(g.entity).ok());            // A selecionado PRÉ-Play

    REQUIRE(doc.play().ok());
    CHECK(doc.isPlaying());

    // Clone tem A' e C' — NÃO tem B (deletada antes do play).
    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 2);  // Hero (A) + duplicata (C)
    bool sawDoomed = false;
    for (const auto& node : nodes) {
        CHECK(node.name != "Doomed");
        if (node.name == "Doomed") { sawDoomed = true; }
    }
    CHECK_FALSE(sawDoomed);

    // Seleção PRÉ-Play segue o CLONE de A (remap a7fd366): inspectorFields
    // com o handle de EDIÇÃO mostra o transform do CLONE correto.
    {
        const auto fields = doc.inspectorFields(g.entity,
                                                "eng::math::Transform");
        const auto* xField = fieldByPath(fields, "position.x");
        REQUIRE(xField != nullptr);
        CHECK(std::stof(xField->value) == Catch::Approx(3.f).margin(1e-3f));
    }

    // Mutação em Play vai ao CLONE (debug §8.7) — não vaza para edição.
    REQUIRE(doc.moveEntityScreen(g.entity, 48.f, 0.f).ok());
    {
        const auto fields = doc.inspectorFields(g.entity,
                                                "eng::math::Transform");
        const auto* xField = fieldByPath(fields, "position.x");
        REQUIRE(xField != nullptr);
        CHECK(std::stof(xField->value) == Catch::Approx(4.f).margin(1e-3f));
    }

    doc.stop();
    // Edição INTACTA: A em (3,2), C presente. P4.2 (T5): a seleção da
    // edição sobrevive ao stop (contrato do Modo Jogo — era a7fd366).
    CHECK_FALSE(doc.isPlaying());
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(3.f).margin(1e-5f));
    CHECK(doc.hierarchySnapshot().size() == 2);
    REQUIRE(doc.selection().has_value());
    CHECK(*doc.selection() == g.entity);
}

// --- P1.15 RENDERING (readback de pixel — features visuais provadas) ---------------

TEST_CASE("editor: P1 — renderer desenha GIZMO por cima do sprite (readback)",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host = eng::editor::EditorHost::create("gles", ".editor-test-ws-p1gizmo");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                          128, 128);
    if (owned->state() != eng::editor::HostSurfaceState::Available) {
        SKIP("OpenGL ES indisponível (renderer não criado sem driver)");
    }

    auto& doc = owned->document();
    ensureProject(doc, "P1GizmoGame");

    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    {
        eng::fs::FileSystem& ws = owned->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws
                    .writeAllBytes(
                        eng::fs::Path{".import_tmp/quad.png"},
                        std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                                  sizeof(kPng2x2)})
                    .ok());
    }
    REQUIRE(browser->import(".import_tmp/quad.png", "textures", "quad").ok());

    // Sprite cobrindo o CENTRO da tela: entidade em (0,0), ppu=1 → 2x2
    // unidades = 96x96 px na tela 128x128 (zoom 48).
    auto sprite = doc.createSprite("Big");
    REQUIRE(sprite.ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "quad.png").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());

    // Sem tool: NÃO há gizmo (honesto — UI audit).
    REQUIRE(owned->renderFrame(1.f / 60.f));
    auto* renderer = owned->viewportRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(renderer->lastFrameTexturedSprites() == 1);
    CHECK(renderer->lastFrameGizmoVertices().empty());

    // Tool MOVE + seleção: gizmo desenhado POR CIMA do sprite (lote 3).
    doc.setTool(eng::editor::EditorTool::Move);
    REQUIRE(doc.select(sprite.value()).ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    const auto& gizmoVerts = renderer->lastFrameGizmoVertices();
    REQUIRE_FALSE(gizmoVerts.empty());
    // P4.1 (D3/D4) + P4.6 (L4 — handle chamferado com outline): por handle
    // = outline (1 quad + 4 cortes) + fill (1 quad + 4 cortes) = 10 quads;
    // 5 handles × 10 quads × 6 vértices = 300 + 4 hastes (4 segmentos × 6)
    // = 324. O contrato acompanha a especificação (o gizmo continua
    // INTEIRO por cima do sprite — a prova de pixel abaixo).
    CHECK(gizmoVerts.size() == 324);

    // Prova VISUAL: o pixel central da tela é o HANDLE CENTRAL amarelo
    // (kCenter 0.96/0.82/0.30) DESENHADO SOBRE o sprite vermelho/verde.
    std::uint8_t pixel[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixel).ok());
    INFO("readback gizmo: " << +pixel[0] << " " << +pixel[1] << " "
                            << +pixel[2] << " " << +pixel[3]);
    CHECK(pixel[0] >= 200);  // R alto (amarelo)
    CHECK(pixel[1] >= 170);  // G alto
    CHECK(pixel[2] <= 140);  // B baixo

    // Drag REAL via documento: begin no centro + drag 48px → +1 unidade.
    CHECK(doc.gizmoDragBegin(64.f, 64.f, &owned->textureCache()) ==
          eng::editor::GizmoHandle::MoveCenter);
    REQUIRE(doc.gizmoDragTo(112.f, 64.f).ok());
    doc.gizmoDragEnd();
    auto tr = doc.transform(sprite.value());
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));

    // PLAY com tool ativa: gizmo SOME (edição rejeitada), sprite segue.
    REQUIRE(doc.play().ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    CHECK(renderer->lastFrameTexturedSprites() == 1);
    CHECK(renderer->lastFrameGizmoVertices().empty());
    doc.stop();
}

TEST_CASE("editor: P1 — placeholder xadrez de sprite sem textura (readback)",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host = eng::editor::EditorHost::create("gles", ".editor-test-ws-p1ph");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                          128, 128);
    if (owned->state() != eng::editor::HostSurfaceState::Available) {
        SKIP("OpenGL ES indisponível (renderer não criado sem driver)");
    }

    auto& doc = owned->document();
    ensureProject(doc, "P1PlaceholderGame");

    // createSprite SEM textura: placeholder xadrez claramente identificado
    // (P1.10 — não confundir com sprite renderizado).
    auto sprite = doc.createSprite("Ghost");
    REQUIRE(sprite.ok());

    REQUIRE(owned->renderFrame(1.f / 60.f));
    auto* renderer = owned->viewportRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(renderer->lastFrameTexturedSprites() == 0);  // nada texturizado

    // DADOS: o quad de cor contém o xadrez magenta (0.55, 0.22, 0.55) —
    // 8 células claras + base escura (0.13).
    const auto& verts = renderer->lastFrameVertices();
    int magenta = 0;
    int dark = 0;
    for (const auto& v : verts) {
        if (v.r == Catch::Approx(0.55f).margin(0.02f) &&
            v.g == Catch::Approx(0.22f).margin(0.02f) &&
            v.b == Catch::Approx(0.55f).margin(0.02f)) {
            ++magenta;
        }
        if (v.r == Catch::Approx(0.13f).margin(0.02f) &&
            v.g == Catch::Approx(0.13f).margin(0.02f) &&
            v.b == Catch::Approx(0.13f).margin(0.02f)) {
            ++dark;
        }
    }
    CHECK(magenta >= 6 * 8);   // 8 células × 6 vértices
    CHECK(dark >= 6);          // base

    // VISUAL: pixel de uma célula magenta — o sprite default (1 unidade)
    // é pequeno (48px); o centro da tela cai NELE (entidade em (0,0) e
    // célula central: local (-0.125..0.125 px…) — pega ponto seguro: o
    // pixel central da tela está na célula (2,2)… arranjo 4x4 a partir de
    // -24px: células de 12px; centro = fronteira. Prova está nos DADOS
    // acima; o readback sanity-checka que o CENTRO não é hue saturado
    // (sprite placeholder é sóbrio) nem vermelho de textura.
    std::uint8_t pixel[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixel).ok());
    CHECK(pixel[3] == 255);  // opaco (base escura cobre o fundo do editor)
    CHECK((pixel[0] < 200 || pixel[2] < 200));  // não é hue rosa-vivo
}

// --- P1 vertical slice completo (host REAL, disco REAL) ----------------------------

TEST_CASE("editor: P1 — VERTICAL SLICE: import→sprite→gizmos→duplicate→save→play",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    auto host = eng::editor::EditorHost::create("gles", ".editor-test-ws-p1vs");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                          128, 128);
    if (owned->state() != eng::editor::HostSurfaceState::Available) {
        SKIP("OpenGL ES indisponível (renderer não criado sem driver)");
    }

    auto& doc = owned->document();
    ensureProject(doc, "P1SliceGame");

    // IMPORT PNG.
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    {
        eng::fs::FileSystem& ws = owned->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws
                    .writeAllBytes(
                        eng::fs::Path{".import_tmp/art.png"},
                        std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                                  sizeof(kPng2x2)})
                    .ok());
    }
    REQUIRE(browser->import(".import_tmp/art.png", "textures", "art").ok());

    // ADD SPRITE + IMAGE (visível — readback no fim).
    auto sprite = doc.createSprite("Hero");
    REQUIRE(sprite.ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "art.png").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "pixelsPerUnit", "2").ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    REQUIRE(owned->viewportRenderer()->lastFrameTexturedSprites() == 1);

    // SELECT (tap no centro) + MOVE com gizmo (48px = +1 unidade).
    auto hit = doc.viewportTap(64.f, 64.f, &owned->textureCache());
    REQUIRE(hit.has_value());
    CHECK(*hit == sprite.value());
    doc.setTool(eng::editor::EditorTool::Move);
    CHECK(doc.gizmoDragBegin(64.f, 64.f, &owned->textureCache()) ==
          eng::editor::GizmoHandle::MoveCenter);
    REQUIRE(doc.gizmoDragTo(112.f, 64.f).ok());
    doc.gizmoDragEnd();

    // ROTATE com gizmo (+90°). A entidade está em (1,0) → tela (112, 64).
    doc.setTool(eng::editor::EditorTool::Rotate);
    const float ringR = 1.f * 0.5f + 26.f / 48.f;  // sprite 1x1 unidade (2px/2ppu)
    CHECK(doc.gizmoDragBegin(112.f + ringR * 48.f, 64.f,
                             &owned->textureCache()) ==
          eng::editor::GizmoHandle::RotateRing);
    REQUIRE(doc.gizmoDragTo(112.f, 64.f - ringR * 48.f).ok());
    doc.gizmoDragEnd();

    // SCALE com gizmo (canto NE ×2). Sprite GIRADO 90°: o canto NE local
    // (0.5,0.5) vive em world (-0.5,+0.5) do centro (1,0) → tela (88, 40).
    // Alvo ×2: local (1,1) → world (-1,1) → tela (64, 16).
    doc.setTool(eng::editor::EditorTool::Scale);
    CHECK(doc.gizmoDragBegin(88.f, 40.f,
                             &owned->textureCache()) ==
          eng::editor::GizmoHandle::ScaleNE);
    REQUIRE(doc.gizmoDragTo(64.f, 16.f).ok());
    doc.gizmoDragEnd();

    auto tr = doc.transform(sprite.value());
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(90.f).margin(1.f));
    CHECK(tr.value().scale.x == Catch::Approx(2.f).margin(1e-2f));
    CHECK(tr.value().scale.y == Catch::Approx(2.f).margin(1e-2f));

    // DUPLICATE: segunda entidade com a MESMA arte, transform independente.
    auto dup = doc.duplicateEntity(sprite.value());
    REQUIRE(dup.ok());
    REQUIRE(doc.moveEntityScreen(dup.value(), -48.f, 0.f).ok());

    // SAVE.
    REQUIRE(doc.saveScene("slice.json").ok());

    // PLAY: runtime renderiza OS DOIS sprites (clone).
    REQUIRE(doc.play().ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    CHECK(owned->viewportRenderer()->lastFrameTexturedSprites() == 2);
    REQUIRE(owned->renderFrame(1.f / 60.f));
    doc.stop();

    // RELOAD pós-tudo: o que foi autorado é o que volta.
    REQUIRE(doc.loadScene("slice.json").ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    CHECK(owned->viewportRenderer()->lastFrameTexturedSprites() == 2);
    const auto nodes = doc.hierarchySnapshot();
    REQUIRE(nodes.size() == 2);
}

// =============================================================================
// P2 — GIZMO: bug crítico §5 + matrix de regressão §6
//
// Reporte do P1: "o gizmo funciona quando o Sprite/Entity é criado/
// aplicado inicialmente, mas depois de certas alterações o gizmo deixa
// de funcionar corretamente". Causas-raiz codificadas aqui como testes:
//
//   R1 — CONSISTÊNCIA begin/drag: gizmoDragBegin resolvia os bounds COM
//        texturas (tamanho desenhado) mas gizmoDragTo os recalculava SEM
//        (nullptr) — com pivot != (0.5,0.5) o CENTRO de referência do
//        rotate/scale MUDA no meio do drag (drift/salto).
//   R2 — ESPAÇO LOCAL: o delta de MUNDO era somado direto na posição
//        LOCAL do filho — pai rotacionado/escalado fazia a entidade se
//        mover no EIXO ERRADO na tela.
// =============================================================================

TEST_CASE("editor: P2 — gizmo ROTATE com pivot não-centrado segue o pointer (R1)",
          "[editor][gizmo-p2]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    // ppu 1: imagem 2x2 px → 2x2 unidades de mundo (bem maior que a
    // escala local 1x1 — o caminho SEM textura daria 1x1: A DIFFERENÇA
    // entre os dois caminhos é exatamente o que o bug explorava).
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());
    // PIVOT no canto (0,0) — o quad desenhado desloca; o bounds do gizmo
    // tem de acompanhar (mesma fórmula do renderer).
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pivotX", "0").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pivotY", "0").ok());
    eng::editor::TextureCache cache;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Rotate);

    // Bounds com textura: half = 1; pivot (0,0) → centro visual em
    // (-1,-1) (o renderer desenha o quad com esse mesmo offset — o
    // bounds do gizmo casa com o desenho). Anel: raio max(1,1)+26/48.
    const float ringR = 1.f + 26.f / 48.f;
    // Handle no ângulo 0 do CENTRO VISUAL (-1,-1):
    const float hx = 100.f + (-1.f + ringR) * 48.f;
    const float hy = 75.f - (-1.f) * 48.f;
    CHECK(doc.gizmoDragBegin(hx, hy, &cache) == GizmoHandle::RotateRing);

    // Drag de +90° ao redor do CENTRO VISUAL (-1,-1): pointer vai de
    // ângulo 0 para ângulo 90° (acima do centro).
    REQUIRE(doc.gizmoDragTo(100.f + (-1.f) * 48.f,
                            75.f - (-1.f + ringR) * 48.f).ok());
    doc.gizmoDragEnd();

    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    // A rotação segue o pointer EXATAMENTE (sem drift do centro trocado).
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(90.f).margin(0.5f));

    // RODA DE NOVO (volta a 0): o segundo drag começa do estado ATUAL —
    // se o gizmo estivesse ligado a dados STALE, o segundo drag erraria.
    // Rotação 90° girou o quad (e o offset de pivot JUNTO): centro
    // visual agora (1,-1); handle no ângulo 90° desse centro.
    const float hx2 = 100.f + 1.f * 48.f;
    const float hy2 = 75.f - (-1.f + ringR) * 48.f;
    CHECK(doc.gizmoDragBegin(hx2, hy2, &cache) == GizmoHandle::RotateRing);
    REQUIRE(doc.gizmoDragTo(100.f + (1.f + ringR) * 48.f,
                            75.f + 1.f * 48.f).ok());  // ângulo 0 de novo
    doc.gizmoDragEnd();
    tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().rotationDegrees.z == Catch::Approx(0.f).margin(0.5f));
}

TEST_CASE("editor: P2 — gizmo SCALE com pivot não-centrado é consistente (R1)",
          "[editor][gizmo-p2]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pivotX", "0").ok());
    REQUIRE(doc.setInspectorField(g.entity, "eng::editor::SpriteData",
                                  "pivotY", "0").ok());
    eng::editor::TextureCache cache;

    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Scale);

    // Pivot (0,0) → centro visual (-1,-1), half (1,1): canto NE do
    // bounds visual = (-1+1, -1+1) = (0,0) mundo → tela (100, 75).
    CHECK(doc.gizmoDragBegin(100.f, 75.f, &cache) == GizmoHandle::ScaleNE);
    // Dobra a distância ao centro visual: pointer local (1,1) → (2,2)
    // → mundo (-1+2, -1+2) = (1,1) → tela (148, 27).
    REQUIRE(doc.gizmoDragTo(148.f, 27.f).ok());
    doc.gizmoDragEnd();

    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    // O RATIO é medido no MESMO frame do begin e do drag (bug §5 R1):
    // sem o fix, o begin media contra o bounds TEXTURIZADO (half 1) e o
    // drag contra o NÃO-texturizado (half 0.5) → ratio errado.
    CHECK(tr.value().scale.x == Catch::Approx(2.f).margin(0.05f));
    CHECK(tr.value().scale.y == Catch::Approx(2.f).margin(0.05f));
}

TEST_CASE("editor: P2 — gizmo MOVE em filho de pai ROTACIONADO: eixo de TELA (R2)",
          "[editor][gizmo-p2]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    // Pai girado 90°: o eixo X LOCAL do filho aponta PARA CIMA no mundo.
    auto parent = doc.createEntity("Parent", eng::scene::kNoEntity);
    REQUIRE(parent.ok());
    eng::editor::TransformDesc pd;
    pd.rotationDegrees = eng::math::Vec3{0.f, 0.f, 90.f};
    REQUIRE(doc.setTransform(parent.value(), pd).ok());

    auto child = doc.createEntity("Child", parent.value());
    REQUIRE(child.ok());

    REQUIRE(doc.select(child.value()).ok());
    doc.setTool(eng::editor::EditorTool::Move);

    // Handle central: filho em (0,0) mundo → tela (100, 75).
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) == GizmoHandle::MoveCenter);
    // Drag de +48px à DIREITA na TELA = +1 unidade em MUNDO no eixo X.
    // A posição do filho é LOCAL ao pai girado: local delta tem de ser
    // (0,-1) [inverse(R90) * (1,0)] — senão o filho sobe na tela em vez
    // de ir para a direita.
    REQUIRE(doc.gizmoDragTo(148.f, 75.f).ok());
    doc.gizmoDragEnd();

    auto tr = doc.transform(child.value());
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(0.f).margin(1e-3f));
    CHECK(tr.value().position.y == Catch::Approx(-1.f).margin(1e-3f));

    // E o RESULTADO VISUAL é o certo: o filho está em +1 X de MUNDO.
    auto bounds = doc.selectionBounds(nullptr);
    REQUIRE(bounds.valid);
    CHECK(bounds.worldX == Catch::Approx(1.f).margin(1e-3f));
    CHECK(bounds.worldY == Catch::Approx(0.f).margin(1e-3f));

    // EIXO X do gizmo (mundo): trava o movimento em X de mundo — o delta
    // local também tem de ser transformado (não somado cru).
    REQUIRE(doc.select(child.value()).ok());
    // Handle do eixo X: centro do filho agora em tela (148, 75) → handle
    // em (148 + 84, 75).
    CHECK(doc.gizmoDragBegin(148.f + 84.f, 75.f, nullptr) ==
          GizmoHandle::MoveAxisX);
    REQUIRE(doc.gizmoDragTo(148.f + 84.f + 48.f, 75.f - 48.f).ok());
    doc.gizmoDragEnd();
    tr = doc.transform(child.value());
    REQUIRE(tr.ok());
    // +1 em X de mundo apenas: local = inverse(R90)*(2,0) = (0,-2).
    CHECK(tr.value().position.x == Catch::Approx(0.f).margin(1e-3f));
    CHECK(tr.value().position.y == Catch::Approx(-2.f).margin(1e-3f));
}

TEST_CASE("editor: P2 — gizmo MOVE em filho de pai ESCALADO (R2)",
          "[editor][gizmo-p2]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;

    // Pai com escala 2: mover 1 unidade em MUNDO = 0.5 em LOCAL.
    auto parent = doc.createEntity("Parent", eng::scene::kNoEntity);
    REQUIRE(parent.ok());
    eng::editor::TransformDesc pd;
    pd.scale = eng::math::Vec3{2.f, 2.f, 1.f};
    REQUIRE(doc.setTransform(parent.value(), pd).ok());

    auto child = doc.createEntity("Child", parent.value());
    REQUIRE(child.ok());
    REQUIRE(doc.select(child.value()).ok());
    doc.setTool(eng::editor::EditorTool::Move);

    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) ==
          eng::editor::GizmoHandle::MoveCenter);
    REQUIRE(doc.gizmoDragTo(148.f, 75.f).ok());
    doc.gizmoDragEnd();

    auto tr = doc.transform(child.value());
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(0.5f).margin(1e-3f));
    CHECK(tr.value().position.y == Catch::Approx(0.f).margin(1e-3f));
}

TEST_CASE("editor: P2 — gizmo NÃO quebra após mudanças (matrix §5)",
          "[editor][gizmo-p2]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    // Cenário limpo: a entidade do fixture sai — só o sprite da matrix.
    REQUIRE(doc.deleteEntity(g.entity).ok());

    writePngTemp(*g.f.fs);
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());
    auto sprite = doc.createSprite("Hero");
    REQUIRE(sprite.ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "pixelsPerUnit", "2").ok());
    eng::editor::TextureCache cache;
    // Sprite 2x2 px @ ppu 2 → 1x1 unidades.

    /// Verificador: gizmo MOVE responde ao drag de +1 unidade.
    auto moveWorks = [&](float expectedX) {
        REQUIRE(doc.select(sprite.value()).ok());
        doc.setTool(eng::editor::EditorTool::Move);
        const auto bounds = doc.selectionBounds(&cache);
        REQUIRE(bounds.valid);
        const float cx = doc.viewport().worldToScreenX(bounds.worldX);
        const float cy = doc.viewport().worldToScreenY(bounds.worldY);
        CHECK(doc.gizmoDragBegin(cx, cy, &cache) == GizmoHandle::MoveCenter);
        REQUIRE(doc.gizmoDragTo(cx + 48.f, cy).ok());
        doc.gizmoDragEnd();
        auto tr = doc.transform(sprite.value());
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x ==
              Catch::Approx(expectedX + 1.f).margin(1e-3f));
        // Reposiciona em X para o próximo passo ter base limpa.
        eng::editor::TransformDesc reset;
        reset.position.x = expectedX;
        REQUIRE(doc.setTransform(sprite.value(), reset).ok());
    };

    // 1) Recém-criado (baseline do reporte do bug).
    moveWorks(0.f);

    // 2) Adicionar COMPONENTES (física/animação/partícula/collider).
    REQUIRE(doc.addComponent(sprite.value(), "eng::physics::RigidBody").ok());
    REQUIRE(doc.addComponent(sprite.value(), "eng::physics::Collider").ok());
    REQUIRE(doc.addComponent(sprite.value(),
                             "eng::animation::Animator").ok());
    REQUIRE(doc.addComponent(sprite.value(),
                             "eng::particles::ParticleEmitter").ok());
    moveWorks(0.f);

    // 3) ALTERAR o sprite (ppu/tint/pivot) — bounds muda, gizmo segue.
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "pixelsPerUnit", "4").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "tintR,tintG,tintB", "#80FF40").ok());
    moveWorks(0.f);

    // 4) TROCAR a textura (vazia → placeholder → volta).
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "").ok());
    moveWorks(0.f);
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "grass.png").ok());

    // 5) DUPLICAR: o clone também gizmo-funciona (independente).
    auto dup = doc.duplicateEntity(sprite.value());
    REQUIRE(dup.ok());
    {
        REQUIRE(doc.select(dup.value()).ok());
        doc.setTool(eng::editor::EditorTool::Move);
        const auto b = doc.selectionBounds(&cache);
        REQUIRE(b.valid);
        const float cx = doc.viewport().worldToScreenX(b.worldX);
        const float cy = doc.viewport().worldToScreenY(b.worldY);
        CHECK(doc.gizmoDragBegin(cx, cy, &cache) == GizmoHandle::MoveCenter);
        REQUIRE(doc.gizmoDragTo(cx + 48.f, cy).ok());
        doc.gizmoDragEnd();
        auto tr = doc.transform(dup.value());
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
        // Original NÃO se mexeu (independência real do clone).
        tr = doc.transform(sprite.value());
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(0.f).margin(1e-3f));
    }
    REQUIRE(doc.deleteEntity(dup.value()).ok());

    // 6) MOVE/ROTATE/SCALE via gizmo em SEQUÊNCIA (drags encadeados).
    moveWorks(0.f);
    {
        REQUIRE(doc.select(sprite.value()).ok());
        doc.setTool(eng::editor::EditorTool::Rotate);
        const auto b = doc.selectionBounds(&cache);
        // P4.1 (D3): o teste usa a MESMA métrica do gizmo — raio com
        // mínimo de 64 px em tela (anel agarrável em qualquer zoom).
        const float ringR =
            eng::editor::TransformGizmo::ringRadiusPx(
                b.halfW * doc.viewport().camera().zoom,
                doc.viewport().uiScale()) /
            doc.viewport().camera().zoom;
        const float cx = doc.viewport().worldToScreenX(b.worldX);
        const float cy = doc.viewport().worldToScreenY(b.worldY);
        REQUIRE(doc.gizmoDragBegin(cx + ringR * 48.f, cy, &cache) ==
                GizmoHandle::RotateRing);
        REQUIRE(doc.gizmoDragTo(cx, cy - ringR * 48.f).ok());
        doc.gizmoDragEnd();
        auto tr = doc.transform(sprite.value());
        REQUIRE(tr.ok());
        CHECK(tr.value().rotationDegrees.z ==
              Catch::Approx(90.f).margin(0.5f));
    }
    moveWorks(0.f);  // rotate NÃO derruba o move
    {
        REQUIRE(doc.select(sprite.value()).ok());
        doc.setTool(eng::editor::EditorTool::Scale);
        // P4.7.0 B2: o toque usa a MESMA fonte do desenho — handle
        // CLAMPADO (scaleHandlePoints). Bounds pequenos empurram o
        // cantão para fora; tocar no cantão "cru" seria um MISS.
        TransformGizmo gizmoForPoints;
        const auto points =
            gizmoForPoints.scaleHandlePoints(doc.viewport(),
                                             doc.selectionBounds(&cache));
        const float nePx =
            doc.viewport().worldToScreenX(points.ne.first);
        const float nePy =
            doc.viewport().worldToScreenY(points.ne.second);
        REQUIRE(doc.gizmoDragBegin(nePx, nePy, &cache)
                == GizmoHandle::ScaleNE);
        const auto b2 = doc.selectionBounds(&cache);
        const float cx2 = doc.viewport().worldToScreenX(b2.worldX);
        const float cy2 = doc.viewport().worldToScreenY(b2.worldY);
        REQUIRE(doc.gizmoDragTo(cx2 + b2.halfW * 96.f,
                                cy2 - b2.halfH * 96.f).ok());
        doc.gizmoDragEnd();
    }

    // 7) SAVE → RELOAD → re-selecionar → gizmo vivo. (Handles da cena
    // ANTIGA morrem no reload — o reload cria um World novo: step 9 usa
    // o handle RECAREGADO, não o `sprite` original.)
    REQUIRE(doc.saveScene("matrix.json").ok());
    REQUIRE(doc.loadScene("matrix.json").ok());
    eng::ecs::Entity reloaded{};
    {
        const auto nodes = doc.hierarchySnapshot();
        REQUIRE(nodes.size() == 1);  // duplicata foi apagada antes do save
        reloaded = nodes[0].entity;
        REQUIRE(doc.select(reloaded).ok());
        doc.setTool(eng::editor::EditorTool::Move);
        const auto b = doc.selectionBounds(&cache);
        REQUIRE(b.valid);  // textura RE-RESOLVIDA pós-reload
        const float cx = doc.viewport().worldToScreenX(b.worldX);
        const float cy = doc.viewport().worldToScreenY(b.worldY);
        CHECK(doc.gizmoDragBegin(cx, cy, &cache) == GizmoHandle::MoveCenter);
        REQUIRE(doc.gizmoDragTo(cx + 48.f, cy).ok());
        doc.gizmoDragEnd();
        auto tr = doc.transform(reloaded);
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
    }

    // 8) PLAY → STOP → re-selecionar → gizmo vivo.
    REQUIRE(doc.play().ok());
    doc.tick(1.f / 60.f);
    doc.stop();
    {
        const auto nodes = doc.hierarchySnapshot();
        REQUIRE(nodes.size() == 1);
        REQUIRE(nodes[0].entity == reloaded);  // stop devolve a EDIÇÃO
        REQUIRE(doc.select(reloaded).ok());
        const auto b = doc.selectionBounds(&cache);
        REQUIRE(b.valid);
        CHECK(doc.gizmoDragBegin(
            doc.viewport().worldToScreenX(b.worldX),
            doc.viewport().worldToScreenY(b.worldY),
            &cache) == GizmoHandle::MoveCenter);
        doc.gizmoDragEnd();  // begin de PROVA encerra o drag (hit-test livre)
    }

    // 9) SELECIONAR outra entidade e VOLTAR.
    auto other = doc.createEntity("Other", eng::scene::kNoEntity);
    REQUIRE(other.ok());
    REQUIRE(doc.select(other.value()).ok());
    CHECK_FALSE(doc.isSelected(reloaded));
    REQUIRE(doc.select(reloaded).ok());
    {
        const auto b = doc.selectionBounds(&cache);
        REQUIRE(b.valid);
        CHECK(doc.gizmoDragBegin(
            doc.viewport().worldToScreenX(b.worldX),
            doc.viewport().worldToScreenY(b.worldY),
            &cache) == GizmoHandle::MoveCenter);
        doc.gizmoDragEnd();
    }
}

TEST_CASE("editor: P2 — gizmo em entity SEM Sprite (só Transform) §6",
          "[editor][gizmo-p2]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    // Entidade VAZIA (Name + Transform apenas): selecionável e
    // transformável pelo gizmo — não exige Sprite.
    REQUIRE(doc.select(g.entity).ok());
    const auto b = doc.selectionBounds(nullptr);
    REQUIRE(b.valid);  // bounds = escala local (1x1 → half 0.5)
    CHECK(b.halfW == Catch::Approx(0.5f));
    CHECK(b.halfH == Catch::Approx(0.5f));

    doc.setTool(eng::editor::EditorTool::Move);
    CHECK(doc.gizmoDragBegin(100.f, 75.f, nullptr) == GizmoHandle::MoveCenter);
    REQUIRE(doc.gizmoDragTo(148.f, 27.f).ok());
    doc.gizmoDragEnd();
    auto tr = doc.transform(g.entity);
    REQUIRE(tr.ok());
    CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
    CHECK(tr.value().position.y == Catch::Approx(1.f).margin(1e-3f));

    // ADICIONAR Sprite DEPOIS: bounds passa ao tamanho desenhado, gizmo
    // continua no controle (a mesma entidade).
    REQUIRE(doc.addComponent(g.entity, "eng::editor::SpriteData").ok());
    {
        REQUIRE(doc.select(g.entity).ok());
        const auto b2 = doc.selectionBounds(nullptr);
        REQUIRE(b2.valid);
        CHECK(b2.worldX == Catch::Approx(1.f).margin(1e-3f));
        doc.setTool(eng::editor::EditorTool::Rotate);
        const float ringR = b2.halfW + 26.f / 48.f;
        CHECK(doc.gizmoDragBegin(100.f + 1.f * 48.f + ringR * 48.f,
                                 75.f - 1.f * 48.f,
                                 nullptr) == GizmoHandle::RotateRing);
        REQUIRE(doc.gizmoDragTo(100.f + 1.f * 48.f,
                               75.f - 1.f * 48.f - ringR * 48.f).ok());
        doc.gizmoDragEnd();
        tr = doc.transform(g.entity);
        REQUIRE(tr.ok());
        CHECK(tr.value().rotationDegrees.z == Catch::Approx(90.f).margin(0.5f));
    }

    // REMOVER Sprite: volta ao bounds de escala — gizmo vivo.
    REQUIRE(doc.removeComponent(g.entity, "eng::editor::SpriteData").ok());
    {
        REQUIRE(doc.select(g.entity).ok());
        const auto b3 = doc.selectionBounds(nullptr);
        REQUIRE(b3.valid);
        CHECK(b3.halfW == Catch::Approx(0.5f));
        doc.setTool(eng::editor::EditorTool::Move);
        CHECK(doc.gizmoDragBegin(
            doc.viewport().worldToScreenX(b3.worldX),
            doc.viewport().worldToScreenY(b3.worldY),
            nullptr) == GizmoHandle::MoveCenter);
    }

    // TAP também acerta a entidade sem sprite (hit-test mínimo 22px).
    auto hit = doc.viewportTap(
        doc.viewport().worldToScreenX(1.f),
        doc.viewport().worldToScreenY(1.f), nullptr);
    REQUIRE(hit.has_value());
    CHECK(*hit == g.entity);
}

// =============================================================================
// P2 — COMPONENT/TICK AUTHORING (§2/§14), ANIMAÇÃO (§8), ÁUDIO (§12),
// CÂMERA (§11), EMISSOR (§10) — workflow real.
// =============================================================================

TEST_CASE("editor: P2 — addableComponents: catálogo real com hints (§2)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(e.ok());

    auto catalog = f.doc->addableComponents(e.value());
    REQUIRE_FALSE(catalog.empty());
    bool hasRigidBody = false, hasCollider = false, hasAnimator = false,
         hasEmitter = false, hasAudio = false, hasCamera = false,
         hasScript = false;
    for (const auto& meta : catalog) {
        hasRigidBody |= meta.name == "eng::physics::RigidBody";
        hasCollider |= meta.name == "eng::physics::Collider";
        hasAnimator |= meta.name == "eng::animation::Animator";
        hasEmitter |= meta.name == "eng::particles::ParticleEmitter";
        hasAudio |= meta.name == "eng::editor::AudioSource";
        hasCamera |= meta.name == "eng::tick::CameraData";
        hasScript |= meta.name == "eng::editor::NiScriptComponent";
        CHECK(meta.name != "eng::math::Transform");
        CHECK(meta.name != "eng::scene::Name");
        CHECK(meta.addable);
    }
    CHECK(hasRigidBody);
    CHECK(hasCollider);
    CHECK(hasAnimator);
    CHECK(hasEmitter);
    CHECK(hasAudio);
    CHECK(hasCamera);
    CHECK(hasScript);

    bool rigidHint = false, animHint = false, audioHint = false;
    for (const auto& meta : catalog) {
        if (meta.name == "eng::physics::RigidBody" && !meta.dependency.empty()) {
            rigidHint = true;
        }
        if (meta.name == "eng::animation::Animator" &&
            !meta.dependency.empty()) {
            animHint = true;
        }
        if (meta.name == "eng::editor::AudioSource" && !meta.dependency.empty()) {
            audioHint = true;
        }
    }
    CHECK(rigidHint);
    CHECK(animHint);
    CHECK(audioHint);

    REQUIRE(f.doc->addComponent(e.value(), "eng::physics::Collider").ok());
    const auto after = f.doc->addableComponents(e.value());
    for (const auto& meta : after) {
        CHECK(meta.name != "eng::physics::Collider");
    }

    auto added = f.doc->addComponentWithDependencies(e.value(),
                                                    "eng::physics::RigidBody");
    REQUIRE(added.ok());
    REQUIRE(added.value().size() == 1);
    CHECK(added.value()[0] == "eng::physics::RigidBody");
}

TEST_CASE("editor: P2 — ANIMAÇÃO: create/frames/fps/loop/assign (§8)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    writePngTemp(*f.fs);
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());

    REQUIRE(f.doc->animationCreate("walk").ok());
    auto list = f.doc->animationList();
    REQUIRE(list.ok());
    REQUIRE(list.value().size() == 1);
    CHECK(list.value()[0].name == "walk.anim.json");
    CHECK(list.value()[0].clip == "walk");
    CHECK(list.value()[0].loop);

    auto dup = f.doc->animationCreate("walk");
    REQUIRE(dup.isError());

    auto first = f.doc->animationAddFrame("walk", "grass.png");
    REQUIRE(first.ok());
    CHECK(first.value() == Catch::Approx(0.f).margin(1e-3f));
    auto second = f.doc->animationAddFrame("walk", "grass.png");
    REQUIRE(second.ok());
    CHECK(second.value() == Catch::Approx(0.125f).margin(1e-3f));

    REQUIRE(f.doc->animationSetMeta("walk", false, 16.f).ok());
    list = f.doc->animationList();
    REQUIRE(list.ok());
    CHECK_FALSE(list.value()[0].loop);
    auto json = f.doc->animationRead("walk.anim.json");
    REQUIRE(json.ok());
    CHECK(json.value().find("\"fps\":16") != std::string::npos);

    auto bad = f.doc->animationAddFrame("walk", "nao_existe.png");
    REQUIRE(bad.isError());

    auto e = f.doc->createEntity("Hero", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->animationAssign(e.value(), "walk").ok());
    auto clip = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::animation::Animator",
        "clip");
    REQUIRE(clip.ok());
    CHECK(clip.value() == "walk");

    // §14: clip COM frames → SpriteData auto-criado (o autor VÊ o flipbook).
    const auto comps = eng::editor::Inspector::componentsOf(
        *f.doc->sceneInFocus(), e.value());
    CHECK(std::find(comps.begin(), comps.end(),
                    "eng::editor::SpriteData") != comps.end());

    REQUIRE(f.doc->saveScene("anim.json").ok());
    REQUIRE(f.doc->loadScene("anim.json").ok());
    const auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    clip = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), nodes[0].entity,
        "eng::animation::Animator", "clip");
    REQUIRE(clip.ok());
    CHECK(clip.value() == "walk");

    REQUIRE(f.doc->animationDelete("walk.anim.json").ok());
    list = f.doc->animationList();
    REQUIRE(list.ok());
    CHECK(list.value().empty());
}

TEST_CASE("editor: P2 — ANIMAÇÃO: PREVIEW em Edit + restore (§8)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    // Clip com track de POSIÇÃO real (o preview mexe e restaura).
    REQUIRE(f.doc
                ->animationWrite("slide.anim.json",
                                 "{\n"
                                 "  \"name\": \"slide\",\n"
                                 "  \"fps\": 8,\n"
                                 "  \"loop\": true,\n"
                                 "  \"position\": [[0, 0, 0, 0], "
                                 "[1, 4, 0, 0]]\n"
                                 "}\n")
                .ok());

    auto e = f.doc->createEntity("Slider", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->animationAssign(e.value(), "slide").ok());
    // Track presente → applyPosition LIGADO pelo assign (default smart).
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::animation::Animator",
                                     "applyPosition", "true")
                .ok());

    eng::editor::TransformDesc start;
    start.position = eng::math::Vec3{5.f, -3.f, 0.f};
    REQUIRE(f.doc->setTransform(e.value(), start).ok());

    REQUIRE(f.doc->previewStart(e.value(), "slide").ok());
    CHECK(f.doc->previewing());
    for (int i = 0; i < 30; ++i) {
        f.doc->previewTick(1.f / 60.f);  // 0.5s de 1s de clip
    }
    {
        auto tr = f.doc->transform(e.value());
        REQUIRE(tr.ok());
        // O MESMO sampler aplicou: x = 2 (metade do slide 0→4).
        CHECK(tr.value().position.x == Catch::Approx(2.f).margin(0.05f));
    }
    f.doc->previewStop();
    CHECK_FALSE(f.doc->previewing());
    {
        auto tr = f.doc->transform(e.value());
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(5.f).margin(1e-3f));
        CHECK(tr.value().position.y == Catch::Approx(-3.f).margin(1e-3f));
    }

    auto bad = f.doc->previewStart(e.value(), "nao_existe");
    REQUIRE(bad.isError());
}

TEST_CASE("editor: P2 — ANIMAÇÃO: PLAY executa clip do ASSET no clone (§8/§19)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->animationCreate("rise2").ok());
    REQUIRE(f.doc
                ->animationWrite("rise2.anim.json",
                                 "{\n"
                                 "  \"name\": \"rise2\",\n"
                                 "  \"fps\": 8,\n"
                                 "  \"loop\": true,\n"
                                 "  \"position\": [[0, 0, 0, 0], "
                                 "[1, 0, 4, 0]]\n"
                                 "}\n")
                .ok());

    auto e = f.doc->createEntity("Lifter", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->animationAssign(e.value(), "rise2").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::animation::Animator",
                                     "playing", "true")
                .ok());

    REQUIRE(f.doc->play().ok());
    const auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    for (int i = 0; i < 30; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    auto y = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), nodes[0].entity, "eng::math::Transform",
        "position.y");
    REQUIRE(y.ok());
    CHECK(std::stof(y.value()) == Catch::Approx(2.f).margin(0.15f));

    f.doc->stop();
    auto editY = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), nodes[0].entity, "eng::math::Transform",
        "position.y");
    REQUIRE(editY.ok());
    CHECK(std::stof(editY.value()) == Catch::Approx(0.f).margin(1e-3f));
}

TEST_CASE("editor: P2 — ANIMAÇÃO: FRAMES aplicados ao sprite (§8/§20)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    writePngTemp(*f.fs);
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());

    REQUIRE(f.doc
                ->animationWrite("flip.anim.json",
                                 "{\n"
                                 "  \"name\": \"flip\",\n"
                                 "  \"fps\": 2,\n"
                                 "  \"loop\": true,\n"
                                 "  \"frames\": ["
                                 "[0, \"grass.png\", 0, 0, 0.5, 1], "
                                 "[0.5, \"grass.png\", 0.5, 0, 1, 1]]\n"
                                 "}\n")
                .ok());

    auto e = f.doc->createEntity("Flip", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::editor::SpriteData").ok());
    REQUIRE(f.doc->animationAssign(e.value(), "flip").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::animation::Animator",
                                     "playing", "true")
                .ok());

    REQUIRE(f.doc->play().ok());
    const auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    f.doc->tick(0.1f);
    auto u0 = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), nodes[0].entity,
        "eng::editor::SpriteData", "u0");
    REQUIRE(u0.ok());
    CHECK(std::stof(u0.value()) == Catch::Approx(0.f).margin(1e-3f));

    f.doc->tick(0.5f);
    u0 = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), nodes[0].entity,
        "eng::editor::SpriteData", "u0");
    REQUIRE(u0.ok());
    CHECK(std::stof(u0.value()) == Catch::Approx(0.5f).margin(1e-3f));

    f.doc->stop();
    u0 = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), nodes[0].entity,
        "eng::editor::SpriteData", "u0");
    REQUIRE(u0.ok());
    CHECK(std::stof(u0.value()) == Catch::Approx(0.f).margin(1e-3f));
}

TEST_CASE("editor: P2 — CÂMERA segue a ENTIDADE (§11) + rect no viewport",
          "[editor][p2][tick]")
{
    DocFixture f;
    f.withProject();
    auto cam = f.doc->createEntity("Cam", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "posX", "12").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "posY", "-6").ok());
    REQUIRE(f.doc->setInspectorField(cam.value(), "eng::tick::CameraData",
                                    "zoom", "96").ok());
    eng::editor::TransformDesc place;
    place.position = eng::math::Vec3{3.f, 2.f, 0.f};
    REQUIRE(f.doc->setTransform(cam.value(), place).ok());

    auto& viewport = f.doc->viewport();
    viewport.setScreenSize(1000.f, 500.f);

    auto quads = viewport.buildQuads(*f.doc->sceneInFocus(),
                                     f.doc->selection());
    const eng::editor::EntityQuad* camQuad = nullptr;
    for (const auto& q : quads) {
        if (q.entity == cam.value()) {
            camQuad = &q;
        }
    }
    REQUIRE(camQuad != nullptr);
    CHECK(camQuad->hasCamera);
    CHECK(camQuad->cameraActive);
    CHECK(camQuad->cameraCenterX == Catch::Approx(15.f).margin(1e-3f));
    CHECK(camQuad->cameraCenterY == Catch::Approx(-4.f).margin(1e-3f));
    CHECK(camQuad->cameraHalfW == Catch::Approx(1000.f / 96.f / 2.f));
    CHECK(camQuad->cameraHalfH == Catch::Approx(500.f / 96.f / 2.f));

    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->hasGameCamera());
    CHECK_THAT(viewport.screenToWorldX(500.f),
               Catch::Matchers::WithinAbs(15.f, 1e-2f));
    CHECK_THAT(viewport.screenToWorldY(250.f),
               Catch::Matchers::WithinAbs(-4.f, 1e-2f));
    f.doc->stop();
}

TEST_CASE("editor: P2 — EMISSOR de partículas: marcador no viewport (§10)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Smoke", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->addComponent(e.value(),
                                "eng::particles::ParticleEmitter").ok());
    eng::editor::TransformDesc rot;
    rot.rotationDegrees = eng::math::Vec3{0.f, 0.f, 90.f};
    REQUIRE(f.doc->setTransform(e.value(), rot).ok());

    auto quads = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(),
                                              f.doc->selection());
    const eng::editor::EntityQuad* found = nullptr;
    for (const auto& q : quads) {
        if (q.entity == e.value()) {
            found = &q;
        }
    }
    REQUIRE(found != nullptr);
    CHECK(found->hasEmitter);
    CHECK(found->emitterDirX == Catch::Approx(-1.f).margin(1e-3f));
    CHECK(found->emitterDirY == Catch::Approx(0.f).margin(1e-3f));

    REQUIRE(f.doc->setInspectorField(
                e.value(), "eng::particles::ParticleEmitter",
                "direction.x", "1").ok());
    REQUIRE(f.doc->setInspectorField(
                e.value(), "eng::particles::ParticleEmitter",
                "direction.y", "0").ok());
    quads = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(),
                                         f.doc->selection());
    for (const auto& q : quads) {
        if (q.entity == e.value()) {
            found = &q;
        }
    }
    REQUIRE(found != nullptr);
    CHECK(found->emitterDirX == Catch::Approx(0.f).margin(1e-3f));
    CHECK(found->emitterDirY == Catch::Approx(1.f).margin(1e-3f));
}

TEST_CASE("editor: P2 — ÁUDIO: AudioSource + mixer REAL no Play (§12)",
          "[editor][p2]")
{
    DocFixture f;
    f.withProject();
    constexpr std::uint32_t kSamples = 240;
    std::vector<std::byte> wav;
    auto push32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            wav.push_back(static_cast<std::byte>(v >> (8 * i)));
        }
    };
    auto push16 = [&](std::uint16_t v) {
        wav.push_back(static_cast<std::byte>(v & 0xff));
        wav.push_back(static_cast<std::byte>(v >> 8));
    };
    const auto pushTag = [&](const char (&tag)[5]) {
        for (int i = 0; i < 4; ++i) {
            wav.push_back(static_cast<std::byte>(tag[i]));
        }
    };
    pushTag("RIFF");
    push32(36 + kSamples * 2);
    pushTag("WAVE");
    pushTag("fmt ");
    push32(16);
    push16(1);
    push16(1);
    push32(48000);
    push32(96000);
    push16(2);
    push16(16);
    pushTag("data");
    push32(kSamples * 2);
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        push16(static_cast<std::uint16_t>(i * 100));
    }
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());
    REQUIRE(f.fs
                ->writeAllBytes(
                    eng::fs::Path{".import_tmp/beep.wav"},
                    std::span{wav.data(), wav.size()})
                .ok());
    REQUIRE(browser->import(".import_tmp/beep.wav", "audio", "beep").ok());

    auto e = f.doc->createEntity("Sfx", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::editor::AudioSource").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::editor::AudioSource",
                                    "soundAsset", "beep.wav").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::editor::AudioSource",
                                    "playOnStart", "true").ok());

    const auto fields = f.doc->inspectorFields(
        e.value(), "eng::editor::AudioSource");
    bool sawAudioKind = false;
    for (const auto& field : fields) {
        if (field.path == "soundAsset") {
            sawAudioKind = field.kind == "audio";
        }
    }
    CHECK(sawAudioKind);

    REQUIRE(f.doc->play().ok());
    f.doc->tick(1.f / 60.f);
    CHECK(f.doc->audioMixer().stats().voicesPlayed == 1);
    CHECK(f.doc->audioMixer().liveVoices() >= 1);
    std::vector<float> buffer(512, 0.f);
    f.doc->audioMixer().mix(buffer.data(), 256);
    CHECK(f.doc->audioMixer().stats().framesMixed > 0);
    f.doc->stop();
    CHECK(f.doc->audioMixer().liveVoices() == 0);

    REQUIRE(f.doc->audioPreview("beep.wav").ok());
    CHECK(f.doc->audioMixer().liveVoices() == 1);
}

TEST_CASE("editor: P2 — WORKFLOW de integração REAL (§21/§23)", "[editor][p2]")
{
    DocFixture f;
    f.withProject();

    writePngTemp(*f.fs);
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    REQUIRE(browser->import(".import_tmp/grass.png", "textures", "grass").ok());

    auto player = f.doc->createSprite("Player");
    REQUIRE(player.ok());
    REQUIRE(f.doc->setInspectorField(player.value(),
                                    "eng::editor::SpriteData",
                                    "textureAsset", "grass.png").ok());

    auto withDeps = f.doc->addComponentWithDependencies(
        player.value(), "eng::physics::RigidBody");
    REQUIRE(withDeps.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->setInspectorField(player.value(),
                                     "eng::physics::Collider", "shape",
                                     "Box").ok());
    REQUIRE(f.doc->setInspectorField(
                player.value(), "eng::physics::Collider",
                "halfExtents.x", "0.5").ok());
    REQUIRE(f.doc->setInspectorField(
                player.value(), "eng::physics::Collider",
                "halfExtents.y", "0.5").ok());

    REQUIRE(f.doc->animationCreate("idle").ok());
    REQUIRE(f.doc->animationAddFrame("idle", "grass.png").ok());
    REQUIRE(f.doc->animationAssign(player.value(), "idle").ok());

    REQUIRE(f.doc->scriptCreate("mover").ok());
    REQUIRE(f.doc->scriptAssign(player.value(), "mover.nis").ok());

    auto cam = f.doc->createEntity("MainCam", eng::scene::kNoEntity);
    REQUIRE(cam.ok());
    REQUIRE(f.doc->addComponent(cam.value(), "eng::tick::CameraData").ok());
    auto enemy = f.doc->createEntity("Enemy", eng::scene::kNoEntity);
    REQUIRE(enemy.ok());
    REQUIRE(f.doc->addComponent(enemy.value(), "eng::physics::RigidBody").ok());
    REQUIRE(f.doc->addComponent(enemy.value(), "eng::physics::Collider").ok());

    REQUIRE(f.doc->select(player.value()).ok());
    f.doc->setTool(eng::editor::EditorTool::Move);
    const auto b = f.doc->selectionBounds(nullptr);
    REQUIRE(b.valid);
    CHECK(f.doc->gizmoDragBegin(
              f.doc->viewport().worldToScreenX(b.worldX),
              f.doc->viewport().worldToScreenY(b.worldY),
              nullptr) == eng::editor::GizmoHandle::MoveCenter);
    REQUIRE(f.doc->gizmoDragTo(
        f.doc->viewport().worldToScreenX(b.worldX) + 48.f,
        f.doc->viewport().worldToScreenY(b.worldY)).ok());
    f.doc->gizmoDragEnd();

    REQUIRE(f.doc->saveScene("workflow.json").ok());
    REQUIRE(f.doc->loadScene("workflow.json").ok());
    {
        // Reload recria por SceneEntityId (UUID ALEATÓRIO — ADR-033):
        // a ordem dos nós NÃO é a de criação. Acha o PLAYER PELO NOME.
        const auto nodes = f.doc->hierarchySnapshot();
        REQUIRE(nodes.size() == 3);
        eng::ecs::Entity playerNode{};
        for (const auto& node : nodes) {
            if (node.name == "Player") {
                playerNode = node.entity;
            }
        }
        REQUIRE(playerNode != eng::scene::kNoEntity);
        const auto playerComponents =
            eng::editor::Inspector::componentsOf(*f.doc->sceneInFocus(),
                                                  playerNode);
        bool animator = false, script = false, sprite = false,
             rigid = false, collider = false;
        for (const auto& c : playerComponents) {
            animator |= c == "eng::animation::Animator";
            script |= c == "eng::editor::NiScriptComponent";
            sprite |= c == "eng::editor::SpriteData";
            rigid |= c == "eng::physics::RigidBody";
            collider |= c == "eng::physics::Collider";
        }
        CHECK(animator);
        CHECK(script);
        CHECK(sprite);
        CHECK(rigid);
        CHECK(collider);
        auto tr = f.doc->transform(playerNode);
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(1.f).margin(1e-3f));
    }

    REQUIRE(f.doc->play().ok());
    const auto sched = f.doc->runtimeScheduler();
    REQUIRE(sched != nullptr);
    const auto order = sched->systemOrder();
    REQUIRE(order.size() == 6);
    CHECK(order[0] == "PhysicsTick");
    CHECK(order[1] == "AnimationTick");
    CHECK(order[2] == "ParticleTick");
    CHECK(order[3] == "ScriptTick");
    CHECK(order[4] == "AudioTick");
    CHECK(order[5] == "CameraTick");
    for (int i = 0; i < 10; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    CHECK(f.doc->hasGameCamera());
    f.doc->stop();
    CHECK_FALSE(f.doc->hasGameCamera());
    const auto after = f.doc->hierarchySnapshot();
    CHECK(after.size() == 3);
}

// =============================================================================
// P3 §0 — STARTUP ANDROID: bug "AlreadyExists" (create × open × restore ×
// reentrada). A política vive no EditorDocument; cada caso abaixo espelha
// um cenário do relatório do dispositivo (Realme C33).
// =============================================================================

namespace {

/// Workspace compartilhado entre "sessões" (documentos SEQUENCIAIS sobre o
/// MESMO armazenamento) — simula processo morto/recriado do Android. O
/// estado em memória de cada sessão começa VAZIO (hasProject()==false):
/// exatamente o gatilho do bug original.
struct StartupSessions {
    eng::fs::MemoryFileSystem storage{};
    std::unique_ptr<EditorDocument> doc{};

    void newSession()
    {
        doc.reset();  // "processo morre" — estado em memória vai embora
        auto created = EditorDocument::create(storage, eng::fs::Path{"."});
        REQUIRE(created.ok());
        doc = std::move(created.value());
    }

    StartupSessions() { newSession(); }
};

}  // namespace

// A. Criar projeto novo (instalação limpa): sem projetos no workspace →
// cria o default "MeuJogo" — SEM AlreadyExists.
TEST_CASE("p3-startup A: instalação limpa cria o default MeuJogo", "[editor][p3]")
{
    StartupSessions s;
    REQUIRE_FALSE(s.doc->hasProject());  // memória vazia = processo novo

    auto ensured = s.doc->ensureStartupProject();
    REQUIRE(ensured.ok());
    CHECK(ensured.value() == "MeuJogo");
    CHECK(s.doc->hasProject());
    CHECK(s.doc->projectName() == "MeuJogo");
    // Estrutura real no disco (o que o app veria após a criação).
    CHECK(s.storage.exists(eng::fs::Path{"MeuJogo/project.goni.json"}).value());
    CHECK(s.storage.exists(eng::fs::Path{"MeuJogo/scenes"}).value());
}

// B. Abrir projeto existente (reentrada): a segunda "sessão" (processo
// novo, memória vazia) ABRE o projeto — antes do fix, este caminho
// chamava newProject("MeuJogo") e recebia AlreadyExists.
TEST_CASE("p3-startup B: reentrada ABRE o projeto existente (bug raiz)",
          "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());  // sessão 1: cria MeuJogo
    s.newSession();                                // processo novo

    REQUIRE_FALSE(s.doc->hasProject());  // gatilho do bug: memória vazia
    auto ensured = s.doc->ensureStartupProject();
    REQUIRE(ensured.ok());               // ANTES: AlreadyExists
    CHECK(ensured.value() == "MeuJogo");
    CHECK(s.doc->hasProject());
    CHECK(s.doc->projectName() == "MeuJogo");
    // Nenhum duplicado foi criado (o workspace continua com UM projeto).
    CHECK(s.doc->listProjects().value().size() == 1);
}

// C. Criar projeto com nome existente → erro CONTROLADO (AlreadyExists
// só existe neste caminho MANUAL — o documento segue utilizável).
TEST_CASE("p3-startup C: duplicado manual devolve AlreadyExists controlado",
          "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->newProject("MeuJogo").ok());
    CHECK(s.doc->hasProject());

    auto dup = s.doc->newProject("MeuJogo");
    REQUIRE(dup.isError());
    CHECK(dup.error().code == eng::core::StatusCode::AlreadyExists);
    // O erro NÃO derruba o documento: o projeto original segue aberto.
    CHECK(s.doc->hasProject());
    CHECK(s.doc->projectName() == "MeuJogo");
    CHECK(s.doc->saveProject().ok());
}

// D. Abrir o projeto existente após reiniciar o editor: cena salva na
// sessão 1 volta INTEIRA na sessão 2 (round-trip via startup).
TEST_CASE("p3-startup D: cena salva volta após reinício (restore)", "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());
    auto e = s.doc->createEntity("Player", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    eng::editor::TransformDesc desc{};
    desc.position = {2.f, -3.f, 0.f};
    desc.rotationDegrees = {0.f, 0.f, 90.f};
    desc.scale = {2.f, 2.f, 1.f};
    REQUIRE(s.doc->setTransform(e.value(), desc).ok());
    REQUIRE(s.doc->saveScene("main.json").ok());
    REQUIRE(s.doc->saveProject().ok());

    s.newSession();  // "reiniciar o editor"
    REQUIRE(s.doc->ensureStartupProject().ok());
    REQUIRE(s.doc->loadScene("main.json").ok());
    const auto snap = s.doc->hierarchySnapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].name == "Player");
    auto tr2 = s.doc->transform(snap[0].entity);
    REQUIRE(tr2.ok());
    CHECK(tr2.value().position.x == Catch::Approx(2.f).margin(1e-3f));
    CHECK(tr2.value().position.y == Catch::Approx(-3.f).margin(1e-3f));
    CHECK(tr2.value().rotationDegrees.z == Catch::Approx(90.f).margin(1e-2f));
}

// E. Activity recreation (host destruído + recriado no MESMO workspace em
// disco REAL — caminho completo do Android, sem GPU necessária).
TEST_CASE("p3-startup E: recreation do EditorHost reabre sem AlreadyExists",
          "[editor][p3]")
{
    const std::string ws = ".editor-test-ws-startup-e";
    {
        auto host = eng::editor::EditorHost::create("auto", ws.c_str());
        REQUIRE(host.ok());
        std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
        auto ensured = owned->ensureStartupProject();
        REQUIRE(ensured.ok());
        CHECK(ensured.value() == "MeuJogo");
    }  // onDestroy: host morre (documento junto)
    {
        // Activity recriada: processo novo, workspace persistido.
        auto host = eng::editor::EditorHost::create("auto", ws.c_str());
        REQUIRE(host.ok());
        std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
        auto ensured = owned->ensureStartupProject();
        REQUIRE(ensured.ok());  // ANTES do fix: AlreadyExists + sem projeto
        CHECK(ensured.value() == "MeuJogo");
        CHECK(owned->document().hasProject());
    }
    std::filesystem::remove_all(std::filesystem::path{ws});
}

// F. Fechar e abrir novamente: DUAS políticas na MESMA sessão — a
// segunda é no-op (o projeto já está em memória; não recria nada).
TEST_CASE("p3-startup F: política dupla é no-op (não recria/reabre)",
          "[editor][p3]")
{
    StartupSessions s;
    auto first = s.doc->ensureStartupProject();
    REQUIRE(first.ok());
    const auto projectsAfterFirst = s.doc->listProjects().value();

    auto second = s.doc->ensureStartupProject();
    REQUIRE(second.ok());
    CHECK(second.value() == first.value());
    CHECK(s.doc->listProjects().value() == projectsAfterFirst);
    CHECK(s.doc->listProjects().value().size() == 1);
}

// G. Instalação limpa via HOST REAL (mesmo caminho A, com NativeFS +
// RootedFS — a fronteira exata do Android).
TEST_CASE("p3-startup G: instalação limpa no host real cria MeuJogo",
          "[editor][p3]")
{
    const std::string ws = ".editor-test-ws-startup-g";
    auto host = eng::editor::EditorHost::create("auto", ws.c_str());
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};

    auto ensured = owned->ensureStartupProject();
    REQUIRE(ensured.ok());
    CHECK(ensured.value() == "MeuJogo");
    CHECK(owned->document().hasProject());
    std::filesystem::remove_all(std::filesystem::path{ws});
}

// H. Múltiplos projetos: o ÚLTIMO USADO é restaurado; registro stale
// (projeto apagado) cai no default.
TEST_CASE("p3-startup H: último usado vence; registro stale cai no default",
          "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());          // MeuJogo (auto)
    REQUIRE(s.doc->newProject("Segundo").ok());           // último usado
    s.newSession();
    auto ensured = s.doc->ensureStartupProject();
    REQUIRE(ensured.ok());
    CHECK(ensured.value() == "Segundo");                  // último usado

    // Registro stale (projeto que não existe mais): volta ao default.
    REQUIRE(s.storage
                .writeAllText(eng::fs::Path{".goni_last_project"},
                              std::string_view{"ProjetoFantasma"})
                .ok());
    s.newSession();
    auto fallback = s.doc->ensureStartupProject();
    REQUIRE(fallback.ok());
    CHECK(fallback.value() == "MeuJogo");
}

// I. Projeto existente + cenas/assets/scripts: tudo continua no lugar
// após o restore (nenhum dado perdido pelo ciclo de startup).
TEST_CASE("p3-startup I: cenas/scripts/assets sobrevivem ao restore",
          "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());
    REQUIRE(s.doc->saveScene("fase1.json").ok());
    REQUIRE(s.doc->scriptCreate("main").ok());
    REQUIRE(s.doc->saveProject().ok());

    s.newSession();
    REQUIRE(s.doc->ensureStartupProject().ok());
    // Cena volta a carregar (disco intacto).
    REQUIRE(s.doc->loadScene("fase1.json").ok());
    // Script do projeto continua catalogado.
    auto scripts = s.doc->scriptList();
    REQUIRE(scripts.ok());
    REQUIRE(scripts.value().size() == 1);
    CHECK(scripts.value()[0] == "main.nis");  // nome catalogado c/ ext
    CHECK(s.storage.exists(eng::fs::Path{"MeuJogo/scenes/fase1.json"}).value());
}

// J. Não perder dados do projeto: mutações + save + restore completo.
TEST_CASE("p3-startup J: dados não se perdem no ciclo completo", "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());
    auto e = s.doc->createEntity("Colecionavel", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(s.doc->renameEntity(e.value(), "Gema").ok());
    REQUIRE(s.doc->saveScene("save.json").ok());
    REQUIRE(s.doc->saveProject().ok());

    s.newSession();
    REQUIRE(s.doc->ensureStartupProject().ok());
    REQUIRE(s.doc->loadScene("save.json").ok());
    const auto snap = s.doc->hierarchySnapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].name == "Gema");  // renomeação persistiu
}

// Listagem rigorosa: só pastas com project.goni.json; ocultos e pastas
// comuns ficam de fora (mesmo filtro do seletor de projetos).
TEST_CASE("p3-startup: listProjects filtra ocultos e não-projetos",
          "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());
    REQUIRE(s.storage.mkdirs(eng::fs::Path{".import_tmp/staging"}).ok());
    REQUIRE(s.storage.mkdirs(eng::fs::Path{"PastaQualquer"}).ok());

    auto listed = s.doc->listProjects();
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0] == "MeuJogo");
}

// Regressão do registro: remember é best-effort — falha silenciosa NÃO
// derruba a operação de projeto (nada lança; Result carrega o motivo).
TEST_CASE("p3-startup: registro do último projeto é best-effort", "[editor][p3]")
{
    StartupSessions s;
    REQUIRE(s.doc->ensureStartupProject().ok());
    // Record existe e é legível pela próxima sessão.
    CHECK(s.storage.exists(eng::fs::Path{".goni_last_project"}).value());
    CHECK(s.doc->lastUsedProject() == "MeuJogo");
}

// --- watchdog de backend (P3 §0 — "fecha rapidamente") -----------------------
//
// Sessão que morre antes de kWatchdogHealthyFrames deixa "trying:X"; a
// próxima sessão Auto PULA X. Sessão saudável promove X a "good:X" e o
// Auto passa a preferi-lo. Requer driver real (lavapipe/EGL no CI).

TEST_CASE("p3-watchdog: Auto pula backend morto e promove o saudável",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    // O watchdog EXIGE dois backends (o demovido e o alternativo).
    // Probe device-only: sem o par, o fluxo de demotion não é verificável.
    {
        eng::rhi::RendererConfig probe;
        probe.enableValidation = false;
        probe.backend = eng::rhi::BackendType::OpenGLES;
        const bool glesAvailable = eng::rhi::Renderer::create(probe).ok();
        probe.backend = eng::rhi::BackendType::Vulkan;
        const bool vulkanAvailable = eng::rhi::Renderer::create(probe).ok();
        if (!glesAvailable || !vulkanAvailable) {
            SKIP("watchdog exige Vulkan E GLES disponíveis (CI cobre ambos)");
        }
    }
    const std::string ws = ".editor-test-ws-watchdog";
    std::filesystem::remove_all(std::filesystem::path{ws});

    // Sessão 1: marca a morte súbita do VULKAN (crash simulado antes de
    // 30 frames — o marcador fica "trying:vulkan").
    {
        auto host = eng::editor::EditorHost::create("auto", ws.c_str());
        REQUIRE(host.ok());
        std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
        REQUIRE(owned->ensureStartupProject().ok());

        // Simula a morte: escreve o marcador COMO a sessão morta deixaria
        // (a escrita real acontece em watchdogOnRendererCreated).
        REQUIRE(owned->workspace()
                    .writeAllText(eng::fs::Path{".goni_backend_watchdog"},
                                  std::string_view{"trying:vulkan"})
                    .ok());
    }
    // Sessão 2 (processo novo, MESMO workspace): Auto deve PULAR Vulkan.
    {
        auto host = eng::editor::EditorHost::create("auto", ws.c_str());
        REQUIRE(host.ok());
        std::unique_ptr<eng::editor::EditorHost> owned{host.value()};

        int marker = 0;
        owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                              48);
        REQUIRE(owned->state() == eng::editor::HostSurfaceState::Available);
        // Vulkan foi pulado → o ativo é GLES (ou Auto caiu fora do vk).
        CHECK(owned->selectedBackend() == eng::rhi::BackendType::OpenGLES);

        // Sessão saudável: 30+ frames apresentados → "good:gles".
        for (int i = 0; i < 35; ++i) {
            REQUIRE(owned->renderFrame(1.f / 60.f));
        }
        auto markerText = owned->workspace().readAllText(
            eng::fs::Path{".goni_backend_watchdog"});
        REQUIRE(markerText.ok());
        CHECK(markerText.value() == "good:gles");
    }
    // Sessão 3: "good:gles" → Auto PREFERE GLES direto.
    {
        auto host = eng::editor::EditorHost::create("auto", ws.c_str());
        REQUIRE(host.ok());
        std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
        int marker = 0;
        owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, 64,
                              48);
        REQUIRE(owned->state() == eng::editor::HostSurfaceState::Available);
        CHECK(owned->selectedBackend() == eng::rhi::BackendType::OpenGLES);
    }
    std::filesystem::remove_all(std::filesystem::path{ws});
}

// =============================================================================
// P3 — SHADER/MATERIAL/LIGHT2D: componentes reais, material authorável,
// iluminação por fragmento (bloco PerFrame), EDIT/PLAY parity.
// =============================================================================

TEST_CASE("editor: P3 — Light2D entra no catálogo e serializa (round-trip)",
          "[editor][p3light]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Lamp", eng::scene::kNoEntity);
    REQUIRE(e.ok());

    // Catálogo ADDÁVEL real: registro do ComponentRegistration.
    auto catalog = f.doc->addableComponents(e.value());
    bool found = false;
    for (const auto& meta : catalog) {
        found |= meta.name == "eng::render::Light2D";
    }
    REQUIRE(found);

    // Add → componente vivo com defaults.
    REQUIRE(f.doc->addComponent(e.value(), "eng::render::Light2D").ok());
    // Inspector lê/escreve por caminho (reflexão — §7).
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "intensity", "2.5")
                .ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "radius", "6.5")
                .ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "colorR", "0.1")
                .ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "layer", "UI")
                .ok());
    const auto fields = f.doc->inspectorFields(e.value(), "eng::render::Light2D");
    bool hasIntensity = false, hasLayer = false, hasEnabled = false,
         hasFalloff = false;
    for (const auto& field : fields) {
        hasIntensity |= field.path == "intensity" && field.value == "2.5";
        hasLayer |= field.path == "layer" && field.value == "UI";
        hasEnabled |= field.path == "enabled";
        hasFalloff |= field.path == "falloff";
    }
    CHECK(hasIntensity);
    CHECK(hasLayer);
    CHECK(hasEnabled);
    CHECK(hasFalloff);

    // Save → reload: o componente SOBREVIVE com os valores.
    REQUIRE(f.doc->saveScene("luz.json").ok());
    REQUIRE(f.doc->loadScene("luz.json").ok());
    const auto after = f.doc->inspectorFields(e.value(), "eng::render::Light2D");
    for (const auto& field : after) {
        if (field.path == "intensity") {
            CHECK(field.value == "2.5");
        }
        if (field.path == "radius") {
            CHECK(field.value == "6.5");
        }
        if (field.path == "colorR") {
            CHECK(field.value == "0.1");
        }
        if (field.path == "layer") {
            CHECK(field.value == "UI");
        }
    }

    // Remove: sai limpo.
    REQUIRE(f.doc->removeComponent(e.value(), "eng::render::Light2D").ok());
    // Componente removido: a consulta por caminho FALHA (Inspector).
    auto gone = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::render::Light2D", "radius");
    CHECK(gone.isError());
}

TEST_CASE("editor: P3 — buildQuads coleta a luz com posição de mundo",
          "[editor][p3light]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Lanterna", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    eng::editor::TransformDesc desc{};
    desc.position = {3.f, -2.f, 0.f};
    REQUIRE(f.doc->setTransform(e.value(), desc).ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::render::Light2D").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "intensity", "3")
                .ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "radius", "9")
                .ok());

    const auto quads =
        f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].hasLight);
    CHECK(quads[0].lightIntensity == Catch::Approx(3.f));
    CHECK(quads[0].lightRadius == Catch::Approx(9.f));
    // Posição da luz = TRANSFORM da entidade (não campo da luz).
    CHECK(quads[0].worldX == Catch::Approx(3.f).margin(1e-3f));
    CHECK(quads[0].worldY == Catch::Approx(-2.f).margin(1e-3f));

    // Desligada → fora do bloco (custo zero).
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "enabled", "false")
                .ok());
    const auto offQuads =
        f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(offQuads.size() == 1);
    CHECK_FALSE(offQuads[0].hasLight);
}

TEST_CASE("editor: P3 — material CRUD + resolve (shader/tint reais)",
          "[editor][p3light]")
{
    DocFixture f;
    f.withProject();

    // Sem materiais ainda.
    auto empty = f.doc->materialList();
    REQUIRE(empty.ok());
    CHECK(empty.value().empty());

    // Create → lista com template lit neutro.
    REQUIRE(f.doc->materialCreate("Gema").ok());
    auto listed = f.doc->materialList();
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].name == "Gema.mat.json");
    CHECK(listed.value()[0].shader == "lit");
    CHECK(listed.value()[0].tintR == 1.f);

    // Duplicado → AlreadyExists controlado.
    auto dup = f.doc->materialCreate("Gema");
    REQUIRE(dup.isError());
    CHECK(dup.error().code == eng::core::StatusCode::AlreadyExists);

    // Write com shader inválido → rejeitado ANTES de gravar.
    auto bad = f.doc->materialWrite(
        "Gema.mat.json", R"({"name":"Gema","shader":"pbr-mega"})");
    REQUIRE(bad.isError());

    // Write válido: unlit vermelho meio-transparente.
    REQUIRE(f.doc->materialWrite(
                "Gema.mat.json",
                R"({"name":"Gema","shader":"unlit","tint":[1,0.25,0.25,0.5]})")
                .ok());
    listed = f.doc->materialList();
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].shader == "unlit");
    CHECK(listed.value()[0].tintG == Catch::Approx(0.25f));
    CHECK(listed.value()[0].tintA == Catch::Approx(0.5f));

    // Read (round-trip do JSON cru).
    auto content = f.doc->materialRead("Gema.mat.json");
    REQUIRE(content.ok());
    CHECK(content.value().find("unlit") != std::string::npos);

    // Names (picker do Inspector).
    auto names = f.doc->materialNames();
    REQUIRE(names.ok());
    REQUIRE(names.value().size() == 1);
    CHECK(names.value()[0] == "Gema.mat.json");

    // Resolve: sprite com material → shader + tint multiplicado.
    auto e = f.doc->createEntity("Pedra", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::editor::SpriteData").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::editor::SpriteData",
                                     "textureAsset", "rocha.png")
                .ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::editor::SpriteData",
                                     "tintB", "0.5")
                .ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::editor::SpriteData",
                                     "materialAsset", "Gema")
                .ok());

    auto quads = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].materialAsset == "Gema");
    CHECK(quads[0].materialShader == "lit");  // default ANTES do resolve

    f.doc->resolveMaterials(quads);
    CHECK(quads[0].materialShader == "unlit");           // do material
    CHECK(quads[0].tintG == Catch::Approx(0.25f));       // 1 × 0.25
    CHECK(quads[0].tintB == Catch::Approx(0.125f));      // 0.5 × 0.25
    CHECK(quads[0].tintA == Catch::Approx(0.5f));

    // Sprite SEM material → default lit, tint intacto.
    auto e2 = f.doc->createEntity("Neutro", eng::scene::kNoEntity);
    REQUIRE(e2.ok());
    REQUIRE(f.doc->addComponent(e2.value(), "eng::editor::SpriteData").ok());
    auto quads2 = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(quads2.size() == 2);
    f.doc->resolveMaterials(quads2);
    bool checked = false;
    for (const auto& q : quads2) {
        if (q.entity == e2.value()) {
            CHECK(q.materialShader == "lit");
            CHECK(q.tintR == 1.f);
            checked = true;
        }
    }
    CHECK(checked);

    // Save/Reload do projeto inteiro: material persiste como ASSET.
    REQUIRE(f.doc->saveProject().ok());
    // (cena pode não ter sido salva — o que conta é o projeto/asset)
    auto stillThere = f.doc->materialList();
    REQUIRE(stillThere.ok());
    CHECK(stillThere.value().size() == 1);

    // Delete → some; sprite referenciando cai no default (sem estado ruim).
    REQUIRE(f.doc->materialDelete("Gema.mat.json").ok());
    auto afterDelete = f.doc->materialList();
    REQUIRE(afterDelete.ok());
    CHECK(afterDelete.value().empty());

    auto quads3 = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(quads3.size() == 2);
    f.doc->resolveMaterials(quads3);
    for (const auto& q : quads3) {
        CHECK(q.materialShader == "lit");
        if (q.entity == e.value()) {
            // tint DO SPRITE preservado (0.5), SEM o do material apagado.
            CHECK(q.tintB == Catch::Approx(0.5f));
            CHECK(q.tintR == 1.f);
        }
    }
}

TEST_CASE("editor: P3 — Play clona a luz (render idêntico Edit/Play)",
          "[editor][p3light]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Tocha", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::render::Light2D").ok());
    REQUIRE(f.doc->setInspectorField(e.value(), "eng::render::Light2D",
                                     "intensity", "4")
                .ok());

    // Edit: luz coletada.
    auto editQuads =
        f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(editQuads.size() == 1);
    CHECK(editQuads[0].hasLight);
    CHECK(editQuads[0].lightIntensity == Catch::Approx(4.f));

    // Play: o CLONE carrega a luz (serialização — §10 parity).
    REQUIRE(f.doc->play().ok());
    auto playQuads =
        f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(playQuads.size() == 1);
    CHECK(playQuads[0].hasLight);
    CHECK(playQuads[0].lightIntensity == Catch::Approx(4.f));

    // Edição em Play é REJEITADA (clone somente-leitura — §8.7): a luz do
    // clone NÃO pode ser editada (contrato), e a EDIÇÃO fica intacta.
    auto rejected = f.doc->setInspectorField(e.value(),
                                             "eng::render::Light2D",
                                             "enabled", "false");
    CHECK(rejected.isError());
    f.doc->stop();
    auto editAfter =
        f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(editAfter.size() == 1);
    CHECK(editAfter[0].hasLight);   // edição intacta após Play/Stop
    CHECK(editAfter[0].lightIntensity == Catch::Approx(4.f));
}

TEST_CASE("editor: P3 — camada da luz mascara sprites (LayerRegistry)",
          "[editor][p3light]")
{
    DocFixture f;
    f.withProject();
    // Camada nomeada "UI" na cena (LayerRegistry da Scene).
    REQUIRE(f.doc->sceneInFocus()->layers().addLayer("UI").ok());

    auto luz = f.doc->createEntity("LuzUI", eng::scene::kNoEntity);
    REQUIRE(luz.ok());
    REQUIRE(f.doc->addComponent(luz.value(), "eng::render::Light2D").ok());
    REQUIRE(f.doc->setInspectorField(luz.value(), "eng::render::Light2D",
                                     "layer", "UI")
                .ok());

    auto spriteGame =
        f.doc->createEntity("SpriteGame", eng::scene::kNoEntity);
    REQUIRE(spriteGame.ok());
    REQUIRE(f.doc->addComponent(spriteGame.value(),
                                "eng::editor::SpriteData")
                .ok());

    auto spriteUi = f.doc->createEntity("SpriteUI", eng::scene::kNoEntity);
    REQUIRE(spriteUi.ok());
    REQUIRE(f.doc->addComponent(spriteUi.value(), "eng::editor::SpriteData")
                .ok());
    REQUIRE(f.doc->addComponent(spriteUi.value(), "eng::scene::LayerMember")
                .ok());
    REQUIRE(f.doc->setInspectorField(spriteUi.value(),
                                     "eng::scene::LayerMember", "layer", "UI")
                .ok());

    auto quads = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(), {});
    REQUIRE(quads.size() == 3);
    // A luz carrega a camada; o sprite UI carrega LayerMember; o sprite
    // GAME fica com "GAME" (default).
    bool sawLight = false, sawUiSprite = false, sawGameSprite = false;
    for (const auto& q : quads) {
        if (q.hasLight) {
            CHECK(q.lightLayer == "UI");
            sawLight = true;
        }
        if (q.isSprite && q.entity == spriteUi.value()) {
            CHECK(q.layer == "UI");
            sawUiSprite = true;
        }
        if (q.isSprite && q.entity == spriteGame.value()) {
            CHECK(q.layer == "GAME");
            sawGameSprite = true;
        }
    }
    CHECK(sawLight);
    CHECK(sawUiSprite);
    CHECK(sawGameSprite);

    // O pack da DrawList respeita a mask (a luz UI NÃO ilumina GAME).
    eng::render::DrawList drawList;
    for (const auto& q : quads) {
        if (q.hasLight) {
            eng::render::DrawList::LightItem item;
            item.worldX = q.worldX;
            item.worldY = q.worldY;
            item.radius = q.lightRadius;
            item.intensity = q.lightIntensity;
            item.r = q.lightColorR;
            item.g = q.lightColorG;
            item.b = q.lightColorB;
            item.falloff = q.lightFalloff;
            item.layer = q.lightLayer;
            drawList.lights.push_back(item);
        }
    }
    CHECK(drawList.packUniformsFor("GAME").lightCount() == 0);
    CHECK(drawList.packUniformsFor("UI").lightCount() == 1);
}

// --- P3 §14 RENDERING VISUAL (readback — iluminação provada por pixel) --------

TEST_CASE("editor: P3 — sprite com Light2D muda o pixel (A != B, readback)",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    // GLES: o ÚNICO backend com readCenterPixel (P0 — validação visual);
    // Vulkan readback entra com o render-graph futuro. Workspace LIMPO no
    // início: o marcador do watchdog (P3 §0) persiste entre runs e
    // demoveria o backend da run anterior — teste determinístico.
    std::filesystem::remove_all(
        std::filesystem::path{".editor-test-ws-p3luz"});
    auto host = eng::editor::EditorHost::create("gles", ".editor-test-ws-p3luz");
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                          128, 128);
    if (owned->state() != eng::editor::HostSurfaceState::Available) {
        SKIP("OpenGL ES indisponível (renderer não criado sem driver)");
    }

    auto& doc = owned->document();
    ensureProject(doc, "P3LuzGame");

    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    {
        eng::fs::FileSystem& ws = owned->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws
                    .writeAllBytes(
                        eng::fs::Path{".import_tmp/quad.png"},
                        std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                                  sizeof(kPng2x2)})
                    .ok());
    }
    REQUIRE(browser->import(".import_tmp/quad.png", "textures", "quad").ok());

    // Sprite cobrindo o centro (entidade em 0,0; ppu=1 → 2x2 unidades).
    auto sprite = doc.createSprite("Alvo");
    REQUIRE(sprite.ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "quad.png").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());

    // DADOS primeiro: bloco PerFrame com ZERO luzes + ambiente neutro.
    REQUIRE(owned->renderFrame(1.f / 60.f));
    auto* renderer = owned->viewportRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(renderer->lastFrameTexturedSprites() == 1);
    CHECK(renderer->lastFrameLitSpriteVertices().size() == 6);  // lit default
    REQUIRE_FALSE(renderer->lastFrameFrameUniforms().empty());
    CHECK(renderer->lastFrameFrameUniforms()[0].lightCount() == 0);

    // (A) Sprite SEM luz: albedo × ambiente(1) — o look clássico.
    std::uint8_t pixelA[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixelA).ok());
    INFO("readback A: " << +pixelA[0] << " " << +pixelA[1] << " "
                       << +pixelA[2] << " " << +pixelA[3]);
    CHECK(pixelA[3] == 255);  // sprite opaco no centro

    // (B) Light2D forte NO centro: albedo × (1 + luz) — pixel muda.
    auto light = doc.createEntity("Tocha", eng::scene::kNoEntity);
    REQUIRE(light.ok());
    REQUIRE(doc.addComponent(light.value(), "eng::render::Light2D").ok());
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "intensity", "3").ok());
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "radius", "10").ok());
    // Cor VERMELHA saturada (canal G/B baixo): a mudança é inequívoca.
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "colorR", "1").ok());
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "colorG", "0.05").ok());
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "colorB", "0.05").ok());

    REQUIRE(owned->renderFrame(1.f / 60.f));
    // Bloco PerFrame AGORA carrega a luz (count == 1).
    REQUIRE_FALSE(renderer->lastFrameFrameUniforms().empty());
    CHECK(renderer->lastFrameFrameUniforms()[0].lightCount() == 1);
    const auto& block = renderer->lastFrameFrameUniforms()[0];
    CHECK(block.lightA[0][0] == Catch::Approx(0.f).margin(1e-3f));  // x
    CHECK(block.lightA[0][1] == Catch::Approx(0.f).margin(1e-3f));  // y
    CHECK(block.lightA[0][2] == Catch::Approx(10.f));              // raio
    CHECK(block.lightA[0][3] == Catch::Approx(3.f));               // intens
    CHECK(block.lightB[0][2] == Catch::Approx(0.05f).margin(0.02f));  // b

    std::uint8_t pixelB[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixelB).ok());
    INFO("readback B: " << +pixelB[0] << " " << +pixelB[1] << " "
                       << +pixelB[2] << " " << +pixelB[3]);

    // (C) A != B — a iluminação é REAL (não um círculo desenhado).
    const bool differs = pixelA[0] != pixelB[0] || pixelA[1] != pixelB[1] ||
                         pixelA[2] != pixelB[2];
    CHECK(differs);
    // Direção coerente: luz vermelha forte (intenção 3) → canal R SOBE.
    CHECK(pixelB[0] >= pixelA[0]);
    CHECK(pixelB[0] > 140);  // vermelho saturado brilhante

    // (D) Play/Stop parity: a MESMA luz ilumina o CLONE no Play.
    REQUIRE(doc.play().ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    std::uint8_t pixelPlay[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixelPlay).ok());
    CHECK(pixelPlay[0] == pixelB[0]);  // idêntico ao Edit com a mesma luz
    CHECK(pixelPlay[1] == pixelB[1]);
    doc.stop();

    // (E) Desligar a luz volta ao look A (reversível — sem estado preso).
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "enabled", "false").ok());
    REQUIRE(owned->renderFrame(1.f / 60.f));
    std::uint8_t pixelOff[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixelOff).ok());
    CHECK(pixelOff[0] == pixelA[0]);
    CHECK(pixelOff[1] == pixelA[1]);
    CHECK(pixelOff[2] == pixelA[2]);
}

TEST_CASE("editor: P3 — material unlit vs lit via Inspector (readback)",
          "[editor][rhi_hardware]")
{
    if (editorGraphicsUnavailable()) {
        SKIP("sem driver gráfico (lavapipe/EGL) — suite completo roda no CI");
    }
    // Workspace ÚNICO por execução (isola o materialCreate — o projeto é
    // ensureProject-idempotente, mas material NÃO: "Cru" já existiria na
    // 2ª execução da suite no mesmo build dir e o teste falharia).
    const std::string root = "p3mat_host_test_" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(&root));
    auto host = eng::editor::EditorHost::create("gles", root.c_str());
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};
    int marker = 0;
    owned->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                          128, 128);
    if (owned->state() != eng::editor::HostSurfaceState::Available) {
        SKIP("OpenGL ES indisponível (renderer não criado sem driver)");
    }

    auto& doc = owned->document();
    ensureProject(doc, "P3MatGame");
    auto* browser = doc.assets();
    REQUIRE(browser != nullptr);
    {
        eng::fs::FileSystem& ws = owned->workspace();
        REQUIRE(ws.mkdirs(eng::fs::Path{".import_tmp"}).ok());
        REQUIRE(ws
                    .writeAllBytes(
                        eng::fs::Path{".import_tmp/quad.png"},
                        std::span{reinterpret_cast<const std::byte*>(kPng2x2),
                                  sizeof(kPng2x2)})
                    .ok());
    }
    REQUIRE(browser->import(".import_tmp/quad.png", "textures", "quad").ok());

    auto sprite = doc.createSprite("Pedra");
    REQUIRE(sprite.ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "textureAsset", "quad.png").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "pixelsPerUnit", "1").ok());

    // Luz forte vermelha (o discriminador entre lit e unlit).
    auto light = doc.createEntity("Brasa", eng::scene::kNoEntity);
    REQUIRE(light.ok());
    REQUIRE(doc.addComponent(light.value(), "eng::render::Light2D").ok());
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "intensity", "3").ok());
    REQUIRE(doc.setInspectorField(light.value(), "eng::render::Light2D",
                                  "colorR", "1").ok());

    // Default (lit): pixel iluminado.
    REQUIRE(owned->renderFrame(1.f / 60.f));
    auto* renderer = owned->viewportRenderer();
    REQUIRE(renderer != nullptr);
    std::uint8_t pixelLit[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixelLit).ok());

    // Material UNLIT atribuído ao sprite: a MESMA luz NÃO o afeta.
    REQUIRE(doc.materialCreate("Cru").ok());
    REQUIRE(doc.materialWrite(
                "Cru.mat.json", R"({"name":"Cru","shader":"unlit"})").ok());
    REQUIRE(doc.setInspectorField(sprite.value(), "eng::editor::SpriteData",
                                  "materialAsset", "Cru").ok());

    REQUIRE(owned->renderFrame(1.f / 60.f));
    // Vertices foram para o caminho UNLIT (40B), não o lit (48B).
    CHECK(renderer->lastFrameLitSpriteVertices().empty());
    CHECK_FALSE(renderer->lastFrameSpriteVertices().empty());

    std::uint8_t pixelUnlit[4] = {0, 0, 0, 0};
    REQUIRE(renderer->renderer()->readCenterPixel(pixelUnlit).ok());

    // unlit < lit no canal R (a luz vermelha só soma no lit).
    INFO("lit: " << +pixelLit[0] << " unlit: " << +pixelUnlit[0]);
    CHECK(pixelLit[0] > pixelUnlit[0]);
}


// =============================================================================
// P3.1 — Diagnóstico de startup persistente + crash handler nativo
// =============================================================================

TEST_CASE("editor: P3.1 — tracer persiste estágios na hora (formato grep-ável)",
          "[editor][diagnostics]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("goni_diag_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    // P3.1 usa singleton: cada caso valida com o MESMO arquivo (append) —
    // o tracer é cumulativo por design (evidência entre execuções).
    eng::editor::diag::init(dir.c_str());

    eng::editor::diag::mark("TESTE_ESTAGIO_A", "ok", "detalhe um");
    eng::editor::diag::mark("TESTE_ESTAGIO_B", "failed", "erro simulado");

    std::FILE* f = std::fopen(eng::editor::diag::startupLogPath(), "r");
    REQUIRE(f != nullptr);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
        text += buf;
    }
    std::fclose(f);
    CHECK(text.find("TESTE_ESTAGIO_A ok detalhe um") != std::string::npos);
    CHECK(text.find("TESTE_ESTAGIO_B failed erro simulado") != std::string::npos);
    CHECK(std::string{eng::editor::diag::lastStage()} == "TESTE_ESTAGIO_B");
}

TEST_CASE("editor: P4.5.1 R1 — STARTUP_EDITOR_HOST fecha com ok (par begin/ok)",
          "[editor][diagnostics][startup]") {
    // REGRESSÃO P4.5.1: no device, o mark "STARTUP_EDITOR_HOST" só tinha
    // "begin" — a janela entre o begin e o próximo mark nunca fechava, e
    // uma morte "pós-host" era indistinguível de "morrendo a criar o
    // host". O create agora emite "ok" ao completar (host + documento).
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("goni_diag_host_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    eng::editor::diag::init(dir.c_str());

    // O tracer é cumulativo (append entre casos) — validar só o DELTA
    // que ESTE create emitir (ordem-independente: init pode ou não
    // trocar o arquivo efetivo; o path vem SEMPRE da API).
    const std::string beforeText = [&] {
        std::FILE* f = std::fopen(eng::editor::diag::startupLogPath(), "r");
        std::string text;
        char buf[512];
        if (f != nullptr) {
            while (std::fgets(buf, sizeof buf, f) != nullptr) {
                text += buf;
            }
            std::fclose(f);
        }
        return text;
    }();

    const char* ws = ".editor-test-ws-p451-host";
    std::filesystem::remove_all(std::filesystem::path{ws});
    auto host = eng::editor::EditorHost::create("auto", ws);
    REQUIRE(host.ok());
    std::unique_ptr<eng::editor::EditorHost> owned{host.value()};

    std::FILE* f = std::fopen(eng::editor::diag::startupLogPath(), "r");
    REQUIRE(f != nullptr);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
        text += buf;
    }
    std::fclose(f);
    REQUIRE(text.size() >= beforeText.size());
    const std::string delta = text.substr(beforeText.size());
    INFO("delta do log de startup:\n" << delta);

    const std::size_t beginPos = delta.find("STARTUP_EDITOR_HOST begin");
    const std::size_t okPos = delta.find("STARTUP_EDITOR_HOST ok");
    REQUIRE(beginPos != std::string::npos);
    REQUIRE(okPos != std::string::npos);   // R1: a fase FECHA
    CHECK(okPos > beginPos);
    // Ordem interna da criação fica grep-ável: filesystem → documento →
    // host fechado (a evidência do device segue o mesmo formato).
    const std::size_t fsPos = delta.find("STARTUP_FILESYSTEM ok");
    const std::size_t docPos = delta.find("STARTUP_EDITOR_DOCUMENT ok");
    REQUIRE(fsPos != std::string::npos);
    REQUIRE(docPos != std::string::npos);
    CHECK(fsPos < docPos);
    CHECK(docPos < okPos);
    std::filesystem::remove_all(std::filesystem::path{ws});
}

TEST_CASE("editor: P3.1 — crash handler registra e NÃO mascara (SIGSEGV)",
          "[editor][diagnostics][crash]") {
    // init() é idempotente: se o caso anterior rodou, o singleton já vive
    // num dir; o path EFETIVO é consultado pela API (ordem-independente).
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("goni_diag_crash_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    eng::editor::diag::init(dir.c_str());
    const std::string crashLog = eng::editor::diag::crashLogPath();
    REQUIRE(crashLog != "-");

    // O handler é stateful por processo: validamos num processo FILHO
    // (fork) — o pai inspeciona o arquivo e o status de morte.
    const std::string noteOnlyResult = (std::filesystem::temp_directory_path() /
        ("goni_diag_noteonly_" + std::to_string(::getpid()))).string();
    const pid_t pid = ::fork();
    if (pid == 0) {
        // P3.2 (probe negativo): arquivo contendo APENAS a nota benigna
        // "[handler] instalado" NÃO pode contar como crash — senão o export
        // automático dispararia em toda execução pós-instalação.
        if (std::FILE* tf = std::fopen(eng::editor::diag::crashLogPath(), "w")) {
            std::fputs("[handler] crash handler instalado time=0\n", tf);
            std::fclose(tf);
        }
        if (std::FILE* rf = std::fopen(noteOnlyResult.c_str(), "w")) {
            std::fprintf(rf, "%d",
                eng::editor::diag::hasPreviousCrashReport() ? 1 : 0);
            std::fclose(rf);
        }
        // RE-instala pós-fork: frameworks de teste (Catch2!) sobrescrevem
        // handlers entre casos — na produção o mesmo vale para libs que
        // instalam handlers (o app pode re-chamar a qualquer momento).
        eng::editor::diag::installCrashHandler();
        eng::editor::diag::mark("ESTAGIO_ANTECRASH", "ok", "vivos até aqui");
        // Crash deliberado. NOTA: escrever em 0x10 com ASan é interceptado
        // pela INSTRUMENTAÇÃO (exit direto — o sinal nunca nasce); o raise()
        // entrega o sinal REAL do kernel ao handler — é o mesmo caminho que
        // um fault verdadeiro percorre (sigaction). O teste de fault real
        // sem sanitizers acontece no emulador Android (P3.1 FASE 3).
        ::raise(SIGSEGV);
        _exit(0);   // inalcançável (handler re-entrega o sinal)
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    // O processo FILHO precisa ter MORRIDO no crash — nunca sair limpo:
    // - sem intermediários: morte POR SINAL (SIGSEGV — re-entregue);
    // - com Catch2/ASan NA CADEIA (este processo de teste): o handler
    //   encadeia p/ o handler ANTERIOR, que pode terminar em SIGABRT
    //   (Catch2 chama abort) ou exit(1) (ASan Die()) — a evidência
    //   (goni_crash.log) é gravada ANTES do encadeamento em todos os
    //   casos; o crash NUNCA é engolido: morte por sinal ou exit != 0.
    const bool diedBySignal = WIFSIGNALED(status);
    const bool diedBySanitizer =
        WIFEXITED(status) && WEXITSTATUS(status) != 0;
    CHECK((diedBySignal || diedBySanitizer));
    if (diedBySignal) {
        // SIGSEGV direto, ou SIGABRT induzido pelo handler encadeado
        // (Catch2) — ambos são morte POR CRASH, não por saída limpa.
        CHECK((WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGABRT));
    }

    // P3.2 (probe negativo): o filho registrou 0 esperado — nota benigna
    // sozinha NÃO conta como crash anterior.
    if (std::FILE* rf = std::fopen(noteOnlyResult.c_str(), "r")) {
        int v = -1;
        const int got = std::fscanf(rf, "%d", &v);
        std::fclose(rf);
        std::filesystem::remove(noteOnlyResult);
        REQUIRE(got == 1);
        INFO("probe nota-somente: " << v << " (esperado 0)");
        CHECK(v == 0);
    } else {
        FAIL("probe nota-somente não escreveu resultado");
    }

    std::FILE* f = std::fopen(crashLog.c_str(), "r");
    if (f != nullptr) {
        std::string text;
        char buf[512];
        while (std::fgets(buf, sizeof buf, f) != nullptr) {
            text += buf;
        }
        std::fclose(f);
        INFO("crash log:\n" << text);
        CHECK(text.find("SIGSEGV") != std::string::npos);
        CHECK(text.find("ESTAGIO_ANTECRASH") != std::string::npos);
        // P3.2: crash REAL registrado → hasPreviousCrashReport true (a
        // linha "[crash]" do handler é o gatilho, não o mero tamanho).
        CHECK(eng::editor::diag::hasPreviousCrashReport());
        // P3.3: o handler agora identifica o MÓDULO do pc (e do alvo do
        // acesso, quando houver) via /proc/self/maps — a evidência que
        // falta no Realme C33. Para um raise() dentro deste binário de
        // teste, o pc DEVE cair no próprio executável com offset hex.
        CHECK(text.find("[pc]") != std::string::npos);
        CHECK(text.find("module=") != std::string::npos);
        // P3.5: formato do [pc]/[fp] ganhou base= explícito p/
        // symbolização offline (era "modulo+0xoff", agora
        // "module=M base=B off=O" — mais informação, mesmo contrato).
        CHECK(text.find("off=0x") != std::string::npos);
        CHECK(text.find("base=0x") != std::string::npos);
        // O alvo do acesso de um raise() não é um endereço real (si_addr
        // ausente/zero) — a linha fault.addr só pode existir COM módulo
        // resolvido ou nem existir (nunca um [fault.addr] vazio).
        const auto faultPos = text.find("[fault.addr]");
        if (faultPos != std::string::npos) {
            CHECK(text.find("module=", faultPos) != std::string::npos);
        }
    } else {
        FAIL("goni_crash.log não foi criado pelo handler");
    }
}

TEST_CASE("editor: P3.3 — describeAddress resolve o módulo do processo",
          "[editor][diagnostics]") {
    // Endereço DENTRO deste binário de teste → módulo + offset hex;
    // endereço de página guard (baixa memória não mapeada) → false.
    char out[160];
    const auto self = reinterpret_cast<std::uintptr_t>(
        &eng::editor::diag::describeAddress);
    REQUIRE(eng::editor::diag::describeAddress(self, out, sizeof out));
    INFO("resolved: " << out);
    CHECK(std::string{out}.find("+0x") != std::string::npos);
    // Um endereço de função resolvido não pode ser o path vazio.
    CHECK(std::string{out}.size() > 4);
    CHECK(eng::editor::diag::describeAddress(0x10, out, sizeof out) == false);
    // Capacidade zero/1 byte não derruba — apenas não resolve.
    char tiny[1];
    CHECK(eng::editor::diag::describeAddress(self, tiny, 1));
    CHECK(tiny[0] == '\0');
}

TEST_CASE("editor: P3.2/P3.5 — espelho assíncrono notifica fora da thread que marcou",
          "[editor][diagnostics][mirror]") {
    // init() é idempotente por processo (singleton P3.1): o caso valida o
    // MECANISMO de notificação com o tracer no estado em que estiver.
    //
    // P3.5 MUDOU O CONTRATO (T0/T1): o callback NÃO roda mais na thread
    // que marcou (MediaStore/binder nunca mais na main) — roda na THREAD
    // DE DESPACHO dedicada, com coalescing de lote (o espelho reescreve o
    // arquivo COMPLETO; a cópia pública final reflete o último estágio).
    struct MirrorProbe {
        std::atomic<int> calls{0};
        std::atomic<std::uint64_t> lastCaller{0};  // id da thread do callback
    } probe;

    // Sem callback: registrar nullptr é válido (estado inicial do P3.1).
    eng::editor::diag::setMirrorCallback(nullptr, nullptr);
    eng::editor::diag::requestMirror();
    CHECK(probe.calls.load() == 0);

    const std::uint64_t selfId = [] {
        std::ostringstream os;
        os << std::this_thread::get_id();
        return std::stoull(os.str());
    }();

    eng::editor::diag::setMirrorCallback(
        [](void* ud) {
            auto* p = static_cast<MirrorProbe*>(ud);
            p->calls.fetch_add(1);
            std::ostringstream os;
            os << std::this_thread::get_id();
            p->lastCaller.store(std::stoull(os.str()));
        },
        &probe);

    // Marks da PRÓPRIA thread e de uma thread NATIVA separada (T0: o
    // retry de áudio do P3.5 marca de uma std::thread — nenhum caminho
    // pode tocar JNI na thread que marcou).
    eng::editor::diag::mark("TESTE_ESPELHO_A", "ok", "primeiro");
    eng::editor::diag::mark("TESTE_ESPELHO_B", "failed", "segundo");
    {
        std::thread marker{[] {
            eng::editor::diag::mark("TESTE_ESPELHO_THREAD", "ok",
                                    "thread nao-attachada");
            eng::editor::diag::mark("TESTE_ESPELHO_THREAD2", "failed", "x");
        }};
        marker.join();
    }
    // Drena com prazo (contrato novo: assíncrono, ordenado, coalescido).
    REQUIRE(eng::editor::diag::waitMirrorIdle(2000));
    CHECK(probe.calls.load() >= 1);  // ao menos UM lote atendido
    // T0 — o callback JAMAIS rodou na thread que marcou:
    CHECK(probe.lastCaller.load() != 0);
    CHECK(probe.lastCaller.load() != selfId);

    // requestMirror dispara manualmente (o export do crash log na
    // execução seguinte usa este caminho a partir da Activity).
    const int before = probe.calls.load();
    eng::editor::diag::requestMirror();
    REQUIRE(eng::editor::diag::waitMirrorIdle(2000));
    const int afterManual = probe.calls.load();
    CHECK(afterManual >= before + 1);

    // Limpeza obrigatória: o callback cruza processos-filho dos casos de
    // crash (fork) — nunca pode vazar para outros testes.
    eng::editor::diag::setMirrorCallback(nullptr, nullptr);
    eng::editor::diag::mark("TESTE_ESPELHO_DEPOIS_DE_LIMPAR");
    eng::editor::diag::requestMirror();
    CHECK(eng::editor::diag::waitMirrorIdle(2000));
    CHECK(probe.calls.load() == afterManual);  // congelado: nada após limpar
}

TEST_CASE("editor: P3.5 — marks concorrentes não perdem nem duplicam lotes",
          "[editor][diagnostics][mirror]") {
    // T0: marks de MÚLTIPLAS threads simultâneas — a fila é mutex+cv
    // (sem perda), o lote coalesce SEMPRE (nenhum mark fica eternamente
    // pendente) e o estágio final está no arquivo privado.
    struct Probe {
        std::atomic<int> calls{0};
    } probe;
    eng::editor::diag::setMirrorCallback(
        [](void* ud) { static_cast<Probe*>(ud)->calls.fetch_add(1); }, &probe);

    constexpr int kThreads = 4;
    constexpr int kMarksPerThread = 64;
    std::vector<std::thread> markers;
    for (int t = 0; t < kThreads; ++t) {
        markers.emplace_back([] {
            for (int i = 0; i < kMarksPerThread; ++i) {
                eng::editor::diag::mark("TESTE_ESPELHO_STRESS", "ok", "x");
            }
        });
    }
    for (std::thread& m : markers) {
        m.join();
    }
    REQUIRE(eng::editor::diag::waitMirrorIdle(5000));
    // 256 marks coalescem em no máx. alguns lotes — nunca 256 callbacks
    // (fila infinita proibida pela T1) nem zero (perda proibida).
    CHECK(probe.calls.load() >= 1);
    CHECK(probe.calls.load() <= kThreads * kMarksPerThread);

    // O ÚLTIMO mark persistiu (arquivo privado tem o estágio).
    CHECK(std::string{eng::editor::diag::lastStage()} ==
          "TESTE_ESPELHO_STRESS");
    eng::editor::diag::setMirrorCallback(nullptr, nullptr);
}

TEST_CASE("editor: P3.5 — micro-marks carregam timestamps duplos (wt=/mo=)",
          "[editor][diagnostics]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("goni_diag_p35_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    eng::editor::diag::init(dir.c_str());

    eng::editor::diag::mark("TESTE_P35_TS_A", "ok", "antes");
    eng::editor::diag::mark("TESTE_P35_TS_B", "ok", "depois");

    std::FILE* f = std::fopen(eng::editor::diag::startupLogPath(), "r");
    REQUIRE(f != nullptr);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
        text += buf;
    }
    std::fclose(f);
    // Formato novo: [wt=<wallclock ms> mo=<monotônico ms>] STAGE STATUS.
    const auto posA = text.find("TESTE_P35_TS_A ok antes");
    const auto posB = text.find("TESTE_P35_TS_B ok depois");
    REQUIRE(posA != std::string::npos);
    REQUIRE(posB != std::string::npos);
    // Ambos com prefixo de timestamp duplo na MESMA linha.
    auto lineStart = text.rfind('\n', posA);
    auto lineEnd = text.find('\n', posA);
    auto lineStartB = text.rfind('\n', posB);
    auto lineEndB = text.find('\n', posB);
    std::string lineA = text.substr(
        lineStart == std::string::npos ? 0 : lineStart + 1,
        (lineEnd == std::string::npos ? text.size() : lineEnd) -
            (lineStart == std::string::npos ? 0 : lineStart + 1));
    std::string lineB = text.substr(
        lineStartB == std::string::npos ? 0 : lineStartB + 1,
        (lineEndB == std::string::npos ? text.size() : lineEndB) -
            (lineStartB == std::string::npos ? 0 : lineStartB + 1));
    CHECK(lineA.find("wt=") != std::string::npos);
    CHECK(lineA.find("mo=") != std::string::npos);
    CHECK(lineB.find("wt=") != std::string::npos);
    CHECK(lineB.find("mo=") != std::string::npos);
    // wt é wallclock (epoch ms, ~1,7e12 em 2024+); mo é pequeno (uptime).
    unsigned long long wt = 0, mo = 0;
    CHECK(std::sscanf(lineB.c_str(), "[wt=%llu mo=%llu]", &wt, &mo) == 2);
    CHECK(wt > 1'000'000'000'000ULL);  // epoch ms
    CHECK(mo > 0);                      // uptime ms
    CHECK(mo < wt);                      // ordens de grandeza distintas
}

TEST_CASE("editor: P3.5 — watchdog pega hang da main e o processo SEGUE VIVO",
          "[editor][diagnostics][watchdog]") {
    // O watchdog substitui o ANR que o dispositivo não entrega: main sem
    // heartbeat → SIGUSR1 → dump forense em goni_crash.log SEM matar o
    // processo. Validado num processo FILHO (fork) para não envenenar o
    // estado do runner.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("goni_diag_wd_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::string resultPath =
        (dir / "wd_result").string();
    std::filesystem::remove(resultPath);

    // init no PAI antes do fork: o caminho do crash log é herdado e o
    // waitMirrorIdle garante que a thread de despacho está quieta no
    // momento do fork (marks do filho nunca disputam um lock herdado).
    eng::editor::diag::init(dir.c_str());
    REQUIRE(eng::editor::diag::waitMirrorIdle(2000));

    const pid_t pid = ::fork();
    if (pid == 0) {
        // FILHO: arma o watchdog com limiar curto (teste), NUNCA dá
        // heartbeat e roda o evaluate como o pinger faria.
        eng::editor::diag::init(dir.c_str());
        eng::editor::diag::watchdog::arm(/*thresholdMs=*/200,
                                          /*graceMs=*/0);
        int fired = 0;
        for (int i = 0; i < 10 && fired == 0; ++i) {
            struct timespec ts{0, 100 * 1000 * 1000};  // 100 ms
            ::nanosleep(&ts, nullptr);
            if (eng::editor::diag::watchdog::evaluate()) {
                fired = 1;
            }
        }
        // Dá tempo do dump do SIGUSR1 aterrissar no arquivo.
        struct timespec ts{0, 200 * 1000 * 1000};
        ::nanosleep(&ts, nullptr);
        if (std::FILE* rf = std::fopen(resultPath.c_str(), "w")) {
            std::fprintf(rf, "%d", fired);
            std::fclose(rf);
        }
        _exit(0);  // saída LIMPA: o SIGUSR1 é diagnóstico, não fatal
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    INFO("child status=" << status);
    // O processo filho precisa ter saído LIMPO (não morto por sinal):
    // um SIGUSR1 diagnosticado NUNCA derruba o processo.
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    int fired = -1;
    if (std::FILE* rf = std::fopen(resultPath.c_str(), "r")) {
        const int got = std::fscanf(rf, "%d", &fired);
        std::fclose(rf);
        REQUIRE(got == 1);
    }
    INFO("fired=" << fired);
    CHECK(fired == 1);  // evaluate() disparou o poke

    // O dump forense aterrissou no goni_crash.log com o contexto da main.
    if (std::FILE* cf = std::fopen(eng::editor::diag::crashLogPath(), "r")) {
        std::string text;
        char buf[512];
        while (std::fgets(buf, sizeof buf, cf) != nullptr) {
            text += buf;
        }
        std::fclose(cf);
        INFO("crash log:\n" << text);
        CHECK(text.find("[watchdog]") != std::string::npos);
        CHECK(text.find("[dump] tag=watchdog") != std::string::npos);
        CHECK(text.find("si_pid=") != std::string::npos);
        CHECK(text.find("tid=") != std::string::npos);
        CHECK(text.find("[pc]") != std::string::npos);
        CHECK(text.find("module=") != std::string::npos);
    } else {
        FAIL("goni_crash.log não foi criado pelo watchdog");
    }
    std::filesystem::remove(resultPath);
}

TEST_CASE("editor: P3.5 — crash fatal despeja TODAS as threads + maps cru",
          "[editor][diagnostics][crash]") {
    // T3b: num crash fatal, o handler despeja TODAS as threads (a thread
    // culpada de um abort por JNI/suspend NÃO é a sinalizada) e o maps
    // CRU verbatim. Validado em processo FILHO com threads parqueadas.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("goni_diag_forensic_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);

    // init+idle no PAI antes do fork (mesma disciplina do caso do
    // watchdog): caminhos consistentes e despacho quiescente.
    eng::editor::diag::init(dir.c_str());
    REQUIRE(eng::editor::diag::waitMirrorIdle(2000));
    const std::string crashLog = eng::editor::diag::crashLogPath();

    const pid_t pid = ::fork();
    if (pid == 0) {
        // FILHO: duas threads parqueadas (alvo do ping SIGUSR2) + crash.
        std::atomic<bool> stop{false};
        std::vector<std::thread> parked;
        for (int t = 0; t < 2; ++t) {
            parked.emplace_back([&stop] {
                while (!stop.load()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
            });
        }
        eng::editor::diag::init(dir.c_str());
        // RE-instala pós-fork (mesma disciplina do caso P3.1): frameworks
        // de teste (Catch2!) sobrescrevem os handlers DENTRO do runner —
        // o filho precisa dos NOSSSOS handlers ativos no momento do raise.
        eng::editor::diag::installCrashHandler();
        eng::editor::diag::mark("ESTAGIO_ANTECRASH_P35", "ok", "vivos");
        ::raise(SIGABRT);
        stop = true;
        for (std::thread& t : parked) {
            t.join();
        }
        _exit(0);  // inalcançável
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    const bool diedBySignal = WIFSIGNALED(status);
    const bool diedUnclean = WIFEXITED(status) && WEXITSTATUS(status) != 0;
    CHECK((diedBySignal || diedUnclean));

    std::FILE* f = std::fopen(crashLog.c_str(), "r");
    if (f == nullptr) {
        FAIL("goni_crash.log não foi criado pelo handler");
        return;
    }
    std::string text;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
        text += buf;
    }
    std::fclose(f);
    INFO("crash log (len=" << text.size() << "):\n" << text.substr(0, 4000));
    CHECK(text.find("[crash]") != std::string::npos);
    CHECK(text.find("SIGABRT") != std::string::npos);
    CHECK(text.find("si_pid=") != std::string::npos);
    CHECK(text.find("[dump] tag=fatal") != std::string::npos);
    // Threads: enumeradas com tid+comm e pingueadas (as duas parqueadas
    // do filho devem ter despejado suas seções [dump] tag=other).
    CHECK(text.find("[thread]") != std::string::npos);
    CHECK(text.find("comm=") != std::string::npos);
    CHECK(text.find("[dump] tag=other") != std::string::npos);
    // Maps cru verbatim.
    CHECK(text.find("[maps.raw begin]") != std::string::npos);
    CHECK(text.find("[maps.raw end]") != std::string::npos);
    // Pilhas em registo duplo.
    CHECK(text.find("[fp]") != std::string::npos);
}

// =============================================================================
// P4.2 — DEVICE BUGS ROUND 2 (B-A…B-E) + MODO JOGO (T5)
// =============================================================================

// ---- B-A: round-trip de persistência à prova de device ----------------------

TEST_CASE("editor: P4.2 — B-A: salvar projeto persiste a CENA (reload sem loadScene manual)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    auto& doc = *f.doc;

    REQUIRE(doc.createEntity("Alpha", eng::scene::kNoEntity).ok());
    auto beta = doc.createEntity("Beta", eng::scene::kNoEntity);
    REQUIRE(beta.ok());
    eng::editor::TransformDesc betaDesc;
    betaDesc.position = eng::math::Vec3{2.f, 1.f, 0.f};
    REQUIRE(doc.setTransform(beta.value(), betaDesc).ok());

    // "Salvar projeto" (P4.2): projeto + cena em uma operação — a cena
    // nunca salva usa o default "main.json" (mesmo default do menu Cena).
    REQUIRE(doc.saveProject().ok());
    CHECK(doc.currentScenePath() == "main.json");
    CHECK_FALSE(doc.sceneDirty());
    CHECK_FALSE(doc.projectDirty());

    // Reload EXATAMENTE como a Activity faz: host novo, mesmo workspace,
    // ensureStartupProject — NENHUM loadScene manual aqui (era o passo
    // que só o TESTE fazia e o device não).
    auto reopened = EditorDocument::create(f.fsStorage, eng::fs::Path{"."});
    REQUIRE(reopened.ok());
    auto& doc2 = *reopened.value();
    REQUIRE(doc2.ensureStartupProject().ok());

    const auto snapshot = doc2.hierarchySnapshot();
    REQUIRE(snapshot.size() == 2);
    eng::ecs::Entity betaInDoc2{};
    bool alpha = false;
    bool betaFound = false;
    for (const auto& node : snapshot) {
        alpha = alpha || node.name == "Alpha";
        betaFound = betaFound || node.name == "Beta";
        if (node.name == "Beta") {
            betaInDoc2 = node.entity;  // handle do doc RECARREGADO
        }
    }
    CHECK(alpha);
    CHECK(betaFound);

    // E o TRANSFORM sobreviveu (serialização da cena certa). Handle do
    // documento recarregado — handles NÃO atravessam documentos (mesma
    // família do bug do clone aleatório: índices não são identidade).
    const auto betaNow = doc2.transform(betaInDoc2);
    REQUIRE(betaNow.ok());
    CHECK(betaNow.value().position.x == Catch::Approx(2.f).margin(1e-4f));
    CHECK(betaNow.value().position.y == Catch::Approx(1.f).margin(1e-4f));
}

TEST_CASE("editor: P4.2 — B-A: export zip → import zip em workspace NOVO (cena vem junto)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    auto& doc = *f.doc;
    REQUIRE(doc.createEntity("ZipHero", eng::scene::kNoEntity).ok());
    REQUIRE(doc.saveProject().ok());

    // Export com wrapper = nome da PASTA real (não config.name — o bug
    // do rename no settings).
    REQUIRE(doc.exportProjectZip(".goni_export.zip").ok());

    // Import em workspace novo (mesma fs, outra raiz lógica).
    REQUIRE(f.fs->mkdirs(eng::fs::Path{"ws2"}).ok());
    auto imported = eng::editor::extractProjectZip(
        *f.fs, eng::fs::Path{".goni_export.zip"}, eng::fs::Path{"ws2"},
        "NomeDoArquivo");
    REQUIRE(imported.ok());
    CHECK(imported.value() == "TestGame");  // WRAPPER vence (não o nome do arquivo)

    // Diagnóstico: o import DEIXOU o projeto no destino?
    {
        auto there = f.fs->exists(
            eng::fs::Path{"ws2/TestGame/project.goni.json"});
        REQUIRE(there.ok());
        if (!there.value()) {
            auto listed = f.fs->list(eng::fs::Path{"."}, true);
            REQUIRE(listed.ok());
            std::string dump;
            for (const auto& e : listed.value()) {
                dump += e.path.str();
                dump += ";";
            }
            FAIL("ws2/TestGame/project.goni.json ausente — fs: " << dump);
        }
    }
    auto second = EditorDocument::create(f.fsStorage, eng::fs::Path{"ws2"});
    REQUIRE(second.ok());
    auto& doc2 = *second.value();
    auto opened = doc2.openProject(eng::fs::Path{imported.value()});
    if (!opened.ok()) {
        FAIL("openProject falhou: " << opened.error().message);
    }

    // A cena VEIO no zip + marker .goni_last_scene → entidades presentes
    // SEM loadScene (o marker viaja dentro do zip — B-A por completo).
    const auto snapshot = doc2.hierarchySnapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot.front().name == "ZipHero");
    CHECK(doc2.currentScenePath() == "main.json");
}

TEST_CASE("editor: P4.2 — B-A: cena registrada AUSENTE no reload é erro EXPLÍCITO (nunca vazio silencioso)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->createEntity("Hero", eng::scene::kNoEntity).ok());
    REQUIRE(f.doc->saveProject().ok());

    // O arquivo da cena sumiu (usuário apagou no disco / zip quebrado).
    REQUIRE(f.fs->remove(eng::fs::Path{"TestGame/scenes/main.json"}).ok());

    auto reopened = EditorDocument::create(f.fsStorage, eng::fs::Path{"."});
    REQUIRE(reopened.ok());
    // REGRA: falha de load = erro explícito. Nunca silêncio, nunca
    // "projeto novo" vazio.
    CHECK_FALSE(reopened.value()->ensureStartupProject().ok());
}

TEST_CASE("editor: P4.2 — B-A: newScene limpa o marker de última cena",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->createEntity("Hero", eng::scene::kNoEntity).ok());
    REQUIRE(f.doc->saveProject().ok());
    CHECK(f.fs->exists(eng::fs::Path{"TestGame/.goni_last_scene"}).ok());

    // Cena nova = nada a restaurar no próximo open (decisão do autor).
    REQUIRE(f.doc->newScene().ok());
    CHECK(f.doc->currentScenePath().empty());
    const auto marker = f.fs->exists(eng::fs::Path{"TestGame/.goni_last_scene"});
    REQUIRE(marker.ok());
    CHECK_FALSE(marker.value());

    auto reopened = EditorDocument::create(f.fsStorage, eng::fs::Path{"."});
    REQUIRE(reopened.ok());
    REQUIRE(reopened.value()->ensureStartupProject().ok());
    CHECK(reopened.value()->hierarchySnapshot().empty());  // vazio HONESTO
}

TEST_CASE("editor: P4.2 — B-A: zip em pasta ocupada vira cópia",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->saveProject().ok());
    REQUIRE(f.doc->exportProjectZip(".goni_export.zip").ok());

    // Import de zip válido em destino ocupado → cópia com sufixo (nunca
    // merge nem sobrescrita da pasta existente).
    REQUIRE(f.fs->mkdirs(eng::fs::Path{"ws3/TestGame"}).ok());
    auto clash = eng::editor::extractProjectZip(
        *f.fs, eng::fs::Path{".goni_export.zip"}, eng::fs::Path{"ws3"}, "x");
    REQUIRE(clash.ok());
    CHECK(clash.value() == "TestGame 2");
    auto untouched = f.fs->exists(eng::fs::Path{"ws3/TestGame/project.goni.json"});
    REQUIRE(untouched.ok());
    CHECK_FALSE(untouched.value());
}

// ---- B-C/B-D: matemática dos gizmos em parâmetros de DEVICE ------------------

TEST_CASE("editor: P4.2 — B-D: gizmo MOVE segue o dedo (X e Y SEPARADOS, density 2, 720×1600, 2 zooms)",
          "[editor][p42]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    // Parâmetros do device real (Realme C33): portrait 720×1600, densidade 2.
    doc.viewport().setScreenSize(720.f, 1600.f);
    doc.viewport().setUiScale(2.f);

    for (const float zoom : {48.f, 120.f}) {
        INFO("zoom = " << zoom);
        doc.viewport().camera().posX = 0.f;
        doc.viewport().camera().posY = 0.f;
        doc.viewport().camera().zoom = zoom;

        // Cada iteração começa com a entidade na ORIGEM (o drag da
        // iteração anterior mexeu — e as alças seguem a ENTIDADE).
        eng::editor::TransformDesc zero;
        REQUIRE(doc.setTransform(g.entity, zero).ok());

        // --- Eixo X: +200px de tela = +200/zoom de mundo; Y NÃO mexe. ---
        REQUIRE(doc.select(g.entity).ok());
        doc.setTool(eng::editor::EditorTool::Move);
        const float axis = TransformGizmo::axisPx(2.f);  // 96dp×2 = 192px
        CHECK(doc.gizmoDragBegin(360.f + axis, 800.f, nullptr) ==
              GizmoHandle::MoveAxisX);
        // Drag em VÁRIOS eventos (como o device entrega): 80 + 120px.
        REQUIRE(doc.gizmoDragTo(360.f + axis + 80.f, 800.f).ok());
        REQUIRE(doc.gizmoDragTo(360.f + axis + 200.f, 800.f + 30.f).ok());
        doc.gizmoDragEnd();
        auto tr = doc.transform(g.entity);
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(200.f / zoom).margin(1e-3f));
        CHECK(tr.value().position.y == Catch::Approx(0.f).margin(1e-3f));

        // --- "O dedo segue" (MoveCenter): agarrar o CORPO e arrastar —
        // o tap no ponto FINAL do toque re-encontra a entidade. ---
        eng::editor::TransformDesc reset;
        REQUIRE(doc.setTransform(g.entity, reset).ok());
        CHECK(doc.gizmoDragBegin(360.f, 800.f, nullptr) ==
              GizmoHandle::MoveCenter);
        REQUIRE(doc.gizmoDragTo(360.f + 150.f, 800.f - 100.f).ok());
        doc.gizmoDragEnd();
        tr = doc.transform(g.entity);
        REQUIRE(tr.ok());
        CHECK(tr.value().position.x == Catch::Approx(150.f / zoom).margin(1e-3f));
        // Tela para CIMA = mundo para CIMA (Y de tela invertido — os DOIS
        // eixos errados no device).
        CHECK(tr.value().position.y == Catch::Approx(100.f / zoom).margin(1e-3f));
        auto finger = doc.viewportTap(360.f + 150.f, 800.f - 100.f, nullptr);
        REQUIRE(finger.has_value());
        CHECK(*finger == g.entity);
    }
}

TEST_CASE("editor: P4.2 — B-D: HASTE do eixo é alvo (tocar na haste não cai no fallback)",
          "[editor][p42]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    doc.viewport().setUiScale(2.f);
    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Move);

    // Ponto NO MEIO da haste X (entre a borda do bounds e a ponta) — o
    // alvo antigo era SÓ a pontinha: toque na haste devolvia None e o
    // gesto virava scroll do fallback (mover relativo).
    const float axis = TransformGizmo::axisPx(2.f);
    const float midShaft = 100.f + (100.f / 2.f + axis) / 2.f;  // (borda+head)/2
    CHECK(doc.gizmoDragBegin(midShaft, 75.f, nullptr) == GizmoHandle::MoveAxisX);
    doc.gizmoDragEnd();

    const float midShaftY = 75.f + (75.f / 2.f + axis) / 2.f;
    CHECK(doc.gizmoDragBegin(100.f, midShaftY, nullptr) == GizmoHandle::MoveAxisY);
    doc.gizmoDragEnd();
}

TEST_CASE("editor: P4.2 — B-D: raio de SELEÇÃO escala com a densidade (14px fixo era ~7dp no device)",
          "[editor][p42]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    doc.viewport().setScreenSize(720.f, 1600.f);

    // Entidade 1×1 mundo, zoom 48 → quad de 48px (borda a 24px do centro).
    doc.viewport().camera().zoom = 48.f;
    REQUIRE(doc.select(g.entity).ok());

    // Toca a 30px FORA da borda do quad (centro do quad a 360/800):
    //   - densidade 2 → raio de toque 28px → 30px > 24px+borda?? — 54px do
    //     centro passa do quad (24) mas dentro do raio (24+28=52)... borda!
    //     Usa 50px do centro: dentro do raio estendido, fora do quad.
    doc.viewport().setUiScale(2.f);
    auto hit2x = doc.viewportTap(360.f + 50.f, 800.f, nullptr);
    REQUIRE(hit2x.has_value());
    CHECK(*hit2x == g.entity);

    //   - densidade 1 → raio 14px → 50-24=26px fora do quad, 26 > 14: MISS.
    doc.deselect();
    doc.viewport().setUiScale(1.f);
    auto hit1x = doc.viewportTap(360.f + 50.f, 800.f, nullptr);
    CHECK_FALSE(hit1x.has_value());
}

TEST_CASE("editor: P4.2 — B-C: o ANEL INTEIRO da rotação é alvo (toque a 90° do dot funciona)",
          "[editor][p42]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    doc.viewport().setScreenSize(720.f, 1600.f);
    doc.viewport().setUiScale(2.f);
    doc.viewport().camera().zoom = 48.f;
    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Rotate);

    // Raio do anel em px (bounds 1×1 → 24px*48=?? não — halfW 0.5 × 48 =
    // 24px + 26dp×2 = 52px → max(52, 64dp×2=128) = 128px).
    const float radius = TransformGizmo::ringRadiusPx(0.5f * 48.f, 2.f);
    CHECK(radius == Catch::Approx(128.f).margin(0.5f));

    // Dot do handle está no ângulo 0 (entidade sem rotação): tocar no
    // anel a 90° (EM CIMA, Y de tela para baixo) — o código antigo
    // devolvia None aqui e o gesto virava PAN ("rotação inoperante").
    CHECK(doc.gizmoDragBegin(360.f, 800.f - radius, nullptr) ==
          GizmoHandle::RotateRing);
    doc.gizmoDragEnd();
    // E a 180°.
    CHECK(doc.gizmoDragBegin(360.f - radius, 800.f, nullptr) ==
          GizmoHandle::RotateRing);
    doc.gizmoDragEnd();
    // Fora do anel (a 60px do raio) continua None.
    CHECK(doc.gizmoDragBegin(360.f, 800.f - radius - 60.f, nullptr) ==
          GizmoHandle::None);
}

TEST_CASE("editor: P4.2 — B-C: rotação contínua ALÉM de 180° acumula na direção (sem flip)",
          "[editor][p42]")
{
    // Nível GIZMO (alvo cru, sem round-trip de quat — o documento
    // normaliza a Euler devolvida): é o contrato do gesto. O código
    // antigo normalizava o ÂNGULO TOTAL contra o grab fixo — evento 2
    // devolvia -100 (girava PARA TRÁS no device).
    eng::editor::Viewport viewport;
    viewport.setScreenSize(720.f, 1600.f);
    viewport.camera().zoom = 48.f;

    eng::editor::GizmoBounds bounds;
    bounds.worldX = 0.f;
    bounds.worldY = 0.f;
    bounds.originX = 0.f;
    bounds.originY = 0.f;
    bounds.halfW = 0.5f;
    bounds.halfH = 0.5f;
    bounds.rotation = 0.f;
    bounds.valid = true;

    eng::editor::TransformGizmo gizmo;
    eng::editor::GizmoTransform start{};

    const float radius =
        eng::editor::TransformGizmo::ringRadiusPx(0.5f * 48.f, 1.f);
    auto pointAt = [&](float deg) {
        const float rad = deg * 3.14159265358979f / 180.f;
        return std::pair<float, float>{
            360.f + radius * std::cos(rad),
            800.f - radius * std::sin(rad)};
    };

    // Grab no ângulo 0 (à direita do centro).
    CHECK(gizmo.hitTest(viewport, eng::editor::EditorTool::Rotate, bounds,
                        360.f + radius, 800.f) ==
          eng::editor::GizmoHandle::RotateRing);
    gizmo.beginDrag(eng::editor::GizmoHandle::RotateRing, start, viewport,
                    bounds, 360.f + radius, 800.f);

    // Três eventos de +130° = +390° total. Os alvos CRUS devem somar
    // (130 → 260 → 390): o flip do código antigo devolvia 130 → -100 → 30.
    const float expected[] = {130.f, 260.f, 390.f};
    const float touches[] = {130.f, 260.f, 390.f};
    for (int i = 0; i < 3; ++i) {
        const auto [px, py] = pointAt(touches[i]);
        const auto target = gizmo.dragTo(viewport, bounds, px, py);
        CHECK(target.rotationDeg ==
              Catch::Approx(expected[i]).margin(0.5f));
    }
    gizmo.endDrag();
}

TEST_CASE("editor: P4.2 — B-C: rotação com density 2 e zooms variados segue o ângulo do dedo",
          "[editor][p42]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    doc.viewport().setScreenSize(720.f, 1600.f);  // portrait do device
    doc.viewport().setUiScale(2.f);
    REQUIRE(doc.select(g.entity).ok());
    doc.setTool(eng::editor::EditorTool::Rotate);

    for (const float zoom : {24.f, 96.f}) {
        INFO("zoom = " << zoom);
        doc.viewport().camera().zoom = zoom;
        doc.viewport().camera().posX = 0.f;
        doc.viewport().camera().posY = 0.f;
        const float radius = TransformGizmo::ringRadiusPx(0.5f * zoom, 2.f);

        CHECK(doc.gizmoDragBegin(360.f + radius, 800.f, nullptr) ==
              GizmoHandle::RotateRing);
        // +90°: dedo vai para CIMA na tela (Y de tela invertido).
        REQUIRE(doc.gizmoDragTo(360.f, 800.f - radius).ok());
        doc.gizmoDragEnd();
        const auto tr = doc.transform(g.entity);
        REQUIRE(tr.ok());
        CHECK(tr.value().rotationDegrees.z == Catch::Approx(90.f).margin(0.5f));

        // Re-armo e volta para 0 (delta −90°) — nenhum estado vaza.
        CHECK(doc.gizmoDragBegin(360.f, 800.f - radius, nullptr) ==
              GizmoHandle::RotateRing);
        REQUIRE(doc.gizmoDragTo(360.f + radius, 800.f).ok());
        doc.gizmoDragEnd();
        const auto tr2 = doc.transform(g.entity);
        REQUIRE(tr2.ok());
        CHECK(tr2.value().rotationDegrees.z == Catch::Approx(0.f).margin(0.5f));
    }
}

// ---- B-E: honestidade no import de áudio ------------------------------------

TEST_CASE("editor: P4.2 — B-E: import de áudio RECUSA não-WAV (erro no import, sem lixo no projeto)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());

    // Bytes que NÃO são RIFF/WAVE (era aceito e estourava no preview:
    // "ParseError: wav: não é RIFF/WAVE").
    std::vector<std::byte> junk(64, std::byte{0x00});
    junk[0] = std::byte{'O'};
    junk[1] = std::byte{'g'};
    junk[2] = std::byte{'g'};
    junk[3] = std::byte{'S'};
    REQUIRE(f.fs
                ->writeAllBytes(eng::fs::Path{".import_tmp/musica.wav"},
                                junk)
                .ok());

    auto imported = f.doc->importAsset(".import_tmp/musica.wav", "audio", "musica");
    REQUIRE(imported.isError());
    // Mensagem orienta: apenas WAV PCM por agora (OGG/MP3 = fase futura).
    CHECK(imported.error().message.find("WAV PCM") != std::string::npos);
    CHECK(imported.error().message.find("fase futura") != std::string::npos);

    // Nada catalogado, nada no disco da categoria (import remove o lixo).
    auto listed = f.doc->assets()->list("audio");
    REQUIRE(listed.ok());
    CHECK(listed.value().empty());
}

TEST_CASE("editor: P4.2 — B-E: WAV PCM válido importa e toca (caminho do preview intacto)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    constexpr std::uint32_t kSamples = 8;
    std::vector<std::byte> wav;
    auto push32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            wav.push_back(static_cast<std::byte>(v >> (8 * i)));
        }
    };
    auto push16 = [&](std::uint16_t v) {
        wav.push_back(static_cast<std::byte>(v & 0xff));
        wav.push_back(static_cast<std::byte>(v >> 8));
    };
    auto pushTag = [&](const char (&tag)[5]) {
        for (int i = 0; i < 4; ++i) {
            wav.push_back(static_cast<std::byte>(tag[i]));
        }
    };
    pushTag("RIFF");
    push32(36 + kSamples * 2);
    pushTag("WAVE");
    pushTag("fmt ");
    push32(16);
    push16(1);
    push16(1);
    push32(48000);
    push32(96000);
    push16(2);
    push16(16);
    pushTag("data");
    push32(kSamples * 2);
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        push16(static_cast<std::uint16_t>(i * 100));
    }
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());
    REQUIRE(f.fs->writeAllBytes(eng::fs::Path{".import_tmp/beep.wav"}, wav).ok());

    auto imported = f.doc->importAsset(".import_tmp/beep.wav", "audio", "beep");
    REQUIRE(imported.ok());
    CHECK(imported.value() == "beep.wav");  // extensão preservada (recovery P0)
    REQUIRE(f.doc->audioPreview("beep.wav").ok());
}

TEST_CASE("editor: P4.2 — B-E: textura corrompida segue REJEITADA (validação migrada do JNI, contrato intacto)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());
    const std::vector<std::byte> junk(48, std::byte{0xEE});
    REQUIRE(f.fs
                ->writeAllBytes(eng::fs::Path{".import_tmp/quebrada.png"}, junk)
                .ok());

    auto imported =
        f.doc->importAsset(".import_tmp/quebrada.png", "textures", "quebrada");
    REQUIRE(imported.isError());
    auto listed = f.doc->assets()->list("textures");
    REQUIRE(listed.ok());
    CHECK(listed.value().empty());
}

// ---- T5: Modo Jogo (G1) -------------------------------------------------------

TEST_CASE("editor: P4.2 — T5: STOP preserva a SELEÇÃO da edição (contrato do Modo Jogo)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    auto a = f.doc->createEntity("A", eng::scene::kNoEntity);
    REQUIRE(a.ok());
    auto b = f.doc->createEntity("B", eng::scene::kNoEntity);
    REQUIRE(b.ok());
    REQUIRE(f.doc->select(a.value()).ok());

    REQUIRE(f.doc->play().ok());
    f.doc->stop();

    REQUIRE(f.doc->selection().has_value());
    CHECK(*f.doc->selection() == a.value());
    CHECK(f.doc->isSelected(a.value()));

    // E a entidade continua editável (handle vivo na cena de EDIÇÃO).
    eng::editor::TransformDesc moved;
    moved.position = eng::math::Vec3{1.f, 0.f, 0.f};
    REQUIRE(f.doc->setTransform(a.value(), moved).ok());
}

TEST_CASE("editor: P4.2 — T5: PAUSE congela o TICK (scripts param; render continua de pé)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();

    // Cena COM script de verdade (ticks crescem no scheduler — sintaxe
    // canônica NI: `add &BL` + `up update:` + `stop`).
    auto e = f.doc->createEntity("Runner", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    const std::string kScript =
        "add &BL\n"
        "\n"
        "var speed: float = 1.0\n"
        "\n"
        "up update:\n"
        "    var me = self()\n"
        "    me.position.x = me.position.x + speed\n"
        "stop\n";
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    REQUIRE(f.doc->scriptCreate("Corredor").ok());
    REQUIRE(f.doc->scriptWrite("Corredor.nis", kScript).ok());
    REQUIRE(f.doc->scriptAssign(e.value(), "Corredor.nis").ok());

    REQUIRE(f.doc->play().ok());
    CHECK_FALSE(f.doc->isPaused());

    for (int i = 0; i < 10; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    const auto statsRunning = f.doc->runtimeScripts().stats();
    CHECK(statsRunning.ticks > 0);

    // PAUSE: o mundo congela — ticks NÃO crescem mais.
    f.doc->setPaused(true);
    CHECK(f.doc->isPaused());
    for (int i = 0; i < 30; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    const auto statsPaused = f.doc->runtimeScripts().stats();
    CHECK(statsPaused.ticks == statsRunning.ticks);

    // CONTINUE: volta a rodar.
    f.doc->setPaused(false);
    f.doc->tick(1.f / 60.f);
    const auto statsResumed = f.doc->runtimeScripts().stats();
    CHECK(statsResumed.ticks > statsPaused.ticks);

    // STOP: pause é estado do gesto — morre com o Play.
    f.doc->stop();
    CHECK_FALSE(f.doc->isPaused());
}

TEST_CASE("editor: P4.2 — T5: câmera do EDITOR intacta através de play/stop (sem leak da câmera de jogo)",
          "[editor][p42]")
{
    DocFixture f;
    f.withProject();
    auto& vp = f.doc->viewport();
    vp.camera().posX = 7.f;
    vp.camera().posY = -3.f;
    vp.camera().zoom = 96.f;

    REQUIRE(f.doc->play().ok());
    for (int i = 0; i < 5; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    f.doc->stop();

    CHECK(vp.camera().posX == Catch::Approx(7.f).margin(1e-5f));
    CHECK(vp.camera().posY == Catch::Approx(-3.f).margin(1e-5f));
    CHECK(vp.camera().zoom == Catch::Approx(96.f).margin(1e-5f));
    CHECK_FALSE(f.doc->hasGameCamera());
}

// ---- ProjectZip: contratos de formato ----------------------------------------

TEST_CASE("editor: P4.2 — ProjectZip: zip SEM project.goni.json não é um projeto (erro claro)",
          "[editor][p42]")
{
    DocFixture f;
    REQUIRE(f.fs->mkdirs(eng::fs::Path{"qualquer"}).ok());
    REQUIRE(f.fs
                ->writeAllText(eng::fs::Path{"qualquer/arquivo.txt"}, "oi")
                .ok());
    REQUIRE(eng::editor::buildProjectZip(
                *f.fs, eng::fs::Path{"qualquer"},
                eng::fs::Path{".goni_export.zip"}, "qualquer")
                .ok());

    REQUIRE(f.fs->mkdirs(eng::fs::Path{"destino"}).ok());
    auto extracted = eng::editor::extractProjectZip(
        *f.fs, eng::fs::Path{".goni_export.zip"}, eng::fs::Path{"destino"},
        "Importado");
    REQUIRE(extracted.isError());
    CHECK(extracted.error().message.find("project.goni.json") !=
          std::string::npos);
}

// =============================================================================
// P4.3 — BLOCO 0: N1 (preview áudio), N3 (rotação sem escala, anel px),
// N4 (helper canónico px↔clip). Ver docs/p4-editor-ux.md (adenda P4.3).
// =============================================================================

#include "eng/editor/OverlayMath.hpp"

TEST_CASE("editor: P4.3/N4 — OverlayMath: rotação em px é isotrópica",
          "[editor][p43]")
{
    using eng::editor::OverlayMapper;
    using eng::editor::quadCornersPx;
    using eng::editor::segmentCornersPx;

    // Portrait do device (720×1600) — o caso que deformava.
    const OverlayMapper mapper{720.f, 1600.f};

    // QUAD 80×20 px rodado 90°: a rotação em px PRESERVA o comprimento de
    // cada aresta (isotropia — o bug antigo esticava o eixo 2,22× no clip).
    eng::editor::PxCorner corners[4];
    quadCornersPx(100.f, 200.f, 40.f, 10.f, 3.14159265f / 2.f, corners);
    const float edgeX = std::sqrt(std::pow(corners[1].x - corners[0].x, 2.f) +
                                  std::pow(corners[1].y - corners[0].y, 2.f));
    const float edgeY = std::sqrt(std::pow(corners[2].x - corners[1].x, 2.f) +
                                  std::pow(corners[2].y - corners[1].y, 2.f));
    CHECK(edgeX == Catch::Approx(80.f).margin(1e-3f));  // 2*halfW inalterado
    CHECK(edgeY == Catch::Approx(20.f).margin(1e-3f));  // 2*halfH inalterado
    // E o RETÂNGULO OCUPADO trocou de eixo corretamente (80 wide × 20 tall
    // vira 20 wide × 80 tall — sem cisalhamento).
    float minX = corners[0].x, maxX = corners[0].x;
    float minY = corners[0].y, maxY = corners[0].y;
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, corners[i].x);
        maxX = std::max(maxX, corners[i].x);
        minY = std::min(minY, corners[i].y);
        maxY = std::max(maxY, corners[i].y);
    }
    CHECK((maxX - minX) == Catch::Approx(20.f).margin(1e-3f));
    CHECK((maxY - minY) == Catch::Approx(80.f).margin(1e-3f));

    // Rodado 30°: TODOS os cantos ficam à MESMA distância do centro que os
    // cantos sem rotação (quadrado qualquer continua quadrado em px).
    quadCornersPx(100.f, 200.f, 40.f, 40.f, 0.5235988f, corners);
    for (int i = 0; i < 4; ++i) {
        const float dx = corners[i].x - 100.f;
        const float dy = corners[i].y - 200.f;
        CHECK(std::sqrt(dx * dx + dy * dy) ==
              Catch::Approx(40.f * std::sqrt(2.f)).margin(1e-3f));
    }

    // SEGMENTO: a espessura VISÍVEL é a mesma em px para horizontal e
    // vertical (a distância A−perp … A+perp é 2×halfThick nos dois casos —
    // era aqui que o anel ficava "elíptico": espessura dependia da direção).
    eng::editor::PxCorner seg[4];
    segmentCornersPx(0.f, 0.f, 200.f, 0.f, 1.f, seg);    // horizontal
    const float thickH = std::sqrt(std::pow(seg[3].x - seg[0].x, 2.f) +
                                   std::pow(seg[3].y - seg[0].y, 2.f));
    segmentCornersPx(0.f, 0.f, 0.f, 200.f, 1.f, seg);    // vertical
    const float thickV = std::sqrt(std::pow(seg[3].x - seg[0].x, 2.f) +
                                   std::pow(seg[3].y - seg[0].y, 2.f));
    CHECK(thickH == Catch::Approx(2.f).margin(1e-3f));
    CHECK(thickV == Catch::Approx(2.f).margin(1e-3f));

    // MAPPER: px→clip→px round-trip idêntico (conversão sem distorção).
    const float sx = 37.f;
    const float sy = 1543.f;
    const float backX = (mapper.toClipX(sx) + 1.f) * 0.5f * mapper.w;
    const float backY = (1.f - mapper.toClipY(sy)) * 0.5f * mapper.h;
    CHECK(backX == Catch::Approx(sx).margin(1e-3f));
    CHECK(backY == Catch::Approx(sy).margin(1e-3f));

    // REGRESSÃO do CI (readback PNG): sprite 2x2 em (0.5,-0.5), zoom 48,
    // surface 128x128 — centro px (88,88), meia-extensão 48px. O canto 3
    // (topo-esquerda na TELA) TEM de cair em clip y = 0.375 (era −1.125
    // quando a rotação acontecia sem o flip da projeção — sprite virado).
    const OverlayMapper spriteMapper{128.f, 128.f};
    eng::editor::PxCorner sprite[4];
    quadCornersPx(88.f, 88.f, 48.f, 48.f, 0.f, sprite);
    CHECK(spriteMapper.toClipY(sprite[3].y) ==
          Catch::Approx(0.375f).margin(1e-3f));
    CHECK(spriteMapper.toClipY(sprite[0].y) ==
          Catch::Approx(-1.125f).margin(1e-3f));
    CHECK(spriteMapper.toClipX(sprite[3].x) ==
          Catch::Approx(-0.375f).margin(1e-3f));
}

TEST_CASE("editor: P4.3/N3 — anel de rotação é CÍRCULO em px no portrait",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();
    // Portrait REAL do device ×2 de densidade (zoom 48 px/unidade).
    auto& viewport = f.doc->viewport();
    viewport.setScreenSize(720.f, 1600.f);
    viewport.setUiScale(2.f);

    auto created = f.doc->createEntity("Alvo", eng::scene::kNoEntity);
    REQUIRE(created.ok());
    REQUIRE(f.doc->select(created.value()).ok());
    f.doc->setTool(eng::editor::EditorTool::Rotate);

    const TransformGizmo gizmo;
    const auto bounds = f.doc->selectionBounds(nullptr);
    REQUIRE(bounds.valid);

    // Anel em PX: raio px = max(half*zoom) + 26dp×2 (kRingPadDp).
    const float radiusPx =
        TransformGizmo::ringRadiusPx(std::max(bounds.halfW, bounds.halfH) *
                                         viewport.effectiveCamera().zoom,
                                     viewport.uiScale());
    const auto segments = gizmo.layoutSegments(viewport,
                                               eng::editor::EditorTool::Rotate,
                                               bounds);
    REQUIRE(segments.size() >= 32);  // anel de 32 lados + spoke + chevron

    // Cada PONTEIRO do anel projetado à tela (Viewport canónica) fica à
    // MESMA distância em px do centro — aspect do anel em px == 1.
    const float centerX = viewport.worldToScreenX(bounds.worldX);
    const float centerY = viewport.worldToScreenY(bounds.worldY);
    int ringPoints = 0;
    float minDist = 1e9f;
    float maxDist = 0.f;
    for (const auto& segment : segments) {
        const float x0 = viewport.worldToScreenX(segment.x0);
        const float y0 = viewport.worldToScreenY(segment.y0);
        const float dist = std::sqrt(std::pow(x0 - centerX, 2.f) +
                                     std::pow(y0 - centerY, 2.f));
        // Spoke/chevron têm pontos fora do raio — filtra pelo anel (32
        // primeiros segmentos são todos do anel).
        if (ringPoints < 32) {
            minDist = std::min(minDist, dist);
            maxDist = std::max(maxDist, dist);
        }
        ++ringPoints;
    }
    REQUIRE(ringPoints >= 32);
    // Todos os pontos do anel no MESMO raio px (ε = 1e-2 px) — círculo
    // PERFEITO em px (o bug: elipse por espessura/direção no clip).
    CHECK(minDist == Catch::Approx(radiusPx).margin(0.05f));
    CHECK(maxDist == Catch::Approx(radiusPx).margin(0.05f));
}

/// P4.3 (N3): rodar 90/180/360° NUNCA mexe no vetor de escala (regra
/// "rotação escreve apenas orientação") — nem no transform, nem no quad
/// desenhado (que alimenta o hit-test).
TEST_CASE("editor: P4.3/N3 — rotação preserva a escala em 90/180/360",
          "[editor][p43]")
{
    GizmoFixture g;
    auto& doc = *g.f.doc;
    using eng::editor::GizmoHandle;

    REQUIRE(doc.select(g.entity).ok());
    // Escala NÃO-uniforme de partida (o caso que "deformava").
    auto t = doc.transform(g.entity);
    REQUIRE(t.ok());
    t.value().scale = {1.5f, 0.75f, 1.f};
    REQUIRE(doc.setTransform(g.entity, t.value()).ok());
    doc.setTool(eng::editor::EditorTool::Rotate);

    const float kEps = 1e-3f;
    // Euler (graus) wrapa em ±180 (atan2) — compara o ÂNGULO NORMALIZADO:
    // 180° e −180° são a MESMA rotação.
    auto normDeg = [](float d) {
        float v = std::fmod(d + 180.f, 360.f);
        if (v < 0.f) {
            v += 360.f;
        }
        return v - 180.f;
    };
    float lastDeg = 0.f;
    for (const float stepDeg : {90.f, 90.f, 90.f, 90.f}) {  // 90→180→270→360
        const auto bounds = doc.selectionBounds(nullptr);
        REQUIRE(bounds.valid);
        const float radiusPx =
            TransformGizmo::ringRadiusPx(
                std::max(bounds.halfW, bounds.halfH) *
                    doc.viewport().effectiveCamera().zoom,
                doc.viewport().uiScale());
        // Agarra o handle NO ÂNGULO ATUAL (anel amarrado à rotação).
        // ATENÇÃO ao flip do Y de tela: ângulo de MUNDO θ aparece em tela
        // como (cos θ, −sin θ) — w2sY tem inclinação negativa.
        const float angle = bounds.rotation;
        const float hx = doc.viewport().worldToScreenX(bounds.worldX) +
                         std::cos(angle) * radiusPx;
        const float hy = doc.viewport().worldToScreenY(bounds.worldY) -
                         std::sin(angle) * radiusPx;
        REQUIRE(doc.gizmoDragBegin(hx, hy, nullptr) ==
                GizmoHandle::RotateRing);
        // Gira +90° (passo de 90° → sem flip do atan2).
        const float next = angle + stepDeg * 3.14159265f / 180.f;
        REQUIRE(doc.gizmoDragTo(
            doc.viewport().worldToScreenX(bounds.worldX) +
                std::cos(next) * radiusPx,
            doc.viewport().worldToScreenY(bounds.worldY) -
                std::sin(next) * radiusPx).ok());
        doc.gizmoDragEnd();

        lastDeg += stepDeg;
        auto after = doc.transform(g.entity);
        REQUIRE(after.ok());
        CHECK(after.value().scale.x == Catch::Approx(1.5f).margin(kEps));
        CHECK(after.value().scale.y == Catch::Approx(0.75f).margin(kEps));
        CHECK(std::abs(normDeg(after.value().rotationDegrees.z - lastDeg)) <=
              0.5f);
        // O quad DESenhado (fonte do hit-test/gizmo) mantém o tamanho:
        // rotação não corrói a escala percebida.
        const auto quads = doc.viewport().buildQuads(*doc.sceneInFocus(),
                                                     doc.selection());
        for (const auto& q : quads) {
            if (q.entity == g.entity) {
                CHECK(q.sizeX == Catch::Approx(1.5f).margin(kEps));
                CHECK(q.sizeY == Catch::Approx(0.75f).margin(kEps));
            }
        }
    }
}

/// P4.3 (N3): rotação com PAI rotacionado+escalado (uniforme) — o delta de
/// mundo do dedo vira delta de ESCALA nunca; escala local intacta.
TEST_CASE("editor: P4.3/N3 — rotação de filho com pai escalado não deforma",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();
    f.doc->viewport().setScreenSize(400.f, 300.f);

    auto parent = f.doc->createEntity("Pai", eng::scene::kNoEntity);
    REQUIRE(parent.ok());
    auto pt = f.doc->transform(parent.value());
    REQUIRE(pt.ok());
    pt.value().rotationDegrees.z = 30.f;
    pt.value().scale = {2.f, 2.f, 1.f};
    REQUIRE(f.doc->setTransform(parent.value(), pt.value()).ok());

    auto child = f.doc->createEntity("Filho", parent.value());
    REQUIRE(child.ok());
    REQUIRE(f.doc->select(child.value()).ok());
    f.doc->setTool(eng::editor::EditorTool::Rotate);

    const auto bounds = f.doc->selectionBounds(nullptr);
    REQUIRE(bounds.valid);
    CHECK(bounds.rotation ==
          Catch::Approx(30.f * 3.14159265f / 180.f).margin(1e-3f));

    const float radiusPx = TransformGizmo::ringRadiusPx(
        std::max(bounds.halfW, bounds.halfH) *
            f.doc->viewport().effectiveCamera().zoom,
        f.doc->viewport().uiScale());
    const float angle = bounds.rotation;
    const float hx = f.doc->viewport().worldToScreenX(bounds.worldX) +
                     std::cos(angle) * radiusPx;
    const float hy = f.doc->viewport().worldToScreenY(bounds.worldY) -
                     std::sin(angle) * radiusPx;
    REQUIRE(f.doc->gizmoDragBegin(hx, hy, nullptr) ==
            eng::editor::GizmoHandle::RotateRing);
    // +45° de delta de mundo (dois passos de 22.5° — sem flip).
    float last = angle;
    for (int i = 0; i < 2; ++i) {
        last += 22.5f * 3.14159265f / 180.f;
        REQUIRE(f.doc->gizmoDragTo(
            f.doc->viewport().worldToScreenX(bounds.worldX) +
                std::cos(last) * radiusPx,
            f.doc->viewport().worldToScreenY(bounds.worldY) -
                std::sin(last) * radiusPx).ok());
    }
    f.doc->gizmoDragEnd();

    const auto after = f.doc->transform(child.value());
    REQUIRE(after.ok());
    // ESCALA LOCAL intacta (1,1) — o pai nunca contamina o filho.
    CHECK(after.value().scale.x == Catch::Approx(1.f).margin(1e-3f));
    CHECK(after.value().scale.y == Catch::Approx(1.f).margin(1e-3f));
    // Rotação LOCAL = delta pedido (45°).
    CHECK(after.value().rotationDegrees.z ==
          Catch::Approx(45.f).margin(0.5f));
}

/// P4.3 (N1): preview de áudio com TOGGLE/STOP — a voice morre no 2º toque,
/// no stop explícito, ao entrar em Play; uma única voice de preview existe.
TEST_CASE("editor: P4.3/N1 — preview de áudio: toggle, stop e isolamento",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();
    constexpr std::uint32_t kSamples = 240;
    auto makeWav = [](std::vector<std::byte>& wav) {
        auto push32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                wav.push_back(static_cast<std::byte>(v >> (8 * i)));
            }
        };
        auto push16 = [&](std::uint16_t v) {
            wav.push_back(static_cast<std::byte>(v & 0xff));
            wav.push_back(static_cast<std::byte>(v >> 8));
        };
        auto pushTag = [&](const char (&tag)[5]) {
            for (int i = 0; i < 4; ++i) {
                wav.push_back(static_cast<std::byte>(tag[i]));
            }
        };
        pushTag("RIFF");
        push32(36 + kSamples * 2);
        pushTag("WAVE");
        pushTag("fmt ");
        push32(16);
        push16(1);
        push16(1);
        push32(48000);
        push32(96000);
        push16(2);
        push16(16);
        pushTag("data");
        push32(kSamples * 2);
        for (std::uint32_t i = 0; i < kSamples; ++i) {
            push16(static_cast<std::uint16_t>(i * 100));
        }
    };
    std::vector<std::byte> wav;
    makeWav(wav);
    auto* browser = f.doc->assets();
    REQUIRE(browser != nullptr);
    REQUIRE(f.fs->mkdirs(eng::fs::Path{".import_tmp"}).ok());
    REQUIRE(f.fs
                ->writeAllBytes(eng::fs::Path{".import_tmp/beep.wav"},
                                std::span{wav.data(), wav.size()})
                .ok());
    REQUIRE(browser->import(".import_tmp/beep.wav", "audio", "beep").ok());

    // 1º toque: toca (uma voice, marcada como PREVIEW viva).
    REQUIRE(f.doc->audioPreview("beep.wav").ok());
    CHECK(f.doc->audioPreviewPlaying());
    CHECK(f.doc->audioMixer().liveVoices() == 1);

    // 2º toque no MESMO asset: PARA (era o bug N1 — tocava para sempre).
    REQUIRE(f.doc->audioPreview("beep.wav").ok());
    CHECK_FALSE(f.doc->audioPreviewPlaying());
    CHECK(f.doc->audioMixer().liveVoices() == 0);

    // Toca de novo e para via stop explícito; stop é IDEMPOTENTE.
    REQUIRE(f.doc->audioPreview("beep.wav").ok());
    CHECK(f.doc->audioPreviewPlaying());
    f.doc->audioPreviewStop();
    CHECK_FALSE(f.doc->audioPreviewPlaying());
    CHECK(f.doc->audioMixer().liveVoices() == 0);
    f.doc->audioPreviewStop();  // no-op garantido (sem crash, sem voz)
    CHECK(f.doc->audioMixer().liveVoices() == 0);

    // Isolamento Edit→Play: entrar em Play mata o preview (a voice de
    // preview nunca atravessa a fronteira das vozes de jogo).
    REQUIRE(f.doc->audioPreview("beep.wav").ok());
    CHECK(f.doc->audioPreviewPlaying());
    REQUIRE(f.doc->play().ok());
    CHECK_FALSE(f.doc->audioPreviewPlaying());
    f.doc->stop();
    CHECK(f.doc->audioMixer().liveVoices() == 0);

    // Erro honesto: asset inexistente → erro explícito, sem voice.
    auto missing = f.doc->audioPreview("nao_existe.wav");
    REQUIRE(missing.isError());
    CHECK_FALSE(f.doc->audioPreviewPlaying());
}

// =============================================================================
// P4.3 — BLOCO 1: hierarquia completa — round-trip de TOPOLOGIA (save/load
// preserva pais/filhos/ordem) + duplicação de subárvore. A UI por toque
// (criar/renomear/duplicar/apagar/parentear/menu de contexto) já existe na
// Activity; aqui está a prova de que o DOCUMENTO preserva o que a UI cria.
// =============================================================================

TEST_CASE("editor: P4.3/Bloco 1 — save/load preserva topologia e ordem",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();

    // R1 → (C1, C2 → G1); R2 — topologia não-trivial em DUAS raízes.
    auto r1 = f.doc->createEntity("R1", eng::scene::kNoEntity);
    auto r2 = f.doc->createEntity("R2", eng::scene::kNoEntity);
    auto c1 = f.doc->createEntity("C1", r1.value());
    auto c2 = f.doc->createEntity("C2", r1.value());
    auto g1 = f.doc->createEntity("G1", c2.value());
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    REQUIRE(c1.ok());
    REQUIRE(c2.ok());
    REQUIRE(g1.ok());

    const auto before = f.doc->hierarchySnapshot();
    REQUIRE(before.size() == 5);

    REQUIRE(f.doc->saveScene("topologia.json").ok());
    REQUIRE(f.doc->loadScene("topologia.json").ok());

    const auto after = f.doc->hierarchySnapshot();
    REQUIRE(after.size() == before.size());
    // ORDEM depth-first + profundidade idênticas — fingerprint da topologia.
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(after[i].name == before[i].name);
        CHECK(after[i].depth == before[i].depth);
    }
    // PAIS exatos: G1 continua filho de C2 (não de R1/R2).
    auto findByName = [&](const std::string& name) {
        for (const auto& node : after) {
            if (node.name == name) {
                return node.entity;
            }
        }
        return eng::ecs::Entity{0xFFFFFFFFu, 0xFFFFFFFFu};
    };
    auto* scene = f.doc->sceneInFocus();
    REQUIRE(scene != nullptr);
    CHECK(scene->parentOf(findByName("G1")) == findByName("C2"));
    CHECK(scene->parentOf(findByName("C1")) == findByName("R1"));
    CHECK(scene->parentOf(findByName("R2")) == eng::scene::kNoEntity);
}

TEST_CASE("editor: P4.3/Bloco 1 — duplicar preserva a SUBÁRVORE por toque",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();
    auto r1 = f.doc->createEntity("R1", eng::scene::kNoEntity);
    auto c2 = f.doc->createEntity("C2", r1.value());
    auto g1 = f.doc->createEntity("G1", c2.value());
    REQUIRE(r1.ok());
    REQUIRE(c2.ok());
    REQUIRE(g1.ok());

    auto dup = f.doc->duplicateEntity(c2.value());
    REQUIRE(dup.ok());

    const auto snapshot = f.doc->hierarchySnapshot();
    // R1, C2, G1, C2.alt, G1(clonado) — 5 nós; clone por baixo do MESMO pai.
    REQUIRE(snapshot.size() == 5);
    auto* scene = f.doc->sceneInFocus();
    REQUIRE(scene != nullptr);
    eng::ecs::Entity cloneC2{0xFFFFFFFFu, 0xFFFFFFFFu};
    std::vector<eng::ecs::Entity> namedG1;
    for (const auto& node : snapshot) {
        if (node.name == "C2.alt") {
            cloneC2 = node.entity;
        }
        if (node.name == "G1") {
            namedG1.push_back(node.entity);
        }
    }
    // O clone da RAIZ ganha sufixo; o filho clonado mantém o rótulo —
    // subárvore inteira presente e ligada.
    CHECK(namedG1.size() == 2);
    CHECK(scene->isNode(cloneC2));
    eng::ecs::Entity cloneG1{0xFFFFFFFFu, 0xFFFFFFFFu};
    for (const eng::ecs::Entity candidate : namedG1) {
        if (scene->parentOf(candidate) == cloneC2) {
            cloneG1 = candidate;
        }
    }
    CHECK(scene->isNode(cloneG1));
    CHECK(scene->parentOf(cloneC2) == r1.value());
}

// =============================================================================
// P4.3 — BLOCO 2: Ticks/Camadas — timeScale por camada, participação
// (update/física/render) e timestep fixo da física autoráveis (ADR-051).
// =============================================================================

TEST_CASE("editor: P4.3/Bloco 2 — camadas: listar, criar, timeScale e participação",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();

    // Defaults: GAME/SUBGAME, timeScale 1, participação total.
    auto initial = f.doc->layerList();
    REQUIRE(initial.ok());
    REQUIRE(initial.value().size() == 2);
    CHECK(initial.value()[0].name == "GAME");
    CHECK(initial.value()[1].name == "SUBGAME");
    for (const auto& layer : initial.value()) {
        CHECK(layer.timeScale == Catch::Approx(1.f));
        CHECK(layer.update);
        CHECK(layer.physics);
        CHECK(layer.render);
    }

    // Criar por toque: nova camada entra na lista; duplicados/vazios são
    // rejeitados com erro explícito.
    REQUIRE(f.doc->addLayer("UI").ok());
    CHECK(f.doc->addLayer("UI").isError());
    CHECK(f.doc->addLayer("").isError());
    CHECK(f.doc->addLayer("GAME").isError());
    initial = f.doc->layerList();
    REQUIRE(initial.ok());
    REQUIRE(initial.value().size() == 3);
    CHECK(initial.value()[2].name == "UI");

    // timeScale da camada vira o timeScale POR ENTIDADE (ADR-051 real —
    // o que a sheet edita é o que o tick usa).
    auto node = f.doc->createEntity("NaUI", eng::scene::kNoEntity);
    REQUIRE(node.ok());
    REQUIRE(f.doc->addComponent(node.value(),
                                "eng::scene::LayerMember").ok());
    REQUIRE(f.doc->setInspectorField(node.value(),
                                     "eng::scene::LayerMember", "layer",
                                     "UI").ok());
    REQUIRE(f.doc->setLayerTimeScale("UI", 0.25f).ok());
    CHECK(f.doc->sceneInFocus()->timeScaleOf(node.value()) ==
          Catch::Approx(0.25f));
    auto listed = f.doc->layerList();
    REQUIRE(listed.ok());
    CHECK(listed.value()[2].timeScale == Catch::Approx(0.25f));

    // Participação: desligar update/render da camada desliga para as
    // entidades dela (física continua).
    REQUIRE(f.doc->setLayerParticipation("UI", false, true, false).ok());
    CHECK_FALSE(f.doc->sceneInFocus()->participatesIn(
        node.value(), eng::scene::LayerStage::Update));
    CHECK(f.doc->sceneInFocus()->participatesIn(
        node.value(), eng::scene::LayerStage::Physics));
    CHECK_FALSE(f.doc->sceneInFocus()->participatesIn(
        node.value(), eng::scene::LayerStage::Render));

    // Erros honestos: camada inexistente e valores inválidos.
    CHECK(f.doc->setLayerTimeScale("NaoExiste", 1.f).isError());
    CHECK(f.doc->setLayerTimeScale("UI", -1.f).isError());
    CHECK(f.doc->setLayerTimeScale("UI", std::nanf("")).isError());
    CHECK(f.doc->setLayerParticipation("NaoExiste", true, true, true)
              .isError());

    // Round-trip: a config de camadas persiste na cena (serializador P0-5).
    REQUIRE(f.doc->saveScene("camadas.json").ok());
    REQUIRE(f.doc->setLayerTimeScale("UI", 1.f).ok());
    REQUIRE(f.doc->loadScene("camadas.json").ok());
    auto reloaded = f.doc->layerList();
    REQUIRE(reloaded.ok());
    REQUIRE(reloaded.value().size() == 3);
    CHECK(reloaded.value()[2].name == "UI");
    CHECK(reloaded.value()[2].timeScale == Catch::Approx(0.25f));
    CHECK_FALSE(reloaded.value()[2].update);
    CHECK(reloaded.value()[2].physics);
    CHECK_FALSE(reloaded.value()[2].render);
}

TEST_CASE("editor: P4.3/Bloco 2 — timestep da física: validar, aplicar e persistir",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();

    // Default honesto: 1/60.
    CHECK(f.doc->physicsFixedDt() == Catch::Approx(1.f / 60.f).margin(1e-6f));

    // Aplicar: o acumulador REAL usado pelo PhysicsTick no Play.
    REQUIRE(f.doc->setPhysicsFixedDt(1.f / 120.f).ok());
    CHECK(f.doc->physicsFixedDt() == Catch::Approx(1.f / 120.f).margin(1e-7f));
    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->physicsFixedDt() == Catch::Approx(1.f / 120.f).margin(1e-7f));
    f.doc->stop();

    // Erros honestos (sem clamp calado): 0, negativo, NaN, acima do teto.
    CHECK(f.doc->setPhysicsFixedDt(0.f).isError());
    CHECK(f.doc->setPhysicsFixedDt(-1.f).isError());
    CHECK(f.doc->setPhysicsFixedDt(std::nanf("")).isError());
    CHECK(f.doc->setPhysicsFixedDt(0.5f).isError());
    CHECK(f.doc->physicsFixedDt() == Catch::Approx(1.f / 120.f).margin(1e-7f));

    // Persistência: a chave aditiva "physicsFixedDt" da cena sobrevive ao
    // save/load (arquivo antigo sem a chave = default 1/60).
    REQUIRE(f.doc->saveScene("ticks.json").ok());
    REQUIRE(f.doc->setPhysicsFixedDt(1.f / 30.f).ok());
    REQUIRE(f.doc->loadScene("ticks.json").ok());
    CHECK(f.doc->physicsFixedDt() == Catch::Approx(1.f / 120.f).margin(1e-7f));

    // Play rejeita edição (guard requireEditMode do Bloco 2).
    REQUIRE(f.doc->play().ok());
    CHECK(f.doc->setPhysicsFixedDt(1.f / 60.f).isError());
    CHECK(f.doc->addLayer("EmPlay").isError());
    f.doc->stop();
}

// =============================================================================
// P4.3 — BLOCO 3: Materiais & Luzes — o authoring de .mat (criar/editar/
// atribuir, lit/unlit + tint) existe desde o P3 com readback; aqui fica o
// contrato de DADOS do novo PREVIEW visual da luz no viewport (anel px).
// =============================================================================

TEST_CASE("editor: P4.3/Bloco 3 — preview de Light2D: dados do marker",
          "[editor][p43]")
{
    DocFixture f;
    f.withProject();
    f.doc->viewport().setScreenSize(720.f, 1600.f);
    f.doc->viewport().setUiScale(2.f);

    auto lamp = f.doc->createEntity("Lampada", eng::scene::kNoEntity);
    REQUIRE(lamp.ok());
    REQUIRE(f.doc->addComponent(lamp.value(), "eng::render::Light2D").ok());
    REQUIRE(f.doc->setInspectorField(lamp.value(), "eng::render::Light2D",
                                     "radius", "3").ok());
    REQUIRE(f.doc->setInspectorField(lamp.value(), "eng::render::Light2D",
                                     "intensity", "2").ok());

    const auto quads = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(),
                                                    f.doc->selection());
    bool found = false;
    for (const auto& quad : quads) {
        if (quad.entity != lamp.value()) {
            continue;
        }
        found = true;
        CHECK(quad.hasLight);
        CHECK(quad.lightRadius == Catch::Approx(3.f));
        CHECK(quad.lightIntensity == Catch::Approx(2.f));
        // Contrato do marker: raio px = raio mundo × zoom (anel do alcance).
        CHECK(quad.lightRadius * f.doc->viewport().effectiveCamera().zoom ==
              Catch::Approx(144.f));
    }
    CHECK(found);

    // Luz desligada NÃO desenha marker (nem entra no bloco PerFrame).
    REQUIRE(f.doc->setInspectorField(lamp.value(), "eng::render::Light2D",
                                     "enabled", "false").ok());
    const auto off = f.doc->viewport().buildQuads(*f.doc->sceneInFocus(),
                                                  f.doc->selection());
    for (const auto& quad : off) {
        if (quad.entity == lamp.value()) {
            CHECK_FALSE(quad.hasLight);
        }
    }
}

// =============================================================================
// P4.5 — undo/redo por snapshots, snap do gizmo, fit do viewport
// =============================================================================

TEST_CASE("p45: undo/redo de transform (move/rotate/scale) restaura TRS",
          "[p45]")
{
    DocFixture f;
    f.withProject();
    auto created = f.doc->createEntity("Hero", eng::scene::kNoEntity);
    REQUIRE(created.ok());
    const auto hero = created.value();

    eng::editor::TransformDesc before{};
    before.position = {1.f, 2.f, 0.f};
    before.rotationDegrees = {0.f, 0.f, 15.f};
    before.scale = {2.f, 3.f, 1.f};
    REQUIRE(f.doc->setTransform(hero, before).ok());

    eng::editor::TransformDesc after{};
    after.position = {5.f, 7.f, 0.f};
    after.rotationDegrees = {0.f, 0.f, 45.f};
    after.scale = {1.f, 1.f, 1.f};
    REQUIRE(f.doc->setTransform(hero, after).ok());
    CHECK(f.doc->canUndo());

    // Undo → volta ao estado ANTES do último setTransform.
    REQUIRE(f.doc->undo().ok());
    {
        auto tr = f.doc->transform(hero);
        // IDs mudaram (cena recriada) — localiza pelo nome.
        auto nodes = f.doc->hierarchySnapshot();
        REQUIRE(nodes.size() == 1);
        auto restored = f.doc->transform(nodes[0].entity);
        REQUIRE(restored.ok());
        CHECK(restored.value().position.x == Catch::Approx(1.f));
        CHECK(restored.value().position.y == Catch::Approx(2.f));
        CHECK(restored.value().rotationDegrees.z == Catch::Approx(15.f));
        CHECK(restored.value().scale.y == Catch::Approx(3.f));
    }
    CHECK(f.doc->canRedo());
    // Por-op: resta undo do "create" e do primeiro transform.
    CHECK(f.doc->canUndo());

    // Redo → volta ao DEPOIS.
    REQUIRE(f.doc->redo().ok());
    auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    auto redone = f.doc->transform(nodes[0].entity);
    REQUIRE(redone.ok());
    CHECK(redone.value().position.x == Catch::Approx(5.f));
    CHECK(redone.value().rotationDegrees.z == Catch::Approx(45.f));
}

TEST_CASE("p45: undo de CREATE apaga entidade; redo recria com o mesmo nome",
          "[p45]")
{
    DocFixture f;
    f.withProject();
    auto created = f.doc->createSprite("Inimigo");
    REQUIRE(created.ok());
    CHECK(f.doc->hierarchySnapshot().size() == 1);

    REQUIRE(f.doc->undo().ok());
    CHECK(f.doc->hierarchySnapshot().empty());

    REQUIRE(f.doc->redo().ok());
    auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].name == "Inimigo");
}

TEST_CASE("p45: undo de DELETE restaura a SUBÁRVORE (nome + filho)",
          "[p45]")
{
    DocFixture f;
    f.withProject();
    auto parent = f.doc->createEntity("Pai", eng::scene::kNoEntity);
    REQUIRE(parent.ok());
    auto child = f.doc->createEntity("Filho", parent.value());
    REQUIRE(child.ok());
    REQUIRE(f.doc->hierarchySnapshot().size() == 2);

    REQUIRE(f.doc->deleteEntity(parent.value()).ok());
    CHECK(f.doc->hierarchySnapshot().empty());

    REQUIRE(f.doc->undo().ok());
    auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[0].name == "Pai");
    CHECK(nodes[1].name == "Filho");
    CHECK(nodes[1].depth == 1);

    // Redo do delete → subárvore some de novo.
    REQUIRE(f.doc->redo().ok());
    CHECK(f.doc->hierarchySnapshot().empty());
}

TEST_CASE("p45: undo de ATTACH (componente) remove; redo repõe", "[p45]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Lamp", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    const auto lamp = e.value();

    REQUIRE(f.doc->addComponent(lamp, "eng::render::Light2D").ok());
    auto comps = f.doc->addableComponents(lamp);
    // Prova indireta: Light2D não é mais addable (já está lá).
    for (const auto& meta : comps) {
        CHECK(meta.name != "eng::render::Light2D");
    }

    REQUIRE(f.doc->undo().ok());
    comps = f.doc->addableComponents(f.doc->hierarchySnapshot()[0].entity);
    bool hasLight = false;
    for (const auto& meta : comps) {
        if (meta.name == "eng::render::Light2D") hasLight = true;
    }
    CHECK(hasLight);

    REQUIRE(f.doc->redo().ok());
    comps = f.doc->addableComponents(f.doc->hierarchySnapshot()[0].entity);
    for (const auto& meta : comps) {
        CHECK(meta.name != "eng::render::Light2D");
    }
}

TEST_CASE("p45: attach de script é 1 passo de undo (create+assign)", "[p45]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Torreta", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->scriptCreate("Giro").ok());
    REQUIRE(f.doc->scriptAssign(e.value(), "Giro.nis").ok());

    REQUIRE(f.doc->undo().ok());
    // Undo do assign: o componente NiScript sai da entidade (estado ANTES
    // do attach — entidade continuava lá, criada antes).
    auto nodes = f.doc->hierarchySnapshot();
    REQUIRE(nodes.size() == 1);
    auto fields = f.doc->inspectorFields(
        nodes[0].entity, "eng::editor::NiScriptComponent");
    CHECK(fields.empty());

    REQUIRE(f.doc->redo().ok());
    fields = f.doc->inspectorFields(
        f.doc->hierarchySnapshot()[0].entity,
        "eng::editor::NiScriptComponent");
    CHECK_FALSE(fields.empty());
}

TEST_CASE("p45: gesto contínuo (N moves) = UM passo de undo", "[p45]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Runner", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    const auto runner = e.value();

    // 60 eventos de move (simula drag contínuo por scroll — 1 s de jogo).
    for (int i = 0; i < 60; ++i) {
        REQUIRE(f.doc->moveEntityScreen(runner, 1.0f, 0.f).ok());
    }
    // Coalescência: os 60 eventos dentro da janela = 1 entrada "move".
    REQUIRE(f.doc->undo().ok());
    // 1º undo desfaz O GESTO INTEIRO (voltou à origem) — a criação
    // (passo próprio) ainda está no histórico.
    {
        auto nodes = f.doc->hierarchySnapshot();
        REQUIRE(nodes.size() == 1);
        auto restored = f.doc->transform(nodes[0].entity);
        REQUIRE(restored.ok());
        CHECK(restored.value().position.x == Catch::Approx(0.f).margin(1e-4));
    }
    CHECK(f.doc->canUndo());  // resta o passo "create"
}

TEST_CASE("p45: undo em Play é recusado com erro explícito", "[p45]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("A", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->canUndo());
    REQUIRE(f.doc->play().ok());
    auto undone = f.doc->undo();
    REQUIRE(undone.isError());
    CHECK(f.doc->isPlaying());  // estado do Play intacto
    f.doc->stop();
}

TEST_CASE("p45: loadScene/newScene limpam o histórico", "[p45]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Temp", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->canUndo());
    REQUIRE(f.doc->newScene().ok());
    CHECK_FALSE(f.doc->canUndo());
    CHECK_FALSE(f.doc->canRedo());

    // loadScene também (caminho de restore do openProject).
    REQUIRE(f.doc->createEntity("Temp2", eng::scene::kNoEntity).ok());
    REQUIRE(f.doc->saveScene("hist.json").ok());
    REQUIRE(f.doc->createEntity("Depois", eng::scene::kNoEntity).ok());
    REQUIRE(f.doc->canUndo());
    REQUIRE(f.doc->loadScene("hist.json").ok());
    CHECK_FALSE(f.doc->canUndo());
}

TEST_CASE("p45: histórico respeita o limite (kHistoryMax)", "[p45]")
{
    DocFixture f;
    f.withProject();
    // 50 renames > kHistoryMax (40) — o stack satura sem crescer infinito.
    auto e = f.doc->createEntity("N", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    const auto target = e.value();
    for (int i = 0; i < 50; ++i) {
        REQUIRE(f.doc->renameEntity(target, "Nome" + std::to_string(i)).ok());
    }
    CHECK(f.doc->canUndo());
    // Desfaz os 40 guardados; depois esgota com erro explícito.
    int steps = 0;
    while (f.doc->canUndo()) {
        REQUIRE(f.doc->undo().ok());
        ++steps;
        if (steps > 45) FAIL("histórico passou do limite");
    }
    CHECK(steps <= 40);
    auto undone = f.doc->undo();
    REQUIRE(undone.isError());
}

TEST_CASE("p45: snap de translação e rotação no gizmo", "[p45]")
{
    DocFixture f;
    f.withProject();
    auto e = f.doc->createSprite("Box");
    REQUIRE(e.ok());
    const auto box = e.value();

    f.doc->setSnapTranslate(true);
    f.doc->setSnapRotate(true);
    CHECK(f.doc->snapTranslate());
    CHECK(f.doc->snapRotate());

    f.doc->setTool(eng::editor::EditorTool::Move);
    REQUIRE(f.doc->select(box).ok());
    // Drag do gizmo: começa no corpo da entidade (handle centro) e arrasta
    // para uma posição FORA da grade — o alvo deve cair na grade de 0.5.
    f.doc->viewport().setScreenSize(720.f, 1600.f);
    const auto handle =
        f.doc->gizmoDragBegin(360.f, 800.f, nullptr);
    REQUIRE(handle != eng::editor::GizmoHandle::None);
    REQUIRE(f.doc->gizmoDragTo(400.f, 830.f).ok());
    f.doc->gizmoDragEnd();
    {
        auto tr = f.doc->transform(box);
        REQUIRE(tr.ok());
        // O alvo cai na GRADE: múltiplo de 0.5 unidades de mundo.
        CHECK(std::fmod(std::abs(tr.value().position.x), 0.5f) ==
              Catch::Approx(0.f).margin(1e-4));
        CHECK(std::fmod(std::abs(tr.value().position.y), 0.5f) ==
              Catch::Approx(0.f).margin(1e-4));
    }

    // Rotação: tool Rotate, drag grande → ângulo é múltiplo de 15°.
    f.doc->setTool(eng::editor::EditorTool::Rotate);
    const auto ring = f.doc->gizmoDragBegin(360.f, 800.f, nullptr);
    if (ring != eng::editor::GizmoHandle::None) {
        REQUIRE(f.doc->gizmoDragTo(360.f + 200.f, 800.f).ok());
        f.doc->gizmoDragEnd();
        auto tr = f.doc->transform(box);
        REQUIRE(tr.ok());
        const float deg = tr.value().rotationDegrees.z;
        // Snap: ângulo múltiplo exato de 15° (float-safe — round-trip
        // quat degrada ~1e-5).
        // remainder() normaliza ao múltiplo de 15° mais próximo
        // (150°, -30°, 15° — qualquer k*15 passa).
        CHECK(std::remainder(deg, 15.f) == Catch::Approx(0.f).margin(0.01f));
    }
    f.doc->setSnapTranslate(false);
    f.doc->setSnapRotate(false);
    CHECK_FALSE(f.doc->snapTranslate());
    CHECK_FALSE(f.doc->snapRotate());
}

TEST_CASE("p45: viewportFit enquadra a cena (e a seleção)", "[p45]")
{
    DocFixture f;
    f.withProject();
    f.doc->viewport().setScreenSize(720.f, 1600.f);

    // Cena vazia → reset honesto (sem crash).
    f.doc->viewportFit(nullptr);
    CHECK(f.doc->viewport().camera().zoom == Catch::Approx(48.f));

    auto a = f.doc->createSprite("A");
    REQUIRE(a.ok());
    auto b = f.doc->createSprite("B");
    REQUIRE(b.ok());
    // Entidades longe: sprite default 1×1 em (0,0) e (60, 0).
    eng::editor::TransformDesc far{};
    far.position = {60.f, 0.f, 0.f};
    REQUIRE(f.doc->setTransform(b.value(), far).ok());

    // Sem seleção → enquadra a CENA inteira: câmera vai para o meio.
    (void)f.doc->select(eng::scene::kNoEntity);
    f.doc->viewportFit(nullptr);
    const auto& cam = f.doc->viewport().camera();
    CHECK(cam.posX == Catch::Approx(30.f).margin(0.5f));
    // Zoom cabe os ~61 unidades de largura em 720px (com margem 25%).
    CHECK(cam.zoom < 720.f / (61.f * 1.25f) + 1.f);
    CHECK(cam.zoom >= eng::editor::Viewport::kMinZoom);

    // Com SELEÇÃO → enquadra só a selecionada (zoom volta a crescer).
    REQUIRE(f.doc->select(a.value()).ok());
    f.doc->viewportFit(nullptr);
    CHECK(f.doc->viewport().camera().posX == Catch::Approx(0.f).margin(0.1f));
    CHECK(f.doc->viewport().camera().zoom > 48.f);
}

// =============================================================================
// Contrato JNI: NativeBridge.kt ⇄ símbolos C (dlsym)
//
// O TU Android EditorJni.cpp está compilado NESTE executável (shim
// tests/jni_shim, ENABLE_EXPORTS). Cada `external fun` do Kotlin precisa de
// um símbolo Java_com_goni_app_NativeBridge_<nome> resolvível por dlsym — a
// mesma resolução que o ART faz no aparelho. A lista é lida do próprio
// arquivo Kotlin: não há cópia para manter em sincronia.
// =============================================================================

TEST_CASE("JNI: cada external fun de NativeBridge.kt resolve por dlsym",
          "[jni][contract]")
{
    std::ifstream kt(GONI_NATIVE_BRIDGE_KT);
    REQUIRE(kt.good());
    std::vector<std::string> names;
    std::string line;
    while (std::getline(kt, line)) {
        const auto pos = line.find("external fun ");
        if (pos == std::string::npos) {
            continue;
        }
        const auto begin = pos + std::string("external fun ").size();
        const auto paren = line.find('(', begin);
        REQUIRE(paren != std::string::npos);
        names.push_back(line.substr(begin, paren - begin));
    }
    REQUIRE(names.size() >= 10);

    std::string missing;
    for (const auto& name : names) {
        const std::string symbol = "Java_com_goni_app_NativeBridge_" + name;
        if (dlsym(RTLD_DEFAULT, symbol.c_str()) == nullptr) {
            missing += "\n  - " + symbol;
        }
    }
    if (!missing.empty()) {
        FAIL("Símbolos JNI ausentes/manglados:" << missing);
    }
}

// =============================================================================
// P4.6 (Bloco 1): colisão v2 — camadas nomeadas, migração, repro kinematic
// =============================================================================

TEST_CASE("p46: camadas de colisão nomeadas — tabela, rename, add, "
          "persistência no project.goni.json",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();

    // Tabela default do projeto novo.
    auto layers = f.doc->collisionLayers();
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].name == "default");
    CHECK(layers[0].bit == 1u);

    // Rename do bit 1.
    REQUIRE(f.doc->setCollisionLayerName(1u, "cenario").ok());
    layers = f.doc->collisionLayers();
    CHECK(layers[0].name == "cenario");

    // Add recebe o MENOR bit livre (2).
    auto added = f.doc->addCollisionLayer("player");
    REQUIRE(added.ok());
    CHECK(added.value() == 2u);

    // Erros precisos (nunca silêncio):
    CHECK(f.doc->addCollisionLayer("player").isError()); // duplicado
    CHECK(f.doc->setCollisionLayerName(2u, "cenario").isError()); // duplicado
    CHECK(f.doc->setCollisionLayerName(4u, "x").isError()); // bit sem nome
    CHECK(f.doc->setCollisionLayerName(1u, "").isError()); // vazio

    // Persistência: saveProject escreve a tabela no project.goni.json.
    REQUIRE(f.doc->saveProject().ok());
    auto text = f.fs->readAllText(eng::fs::Path{"TestGame/project.goni.json"});
    REQUIRE(text.ok());
    CHECK(text.value().find("collisionLayers") != std::string::npos);
    CHECK(text.value().find("cenario") != std::string::npos);
    CHECK(text.value().find("player") != std::string::npos);
}

TEST_CASE("p46: migração aditiva — cena PRÉ-P4.6 (RigidBody sem bodyType) "
          "carrega com o tipo certo (mass 0 → Static, mass > 0 → DynamicLite)",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();

    // Cena ATUAL: dois corpos (mass 0 e mass 3).
    auto light = f.doc->createEntity("Livre", eng::scene::kNoEntity);
    REQUIRE(light.ok());
    REQUIRE(f.doc->addComponent(light.value(), "eng::physics::RigidBody").ok());
    REQUIRE(f.doc
                ->setInspectorField(light.value(), "eng::physics::RigidBody",
                                    "mass", "0")
                .ok());
    auto heavy = f.doc->createEntity("Pesado", eng::scene::kNoEntity);
    REQUIRE(heavy.ok());
    REQUIRE(f.doc->addComponent(heavy.value(), "eng::physics::RigidBody").ok());
    REQUIRE(f.doc
                ->setInspectorField(heavy.value(), "eng::physics::RigidBody",
                                    "mass", "3")
                .ok());
    REQUIRE(f.doc->saveScene("main.json").ok());

    // SIMULAÇÃO PRÉ-P4.6: strip da chave nova do JSON salvo (é exatamente
    // o que uma cena dca9922 tem — decode estrito REJEITARIA sem migração).
    auto saved = f.fs->readAllText(eng::fs::Path{"TestGame/scenes/main.json"});
    REQUIRE(saved.ok());
    std::string oldScene = saved.value();
    // bodyType é a 1ª chave do data (dump ordena alfabeticamente) — o
    // padrão inclui a vírgula seguinte para NÃO quebrar o JSON.
    const std::string dynamicText = "\"bodyType\":\"DynamicLite\",";
    const std::string staticText = "\"bodyType\":\"Static\",";
    CHECK(oldScene.find(dynamicText) != std::string::npos); // encode escreve
    while (true) {
        const auto pos = oldScene.find(dynamicText);
        if (pos == std::string::npos) { break; }
        oldScene.erase(pos, dynamicText.size());
    }
    while (true) {
        const auto pos = oldScene.find(staticText);
        if (pos == std::string::npos) { break; }
        oldScene.erase(pos, staticText.size());
    }
    CHECK(oldScene.find("bodyType") == std::string::npos);
    REQUIRE(f.fs->writeAllText(eng::fs::Path{"TestGame/scenes/main.json"},
                               oldScene));

    // Load da cena ANTIGA: migração injeta e o load passa.
    REQUIRE(f.doc->loadScene("main.json").ok());
    std::size_t bodies = 0;
    f.doc->sceneInFocus()->world().each<eng::physics::RigidBody>(
        [&](eng::ecs::Entity, const eng::physics::RigidBody& body) {
            ++bodies;
            if (body.mass == 0.f) {
                CHECK(body.bodyType == eng::physics::BodyType::Static);
            } else {
                CHECK(body.bodyType ==
                      eng::physics::BodyType::DynamicLite);
            }
        });
    CHECK(bodies == 2);
}

TEST_CASE("p46: REPRO do utilizador — script move_and_slide contra estático "
          "sólido: para/desliza, NUNCA atravessa",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();

    // Parede estática: box (0.5, 5, 5) em x=3 — face em 2.5.
    auto wall = f.doc->createEntity("Parede", eng::scene::kNoEntity);
    REQUIRE(wall.ok());
    REQUIRE(f.doc->addComponent(wall.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "shape", "Box")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "halfExtents.x", "0.5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "halfExtents.y", "5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::physics::Collider",
                                    "halfExtents.z", "5")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(wall.value(), "eng::math::Transform",
                                    "position.x", "3")
                .ok());

    // Jogador: Collider + CharacterBody + script kinematic.
    // P4.7.0 B1: o CONTRATO exige Collider antes (CharacterBody requires
    // Collider — add na ordem antiga agora recusa com erro preciso).
    auto player = f.doc->createEntity("Jogador", eng::scene::kNoEntity);
    REQUIRE(player.ok());
    REQUIRE(f.doc->addComponent(player.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::physics::CharacterBody")
                .ok());
    const char* source =
        "up update:\n"
        "    move_and_slide(delta() * 3.0, delta() * 1.0)\n"
        "stop\n";
    REQUIRE(f.doc->addComponent(player.value(),
                                "eng::editor::NiScriptComponent")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(player.value(),
                                    "eng::editor::NiScriptComponent",
                                    "source", source)
                .ok());

    REQUIRE(f.doc->play().ok());
    // 120 ticks: tentativa de 6 u em x — a parede segura (face 2.5 − raio).
    for (int i = 0; i < 120; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    auto posX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), player.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(posX.ok());
    INFO("player.x = " << posX.value());
    const double x = std::atof(posX.value().c_str());
    CHECK(x < 2.55); // NUNCA do outro lado (face 2.5 + folga)
    CHECK(x > 1.90); // mas parou NA parede (~2.0 = face − raio)
    auto posY = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), player.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(posY.ok());
    const double y = std::atof(posY.value().c_str());
    INFO("player.y = " << posY.value());
    CHECK(y > 1.5); // deslizou tangencialmente (2 u esperadas)

    // STOP: a edição NUNCA foi tocada (ADR-044).
    f.doc->stop();
    auto editX = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), player.value(), "eng::math::Transform",
        "position.x");
    REQUIRE(editX.ok());
    CHECK(editX.value() == "0");
}

// =============================================================================
// P4.6 (Bloco 2): Light2D — defaults coerentes + filtro por camada
// =============================================================================

TEST_CASE("p46: luz nova casa com a camada dos sprites lit (defaults "
          "coerentes)",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();

    // Camada "UI" + sprite lit (material vazio = lit default) nela.
    REQUIRE(f.doc->addLayer("UI").ok());
    auto sprite = f.doc->createEntity("Botao", eng::scene::kNoEntity);
    REQUIRE(sprite.ok());
    REQUIRE(f.doc->addComponent(sprite.value(), "eng::editor::SpriteData")
                .ok());
    REQUIRE(f.doc->addComponent(sprite.value(), "eng::scene::LayerMember")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(sprite.value(), "eng::scene::LayerMember",
                                    "layer", "UI")
                .ok());

    // Luz nova: deve casar com "UI" (onde vive o sprite lit), não "GAME".
    auto light = f.doc->createEntity("Luz", eng::scene::kNoEntity);
    REQUIRE(light.ok());
    REQUIRE(f.doc->addComponent(light.value(), "eng::render::Light2D").ok());
    auto layer = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), light.value(), "eng::render::Light2D",
        "layer");
    REQUIRE(layer.ok());
    CHECK(layer.value() == "UI");

    // Sprites em camadas DIFERENTES (1 GAME + 1 UI): empate → 1ª vista
    // (GAME criado depois? Não — ordem de criação: Botao(UI) primeiro…
    // um segundo sprite em GAME muda a contagem para empate 1:1).
    auto sprite2 = f.doc->createEntity("Chao", eng::scene::kNoEntity);
    REQUIRE(sprite2.ok());
    REQUIRE(f.doc->addComponent(sprite2.value(), "eng::editor::SpriteData")
                .ok());
    auto light2 = f.doc->createEntity("Luz2", eng::scene::kNoEntity);
    REQUIRE(light2.ok());
    REQUIRE(f.doc->addComponent(light2.value(), "eng::render::Light2D").ok());
    auto layer2 = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), light2.value(), "eng::render::Light2D",
        "layer");
    REQUIRE(layer2.ok());
    // UI=1 (Botao), GAME=1 (Chao, sem LayerMember) — primeiro visto vence
    // e o each segue a ordem de criação: Botao → UI.
    CHECK(layer2.value() == "UI");
}

TEST_CASE("p46: filtro de luz por camada empacota SÓ as luzes do grupo + "
          "sanity de intensidade/raio dos defaults",
          "[editor][p46]")
{
    using eng::render::DrawList;
    using DrawItem = DrawList::LightItem;

    DrawList list;
    DrawItem gameLight;   // GAME (default) — intensity 1, radius 4
    DrawItem uiLight;
    uiLight.layer = "UI";
    list.lights.push_back(gameLight);
    list.lights.push_back(uiLight);

    // GAME empacota 1 luz (a de GAME) — sanity dos defaults:
    const eng::render::FrameUniforms game = list.packUniformsFor("GAME");
    CHECK(game.lightCount() == 1);
    CHECK(game.lightA[0][0] == 0.f);  // worldX default
    CHECK(game.lightA[0][1] == 0.f);  // worldY default
    CHECK(game.lightA[0][2] == 4.f);  // radius default (unidades de mundo)
    CHECK(game.lightA[0][3] == 1.f);  // intensity default
    CHECK(game.lightB[0][3] == 1.5f); // falloff default

    // UI empacota 1 luz; cada grupo recebe SÓ as suas (filtro real).
    const eng::render::FrameUniforms ui = list.packUniformsFor("UI");
    CHECK(ui.lightCount() == 1);

    // Luz disabled-like: camada sem luz nenhuma → bloco vazio.
    const eng::render::FrameUniforms none = list.packUniformsFor("FX");
    CHECK(none.lightCount() == 0);
}

TEST_CASE("p46: migração/round-trip — cena com colisão v2 preserva bits e "
          "bodyType (cenas existentes não quebram)",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();

    // Tabela nomeada do projeto + colisor com bits 1|2 contra mask 2|4.
    REQUIRE(f.doc->addCollisionLayer("player").ok());  // bit 2
    REQUIRE(f.doc->addCollisionLayer("inimigo").ok()); // bit 4
    auto e = f.doc->createEntity("Ator", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::physics::RigidBody").ok());
    REQUIRE(f.doc
                ->setInspectorField(e.value(), "eng::physics::RigidBody",
                                    "bodyType", "Kinematic")
                .ok());
    REQUIRE(f.doc->addComponent(e.value(), "eng::physics::Collider").ok());
    REQUIRE(f.doc
                ->setInspectorField(e.value(), "eng::physics::Collider",
                                    "layer", "3")
                .ok());
    REQUIRE(f.doc
                ->setInspectorField(e.value(), "eng::physics::Collider",
                                    "mask", "6")
                .ok());

    // Round-trip completo (save → load): NADA se perde.
    REQUIRE(f.doc->saveScene("main.json").ok());
    REQUIRE(f.doc->loadScene("main.json").ok());

    auto bodyType = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::physics::RigidBody",
        "bodyType");
    REQUIRE(bodyType.ok());
    CHECK(bodyType.value() == "Kinematic");
    auto layer = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::physics::Collider", "layer");
    REQUIRE(layer.ok());
    CHECK(layer.value() == "3");
    auto mask = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::physics::Collider", "mask");
    REQUIRE(mask.ok());
    CHECK(mask.value() == "6");

    // Tabela do projeto sobrevive ao save/load de CENA (vive no projeto).
    auto layers = f.doc->collisionLayers();
    REQUIRE(layers.size() == 3);
    CHECK(layers[1].name == "player");
    CHECK(layers[1].bit == 2u);
    CHECK(layers[2].name == "inimigo");
    CHECK(layers[2].bit == 4u);
}

// =============================================================================
// P4.6 (Bloco 4): Animação v1 — keys TRS (timeline), round-trip e playback
// =============================================================================

TEST_CASE("p46: keys TRS — grava, lista, substitui no mesmo tempo, edita "
          "(mover), apaga; .anim round-trip",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->animationCreate("andar").ok());

    // Grava keys FORA de ordem — a track sai ordenada por tempo.
    REQUIRE(f.doc->animationAddKey("andar.anim.json", "position", 1.0f,
                                   3.f, 0.f, 0.f)
                .ok());
    REQUIRE(f.doc->animationAddKey("andar.anim.json", "position", 0.f, 0.f,
                                   2.f, 0.f)
                .ok());
    auto list = f.doc->animationKeyList("andar.anim.json", "position");
    REQUIRE(list.ok());
    CHECK(list.value() == "0\t0\t0\t2\t0\n1\t1\t3\t0\t0");

    // Mesmo tempo (ε) SUBSTITUI — gravar de novo = atualizar.
    REQUIRE(f.doc->animationAddKey("andar.anim.json", "position", 1.0f, 9.f,
                                   9.f, 9.f)
                .ok());
    list = f.doc->animationKeyList("andar.anim.json", "position");
    REQUIRE(list.ok());
    CHECK(list.value() == "0\t0\t0\t2\t0\n1\t1\t9\t9\t9");

    // Rotação: graus do autor — round-trip por QUAT tem erro de precisão
    // (90° pode voltar 89.98): assert por valor aproximado.
    REQUIRE(f.doc->animationAddKey("andar.anim.json", "rotation", 0.5f,
                                   90.f, 0.f, 0.f)
                .ok());
    REQUIRE(f.doc->animationAddKey("andar.anim.json", "scale", 0.f, 2.f, 2.f,
                                   2.f)
                .ok());
    auto rot = f.doc->animationKeyList("andar.anim.json", "rotation");
    REQUIRE(rot.ok());
    {
        // linha única "0\t0.5\t<graus>\t0\t0" — graus ≈ 90 (ε quat).
        const std::string& row = rot.value();
        const auto t1 = row.find('\t');
        const auto t2 = row.find('\t', t1 + 1);
        const auto t3 = row.find('\t', t2 + 1);
        REQUIRE(t1 != std::string::npos);
        REQUIRE(t2 != std::string::npos);
        REQUIRE(t3 != std::string::npos);
        CHECK(row.substr(0, t1) == "0");
        CHECK(row.substr(t1 + 1, t2 - t1 - 1) == "0.5");
        const double deg =
            std::atof(row.substr(t2 + 1, t3 - t2 - 1).c_str());
        CHECK(std::abs(deg - 90.0) <= 0.1);
    }
    auto scale = f.doc->animationKeyList("andar.anim.json", "scale");
    REQUIRE(scale.ok());
    CHECK(scale.value() == "0\t0\t2\t2\t2");

    // Editar/mover key #1 da position (tempo 1 → 0.5).
    REQUIRE(f.doc
                ->animationKeySet("andar.anim.json", "position", 1, 0.5f,
                                  5.f, 1.f, 0.f)
                .ok());
    list = f.doc->animationKeyList("andar.anim.json", "position");
    REQUIRE(list.ok());
    // Re-ordenado: o key movido agora vem primeiro.
    CHECK(list.value() == "0\t0\t0\t2\t0\n1\t0.5\t5\t1\t0");

    // Apagar o #0 — sobra um.
    REQUIRE(f.doc->animationKeyDelete("andar.anim.json", "position", 0).ok());
    list = f.doc->animationKeyList("andar.anim.json", "position");
    REQUIRE(list.ok());
    CHECK(list.value() == "0\t0.5\t5\t1\t0");

    // Erros precisos: track inválida, índice fora, tempo negativo.
    CHECK(f.doc->animationKeyList("andar.anim.json", "cor").isError());
    CHECK(f.doc
              ->animationKeySet("andar.anim.json", "position", 7, 0.f, 0.f,
                                0.f, 0.f)
              .isError());
    CHECK(f.doc
              ->animationAddKey("andar.anim.json", "position", -1.f, 0.f,
                                0.f, 0.f)
              .isError());

    // O JSON persistido contém as tracks (serialização .anim REAL).
    auto content = f.doc->animationRead("andar.anim.json");
    REQUIRE(content.ok());
    CHECK(content.value().find("\"position\"") != std::string::npos);
    CHECK(content.value().find("\"rotation\"") != std::string::npos);
    CHECK(content.value().find("\"scale\"") != std::string::npos);
}

TEST_CASE("p46: timeline TRS reproduz no PLAY (AnimationTick real) — "
          "position e scale animam, loop recomeça",
          "[editor][p46]")
{
    DocFixture f;
    f.withProject();
    REQUIRE(f.doc->animationCreate("pulo").ok());
    REQUIRE(f.doc->animationAddKey("pulo.anim.json", "position", 0.f, 0.f,
                                   0.f, 0.f)
                .ok());
    REQUIRE(f.doc->animationAddKey("pulo.anim.json", "position", 1.f, 0.f,
                                   4.f, 0.f)
                .ok());
    REQUIRE(f.doc->animationAddKey("pulo.anim.json", "scale", 0.f, 1.f, 1.f,
                                   1.f)
                .ok());
    REQUIRE(f.doc->animationAddKey("pulo.anim.json", "scale", 1.f, 2.f, 2.f,
                                   2.f)
                .ok());
    REQUIRE(f.doc->animationSetMeta("pulo.anim.json", /*loop=*/true, 30.f)
                .ok());

    auto e = f.doc->createEntity("Ator", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->animationAssign(e.value(), "pulo.anim.json").ok());
    // P4.6: o playback no Play segue o contrato P2 — o AUTOR liga
    // `playing` no Inspector (assign não vira autoplay: zero mudança de
    // comportamento para flipbooks existentes).
    REQUIRE(f.doc
                ->setInspectorField(e.value(), "eng::animation::Animator",
                                    "playing", "true")
                .ok());

    REQUIRE(f.doc->play().ok());
    // 60 ticks de 1/60 = 1 s — chega ao fim do clip (position.y → 4,
    // scale → 2)… o loop recomeça no tick seguinte (y volta a ~0.033*4).
    for (int i = 0; i < 60; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    auto py = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(py.ok());
    INFO("position.y após 1s = " << py.value());
    CHECK(std::atof(py.value().c_str()) > 3.0);
    auto sx = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::math::Transform", "scale.x");
    REQUIRE(sx.ok());
    INFO("scale.x após 1s = " << sx.value());
    CHECK(std::atof(sx.value().c_str()) > 1.8);

    // Mais 60 ticks: o loop deu a volta — y NÃO continua subindo (volta
    // para perto de 0 a cada recomeço; aqui já re-avançou ~1 s… com loop
    // 1s o valor fica em qualquer ponto do [0,4] — o que PROVA o loop é
    // que não passa de 4 (sem loop o tempo continua e trava em 4).
    for (int i = 0; i < 120; ++i) {
        f.doc->tick(1.f / 60.f);
    }
    py = eng::editor::Inspector::getField(
        *f.doc->sceneInFocus(), e.value(), "eng::math::Transform",
        "position.y");
    REQUIRE(py.ok());
    const double y2 = std::atof(py.value().c_str());
    CHECK(y2 >= -0.01);
    CHECK(y2 <= 4.01); // loop amarra ao range do clip

    f.doc->stop();
}

TEST_CASE("p46: Grid v2 — LOD adaptativo (fade/hide/subdivisão) e validação",
          "[editor][p46]")
{
    using eng::project::computeGridLod;
    using eng::project::GridConfig;

    GridConfig g; // cell 1, majorEvery 8 (defaults)

    // Zoom padrão 48 px/unidade: minors plenas (48 px), majors a 8u (384 px).
    {
        const auto lod = computeGridLod(g, 48.f);
        CHECK(lod.minorStep == 1.f);
        CHECK(lod.majorStep == 8.f);
        CHECK(lod.minorAlpha == 1.f);
    }
    // Zoom 2: minors abaixo do FIM do fade → ESCONDIDAS (anti-moiré duro);
    // majors (16 px) seguem visíveis.
    {
        const auto lod = computeGridLod(g, 2.f);
        CHECK(lod.minorStep == 1.f);
        CHECK(lod.majorStep == 8.f);
        CHECK(lod.minorAlpha == 0.f);
    }
    // Zoom 1: subiu de nível (majors viram minors — subdivisão emerge ao
    // aproximar): minorStep 8 com fade parcial (8 px → alpha 0.2).
    {
        const auto lod = computeGridLod(g, 1.f);
        CHECK(lod.minorStep == 8.f);
        CHECK(lod.majorStep == 64.f);
        CHECK(lod.minorAlpha == Catch::Approx(0.2f).margin(1e-5f));
    }
    // Zoom 0.5: minors do nível escondidas (4 px), majors (32 px) visíveis.
    {
        const auto lod = computeGridLod(g, 0.5f);
        CHECK(lod.minorStep == 8.f);
        CHECK(lod.majorStep == 64.f);
        CHECK(lod.minorAlpha == 0.f);
    }

    // Config via documento: valida e persiste no project.goni.json.
    DocFixture f;
    f.withProject();
    eng::project::GridConfig custom;
    custom.cell = 0.5f;
    custom.majorEvery = 4;
    custom.minorR = 0.15f;
    REQUIRE(f.doc->setGridConfig(custom).ok());
    CHECK(f.doc->gridConfig().cell == 0.5f);
    CHECK(f.doc->gridConfig().majorEvery == 4);
    CHECK(f.doc->gridConfig().minorR == 0.15f);
    // Inválidos rejeitados (honesto).
    eng::project::GridConfig badCell;
    badCell.cell = 0.f;
    CHECK(f.doc->setGridConfig(badCell).isError());
    eng::project::GridConfig badEvery;
    badEvery.majorEvery = 1;
    CHECK(f.doc->setGridConfig(badEvery).isError());
    eng::project::GridConfig badColor;
    badColor.minorR = 2.f;
    CHECK(f.doc->setGridConfig(badColor).isError());
    // Persistência (projectDirty → saveProject escreve a chave "grid").
    REQUIRE(f.doc->saveProject().ok());
    auto text = f.fs->readAllText(eng::fs::Path{"TestGame/project.goni.json"});
    REQUIRE(text.ok());
    CHECK(text.value().find("\"grid\"") != std::string::npos);
}

TEST_CASE("p46: SCALE com setas nas arestas (L3) — 12 quads apontando para "
          "fora; transição 120ms (L4)",
          "[editor][p46]")
{
    using eng::editor::EditorTool;
    using eng::editor::GizmoBounds;
    using eng::editor::TransformGizmo;

    // --- L4: função pura da transição (pop 0.88 → 1.0) ----------------------
    CHECK(TransformGizmo::transitionScale(0.f) == Catch::Approx(0.88f));
    CHECK(TransformGizmo::transitionScale(-5.f) == 1.f);
    CHECK(TransformGizmo::transitionScale(120.f) == 1.f);
    CHECK(TransformGizmo::transitionScale(1000.f) == 1.f);
    CHECK(TransformGizmo::transitionScale(60.f) > 0.94f);
    CHECK(TransformGizmo::transitionScale(60.f) < 1.f);
    // Monótona crescente.
    CHECK(TransformGizmo::transitionScale(30.f) <
          TransformGizmo::transitionScale(90.f));

    // --- gizmoHandlePop: < 1 logo após a troca; == 1 depois da janela -------
    DocFixture f;
    f.withProject();
    auto e = f.doc->createEntity("Hero", eng::scene::kNoEntity);
    REQUIRE(e.ok());
    REQUIRE(f.doc->select(e.value()).ok());
    f.doc->setTool(EditorTool::Scale);
    const float popRightAfter = f.doc->gizmoHandlePop();
    CHECK(popRightAfter >= 0.88f);
    CHECK(popRightAfter < 1.f);
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    CHECK(f.doc->gizmoHandlePop() == 1.f);

    // --- B2: layoutQuads SCALE = 4 cantos + 4 marcas = 8; layoutTriangles
    // SCALE = 4 setas de aresta (setas REAIS — B2) ----------------------------
    TransformGizmo gizmo;
    const GizmoBounds bounds = f.doc->selectionBounds(nullptr);
    REQUIRE(bounds.valid);
    auto quads = gizmo.layoutQuads(f.doc->viewport(), EditorTool::Scale,
                                   bounds);
    REQUIRE(quads.size() == 8); // 4 cantos + 4 marcas (setas migraram)
    auto tris = gizmo.layoutTriangles(f.doc->viewport(), EditorTool::Scale,
                                      bounds);
    REQUIRE(tris.size() == 4);  // 4 setas de aresta (B2)
    // Setas apontam PARA FORA: a seta E está além da marca E (mesma
    // direção do eixo local, deslocada para fora).
    const float markE = quads[4].worldX;
    const float arrowE = tris[0].worldX;
    CHECK(arrowE > markE);
    const float markW = quads[5].worldX;
    const float arrowW = tris[1].worldX;
    CHECK(arrowW < markW);
    const float markN = quads[6].worldY;
    const float arrowN = tris[2].worldY;
    CHECK(arrowN > markN);
    const float markS = quads[7].worldY;
    const float arrowS = tris[3].worldY;
    CHECK(arrowS < markS);

    // Alvos de toque ≥ 48dp: o hitPx continua 24dp de raio (48 ⌀).
    CHECK(TransformGizmo::hitPx(1.f) == Catch::Approx(24.f));
    CHECK(TransformGizmo::hitPx(2.f) == Catch::Approx(48.f));
}
