#pragma once

/// eng::platform::Environment — wrapper de variáveis de ambiente
///.
///
/// Thread-safety: get é reatrido; set/unset modificam o processo
/// inteiro (semântica de setenv) — NÃO usem concorrentemente.
#include <optional>
#include <string>
#include <string_view>

namespace eng::platform {

class Environment final {
public:
    Environment() = delete;

    /// Valor da variável (nullopt se não definida). Nome vazio → nullopt.
    [[nodiscard]] static std::optional<std::string> get(std::string_view name);

    /// Define a variável. overwrite=false não substitui valor existente.
    /// false: nome vazio/contém '=' ou falha de setenv.
    static bool set(std::string_view name, std::string_view value,
                    bool overwrite = true);

    /// Remove a variável. false: nome vazio ou variável inexistente.
    static bool unset(std::string_view name);
};

} // namespace eng::platform
