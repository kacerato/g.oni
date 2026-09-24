#include "eng/editor/AssetBrowser.hpp"

/// AssetBrowser — composição registry × disco.

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "eng/assets/AssetId.hpp"
#include "eng/assets/AssetType.hpp"

namespace eng::editor {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error browserError(StatusCode code, std::string message)
{
    return Error{code, "AssetBrowser: " + std::move(message)};
}

/// Nome estável de arquivo (stem quando vazio não serve — usa o nome dado).
[[nodiscard]] std::string fileDisplayName(const eng::fs::Path& path)
{
    const std::string filename = path.filename().str();
    return filename;
}

}  // namespace

// =============================================================================
// Categorias
// =============================================================================

const std::vector<std::string>& AssetBrowser::categories()
{
    static const std::vector<std::string> kCategories = {
        "scenes", "prefabs", "json", "textures", "models",
        "materials", "shaders", "audio", "scripts", "animations",
    };
    return kCategories;
}

const char* AssetBrowser::assetTypeFor(std::string_view category)
{
    static const std::unordered_map<std::string, eng::assets::AssetType> kMap = {
        {"scenes", eng::assets::AssetType::Scene},
        {"prefabs", eng::assets::AssetType::Prefab},
        {"json", eng::assets::AssetType::Json},
        {"textures", eng::assets::AssetType::Texture},
        {"models", eng::assets::AssetType::Mesh},
        {"materials", eng::assets::AssetType::Material},
        {"shaders", eng::assets::AssetType::Shader},
        {"audio", eng::assets::AssetType::Audio},
        {"scripts", eng::assets::AssetType::Script},
        {"animations", eng::assets::AssetType::Animation},
    };
    const auto it = kMap.find(std::string(category));
    if (it == kMap.end()) {
        return nullptr;
    }
    // Nome estável do AssetType — a serialização do registry usa exatamente
    // este nome (assetTypeName), então devolver o mesmo literal aqui é o
    // contrato do meta persistido.
    switch (it->second) {
    case eng::assets::AssetType::Scene: return "Scene";
    case eng::assets::AssetType::Prefab: return "Prefab";
    case eng::assets::AssetType::Json: return "Json";
    case eng::assets::AssetType::Texture: return "Texture";
    case eng::assets::AssetType::Mesh: return "Mesh";
    case eng::assets::AssetType::Material: return "Material";
    case eng::assets::AssetType::Shader: return "Shader";
    case eng::assets::AssetType::Audio: return "Audio";
    case eng::assets::AssetType::Script: return "Script";
    case eng::assets::AssetType::Animation: return "Animation";
    default: return nullptr;
    }
}

AssetBrowser::AssetBrowser(eng::fs::FileSystem& fs,
                           const eng::fs::Path& assetsRoot,
                           const eng::fs::Path& registryPath)
    : fs_(&fs), assetsRoot_(assetsRoot), registryPath_(registryPath)
{
}

// =============================================================================
// Listagem
// =============================================================================

Result<std::vector<AssetBrowser::Entry>> AssetBrowser::list(
    std::string_view category) const
{
    const char* typeName = assetTypeFor(category);
    if (typeName == nullptr) {
        return makeUnexpected(browserError(StatusCode::InvalidArgument,
                                           "categoria desconhecida: '" +
                                               std::string(category) + "'"));
    }

    std::vector<Entry> entries;
    std::unordered_set<std::string> onDisk;

    // 1) Diretório da categoria (bytes existem de fato).
    const eng::fs::Path dir = categoryDir(category);
    auto exists = fs_->exists(dir);
    if (exists.isError()) {
        return makeUnexpected(exists.error());
    }
    if (exists.value()) {
        auto listed = fs_->list(dir, false);
        if (listed.isError()) {
            return makeUnexpected(listed.error());
        }
        for (const auto& entry : listed.value()) {
            if (entry.isDirectory) {
                continue;
            }
            onDisk.insert(entry.path.str());
            entries.push_back(Entry{
                fileDisplayName(entry.path),
                entry.path.str(),
                "-",
                false,
            });
        }
    }

    // 2) Registry do tipo (referências por id — inclusive para arquivos que
    //    ainda não estão na pasta da categoria, ex.: movidos à mão).
    const auto typeResult = eng::assets::assetTypeFromName(typeName);
    if (typeResult.isError()) {
        return makeUnexpected(typeResult.error()); // inalcançável: nome do mapa
    }
    const eng::assets::AssetType type = typeResult.value();
    for (const auto& meta : registry_.all()) {
        if (meta.type != type) {
            continue;
        }
        const std::string pathStr = meta.sourcePath.str();
        if (onDisk.contains(pathStr)) {
            // Casa com arquivo em disco — marca como registrado.
            for (auto& entry : entries) {
                if (entry.sourcePath == pathStr) {
                    entry.id = meta.id.toString();
                    entry.registered = true;
                    break;
                }
            }
            continue;
        }
        entries.push_back(Entry{
            fileDisplayName(meta.sourcePath),
            pathStr,
            meta.id.toString(),
            true,
        });
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.name < b.name;
              });
    return entries;
}

// =============================================================================
// Operações
// =============================================================================

Result<std::string> AssetBrowser::import(std::string_view tempRelPath,
                                          std::string_view category,
                                          std::string_view name,
                                          std::string* outFinalName)
{
    const char* typeName = assetTypeFor(category);
    if (typeName == nullptr) {
        return makeUnexpected(browserError(StatusCode::InvalidArgument,
                                           "categoria desconhecida"));
    }
    const eng::fs::Path from{tempRelPath};
    if (from.isAbsolute() || from.str().find("..") != std::string::npos) {
        return makeUnexpected(browserError(
            StatusCode::InvalidArgument,
            "path temporário deve ser RELATIVO ao projeto (§8.1, sem ..)"));
    }

    // Nome final: preserva a extensão do original quando existe.
    // RECOVERY P0 (duas passadas): o guard `size() >= ext.size() + 1`
    // negava a extensão a nomes com comprimento ≤ da extensão ("hero" vs
    // ".png", 4=4); a primeira correção (`>= ext.size()`) ainda NEGAVA
    // nomes MAIS CURTOS que a extensão ("art", 3 < 4 — descoberto pelo
    // vertical slice P1: o arquivo salvava como "art" e o upload da
    // textura morria em "No such file"). O guard do compare é de LIMITE
    // (só existe para compare in-bounds); o append é para TODO nome que
    // não TERMINA com a extensão.
    std::string finalName(name);
    const std::string ext = from.extension().str();
    const bool alreadyEndsWithExt =
        finalName.size() >= ext.size() &&
        finalName.compare(finalName.size() - ext.size(), ext.size(), ext) == 0;
    if (!ext.empty() && !alreadyEndsWithExt) {
        finalName += ext;
    }
    if (outFinalName != nullptr) {
        *outFinalName = finalName;
    }
    const eng::fs::Path dir = categoryDir(category);
    const eng::fs::Path to = dir / eng::fs::Path{finalName};
    if (to.isAbsolute()) {
        return makeUnexpected(browserError(
            StatusCode::InvalidArgument, "destino absoluto é proibido"));
    }

    auto made = fs_->mkdirs(dir);
    if (made.isError()) {
        return makeUnexpected(made.error());
    }
    auto moved = fs_->rename(from, to);
    if (moved.isError()) {
        return makeUnexpected(moved.error());
    }

    eng::assets::AssetMeta meta;
    meta.id = eng::assets::AssetId::generate();
    meta.type = eng::assets::assetTypeFromName(typeName).value();
    meta.sourcePath = to;
    if (auto bytes = fs_->readAllBytes(to); !bytes.isError()) {
        meta.size = bytes.value().size();
    }
    auto upserted = registry_.upsert(std::move(meta));
    if (upserted.isError()) {
        return makeUnexpected(upserted.error());
    }
    auto saved = persist();
    if (saved.isError()) {
        return makeUnexpected(saved.error());
    }
    // id do meta inserido (recuperado por path).
    for (const auto& m : registry_.all()) {
        if (m.sourcePath == to) {
            return m.id.toString();
        }
    }
    return makeUnexpected(browserError(StatusCode::Internal,
                                       "meta recém-inserido não encontrado"));
}

Result<void> AssetBrowser::rename(std::string_view category,
                                  std::string_view name,
                                  std::string_view newName)
{
    const eng::fs::Path from = categoryDir(category) / eng::fs::Path{std::string(name)};
    const eng::fs::Path to = categoryDir(category) / eng::fs::Path{std::string(newName)};
    if (from.isAbsolute() || to.isAbsolute()) {
        return makeUnexpected(browserError(StatusCode::InvalidArgument,
                                           "paths devem ser relativos"));
    }
    auto moved = fs_->rename(from, to);
    if (moved.isError()) {
        return makeUnexpected(moved.error());
    }
    // Referências por AssetId sobrevivem — atualiza sourcePath.
    for (const auto& meta : registry_.all()) {
        if (meta.sourcePath == from) {
            auto updated = meta;
            updated.sourcePath = to;
            auto upserted = registry_.upsert(std::move(updated));
            if (upserted.isError()) {
                return makeUnexpected(upserted.error());
            }
            break;
        }
    }
    return persist();
}

Result<void> AssetBrowser::remove(std::string_view category,
                                  std::string_view name)
{
    const eng::fs::Path path = categoryDir(category) / eng::fs::Path{std::string(name)};
    auto erased = fs_->remove(path);
    if (erased.isError()) {
        return makeUnexpected(erased.error());
    }
    for (const auto& meta : registry_.all()) {
        if (meta.sourcePath == path) {
            auto removed = registry_.remove(meta.id);
            if (removed.isError()) {
                return makeUnexpected(removed.error());
            }
            break;
        }
    }
    return persist();
}

Result<std::vector<std::byte>> AssetBrowser::read(std::string_view category,
                                                 std::string_view name) const
{
    if (assetTypeFor(category) == nullptr) {
        return makeUnexpected(browserError(StatusCode::InvalidArgument,
                                           "categoria desconhecida"));
    }
    // categoryDir é relativo ao assetsRoot do projeto; Path::operator/
    // valida contra traversal — "../" não escapa.
    const eng::fs::Path path = categoryDir(category) / eng::fs::Path{std::string{name}};
    return fs_->readAllBytes(path);
}

Result<void> AssetBrowser::registerExisting(std::string_view category,
                                             std::string_view name)
{
    const char* typeName = assetTypeFor(category);
    if (typeName == nullptr) {
        return makeUnexpected(browserError(StatusCode::InvalidArgument,
                                           "categoria desconhecida"));
    }
    const eng::fs::Path path = categoryDir(category) / eng::fs::Path{std::string{name}};
    auto exists = fs_->exists(path);
    if (exists.isError()) {
        return makeUnexpected(exists.error());
    }
    if (!exists.value()) {
        return makeUnexpected(browserError(
            StatusCode::NotFound,
            "arquivo não existe em '" + path.str() + "'"));
    }
    // Idempotente por path: já registrado → nada a fazer (tamanho é
    // atualizado só quando o arquivo muda de fato — aqui o chamador
    // acabou de escrever, então refresh do meta sempre).
    eng::assets::AssetMeta meta;
    meta.type = eng::assets::assetTypeFromName(typeName).value();
    meta.sourcePath = path;
    if (auto bytes = fs_->readAllBytes(path); !bytes.isError()) {
        meta.size = bytes.value().size();
    }
    // Preserva o id quando o path já estava catalogado (referências por
    // AssetId sobrevivem — ADR-029); senão gera um novo.
    for (const auto& existing : registry_.all()) {
        if (existing.sourcePath == path) {
            meta.id = existing.id;
            break;
        }
    }
    if (meta.id.isNil()) {
        meta.id = eng::assets::AssetId::generate();
    }
    auto upserted = registry_.upsert(std::move(meta));
    if (upserted.isError()) {
        return makeUnexpected(upserted.error());
    }
    return persist();
}

Result<void> AssetBrowser::move(std::string_view fromCategory,
                                std::string_view name,
                                std::string_view toCategory)
{
    if (assetTypeFor(fromCategory) == nullptr ||
        assetTypeFor(toCategory) == nullptr) {
        return makeUnexpected(browserError(StatusCode::InvalidArgument,
                                           "categoria desconhecida"));
    }
    const eng::fs::Path from = categoryDir(fromCategory) / eng::fs::Path{std::string(name)};
    const eng::fs::Path to = categoryDir(toCategory) / eng::fs::Path{std::string(name)};
    auto made = fs_->mkdirs(to.parent());
    if (made.isError()) {
        return makeUnexpected(made.error());
    }
    auto moved = fs_->rename(from, to);
    if (moved.isError()) {
        return makeUnexpected(moved.error());
    }
    for (const auto& meta : registry_.all()) {
        if (meta.sourcePath == from) {
            auto updated = meta;
            updated.sourcePath = to;
            updated.type = eng::assets::assetTypeFromName(
                                 assetTypeFor(toCategory))
                                 .value();
            auto upserted = registry_.upsert(std::move(updated));
            if (upserted.isError()) {
                return makeUnexpected(upserted.error());
            }
            break;
        }
    }
    return persist();
}

// =============================================================================
// Persistência
// =============================================================================

Result<void> AssetBrowser::saveRegistry() const
{
    auto text = registry_.serialize();
    if (text.isError()) {
        return makeUnexpected(text.error());
    }
    auto written = fs_->writeAllText(registryPath_, text.value());
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    return {};
}

Result<void> AssetBrowser::loadRegistry()
{
    auto exists = fs_->exists(registryPath_);
    if (exists.isError()) {
        return makeUnexpected(exists.error());
    }
    if (!exists.value()) {
        registry_ = {};
        return {}; // projeto novo — registry vazio
    }
    auto text = fs_->readAllText(registryPath_);
    if (text.isError()) {
        return makeUnexpected(text.error());
    }
    auto parsed = eng::assets::AssetRegistry::deserialize(text.value());
    if (parsed.isError()) {
        return makeUnexpected(parsed.error());
    }
    registry_ = std::move(parsed.value());
    return {};
}

eng::fs::Path AssetBrowser::categoryDir(std::string_view category) const
{
    return assetsRoot_ / eng::fs::Path{std::string(category)};
}

Result<void> AssetBrowser::persist()
{
    return saveRegistry();
}

}  // namespace eng::editor
