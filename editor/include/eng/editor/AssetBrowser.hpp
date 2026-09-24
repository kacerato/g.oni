#pragma once

/// eng::editor::AssetBrowser — descoberta/importação/operações de assets
///.
///
/// COMPOSIÇÃO (auditoria G7): eng::assets fornece identidade/registry;
/// eng::fs fornece bytes; este módulo compõe os dois sob as regras de
/// projeto.
///
/// Categorias (missão §8.5) mapeiam para `eng::assets::AssetType`:
///   scenes→Scene, prefabs→Prefab, json→Json, textures→Texture,
///   models→Mesh, materials→Material, shaders→Shader, audio→Audio,
///   scripts→Script (as três últimas RESERVADAS na FASE 8 — listam/organizam
///   arquivos; loaders vêm com as fases de conteúdo).
///
/// Layout de disco criado por newProject:
///   <root>/project.goni.json
///   <root>/assets/<categoria>/...
///   <root>/scenes/...
/// Registry persistido no `assetRegistryPath` do ProjectConfig.
///
/// Thread-safety: single-threaded (como AssetRegistry — ADR-034).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "eng/assets/AssetRegistry.hpp"
#include "eng/core/Result.hpp"
#include "eng/fs/FileSystem.hpp"
#include "eng/fs/Path.hpp"

namespace eng::editor {

class AssetBrowser final {
public:
    /// `fs` é EMPRESTADO (dono é o documento/projeto). `assetsRoot` e
    /// `registryPath` são relativos ao root do projeto.
    AssetBrowser(eng::fs::FileSystem& fs, const eng::fs::Path& assetsRoot,
                 const eng::fs::Path& registryPath);

    /// Entrada listada (registry ∪ varredura de disco).
    struct Entry {
        std::string name;         ///< nome do arquivo (sem diretório)
        std::string sourcePath;   ///< relativo ao root (estável)
        std::string id;           ///< AssetId canônico ("-" quando não catalogado)
        bool registered{false};
    };

    /// Nomes canônicos das categorias suportadas (ordem estável).
    [[nodiscard]] static const std::vector<std::string>& categories();

    /// Categoria → AssetType ("scenes"→Scene...). Desconhecida → nullptr.
    [[nodiscard]] static const char* assetTypeFor(std::string_view category);

    /// Lista a categoria: entradas do registry DO tipo + arquivos da pasta
    /// `assets/<category>` ainda não catalogados (registered=false).
    [[nodiscard]] eng::core::Result<std::vector<Entry>> list(
        std::string_view category) const;

    /// Importa: move `tempRelPath` (dentro do projeto, ex.: SAF copiou para
    /// `.import_tmp/foo.png`) para `assets/<category>/<name>`, cataloga com
    /// novo AssetId e persiste o registry. Retorna o id canônico.
    /// `outFinalName` (opcional): nome do arquivo APÓS o sufixo de extensão
    /// do original ser preservado (ex.: name="hero" + .png → "hero.png") —
    /// a fronteira JNI valida o CONTEÚDO lendo exatamente este nome.
    [[nodiscard]] eng::core::Result<std::string> import(
        std::string_view tempRelPath, std::string_view category,
        std::string_view name, std::string* outFinalName = nullptr);

    /// Renomeia (disco + registry; referências por AssetId sobrevivem).
    [[nodiscard]] eng::core::Result<void> rename(std::string_view category,
                                                 std::string_view name,
                                                 std::string_view newName);

    /// Remove arquivo + entrada do registry.
    [[nodiscard]] eng::core::Result<void> remove(std::string_view category,
                                                std::string_view name);

    /// Move entre categorias (disco + tipo no registry).
    [[nodiscard]] eng::core::Result<void> move(std::string_view fromCategory,
                                               std::string_view name,
                                               std::string_view toCategory);

    /// Lê os BYTES de um asset da categoria (validação anti-traversal vem
    /// do Path::isWithin — ADR-027). Para decode de imagem/preview/áudio.
    [[nodiscard]] eng::core::Result<std::vector<std::byte>> read(
        std::string_view category, std::string_view name) const;

    /// Registra no AssetRegistry um arquivo JÁ POSICIONADO em
    /// `assets/<category>/<name>` (evolução P0-7: scripts são escritos
    /// diretamente pelo editor — writeAllText — e precisam catalogar SEM
    /// passar pelo fluxo de import/move). Idempotente por path (upsert).
    [[nodiscard]] eng::core::Result<void> registerExisting(
        std::string_view category, std::string_view name);

    /// Persistência explícita (import/rename/remove já salvam).
    [[nodiscard]] eng::core::Result<void> saveRegistry() const;
    [[nodiscard]] eng::core::Result<void> loadRegistry();

    [[nodiscard]] const eng::assets::AssetRegistry& registry() const
    {
        return registry_;
    }

private:
    [[nodiscard]] eng::fs::Path categoryDir(std::string_view category) const;
    [[nodiscard]] eng::core::Result<void> persist();

    eng::fs::FileSystem* fs_;       ///< emprestado
    eng::fs::Path assetsRoot_;
    eng::fs::Path registryPath_;
    eng::assets::AssetRegistry registry_{};
};

} // namespace eng::editor
