#pragma once

/// LogcatSink — integra o eng::log ao logcat Android (FASE 7, missão §XVIII).
///
/// Sink de LOG do NDK (android/log.h — NÃO é JNI): as mensagens chegam já
/// formatadas pelo Logger com categoria do engine; o prefixo [G.ONI] é
/// adicionado pelo Chamador nos eventos-chave (a tag fixa "GONI" agrupa
/// o output no logcat). Sem logs por frame em Release: eventos de frame
/// são apenas os "first frame" one-shot do runtime.

#ifdef __ANDROID__

#include <android/log.h>

#include <string_view>

#include "eng/log/LogLevel.hpp"
#include "eng/log/LogSink.hpp"

namespace eng::android {

class LogcatSink final : public eng::log::LogSink {
public:
    void write(eng::log::LogLevel level, std::string_view category,
               std::string_view message) override {
        const int priority = [&] {
            switch (level) {
            case eng::log::LogLevel::Trace: return ANDROID_LOG_VERBOSE;
            case eng::log::LogLevel::Debug: return ANDROID_LOG_DEBUG;
            case eng::log::LogLevel::Info: return ANDROID_LOG_INFO;
            case eng::log::LogLevel::Warn: return ANDROID_LOG_WARN;
            case eng::log::LogLevel::Error: return ANDROID_LOG_ERROR;
            case eng::log::LogLevel::Fatal: return ANDROID_LOG_FATAL;
            }
            return ANDROID_LOG_DEFAULT;
        }();
        // Tag fixa agrupa no logcat; categoria do engine vai no corpo.
        __android_log_print(priority, "GONI", "%.*s: %.*s",
                           static_cast<int>(category.size()), category.data(),
                           static_cast<int>(message.size()), message.data());
    }
};

}  // namespace eng::android

#endif  // __ANDROID__
