#pragma once

/// eng::project::ProjectConfig — metadados do projeto (FASE 3; ADR-032).
///
/// REGRA DURA (missão §2.6/R10): nenhuma string ABSOLUTA pode ser
/// persistida — assetRegistryPath e sceneRoots são relativos ao diretório
/// do arquivo de projeto (project.goni.json); o parse REJEITA absolutos.
#include <cstdint>
#include <string>
#include <vector>

#include "eng/core/Version.hpp"
#include "eng/fs/Path.hpp"
#include "eng/project/GridConfig.hpp"
#include "eng/project/ProjectId.hpp"

namespace eng::project {

inline constexpr std::uint32_t kProjectFormatVersion = 1;

/// Camada de COLISÃO nomeada (bitfield) — ≠ camadas de
/// cena/tick. Filtragem de pares: (A.mask & B.layer)
/// && (B.mask & A.layer). Default do projeto novo: bit 1 "default".
struct CollisionLayerName {
    std::string name;
    std::uint32_t bit; ///< potência de 2 (1, 2, 4, …)
    [[nodiscard]] bool operator==(const CollisionLayerName&) const = default;
};

/// Tabela default (ausente no project.goni.json de versões anteriores).
[[nodiscard]] inline std::vector<CollisionLayerName>
defaultCollisionLayers()
{
    return {CollisionLayerName{"default", 1u}};
}

struct ProjectConfig {
    ProjectId projectId;
    std::string name;
    eng::core::Version engineVersion;      ///< versão do motor que escreveu
    eng::fs::Path assetRegistryPath;       ///< relativo (ex.: "asset_registry.json")
    std::vector<eng::fs::Path> sceneRoots;  ///< relativos (ex.: "assets/scenes")
    /// Bitfields nomeados de colisão (project settings).
    /// Vazio = tabela default (defaultCollisionLayers) — o parse preenche
    /// quando a chave ausente; toJson SEMPRE escreve (aditivo, ADR-031).
    std::vector<CollisionLayerName> collisionLayers{};
    /// Grade do viewport em unidades de mundo (chave
    /// aditiva "grid" — ausente = default).
    GridConfig grid{};

    [[nodiscard]] bool operator==(const ProjectConfig&) const = default;
};

} // namespace eng::project
