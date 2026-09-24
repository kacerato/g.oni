/// Testes do Build & Export Pipeline.
///
/// Cobertura obrigatória da missão (design phase12_audit/design.md §11):
/// projeto vazio/mínimo, asset ausente/não-usado/duplicado, script que
/// não compila (embutido e standalone), manifest/target inválidos, path
/// absoluto, cache hit/miss/invalidação, clean/incremental, determinismo
/// byte-a-byte e E2E de export (android-arm64 + linux-dev).

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "eng/build/Build.hpp"
#include "eng/fs/FileSystem.hpp"
#include "eng/fs/MemoryFileSystem.hpp"

using eng::build::BuildDiag;
using eng::build::BuildOptions;
using eng::build::buildProject;
using eng::fs::MemoryFileSystem;
using eng::fs::Path;

namespace {

/// Fixture: projeto mínimo montado em memória.
struct ProjectFixture {
    MemoryFileSystem fs;
    Path root{".p12"};

    ProjectFixture()
    {
        REQUIRE(fs.mkdirs(root / Path{"scenes"}).ok());
        writeProjectJson();
        writeBuildJson();
        writeScene("scenes/main.json");
        writeRegistry({"scenes/main.json"});
    }

    void writeProjectJson()
    {
        const std::string text =
            "{\"formatVersion\":1,"
            "\"projectId\":\"01234567-89ab-4def-8123-456789abcdef\","
            "\"name\":\"TestGame\","
            "\"engineVersion\":\"0.1.0\","
            "\"assetRegistryPath\":\"asset_registry.json\","
            "\"sceneRoots\":[\"scenes\"]}";
        REQUIRE(fs.writeAllText(root / Path{"project.goni.json"}, text).ok());
    }

    void writeBuildJson(const std::string& extraTargets = "",
                        const std::string& entries = "\"scenes/main.json\"")
    {
        const std::string text = "{\"formatVersion\":1,"
                                 "\"targets\":[\"android-arm64\",\"linux-dev\""
                                 + extraTargets
                                 + "],\"entryScenes\":[" + entries + "]}";
        REQUIRE(fs.writeAllText(root / Path{"build.json"}, text).ok());
    }

    void writeScene(const std::string& rel)
    {
        const std::string scene =
            "{\"formatVersion\":1,\"sceneEntityIds\":[],\"entities\":[]}";
        REQUIRE(fs.writeAllText(root / Path{rel}, scene).ok());
    }

    void writeRegistry(const std::vector<std::string>& paths)
    {
        std::string assets;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            char suffix[32]; // folga: GCC não consegue provar o bound
            std::snprintf(suffix, sizeof suffix, "%03zx", i);
            if (i != 0) {
                assets += ",";
            }
            assets += std::string("{\"id\":\"")
                    + "01234567-89ab-4def-8123-456789abc" + suffix
                    + "\",\"type\":\"Scene\",\"sourcePath\":\"" + paths[i]
                    + "\"}";
        }
        const std::string text =
            "{\"formatVersion\":1,\"assets\":[" + assets + "]}";
        REQUIRE(
            fs.writeAllText(root / Path{"asset_registry.json"}, text).ok());
    }

    void writeFile(const std::string& rel, const std::string& text)
    {
        const auto target = root / Path{rel};
        const auto parent = target.parent();
        if (!parent.str().empty()) {
            REQUIRE(fs.mkdirs(parent).ok());
        }
        REQUIRE(fs.writeAllText(target, text).ok());
    }

    /// Cena com script NI embutido (componente do catálogo).
    void writeSceneWithScript(const std::string& rel,
                              const std::string& scriptSource)
    {
        std::string escaped;
        for (const char c : scriptSource) {
            if (c == '"') {
                escaped += "\\\"";
            } else if (c == '\\') {
                escaped += "\\\\";
            } else if (c == '\n') {
                escaped += "\\n";
            } else {
                escaped += c;
            }
        }
        const std::string scene =
            "{\"formatVersion\":1,"
            "\"sceneEntityIds\":[\"01234567-89ab-4def-8123-456789abc000\"],"
            "\"entities\":[{\"id\":\"01234567-89ab-4def-8123-456789abc000\","
            "\"parent\":null,\"components\":[{\"type\":"
            "\"eng::editor::NiScriptComponent\",\"data\":{\"source\":\""
            + escaped + "\"}}]}]}";
        writeFile(rel, scene);
    }
};

} // namespace

// =============================================================================
// Config (etapa 2) — alvos/paths/manifesto
// =============================================================================

TEST_CASE("build: build.json ausente → erro claro", "[build]")
{
    ProjectFixture f;
    REQUIRE(f.fs.remove(f.root / Path{"build.json"}).ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("build.json") != std::string::npos);
}

TEST_CASE("build: target desconhecido → erro", "[build]")
{
    ProjectFixture f;
    f.writeBuildJson(",\"windows-x64\"");
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("target desconhecido") != std::string::npos);
}

TEST_CASE("build: targets vazio → erro", "[build]")
{
    ProjectFixture f;
    REQUIRE(f.fs.writeAllText(f.root / Path{"build.json"},
                               "{\"formatVersion\":1,\"targets\":[],"
                               "\"entryScenes\":[\"scenes/main.json\"]}")
                .ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("targets") != std::string::npos);
}

TEST_CASE("build: formatVersion do manifest inválido → NotSupported", "[build]")
{
    ProjectFixture f;
    REQUIRE(f.fs.writeAllText(f.root / Path{"build.json"},
                               "{\"formatVersion\":99,\"targets\":"
                               "[\"linux-dev\"],\"entryScenes\":"
                               "[\"scenes/main.json\"]}")
                .ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("formatVersion") != std::string::npos);
}

TEST_CASE("build: entryScene inexistente → erro bloqueante", "[build]")
{
    ProjectFixture f;
    f.writeBuildJson("", "\"scenes/fantasma.json\"");
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("fantasma") != std::string::npos);
}

TEST_CASE("build: path ABSOLUTO na config → erro (missão: relativos)",
          "[build]")
{
    ProjectFixture f;
    REQUIRE(f.fs.writeAllText(
                 f.root / Path{"build.json"},
                 "{\"formatVersion\":1,\"targets\":[\"linux-dev\"],"
                 "\"entryScenes\":[\"/absoluto/cena.json\"]}")
                .ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("RELATIVO") != std::string::npos);
}

// =============================================================================
// Projeto mínimo + determinismo (missão §2/§17)
// =============================================================================

TEST_CASE("build: projeto mínimo — build ok, report completo", "[build]")
{
    ProjectFixture f;
    std::vector<BuildDiag> diags;
    auto r = buildProject(f.fs, f.root, BuildOptions{}, &diags);
    REQUIRE(r.ok());
    REQUIRE(r.value().assetsCooked == 1);
    REQUIRE(r.value().scriptsValidated == 0);
    REQUIRE(r.value().targets.size() == 2);
    REQUIRE(r.value().manifestJson.find("TestGame") != std::string::npos);
}

TEST_CASE("build: determinismo byte-a-byte (2 builds limpos)", "[build]")
{
    ProjectFixture f;
    auto a = buildProject(f.fs, f.root, BuildOptions{});
    auto b = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.value().manifestJson == b.value().manifestJson);
    REQUIRE(a.value().bundleBytes.size() == b.value().bundleBytes.size());
    REQUIRE(a.value().bundleBytes == b.value().bundleBytes);
}

TEST_CASE("build: projeto SEM cena de entrada (vazio) → erro de config",
          "[build]")
{
    ProjectFixture f;
    REQUIRE(f.fs.writeAllText(f.root / Path{"build.json"},
                               "{\"formatVersion\":1,\"targets\":"
                               "[\"linux-dev\"],\"entryScenes\":[]}")
                .ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
}

// =============================================================================
// Assets: ausente / não usado / duplicado (missão §3/§4/§5)
// =============================================================================

TEST_CASE("build: asset registrado ausente no disco → BLOCK", "[build]")
{
    ProjectFixture f;
    f.writeRegistry({"scenes/main.json", "scenes/sumiu.json"});
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("ausente no disco") != std::string::npos);
}

TEST_CASE("build: asset não usado → WARN no report, build segue", "[build]")
{
    ProjectFixture f;
    f.writeFile("scenes/outra.json",
                "{\"formatVersion\":1,\"sceneEntityIds\":[],"
                "\"entities\":[]}");
    f.writeRegistry({"scenes/main.json", "scenes/outra.json"});
    std::vector<BuildDiag> diags;
    auto r = buildProject(f.fs, f.root, BuildOptions{}, &diags);
    REQUIRE(r.ok());
    REQUIRE(r.value().unusedAssets.size() == 1);
    REQUIRE(r.value().assetsCooked == 2); // empacota mesmo (dados do projeto)
    bool warned = false;
    for (const BuildDiag& d : diags) {
        warned = warned || d.warning;
    }
    REQUIRE(warned);
}

TEST_CASE("build: ids duplicados (mesmo sourcePath) → BLOCK", "[build]")
{
    ProjectFixture f;
    // dois ids apontando o MESMO arquivo
    const std::string text =
        "{\"formatVersion\":1,\"assets\":["
        "{\"id\":\"01234567-89ab-4def-8123-456789abc000\","
        "\"type\":\"Scene\",\"sourcePath\":\"scenes/main.json\"},"
        "{\"id\":\"01234567-89ab-4def-8123-456789abc001\","
        "\"type\":\"Scene\",\"sourcePath\":\"scenes/main.json\"}]}";
    REQUIRE(f.fs.writeAllText(f.root / Path{"asset_registry.json"}, text).ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("duplicados") != std::string::npos);
}

TEST_CASE("build: cena referenciando asset por id → aresta (não é unused)",
          "[build]")
{
    ProjectFixture f;
    f.writeFile("scenes/ref.json",
                "{\"formatVersion\":1,\"sceneEntityIds\":[],"
                "\"entities\":[],\"ref\":"
                "\"01234567-89ab-4def-8123-456789abc001\"}");
    f.writeRegistry({"scenes/main.json", "scenes/ref.json"});
    // ref.json NÃO é entry — mas main referencia o id de ref.json
    REQUIRE(f.fs.writeAllText(f.root / Path{"scenes/main.json"},
                              "{\"formatVersion\":1,\"sceneEntityIds\":[],"
                              "\"entities\":[],\"usa\":"
                              "\"01234567-89ab-4def-8123-456789abc001\"}")
                .ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.ok());
    REQUIRE(r.value().unusedAssets.empty());
}

// =============================================================================
// Scripts (missão §6) — compilados no build, bloqueante
// =============================================================================

TEST_CASE("build: script embutido válido é validado e contabilizado",
          "[build]")
{
    ProjectFixture f;
    f.writeSceneWithScript("scenes/main.json",
                           "add &BL\n"
                           "var n: int = 1\n"
                           "up update:\n"
                           "    n = n + 1\n"
                           "stop\n");
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.ok());
    REQUIRE(r.value().scriptsValidated == 1);
}

TEST_CASE("build: script embutido que NÃO compila → BLOCK", "[build]")
{
    ProjectFixture f;
    f.writeSceneWithScript("scenes/main.json",
                           "up update:\n    nada()\nstop\n");
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("não compila") != std::string::npos);
}

TEST_CASE("build: script standalone (.nis) inválido → BLOCK", "[build]")
{
    ProjectFixture f;
    f.writeFile("scripts/game.nis", "var x =\n");
    const std::string text =
        "{\"formatVersion\":1,\"assets\":["
        "{\"id\":\"01234567-89ab-4def-8123-456789abc000\","
        "\"type\":\"Script\",\"sourcePath\":\"scripts/game.nis\"}]}";
    REQUIRE(f.fs.writeAllText(f.root / Path{"asset_registry.json"}, text).ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("script") != std::string::npos);
}

TEST_CASE("build: cena com JSON corrompido → BLOCK", "[build]")
{
    ProjectFixture f;
    REQUIRE(f.fs.writeAllText(f.root / Path{"scenes/main.json"},
                               "{isto não é json")
                .ok());
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.isError());
    REQUIRE(r.error().message.find("JSON") != std::string::npos);
}

// =============================================================================
// Cache — hit/miss/invalidação; clean vs incremental
// =============================================================================

TEST_CASE("build: cache — 1º miss, 2º hit (incremental)", "[build]")
{
    ProjectFixture f;
    auto first = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(first.ok());
    REQUIRE(first.value().cacheMisses == 1);
    REQUIRE(first.value().cacheHits == 0);

    auto second = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(second.ok());
    REQUIRE(second.value().cacheHits == 1);
    REQUIRE(second.value().cacheMisses == 0);

    // bundles idênticos (cache é transparente para o resultado)
    REQUIRE(first.value().bundleBytes == second.value().bundleBytes);
}

TEST_CASE("build: cache — conteúdo mudou → miss (chave inválida)", "[build]")
{
    ProjectFixture f;
    auto first = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(first.ok());
    // muda o conteúdo da cena
    REQUIRE(f.fs.writeAllText(f.root / Path{"scenes/main.json"},
                              "{\"formatVersion\":2,\"sceneEntityIds\":[],"
                              "\"entities\":[]}")
                .ok());
    auto second = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(second.ok());
    REQUIRE(second.value().cacheMisses == 1); // re-cookou
    REQUIRE(second.value().cacheHits == 0);
    REQUIRE(first.value().bundleBytes != second.value().bundleBytes);
}

TEST_CASE("build: cache — cookerVersion mudou → miss", "[build]")
{
    ProjectFixture f;
    auto first = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(first.ok());
    BuildOptions bumped;
    bumped.cookerVersionOverride = 99;
    auto second = buildProject(f.fs, f.root, bumped);
    REQUIRE(second.ok());
    REQUIRE(second.value().cacheMisses == 1);
    REQUIRE(second.value().cacheHits == 0);
}

TEST_CASE("build: forceCook (clean) — ignora o cache", "[build]")
{
    ProjectFixture f;
    auto first = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(first.ok());
    BuildOptions clean;
    clean.forceCook = true;
    auto second = buildProject(f.fs, f.root, clean);
    REQUIRE(second.ok());
    REQUIRE(second.value().cacheMisses == 1);
    REQUIRE(second.value().cacheHits == 0);
    // mesmo assim: determinístico
    REQUIRE(first.value().bundleBytes == second.value().bundleBytes);
}

// =============================================================================
// E2E export + verify
// =============================================================================

TEST_CASE("build E2E: export android-arm64 + linux-dev + verify", "[build][e2e]")
{
    ProjectFixture f;
    // cena com script + asset extra referenciado + script standalone
    f.writeSceneWithScript("scenes/main.json",
                           "add &BL\n"
                           "up update:\n"
                           "    var me = self()\n"
                           "    me.position.x = 1.0\n"
                           "stop\n");
    f.writeFile("scenes/nivel2.json",
                "{\"formatVersion\":1,\"sceneEntityIds\":[],\"entities\":[]}");
    f.writeFile("scripts/game.nis", "var hp: int = 3\n");
    const std::string registry =
        "{\"formatVersion\":1,\"assets\":["
        "{\"id\":\"01234567-89ab-4def-8123-456789abc000\","
        "\"type\":\"Scene\",\"sourcePath\":\"scenes/main.json\"},"
        "{\"id\":\"01234567-89ab-4def-8123-456789abc001\","
        "\"type\":\"Scene\",\"sourcePath\":\"scenes/nivel2.json\"},"
        "{\"id\":\"01234567-89ab-4def-8123-456789abc002\","
        "\"type\":\"Script\",\"sourcePath\":\"scripts/game.nis\"}]}";
    REQUIRE(f.fs.writeAllText(f.root / Path{"asset_registry.json"}, registry)
                .ok());

    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.ok());
    const auto& report = r.value();
    REQUIRE(report.assetsCooked == 3);
    REQUIRE(report.scriptsValidated == 2); // embutido + standalone

    // Arquivos exportados existem (ambos os alvos)
    for (const std::string& target : report.targets) {
        const auto path =
            f.root / Path{"export"} / Path{target} / Path{"TestGame.goni"};
        auto exists = f.fs.exists(path);
        REQUIRE(exists.ok());
        REQUIRE(exists.value());
    }
    auto install = f.fs.exists(
        f.root / Path{"export/android-arm64/INSTALL.md"});
    REQUIRE((install.ok() && install.value()));
    auto readme = f.fs.exists(f.root / Path{"export/linux-dev/README.md"});
    REQUIRE((readme.ok() && readme.value()));

    // O bundle do REPORT decoda e confere (verify é parte do pipeline,
    // mas o teste re-verifica INDEPENDENTEMENTE)
    auto verified = eng::build::verifyBundle(report.bundleBytes);
    REQUIRE(verified.ok());
    REQUIRE(verified.value().entries.size() == 3);
    REQUIRE(verified.value().manifestJson.find("scriptsValidated")
            != std::string::npos);
    // payload SOURCE == conteúdo do arquivo original (verbatim)
    for (const auto& entry : verified.value().entries) {
        auto original = f.fs.readAllBytes(f.root / Path{entry.path});
        REQUIRE(original.ok());
        REQUIRE(original.value()
                == entry.payload); // cook verbatim (design §5)
    }

    // bundle gravado no disco == bundle do report
    auto diskBundle = f.fs.readAllBytes(
        f.root / Path{"export/linux-dev/TestGame.goni"});
    REQUIRE(diskBundle.ok());
    REQUIRE(diskBundle.value() == report.bundleBytes);
}

TEST_CASE("build: bundle corrompido → verify falha com entrada culpada",
          "[build]")
{
    ProjectFixture f;
    auto r = buildProject(f.fs, f.root, BuildOptions{});
    REQUIRE(r.ok());
    auto& bytes = r.value().bundleBytes;
    REQUIRE(bytes.size() > 40);
    // corrompe um byte do payload (após header 28B do envelope externo)
    bytes[60] = static_cast<std::byte>(
        static_cast<std::uint8_t>(bytes[60]) ^ 0xFF);
    auto verified = eng::build::verifyBundle(bytes);
    REQUIRE(verified.isError());
}
