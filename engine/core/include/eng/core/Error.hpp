#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace eng::core {

/// Códigos de status canônicos do motor. Códigos específicos de domínio
/// (rhi, physics, assets) entram em enums próprios; StatusCode cobre o
/// núcleo comum.
enum class StatusCode : std::uint32_t {
    Ok = 0,
    Unknown,
    InvalidArgument,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    ParseError,
    NotSupported,
    IOError,
    // Estados de máquina/contrato distintos de argumento
    // inválido — a fronteira do editor precisa classificá-los separadamente.
    InvalidState,
    Internal,
};

/// Erro leve carregado por Result<T, E>.
/// Agregado: `Error{StatusCode::ParseError, "mensagem"}`.
struct Error {
    StatusCode code{StatusCode::Unknown};
    std::string message{};

    /// Nome estável do código ("ParseError", ...). Não é traduzível —
    /// é chave de log/telemetria.
    [[nodiscard]] std::string_view codeName() const noexcept;

    [[nodiscard]] bool operator==(const Error&) const = default;
};

} // namespace eng::core
