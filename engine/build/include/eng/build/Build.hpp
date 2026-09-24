#pragma once

/// eng::build — Build & Export Pipeline.
///
/// Projeto → manifesto determinístico → grafo de dependências → scan de
/// referências → validação bloqueante → cook em envelope GONI → cache
/// conteúdo-endereçado → bundle verificado → export por alvo.
///
/// Princípios (missão FASE 12 + design phase12_audit/design.md):
///   - ENGINE e DADOS separados: o pipeline empacota DADOS do projeto,
///     nunca recompila o motor, nunca conhece um projeto específico;
///   - TODOS os paths da BuildConfig são RELATIVOS à raiz do projeto
///     (path absoluto = erro — nada de estado fora do projeto);
///   - determinismo: mesma árvore ⇒ mesmos bytes do manifest e do bundle
///     (testado); o cache é conteúdo-endereçado (FNV-1a 64 de conteúdo +
///     versão do cooker + tipo), sem timestamps;
///   - validação bloqueante: fonte ausente, ids duplicados, script que
///     não compila, manifest/target inválido; asset NÃO-USADO = WARN;
///   - scripts: validados por COMPILAÇÃO no build (bloqueante) e
///     empacotados como FONTE (serialização de bytecode é futuro do
///     ADR-049 — uma única fonte de verdade);
///   - envelope GONI por asset marcado SOURCE/DERIVED (v1:
///     tudo SOURCE — o marcador existe para cooks derivados futuros não
///     mudarem o formato do bundle).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/fs/FileSystem.hpp"
#include "eng/fs/Path.hpp"

namespace eng::build {

inline constexpr std::uint32_t kBuildConfigFormatVersion = 1;
inline constexpr std::uint32_t kManifestFormatVersion = 1;
inline constexpr std::uint32_t kCookerVersion = 1;
inline constexpr std::uint32_t kBundlePayloadVersion = 1;
/// assetType do envelope EXTERNO do bundle (fora do range do
/// eng::assets::AssetType — reservado pela FASE 12, ADR-050).
inline constexpr std::uint32_t kBundleEnvelopeType = 1000;

/// Alvos de export suportados (missão: android primeiro).
inline constexpr std::string_view kTargetAndroidArm64 = "android-arm64";
inline constexpr std::string_view kTargetLinuxDev = "linux-dev";

/// Diagnóstico de build (linha do pipeline + detalhe).
struct BuildDiag {
    std::string stage;   ///< "config", "manifest", "scan", "cook", …
    std::string message;
    bool warning = false; ///< WARN (não-usado) vs erro bloqueante
};

/// Opções de execução.
struct BuildOptions {
    bool forceCook = false; ///< clean build: ignora o cache (missão)
    /// Sobrescreve a versão do cooker para TESTAR invalidação de cache.
    std::uint32_t cookerVersionOverride = 0;
};

/// Relatório de build (evidência completa para o chamador/testes).
struct BuildReport {
    std::string manifestJson;         ///< manifesto gerado (determinístico)
    std::string projectId;
    std::string projectName;
    std::size_t assetsCooked = 0;      ///< entradas no bundle
    std::size_t cacheHits = 0;
    std::size_t cacheMisses = 0;
    std::size_t scriptsValidated = 0;  ///< .nis compilados com sucesso
    std::vector<std::string> unusedAssets; ///< assetIds não alcançados (WARN)
    std::vector<std::string> targets;  ///< alvos exportados
    std::vector<std::byte> bundleBytes; ///< bundle VERIFICADO (pós-decode)
};

/// Pipeline completo. `projectRoot` aponta a raiz (project.goni.json).
/// Erros: Result com o PRIMEIRO bloqueante + `diags` recebe TODOS os
/// achados (erros e warnings — o chamador decide logar).
[[nodiscard]] eng::core::Result<BuildReport> buildProject(
    eng::fs::FileSystem& fs, const eng::fs::Path& projectRoot,
    const BuildOptions& options, std::vector<BuildDiag>* diags = nullptr);

/// Decodifica e VERIFICA um bundle (CRC de todos os envelopes, contagem,
/// ids do manifest, contentHash por entrada). Uso: consumidores/tests.
struct BundleEntry {
    std::string assetId;
    std::uint32_t assetType = 0;
    std::string path;
    std::string contentHash; ///< FNV-1a 64 hex do CONTEÚDO do fonte
    bool derived = false;
    std::vector<std::byte> payload; ///< bytes do envelope de ASSET decodado
};
struct BundleInfo {
    std::string manifestJson;
    std::vector<BundleEntry> entries; ///< ordem do manifest (AssetId)
};
[[nodiscard]] eng::core::Result<BundleInfo> verifyBundle(
    std::span<const std::byte> bundleBytes);

/// FNV-1a 64 de bytes — hash de conteúdo do manifest/cache (público para
/// testes de determinismo; MESMA função do typeIdOf do reflect).
[[nodiscard]] std::string contentHashHex(std::span<const std::byte> data);

} // namespace eng::build
