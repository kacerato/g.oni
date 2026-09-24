#pragma once

#include <cstdint>
#include <string_view>

namespace eng::log {

/// Severidade canônica. A ordem enumérica define a filtragem:
/// mensagens abaixo do nível mínimo do Logger são descartadas.
enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

/// Nome estável em caixa alta ("TRACE" ... "FATAL") para sinks e parsing.
[[nodiscard]] constexpr std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "???";
}

[[nodiscard]] constexpr bool operator<(LogLevel a, LogLevel b) noexcept {
    return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
}
[[nodiscard]] constexpr bool operator<=(LogLevel a, LogLevel b) noexcept {
    return static_cast<std::uint8_t>(a) <= static_cast<std::uint8_t>(b);
}
[[nodiscard]] constexpr bool operator>(LogLevel a, LogLevel b) noexcept {
    return static_cast<std::uint8_t>(a) > static_cast<std::uint8_t>(b);
}
[[nodiscard]] constexpr bool operator>=(LogLevel a, LogLevel b) noexcept {
    return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b);
}

} // namespace eng::log
