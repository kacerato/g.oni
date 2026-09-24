#include "eng/project/ProjectFile.hpp"

#include <cmath>

#include "eng/serial/Json.hpp"

namespace eng::project {

namespace {

using eng::core::Error;
using eng::core::StatusCode;
using eng::core::makeUnexpected;
using eng::serial::JsonValue;

[[nodiscard]] Error bad(const std::string& what)
{
    return Error{StatusCode::ParseError, "ProjectFile: " + what};
}

/// Valida que `value` é um path RELATIVO válido (regra dura §2.6).
[[nodiscard]] eng::core::Result<eng::fs::Path> relativePathField(
    const JsonValue& value, const char* fieldName)
{
    if (!value.isString()) {
        return makeUnexpected(
            bad(std::string(fieldName) + " ausente ou não-string"));
    }
    const eng::fs::Path path{value.asString()};
    if (path.isAbsolute()) {
        return makeUnexpected(Error{
            StatusCode::InvalidArgument,
            std::string("ProjectFile: ") + fieldName +
                " ABSOLUTO é proibido ('" + path.str() +
                "') — paths do projeto são relativos"});
    }
    if (!path.valid()) {
        return makeUnexpected(
            bad(std::string(fieldName) + " inválido (vazio/NUL)"));
    }
    return path;
}

} // namespace

eng::core::Result<ProjectConfig> ProjectFile::configFromJson(
    const JsonValue& value)
{
    if (!value.isObject()) {
        return makeUnexpected(bad("raiz não é objeto"));
    }

    const auto format = value.find("formatVersion");
    if (!format.has_value() || !format->isUnsigned() ||
        format->asU64() > kProjectFormatVersion) {
        return makeUnexpected(Error{
            StatusCode::NotSupported,
            "ProjectFile: formatVersion ausente/inválida/maior que a "
            "suportada (" +
                std::to_string(kProjectFormatVersion) + ")"});
    }

    const auto id = value.find("projectId");
    if (!id.has_value() || !id->isString()) {
        return makeUnexpected(bad("projectId ausente ou não-string"));
    }
    const auto projectId = ProjectId::fromString(id->asString());
    if (projectId.isError()) {
        return makeUnexpected(projectId.error());
    }

    const auto name = value.find("name");
    if (!name.has_value() || !name->isString() || name->asString().empty()) {
        return makeUnexpected(bad("name ausente, não-string ou vazio"));
    }

    const auto engine = value.find("engineVersion");
    if (!engine.has_value() || !engine->isString()) {
        return makeUnexpected(
            bad("engineVersion ausente ou não-string"));
    }
    const auto engineVersion =
        eng::core::Version::parse(engine->asString());
    if (engineVersion.isError()) {
        return makeUnexpected(engineVersion.error());
    }

    const auto registry = value.find("assetRegistryPath");
    if (!registry.has_value()) {
        return makeUnexpected(bad("assetRegistryPath ausente"));
    }
    const auto assetRegistryPath =
        relativePathField(*registry, "assetRegistryPath");
    if (assetRegistryPath.isError()) {
        return makeUnexpected(assetRegistryPath.error());
    }

    std::vector<eng::fs::Path> sceneRoots;
    const auto roots = value.find("sceneRoots");
    if (roots.has_value()) {
        if (!roots->isArray()) {
            return makeUnexpected(bad("sceneRoots não é array"));
        }
        for (std::size_t i = 0; i < roots->size(); ++i) {
            const auto root =
                relativePathField(roots->at(i), "sceneRoots[]");
            if (root.isError()) {
                return makeUnexpected(root.error());
            }
            sceneRoots.push_back(root.value());
        }
    }

    // Camadas de colisão nomeadas — chave ADITIVA
    // (ausente = tabela default "default" bit 1, compatível com projetos
    // pré-P4.6). Estrita quando presente: nome não-vazio único + bit
    // potência de 2 não repetido (erros precisos, nunca silêncio).
    std::vector<CollisionLayerName> collisionLayers = defaultCollisionLayers();
    const auto layersKey = value.find("collisionLayers");
    if (layersKey.has_value()) {
        if (!layersKey->isArray()) {
            return makeUnexpected(bad("collisionLayers não é array"));
        }
        collisionLayers.clear();
        for (std::size_t i = 0; i < layersKey->size(); ++i) {
            const JsonValue& entry = layersKey->at(i);
            if (!entry.isObject()) {
                return makeUnexpected(bad("collisionLayers[] não é objeto"));
            }
            const auto entryName = entry.find("name");
            const auto entryBit = entry.find("bit");
            if (!entryName.has_value() || !entryName->isString() ||
                entryName->asString().empty()) {
                return makeUnexpected(
                    bad("collisionLayers[]: name ausente/vazio"));
            }
            if (!entryBit.has_value() || !entryBit->isUnsigned()) {
                return makeUnexpected(
                    bad("collisionLayers[]: bit ausente/não-unsigned"));
            }
            const auto bit = entryBit->asU64();
            if (bit == 0u || (bit & (bit - 1u)) != 0u || bit > 0x80000000ull) {
                return makeUnexpected(bad(
                    "collisionLayers[]: bit não é potência de 2 (1..2^31)"));
            }
            for (const CollisionLayerName& existing : collisionLayers) {
                if (existing.name == entryName->asString()) {
                    return makeUnexpected(bad("collisionLayers[]: nome '" +
                                              existing.name + "' duplicado"));
                }
                if (existing.bit == static_cast<std::uint32_t>(bit)) {
                    return makeUnexpected(bad(
                        "collisionLayers[]: bit duplicado '" +
                        existing.name + "'"));
                }
            }
            collisionLayers.push_back(CollisionLayerName{
                entryName->asString(), static_cast<std::uint32_t>(bit)});
        }
        if (collisionLayers.empty()) {
            return makeUnexpected(bad("collisionLayers vazio"));
        }
    }

    // Grade do viewport — chave ADITIVA (ausente =
    // default). Estrita quando presente (valores do nosso writer).
    GridConfig grid{};
    const auto gridKey = value.find("grid");
    if (gridKey.has_value()) {
        if (!gridKey->isObject()) {
            return makeUnexpected(bad("grid não é objeto"));
        }
        const auto visible = gridKey->find("visible");
        if (visible.has_value() && visible->isBool()) {
            grid.visible = visible->asBool();
        }
        const auto cell = gridKey->find("cell");
        if (cell.has_value() && cell->isNumber()) {
            const double v = cell->asF64();
            if (!std::isfinite(v) || v <= 0.0 || v > 4096.0) {
                return makeUnexpected(
                    bad("grid.cell inválido (0 < cell <= 4096)"));
            }
            grid.cell = static_cast<float>(v);
        }
        const auto every = gridKey->find("majorEvery");
        if (every.has_value() && every->isUnsigned()) {
            const std::uint64_t v = every->asU64();
            if (v < 2 || v > 1024) {
                return makeUnexpected(
                    bad("grid.majorEvery inválido (2..1024)"));
            }
            grid.majorEvery = static_cast<int>(v);
        }
        // Cores: R/G/B independentes (cada uma com seu limite).
        const auto apply = [&](const char* key, float& out)
            -> eng::core::Result<void> {
            const auto v = gridKey->find(key);
            if (v.has_value() && v->isNumber()) {
                const double f = v->asF64();
                if (!std::isfinite(f) || f < 0.0 || f > 1.0) {
                    return makeUnexpected(bad(std::string("grid.") + key +
                                              " fora de [0,1]"));
                }
                out = static_cast<float>(f);
            }
            return {};
        };
        auto result = apply("minorR", grid.minorR);
        if (result.ok()) { result = apply("minorG", grid.minorG); }
        if (result.ok()) { result = apply("minorB", grid.minorB); }
        if (result.ok()) { result = apply("majorR", grid.majorR); }
        if (result.ok()) { result = apply("majorG", grid.majorG); }
        if (result.ok()) { result = apply("majorB", grid.majorB); }
        if (result.isError()) {
            return makeUnexpected(result.error());
        }
    }

    ProjectConfig config;
    config.projectId = projectId.value();
    config.name = name->asString();
    config.engineVersion = engineVersion.value();
    config.assetRegistryPath = assetRegistryPath.value();
    config.sceneRoots = std::move(sceneRoots);
    config.collisionLayers = std::move(collisionLayers);
    config.grid = grid;
    return config;
}

eng::core::Result<eng::serial::JsonValue> ProjectFile::toJson(
    const ProjectConfig& config)
{
    JsonValue roots = JsonValue::array();
    for (const eng::fs::Path& root : config.sceneRoots) {
        roots.append(JsonValue::string(root.str()));
    }

    JsonValue value = JsonValue::object();
    value.set("formatVersion", JsonValue::uinteger(kProjectFormatVersion));
    value.set("projectId", JsonValue::string(config.projectId.toString()));
    value.set("name", JsonValue::string(config.name));
    value.set("engineVersion",
              JsonValue::string(config.engineVersion.toString()));
    value.set("assetRegistryPath",
              JsonValue::string(config.assetRegistryPath.str()));
    value.set("sceneRoots", std::move(roots));
    // SEMPRE escreve a tabela — vazio no struct significa
    // "tabela default" e é NORMALIZADO aqui (um array vazio no arquivo
    // seria rejeitado pelo parse estrito; default é gerido num lugar só).
    std::vector<CollisionLayerName> defaultTable;
    const std::vector<CollisionLayerName>* table = &config.collisionLayers;
    if (table->empty()) {
        defaultTable = defaultCollisionLayers();
        table = &defaultTable;
    }
    JsonValue layersJson = JsonValue::array();
    for (const CollisionLayerName& layer : *table) {
        JsonValue entry = JsonValue::object();
        entry.set("name", JsonValue::string(layer.name));
        entry.set("bit", JsonValue::uinteger(layer.bit));
        layersJson.append(std::move(entry));
    }
    value.set("collisionLayers", std::move(layersJson));
    // Grade do viewport — SEMPRE escreve (aditivo).
    {
        JsonValue gridJson = JsonValue::object();
        gridJson.set("visible", JsonValue::boolean(config.grid.visible));
        gridJson.set("cell", JsonValue::real(config.grid.cell));
        gridJson.set("majorEvery",
                     JsonValue::uinteger(static_cast<std::uint64_t>(
                         config.grid.majorEvery)));
        gridJson.set("minorR", JsonValue::real(config.grid.minorR));
        gridJson.set("minorG", JsonValue::real(config.grid.minorG));
        gridJson.set("minorB", JsonValue::real(config.grid.minorB));
        gridJson.set("majorR", JsonValue::real(config.grid.majorR));
        gridJson.set("majorG", JsonValue::real(config.grid.majorG));
        gridJson.set("majorB", JsonValue::real(config.grid.majorB));
        value.set("grid", std::move(gridJson));
    }
    return value;
}

eng::core::Result<ProjectFile> ProjectFile::parse(
    const eng::fs::Path& filePath, std::string_view text)
{
    const auto parsed = eng::serial::parseJson(text);
    if (parsed.isError()) {
        return makeUnexpected(parsed.error());
    }
    const auto config = configFromJson(parsed.value());
    if (config.isError()) {
        return makeUnexpected(config.error());
    }
    ProjectFile file;
    file.config = config.value();
    file.filePath = filePath;
    return file;
}

eng::core::Result<ProjectFile> ProjectFile::readFrom(
    const eng::fs::FileSystem& fs, const eng::fs::Path& filePath)
{
    const auto text = fs.readAllText(filePath);
    if (text.isError()) {
        return makeUnexpected(text.error());
    }
    return parse(filePath, text.value());
}

eng::core::Result<std::string> ProjectFile::serialize() const
{
    const auto json = toJson(config);
    if (json.isError()) {
        return makeUnexpected(json.error());
    }
    return eng::serial::dumpJson(json.value());
}

eng::core::Result<void> ProjectFile::writeTo(eng::fs::FileSystem& fs) const
{
    const auto text = serialize();
    if (text.isError()) {
        return makeUnexpected(text.error());
    }
    return fs.writeAllText(filePath, text.value());
}

} // namespace eng::project
