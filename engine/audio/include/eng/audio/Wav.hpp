#pragma once

/// eng::audio::Wav — parser RIFF/WAVE.
///
/// Suporta PCM 8-bit unsigned, 16-bit, 24-bit e 32-bit float (formato 3).
/// Saída NORMALIZADA: f32 interleaved [-1..1] — o mixer só conhece floats.
/// Chunks RIFF desconhecidos são PULSADOS (arquivos com LIST/INFO ok).
/// NUNCA lança; erros precisos (magic, formato, truncado).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "eng/core/Result.hpp"

namespace eng::audio {

struct WavData {
    std::uint32_t sampleRate{48000};
    std::uint16_t channels{2};
    std::vector<float> samples; ///< interleaved, [-1..1]
};

class Wav final {
public:
    Wav() = delete;

    [[nodiscard]] static eng::core::Result<WavData> parse(
        std::span<const std::byte> bytes);
};

}  // namespace eng::audio
