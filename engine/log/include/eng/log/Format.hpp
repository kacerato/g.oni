#pragma once

#include <string>
#include <string_view>
#include <type_traits>

namespace eng::log {

/// Formatação minimalista estilo "{}": substitui cada "{}" pelo próximo
/// argumento, em ordem. Escapes: "{{" → "{", "}}" → "}".
///
/// Suporta: string_view, std::string, const char*, char, bool, integrais
/// (incluindo int8_t/uint8_t como NÚMEROS), enums (como inteiro) e pontos
/// flutuantes (float → double, "%.6g").
///
/// Regras sem exceções:
/// - "{}" excedente (sem argumento restante) permanece literal "{}".
/// - Argumentos excedentes (sem "{}" restante) são ignorados.
/// - const char* nulo vira "(null)".
///
/// Substituída por fmt/spdlog quando ADR permitir nova dependência.
template <typename... Args>
[[nodiscard]] std::string format(std::string_view pattern, const Args&... args);

namespace detail {

void appendInteger(std::string& out, long long value);
void appendUnsigned(std::string& out, unsigned long long value);
void appendFloating(std::string& out, double value);

template <typename T>
void appendValue(std::string& out, const T& value) {
    if constexpr (std::is_same_v<T, char>) {
        out.push_back(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        out.append(value ? "true" : "false");
    } else if constexpr (std::is_same_v<T, char*> || std::is_same_v<T, const char*>) {
        out.append(value != nullptr ? value : "(null)");
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        out.append(std::string_view{value});
    } else if constexpr (std::is_enum_v<T>) {
        appendInteger(out, static_cast<long long>(value));
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) {
            appendInteger(out, static_cast<long long>(value));
        } else {
            appendUnsigned(out, static_cast<unsigned long long>(value));
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        appendFloating(out, static_cast<double>(value));
    } else {
        static_assert(sizeof(T) == 0, "tipo não suportado por eng::log::format");
    }
}

void formatAppend(std::string& out, std::string_view rest);

template <typename T, typename... Rest>
void formatAppend(std::string& out, std::string_view rest, const T& first,
                  const Rest&... restArgs) {
    std::size_t i = 0;
    std::size_t placeholder = rest.size();
    while (i < rest.size()) {
        const char c = rest[i];
        const bool hasNext = (i + 1 < rest.size());
        if (c == '{') {
            if (hasNext && rest[i + 1] == '{') {
                out.push_back('{');
                i += 2;
                continue;
            }
            if (hasNext && rest[i + 1] == '}') {
                placeholder = i; // "{}" encontrado
                break;
            }
            out.push_back('{'); // "{" solto é literal
            ++i;
            continue;
        }
        if (c == '}' && hasNext && rest[i + 1] == '}') {
            out.push_back('}');
            i += 2;
            continue;
        }
        out.push_back(c);
        ++i;
    }

    if (placeholder < rest.size()) {
        appendValue(out, first);
        formatAppend(out, rest.substr(placeholder + 2), restArgs...);
    }
    // Sem "{}" restante: argumentos excedentes são ignorados (documentado).
}

} // namespace detail

template <typename... Args>
std::string format(std::string_view pattern, const Args&... args) {
    std::string out;
    out.reserve(pattern.size() + 24 * sizeof...(Args));
    detail::formatAppend(out, pattern, args...);
    return out;
}

} // namespace eng::log
