/// eng::build — implementação do pipeline (FASE 12; design:
/// phase12_audit/design.md — divergência código↔design = bug do código).

#include "eng/build/Build.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <utility>

#include "eng/assets/AssetId.hpp"
#include "eng/assets/AssetMeta.hpp"
#include "eng/assets/AssetRegistry.hpp"
#include "eng/assets/AssetType.hpp"
#include "eng/niscript/NiScript.hpp"
#include "eng/project/ProjectFile.hpp"
#include "eng/serial/Envelope.hpp"
#include "eng/serial/Json.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::build {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error buildError(StatusCode code, std::string message)
{
    return Error{code, std::move(message)};
}

void pushDiag(std::vector<BuildDiag>* diags, const char* stage,
              std::string message, bool warning = false)
{
    if (diags == nullptr) {
        return;
    }
    diags->push_back(BuildDiag{stage, std::move(message), warning});
}

// --- big-endian helpers (container do bundle — consistente com GONI) ------

void putU32(std::vector<std::byte>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(v & 0xFF));
}

void putBytes(std::vector<std::byte>& out, std::span<const std::byte> data)
{
    out.insert(out.end(), data.begin(), data.end());
}

struct Cursor {
    std::span<const std::byte> data;
    std::size_t pos = 0;

    [[nodiscard]] bool canRead(std::size_t n) const noexcept
    {
        return pos + n <= data.size();
    }
    [[nodiscard]] std::uint32_t readU32() noexcept
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v = (v << 8)
                | static_cast<std::uint32_t>(
                      static_cast<std::uint8_t>(data[pos + static_cast<std::size_t>(i)]));
        }
        pos += 4;
        return v;
    }
    [[nodiscard]] std::span<const std::byte> readSpan(std::size_t n) noexcept
    {
        const auto span = data.subspan(pos, n);
        pos += n;
        return span;
    }
};

// =============================================================================
// Etapa 2 — BuildConfig (build.json; paths RELATIVOS — missão)
// =============================================================================

struct BuildConfig {
    std::vector<std::string> targets;
    std::vector<std::string> entryScenes;
    std::string cacheDir = "build-cache";
    std::string outDir = "export";
};

[[nodiscard]] bool isAbsolutePath(const std::string& text) noexcept
{
    return !text.empty() && text[0] == '/';
}

[[nodiscard]] bool isKnownTarget(std::string_view name) noexcept
{
    return name == kTargetAndroidArm64 || name == kTargetLinuxDev;
}

[[nodiscard]] Result<BuildConfig> parseBuildConfig(std::string_view text)
{
    auto parsed = eng::serial::parseJson(text);
    if (parsed.isError()) {
        return makeUnexpected(parsed.error());
    }
    const eng::serial::JsonValue& root = parsed.value();
    if (!root.isObject()) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "build.json: raiz deve ser objeto"));
    }
    const auto version = root.find("formatVersion");
    if (!version.has_value() || !version->isNumber()) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "build.json: formatVersion ausente"));
    }
    const auto v = static_cast<std::uint32_t>(version->asU64());
    if (v != kBuildConfigFormatVersion) {
        return makeUnexpected(buildError(
            StatusCode::NotSupported,
            "build.json: formatVersion " + std::to_string(v)
                + " não suportado (esperado "
                + std::to_string(kBuildConfigFormatVersion) + ")"));
    }

    BuildConfig config;

    const auto targets = root.find("targets");
    if (!targets.has_value() || !targets->isArray() || targets->size() == 0) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "build.json: 'targets' deve ser array não-vazio"));
    }
    for (std::size_t i = 0; i < targets->size(); ++i) {
        const auto t = targets->at(i);
        if (!t.isString()) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: targets[" + std::to_string(i)
                    + "] deve ser string"));
        }
        const std::string& name = t.asString();
        if (!isKnownTarget(name)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: target desconhecido '" + name
                    + "' (suportados: " + std::string(kTargetAndroidArm64)
                    + ", " + std::string(kTargetLinuxDev) + ")"));
        }
        if (std::find(config.targets.begin(), config.targets.end(), name)
            == config.targets.end()) {
            config.targets.push_back(name);
        }
    }

    const auto entries = root.find("entryScenes");
    if (!entries.has_value() || !entries->isArray()
        || entries->size() == 0) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "build.json: 'entryScenes' deve ser array não-vazio"));
    }
    for (std::size_t i = 0; i < entries->size(); ++i) {
        const auto e = entries->at(i);
        if (!e.isString()) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: entryScenes[" + std::to_string(i)
                    + "] deve ser string"));
        }
        const std::string& scene = e.asString();
        if (isAbsolutePath(scene)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: entryScenes deve ser RELATIVO (recebido '"
                    + scene + "')"));
        }
        config.entryScenes.push_back(scene);
    }

    if (const auto cache = root.find("cacheDir"); cache.has_value()) {
        if (!cache->isString()) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: cacheDir deve ser string"));
        }
        const std::string& dir = cache->asString();
        if (isAbsolutePath(dir)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: cacheDir deve ser RELATIVO (recebido '" + dir
                    + "')"));
        }
        config.cacheDir = dir;
    }
    if (const auto out = root.find("outDir"); out.has_value()) {
        if (!out->isString()) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: outDir deve ser string"));
        }
        const std::string& dir = out->asString();
        if (isAbsolutePath(dir)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "build.json: outDir deve ser RELATIVO (recebido '" + dir
                    + "')"));
        }
        config.outDir = dir;
    }
    return config;
}

// =============================================================================
// Etapa 4/5 — scan de referências (agnóstico: texto JSON + ids do registro)
// =============================================================================

/// Extrai TODOS os AssetIds canônicos REGISTRADOS que aparecem no texto
/// da cena (referência por identidade estável — ADR-028; agnóstico de
/// campo: qualquer componente que porte um AssetId aparece no texto).
[[nodiscard]] std::vector<eng::assets::AssetId> scanReferencedIds(
    const std::string& sceneText,
    const std::vector<eng::assets::AssetMeta>& registry)
{
    std::vector<eng::assets::AssetId> found;
    for (const eng::assets::AssetMeta& meta : registry) {
        if (sceneText.find(meta.id.toString()) != std::string::npos) {
            found.push_back(meta.id);
        }
    }
    return found;
}

/// Extrai os FONTES dos scripts NI embutidos nas cenas (componente do
/// catálogo). Percorre o JSON da cena buscando componentes do tipo
/// conhecido — agnóstico do restante do formato.
[[nodiscard]] std::vector<std::string> extractEmbeddedScripts(
    const eng::serial::JsonValue& scene)
{
    std::vector<std::string> sources;
    std::function<void(const eng::serial::JsonValue&)> walk =
        [&](const eng::serial::JsonValue& node) {
            if (node.isObject()) {
                if (const auto type = node.find("type");
                    type.has_value() && type->isString()
                    && type->asString() == "eng::editor::NiScriptComponent") {
                    if (const auto data = node.find("data");
                        data.has_value() && data->isObject()) {
                        if (const auto src = data->find("source");
                            src.has_value() && src->isString()) {
                            sources.push_back(src->asString());
                        }
                    }
                }
                // varredura pelas chaves do formato de cena:
                // components é array de {type,data}
                if (const auto comps = node.find("components");
                    comps.has_value() && comps->isArray()) {
                    for (std::size_t i = 0; i < comps->size(); ++i) {
                        walk(comps->at(i));
                    }
                }
                if (const auto entities = node.find("entities");
                    entities.has_value() && entities->isArray()) {
                    for (std::size_t i = 0; i < entities->size(); ++i) {
                        walk(entities->at(i));
                    }
                }
            } else if (node.isArray()) {
                for (std::size_t i = 0; i < node.size(); ++i) {
                    walk(node.at(i));
                }
            }
        };
    walk(scene);
    return sources;
}

// =============================================================================
// Cache — conteúdo-endereçado
// =============================================================================

[[nodiscard]] std::string cacheKeyHex(std::uint32_t cookerVersion,
                                      std::uint32_t assetType,
                                      std::span<const std::byte> content)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            hash ^= static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
            hash *= 1099511628211ull;
        }
    };
    mix(cookerVersion);
    mix(assetType);
    for (const std::byte b : content) {
        hash ^= static_cast<unsigned char>(b);
        hash *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(hex, 16);
}

} // namespace

// =============================================================================
// contentHashHex — FNV-1a 64 do conteúdo (manifest + verificação)
// =============================================================================

std::string contentHashHex(std::span<const std::byte> data)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::byte b : data) {
        hash ^= static_cast<unsigned char>(b);
        hash *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(hex, 16);
}

// =============================================================================
// verifyBundle — §10 (decode + CRC + manifest ↔ entradas + contentHash)
// =============================================================================

Result<BundleInfo> verifyBundle(std::span<const std::byte> bundleBytes)
{
    auto outer = eng::serial::decodeEnvelope(bundleBytes);
    if (outer.isError()) {
        return makeUnexpected(outer.error());
    }
    if (outer.value().assetType != kBundleEnvelopeType) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "bundle: assetType externo " + std::to_string(outer.value().assetType)
                + " não é bundle (esperado "
                + std::to_string(kBundleEnvelopeType) + ")"));
    }
    if (outer.value().payloadVersion != kBundlePayloadVersion) {
        return makeUnexpected(buildError(
            StatusCode::NotSupported,
            "bundle: payloadVersion " + std::to_string(outer.value().payloadVersion)
                + " não suportado"));
    }

    Cursor cursor{outer.value().payload, 0};
    if (!cursor.canRead(4)) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument, "bundle: truncado (manifestSize)"));
    }
    const std::uint32_t manifestSize = cursor.readU32();
    if (!cursor.canRead(manifestSize)) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument, "bundle: truncado (manifest)"));
    }
    const auto manifestSpan = cursor.readSpan(manifestSize);
    const std::string manifestJson(
        reinterpret_cast<const char*>(manifestSpan.data()), manifestSize);

    if (!cursor.canRead(4)) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument, "bundle: truncado (entryCount)"));
    }
    const std::uint32_t entryCount = cursor.readU32();

    // Manifest parse (para ids/paths na verificação cruzada)
    auto manifestParsed = eng::serial::parseJson(manifestJson);
    if (manifestParsed.isError()) {
        return makeUnexpected(manifestParsed.error());
    }
    const eng::serial::JsonValue& manifest = manifestParsed.value();
    const auto assetsArray = manifest.find("assets");
    const std::size_t manifestCount =
        assetsArray.has_value() && assetsArray->isArray()
            ? assetsArray->size()
            : 0;
    if (manifestCount != entryCount) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "bundle: manifest declara " + std::to_string(manifestCount)
                + " assets mas o bundle carrega " + std::to_string(entryCount)));
    }

    BundleInfo info;
    info.manifestJson = manifestJson;
    for (std::uint32_t i = 0; i < entryCount; ++i) {
        if (!cursor.canRead(4)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: truncado (envelopeSize da entrada "
                    + std::to_string(i) + ")"));
        }
        const std::uint32_t envelopeSize = cursor.readU32();
        if (!cursor.canRead(envelopeSize)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: truncado (envelope da entrada " + std::to_string(i)
                    + ")"));
        }
        const auto envelopeSpan = cursor.readSpan(envelopeSize);
        auto entry = eng::serial::decodeEnvelope(envelopeSpan);
        if (entry.isError()) {
            return makeUnexpected(entry.error());
        }
        if (!cursor.canRead(4 + 4 + 4 + 4)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: truncado (meta da entrada " + std::to_string(i)
                    + ")"));
        }
        const std::uint32_t idLen = cursor.readU32();
        const std::uint32_t pathLen = cursor.readU32();
        const std::uint32_t hashLen = cursor.readU32();
        const std::uint32_t flags = cursor.readU32();
        if (!cursor.canRead(idLen + pathLen + hashLen)) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: truncado (campos da entrada " + std::to_string(i)
                    + ")"));
        }
        const auto idSpan = cursor.readSpan(idLen);
        const auto pathSpan = cursor.readSpan(pathLen);
        const auto hashSpan = cursor.readSpan(hashLen);

        BundleEntry out;
        out.assetId = std::string(
            reinterpret_cast<const char*>(idSpan.data()), idLen);
        out.path = std::string(
            reinterpret_cast<const char*>(pathSpan.data()), pathLen);
        out.contentHash = std::string(
            reinterpret_cast<const char*>(hashSpan.data()), hashLen);
        out.assetType = entry.value().assetType;
        out.derived = (flags & 1u) != 0u;
        out.payload = entry.value().payload;

        // cruzamento com o manifest (mesma posição — mesma ordenação)
        const auto& m = assetsArray->at(i);
        const auto mId = m.find("id");
        const auto mHash = m.find("contentHash");
        if (mId.has_value() && mId->isString()
            && mId->asString() != out.assetId) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: entrada " + std::to_string(i) + " id '"
                    + out.assetId + "' != manifest '" + mId->asString()
                    + "'"));
        }
        if (mHash.has_value() && mHash->isString()
            && mHash->asString() != out.contentHash) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: contentHash da entrada " + std::to_string(i)
                    + " não confere com o manifest"));
        }
        // hash do CONTEÚDO confere com o declarado (FNV-1a 64)
        if (contentHashHex(out.payload) != out.contentHash) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "bundle: conteúdo da entrada " + std::to_string(i)
                    + " não bate com o contentHash declarado"));
        }
        info.entries.push_back(std::move(out));
    }
    if (cursor.pos != cursor.data.size()) {
        return makeUnexpected(buildError(
            StatusCode::InvalidArgument,
            "bundle: " + std::to_string(cursor.data.size() - cursor.pos)
                + " bytes residuais após as entradas"));
    }
    return info;
}

// =============================================================================
// buildProject — pipeline completo
// =============================================================================

Result<BuildReport> buildProject(eng::fs::FileSystem& fs,
                                 const eng::fs::Path& projectRoot,
                                 const BuildOptions& options,
                                 std::vector<BuildDiag>* diags)
{
    // --- 1. loadProject -------------------------------------------------------
    const eng::fs::Path projectFile = projectRoot / eng::fs::Path{"project.goni.json"};
    auto project = eng::project::ProjectFile::readFrom(fs, projectFile);
    if (project.isError()) {
        return makeUnexpected(buildError(
            project.error().code, "loadProject: " + project.error().message));
    }
    const eng::project::ProjectPaths paths = project.value().paths();

    const eng::fs::Path buildJsonPath =
        projectRoot / eng::fs::Path{"build.json"};
    auto buildJsonExists = fs.exists(buildJsonPath);
    if (buildJsonExists.isError() || !buildJsonExists.value()) {
        return makeUnexpected(buildError(
            StatusCode::NotFound,
            "loadBuildConfig: build.json ausente na raiz do projeto"));
    }
    auto buildJsonText = fs.readAllText(buildJsonPath);
    if (buildJsonText.isError()) {
        return makeUnexpected(buildError(
            buildJsonText.error().code,
            "loadBuildConfig: " + buildJsonText.error().message));
    }

    // --- 2. loadBuildConfig (paths RELATIVOS) ----------------------------------
    auto configResult = parseBuildConfig(buildJsonText.value());
    if (configResult.isError()) {
        return makeUnexpected(std::move(configResult).error());
    }
    const BuildConfig& config = configResult.value();

    // --- asset registry ---------------------------------------------------------
    const eng::fs::Path registryPath =
        paths.resolve(project.value().config.assetRegistryPath);
    std::vector<eng::assets::AssetMeta> registryAssets;
    if (auto exists = fs.exists(registryPath);
        !exists.isError() && exists.value()) {
        auto registryText = fs.readAllText(registryPath);
        if (registryText.isError()) {
            return makeUnexpected(buildError(
                registryText.error().code,
                "loadProject: " + registryText.error().message));
        }
        auto registry = eng::assets::AssetRegistry::deserialize(
            registryText.value());
        if (registry.isError()) {
            return makeUnexpected(buildError(
                registry.error().code,
                "loadProject: asset_registry.json: "
                    + registry.error().message));
        }
        registryAssets = registry.value().all(); // ordenado por AssetId
    }

    // --- 3/4/5. manifest + grafo + scan de referências -------------------------
    // validação duplicada (2 ids, mesmo sourcePath) — BLOCK (design §4)
    for (std::size_t i = 1; i < registryAssets.size(); ++i) {
        if (registryAssets[i].sourcePath.str()
            == registryAssets[i - 1].sourcePath.str()) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "manifest: assets duplicados para '"
                    + registryAssets[i].sourcePath.str() + "' ("
                    + registryAssets[i - 1].id.toString() + " e "
                    + registryAssets[i].id.toString() + ")"));
        }
    }

    // lê cenas de entrada + valida existência; scan de refs
    std::vector<std::string> sceneTexts;
    std::vector<std::string> referencedIds;
    for (const std::string& entry : config.entryScenes) {
        const eng::fs::Path scenePath =
            projectRoot / eng::fs::Path{entry};
        auto sceneText = fs.readAllText(scenePath);
        if (sceneText.isError()) {
            return makeUnexpected(buildError(
                sceneText.error().code,
                "scan: cena de entrada '" + entry
                    + "' não pôde ser lida: " + sceneText.error().message));
        }
        // JSON válido? (bloqueante — cena corrompida não empacota)
        auto sceneJson = eng::serial::parseJson(sceneText.value());
        if (sceneJson.isError()) {
            return makeUnexpected(buildError(
                StatusCode::InvalidArgument,
                "scan: cena de entrada '" + entry + "' não é JSON válido"));
        }
        for (const eng::assets::AssetId& id :
             scanReferencedIds(sceneText.value(), registryAssets)) {
            if (std::find(referencedIds.begin(), referencedIds.end(),
                         id.toString())
                == referencedIds.end()) {
                referencedIds.push_back(id.toString());
            }
        }
        sceneTexts.push_back(std::move(sceneText).value());
    }

    // scripts embutidos nas cenas + standalone — validar compilando
    std::size_t scriptsValidated = 0;
    {
        eng::ni::NiNativeTable natives;
        natives.addBaseLibrary();
        natives.addStandardHost();
        const eng::ni::CompileOptions compileOptions{&natives};
        std::vector<std::string> scripts;
        for (std::size_t i = 0; i < config.entryScenes.size(); ++i) {
            auto sceneJson = eng::serial::parseJson(sceneTexts[i]);
            // (parse já validado acima; aqui só extração)
            for (std::string& src :
                 extractEmbeddedScripts(sceneJson.value())) {
                scripts.push_back(std::move(src));
            }
        }
        // scripts standalone (assets/scripts/*.nis do registro)
        for (const eng::assets::AssetMeta& meta : registryAssets) {
            const std::string& p = meta.sourcePath.str();
            if (meta.type == eng::assets::AssetType::Script
                || (p.size() > 4
                    && p.compare(p.size() - 4, 4, ".nis") == 0)) {
                const eng::fs::Path scriptPath =
                    projectRoot / meta.sourcePath;
                auto text = fs.readAllText(scriptPath);
                if (text.isError()) {
                    return makeUnexpected(buildError(
                        text.error().code,
                        "scan: script '" + p
                            + "' não pôde ser lido"));
                }
                scripts.push_back(std::move(text).value());
            }
        }
        for (const std::string& source : scripts) {
            auto program =
                eng::ni::compile(source, compileOptions, nullptr);
            if (program.isError()) {
                return makeUnexpected(buildError(
                    StatusCode::InvalidArgument,
                    "scan: script NI-Script não compila: "
                        + program.error().message));
            }
            ++scriptsValidated;
        }
    }

    // unused (WARN — não bloqueante; design §4)
    std::vector<std::string> unused;
    for (const eng::assets::AssetMeta& meta : registryAssets) {
        // cenas de entrada e referenciadas são "usadas"; scripts
        // standalone validam mas só contam como usados se referenciados
        const bool isEntry =
            std::find(config.entryScenes.begin(), config.entryScenes.end(),
                      meta.sourcePath.str())
            != config.entryScenes.end();
        const bool isReferenced =
            std::find(referencedIds.begin(), referencedIds.end(),
                      meta.id.toString())
            != referencedIds.end();
        if (!isEntry && !isReferenced) {
            unused.push_back(meta.id.toString());
            pushDiag(diags, "scan",
                     "asset não referenciado por nenhuma cena: "
                         + meta.id.toString() + " (" + meta.sourcePath.str()
                         + ")",
                     true);
        }
    }

    // fonte AUSENTE (registrado mas arquivo sumiu) — BLOCK (design §4)
    for (const eng::assets::AssetMeta& meta : registryAssets) {
        const eng::fs::Path src = projectRoot / meta.sourcePath;
        auto exists = fs.exists(src);
        if (exists.isError() || !exists.value()) {
            return makeUnexpected(buildError(
                StatusCode::NotFound,
                "validation: asset registrado ausente no disco: "
                    + meta.id.toString() + " -> " + meta.sourcePath.str()));
        }
    }

    // --- manifest determinístico ------------------------------------------------
    const std::uint32_t cookerVersion =
        options.cookerVersionOverride != 0 ? options.cookerVersionOverride
                                           : kCookerVersion;
    {
        eng::serial::JsonValue manifest = eng::serial::JsonValue::object();
        manifest.set("formatVersion",
                     eng::serial::JsonValue::uinteger(kManifestFormatVersion));
        manifest.set("projectName",
                     eng::serial::JsonValue::string(
                         project.value().config.name));
        manifest.set("projectId",
                     eng::serial::JsonValue::string(
                         project.value().config.projectId.toString()));
        manifest.set("cookerVersion",
                     eng::serial::JsonValue::uinteger(cookerVersion));
        eng::serial::JsonValue entries = eng::serial::JsonValue::array();
        for (const eng::assets::AssetMeta& meta : registryAssets) {
            eng::serial::JsonValue e = eng::serial::JsonValue::object();
            e.set("id", eng::serial::JsonValue::string(meta.id.toString()));
            e.set("type",
                  eng::serial::JsonValue::string(
                      eng::assets::assetTypeName(meta.type)));
            e.set("path",
                  eng::serial::JsonValue::string(meta.sourcePath.str()));
            // hash preenchido após a leitura (abaixo, junto do cook)
            entries.append(std::move(e));
        }
        manifest.set("assets", std::move(entries));
        // serializado no fim (com hashes)
    }

    // --- 7/8. cook + cache ---------------------------------------------------------
    const eng::fs::Path cacheDir = projectRoot / eng::fs::Path{config.cacheDir};
    if (!options.forceCook) {
        (void)fs.mkdirs(cacheDir); // artefato — criação lazy é segura
    }
    std::size_t cacheHits = 0;
    std::size_t cacheMisses = 0;

    struct CookedEntry {
        eng::assets::AssetMeta meta;
        std::string contentHash;
        std::vector<std::byte> envelope; // envelope GONI do asset
    };
    std::vector<CookedEntry> cooked;
    cooked.reserve(registryAssets.size());

    for (const eng::assets::AssetMeta& meta : registryAssets) {
        const eng::fs::Path src = projectRoot / meta.sourcePath;
        auto bytes = fs.readAllBytes(src);
        if (bytes.isError()) {
            return makeUnexpected(buildError(
                bytes.error().code,
                "cook: leitura de " + meta.sourcePath.str() + " falhou: "
                    + bytes.error().message));
        }
        const std::string hash = contentHashHex(bytes.value());
        const std::uint32_t envelopeType =
            static_cast<std::uint32_t>(meta.type);
        const std::string key =
            cacheKeyHex(cookerVersion, envelopeType, bytes.value());
        const eng::fs::Path cacheFile = cacheDir / eng::fs::Path{key};

        std::vector<std::byte> envelope;
        bool fromCache = false;
        if (!options.forceCook) {
            auto cached = fs.readAllBytes(cacheFile);
            if (!cached.isError()) {
                envelope = std::move(cached).value();
                fromCache = true;
                ++cacheHits;
            }
        }
        if (!fromCache) {
            ++cacheMisses;
            envelope = eng::serial::encodeEnvelope(
                envelopeType, 1, bytes.value()); // SOURCE/verbatim
            if (!options.forceCook) {
                (void)fs.mkdirs(cacheDir);
                (void)fs.writeAllBytes(cacheFile, envelope);
            }
        }
        cooked.push_back(CookedEntry{meta, hash, std::move(envelope)});
    }

    // --- 9. bundle -----------------------------------------------------------------
    // manifest FINAL (com hashes) — determinístico (registry é ordenado)
    eng::serial::JsonValue manifest = eng::serial::JsonValue::object();
    manifest.set("formatVersion",
                 eng::serial::JsonValue::uinteger(kManifestFormatVersion));
    manifest.set("projectName",
                 eng::serial::JsonValue::string(project.value().config.name));
    manifest.set(
        "projectId",
        eng::serial::JsonValue::string(
            project.value().config.projectId.toString()));
    manifest.set("cookerVersion",
                 eng::serial::JsonValue::uinteger(cookerVersion));
    manifest.set("scriptsValidated",
                 eng::serial::JsonValue::uinteger(scriptsValidated));
    eng::serial::JsonValue entriesJson = eng::serial::JsonValue::array();
    for (const CookedEntry& e : cooked) {
        eng::serial::JsonValue j = eng::serial::JsonValue::object();
        j.set("id", eng::serial::JsonValue::string(e.meta.id.toString()));
        j.set("type",
              eng::serial::JsonValue::string(
                  eng::assets::assetTypeName(e.meta.type)));
        j.set("path",
              eng::serial::JsonValue::string(e.meta.sourcePath.str()));
        j.set("contentHash",
              eng::serial::JsonValue::string(e.contentHash));
        entriesJson.append(std::move(j));
    }
    manifest.set("assets", std::move(entriesJson));

    const std::string manifestText = eng::serial::dumpJson(manifest);

    std::vector<std::byte> payload;
    putU32(payload, static_cast<std::uint32_t>(manifestText.size()));
    putBytes(payload,
             std::as_bytes(std::span<const char>(manifestText.data(),
                                                 manifestText.size())));
    putU32(payload, static_cast<std::uint32_t>(cooked.size()));
    for (const CookedEntry& e : cooked) {
        putU32(payload, static_cast<std::uint32_t>(e.envelope.size()));
        putBytes(payload, e.envelope);
        const std::string& id = e.meta.id.toString();
        const std::string& path = e.meta.sourcePath.str();
        putU32(payload, static_cast<std::uint32_t>(id.size()));
        putU32(payload, static_cast<std::uint32_t>(path.size()));
        putU32(payload,
               static_cast<std::uint32_t>(e.contentHash.size()));
        putU32(payload, 0u); // flags: bit0=DERIVED (v1: tudo SOURCE)
        putBytes(payload, std::as_bytes(std::span<const char>(
                             id.data(), id.size())));
        putBytes(payload, std::as_bytes(std::span<const char>(
                             path.data(), path.size())));
        putBytes(payload, std::as_bytes(std::span<const char>(
                             e.contentHash.data(), e.contentHash.size())));
    }
    std::vector<std::byte> bundle = eng::serial::encodeEnvelope(
        kBundleEnvelopeType, kBundlePayloadVersion, payload);

    // --- 10. verify (decode completo ANTES de exportar) ----------------------------
    auto verified = verifyBundle(bundle);
    if (verified.isError()) {
        return makeUnexpected(buildError(
            verified.error().code,
            "verify: " + verified.error().message));
    }

    // --- 11. export por alvo ----------------------------------------------------------
    const eng::fs::Path outRoot =
        projectRoot / eng::fs::Path{config.outDir};
    for (const std::string& target : config.targets) {
        const eng::fs::Path targetDir = outRoot / eng::fs::Path{target};
        if (auto made = fs.mkdirs(targetDir); made.isError()) {
            return makeUnexpected(buildError(
                made.error().code,
                "export: " + target + ": " + made.error().message));
        }
        const std::string fileName =
            project.value().config.name + ".goni";
        const eng::fs::Path bundlePath = targetDir / eng::fs::Path{fileName};
        if (auto written = fs.writeAllBytes(bundlePath, bundle);
            written.isError()) {
            return makeUnexpected(buildError(
                written.error().code,
                "export: " + target + ": " + written.error().message));
        }
        if (target == kTargetAndroidArm64) {
            (void)fs.writeAllText(
                targetDir / eng::fs::Path{"INSTALL.md"},
                std::string("# Instalação (Android arm64-v8a)\n\n")
                    + "O APK existente embute engine+editor (FASES 7/8). "
                      "Este pacote é DADO do projeto:\n\n"
                      "1. Copie `"
                    + fileName
                    + "` para `filesDir/projects/"
                    + project.value().config.projectId.toString()
                    + "/bundle.goni` do app com.goni.runtime.\n"
                      "2. O bundle é autossuficiente e verificado "
                      "(envelope GONI por asset + manifest com "
                      "contentHash FNV-1a 64).\n\n"
                      "NOTA HONESTA: o loader de projetos na Activity do "
                      "runtime é FUTURO declarado (FASE 12 não inclui UI "
                      "de browsing de projetos no APK).\n");
        } else if (target == kTargetLinuxDev) {
            (void)fs.writeAllText(
                targetDir / eng::fs::Path{"README.md"},
                std::string("# Bundle de desenvolvimento (Linux)\n\n")
                    + "`" + fileName
                    + "` é um envelope GONI (assetType="
                    + std::to_string(kBundleEnvelopeType)
                    + ") contendo o manifest JSON + um envelope por asset "
                      "(SOURCE, payload verbatim). Valide com "
                      "`eng::build::verifyBundle`.\n");
        }
    }

    BuildReport report;
    report.manifestJson = manifestText;
    report.projectId = project.value().config.projectId.toString();
    report.projectName = project.value().config.name;
    report.assetsCooked = cooked.size();
    report.cacheHits = cacheHits;
    report.cacheMisses = cacheMisses;
    report.scriptsValidated = scriptsValidated;
    report.unusedAssets = std::move(unused);
    report.targets = config.targets;
    report.bundleBytes = std::move(bundle);
    return report;
}

} // namespace eng::build
