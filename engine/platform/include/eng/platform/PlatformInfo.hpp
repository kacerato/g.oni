#pragma once

/// eng::platform::PlatformInfo — fatos estáticos do sistema hospedeiro
///.
///
/// Este é o ÚNICO módulo do motor autorizado a ramificar por SO em tempo de
/// compilação (é a razão dele existir): core/ecs/scene e os demais módulos
/// permanecem livres de #ifdef de plataforma.
#include <cstdint>
#include <string_view>

namespace eng::platform {

enum class Endianness : std::uint8_t {
    Little,
    Big,
};

enum class BuildType : std::uint8_t {
    Debug,
    Release,
};

/// Snapshot imutável do processo/build. Todos os campos apontam para
/// literais estáticos (string_views válidas para sempre).
struct PlatformInfo {
    std::string_view name;        ///< "Linux" (SO hospedeiro)
    std::string_view arch;        ///< "x86_64", "aarch64", ...
    Endianness endianness = Endianness::Little;
    BuildType buildType = BuildType::Debug;
    bool addressSanitizer = false;  ///< __SANITIZE_ADDRESS__ no build corrente
    bool undefinedSanitizer = false; ///< __SANITIZE_UNDEFINED__ no build corrente

    /// Fatos do processo/build corrente (determinístico por binário).
    [[nodiscard]] static PlatformInfo current();
};

} // namespace eng::platform
