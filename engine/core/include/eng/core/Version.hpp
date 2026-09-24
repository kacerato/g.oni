#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "eng/core/Result.hpp"

namespace eng::core {

/// Versão semântica do motor e do formato de projeto.
/// Agregado: `Version{0, 1, 0}`. Comparação lexicográfica por
/// (major, minor, patch) via operator<=>.
struct Version {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    /// Faz o parse de "MAJOR.MINOR.PATCH" (dígitos, sem espaços, sem sufixo).
    /// Aceita zeros à esquerda ("01.2.3" == "1.2.3"). Rejeita "v"-prefixo,
    /// partes ausentes, estouro de uint32 e caracteres sobrando.
    [[nodiscard]] static Result<Version> parse(std::string_view text);

    /// "MAJOR.MINOR.PATCH" (snprintf — sem exceções, fallback "0.0.0").
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] constexpr std::strong_ordering operator<=>(const Version&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const Version&) const noexcept = default;
};

} // namespace eng::core
