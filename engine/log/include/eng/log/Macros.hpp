#pragma once

/// Macros de log. A categoria é fixada por unidade de tradução com
/// ENG_LOG_CATEGORY("nome") ANTES do primeiro uso das macros — sem categoria
/// declarada, o uso dos macros é erro de compilação (explícito de propósito).
///
/// Nível filtrado ⇒ custo zero: a checagem acontece antes de formatar.
///
/// Uso:
///   ENG_LOG_CATEGORY("rhi")
///   ...
///   ENG_INFO("swapchain {}x{} recriada", width, height);

#include "eng/log/Format.hpp"
#include "eng/log/Logger.hpp"

/// Declara a categoria usada pelos macros ENG_* neste arquivo.
/// [[maybe_unused]]: TUs que declaram categoria mas não logam são legais
/// (o clang/NDK pune const não-usada com -Wunused-const-variable — o GCC
/// não; fix pós-falha real no CI Android do commit 50966b0).
#define ENG_LOG_CATEGORY(categoryName)                                     \
    [[maybe_unused]] static constexpr ::std::string_view eng_log_category_{ \
        categoryName};

#define ENG_LOG(level, ...)                                                                     \
    do {                                                                                        \
        if ((level) >= ::eng::log::Logger::get().minLevel()) {                                  \
            ::eng::log::Logger::get().log((level), eng_log_category_,                          \
                                          ::eng::log::format(__VA_ARGS__));                     \
        }                                                                                       \
    } while (false)

#define ENG_TRACE(...) ENG_LOG(::eng::log::LogLevel::Trace, __VA_ARGS__)
#define ENG_DEBUG(...) ENG_LOG(::eng::log::LogLevel::Debug, __VA_ARGS__)
#define ENG_INFO(...) ENG_LOG(::eng::log::LogLevel::Info, __VA_ARGS__)
#define ENG_WARN(...) ENG_LOG(::eng::log::LogLevel::Warn, __VA_ARGS__)
#define ENG_ERROR(...) ENG_LOG(::eng::log::LogLevel::Error, __VA_ARGS__)
#define ENG_FATAL(...) ENG_LOG(::eng::log::LogLevel::Fatal, __VA_ARGS__)
