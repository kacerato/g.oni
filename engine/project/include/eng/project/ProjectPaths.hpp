#pragma once

/// eng::project::ProjectPaths — resolução de raízes RELATIVAS ao diretório
/// do arquivo de projeto (FASE 3; ADR-032).
///
/// - NUNCA armazena absolutos no arquivo; absolutos são COMPUTADOS aqui a
///   partir do diretório onde o project.goni.json vive — mover o projeto
///   de lugar não invalida NADA (critério E da missão).
/// - Raízes convencionais: assets/, cache/, build/ sob o diretório do
///   projeto; sceneRoots vêm do ProjectConfig.
#include <string_view>
#include <vector>

#include "eng/fs/Path.hpp"

namespace eng::project {

class ProjectPaths final {
public:
    ProjectPaths() = default;

    /// `projectDir` = diretório do project.goni.json (base de tudo).
    explicit ProjectPaths(eng::fs::Path projectDir);

    /// Defaults de SISTEMA (fora do projeto) via eng::platform — cache de
    /// usuário compartilhado quando o projeto não é "portable". Consome a
    /// aresta platform de forma real.
    [[nodiscard]] static ProjectPaths systemDefaults(std::string_view appName);

    [[nodiscard]] const eng::fs::Path& projectDir() const noexcept
    {
        return projectDir_;
    }

    /// Raiz de assets (base dos sourcePaths do AssetRegistry).
    [[nodiscard]] eng::fs::Path assetsRoot() const;

    /// Cache derivado do projeto (formato binário futuro — ADR-029).
    [[nodiscard]] eng::fs::Path cacheRoot() const;

    /// Saídas de build do projeto.
    [[nodiscard]] eng::fs::Path buildRoot() const;

    /// Resolve um path RELATIVO do config contra o diretório do projeto.
    [[nodiscard]] eng::fs::Path resolve(const eng::fs::Path& relative) const;

    /// Resolve TODOS os sceneRoots do config (relativos → absolutos).
    [[nodiscard]] std::vector<eng::fs::Path> sceneRoots(
        const std::vector<eng::fs::Path>& configRoots) const;

private:
    eng::fs::Path projectDir_{std::string_view(".")};
};

} // namespace eng::project
