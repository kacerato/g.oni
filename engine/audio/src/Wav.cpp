#include "eng/audio/Wav.hpp"

/// Wav — parser RIFF/WAVE. Sem exceções; chunks extras pulsados.

#include <cstring>

namespace eng::audio {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error wavError(StatusCode code, std::string message)
{
    return Error{code, "wav: " + std::move(message)};
}

[[nodiscard]] std::uint16_t readU16(const std::byte* p) noexcept
{
    std::uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] std::uint32_t readU32(const std::byte* p) noexcept
{
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

/// Amostra little-endian do formato dado → float [-1..1].
[[nodiscard]] float sampleToFloat(const std::byte* p, std::uint16_t bits,
                                  std::uint16_t format) noexcept
{
    switch (bits) {
    case 8: {
        // PCM 8-bit é UNSIGNED (padrão WAVE).
        const std::uint8_t raw = static_cast<std::uint8_t>(*p);
        return (static_cast<float>(raw) - 128.f) / 128.f;
    }
    case 16: {
        const std::int16_t raw = static_cast<std::int16_t>(readU16(p));
        return static_cast<float>(raw) / 32768.f;
    }
    case 24: {
        const std::uint32_t raw = static_cast<std::uint32_t>(
                                      static_cast<std::uint8_t>(p[0])) |
                                  (static_cast<std::uint32_t>(
                                       static_cast<std::uint8_t>(p[1]))
                                   << 8) |
                                  (static_cast<std::uint32_t>(
                                       static_cast<std::uint8_t>(p[2]))
                                   << 16);
        // Sign-extend de 24 bits.
        const std::int32_t signed24 = static_cast<std::int32_t>(raw << 8) >> 8;
        return static_cast<float>(signed24) / 8388608.f;
    }
    case 32: {
        if (format == 3) { // IEEE float 32
            std::uint32_t raw = readU32(p);
            float out = 0.f;
            std::memcpy(&out, &raw, sizeof(out));
            return out;
        }
        const std::int32_t raw = static_cast<std::int32_t>(readU32(p));
        return static_cast<float>(raw) / 2147483648.f;
    }
    default:
        return 0.f;
    }
}

}  // namespace

Result<WavData> Wav::parse(std::span<const std::byte> bytes)
{
    constexpr std::size_t kHeaderSize = 12;
    if (bytes.size() < kHeaderSize) {
        return makeUnexpected(wavError(StatusCode::ParseError,
                                       "arquivo menor que o header RIFF"));
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return makeUnexpected(
            wavError(StatusCode::ParseError, "não é RIFF/WAVE"));
    }

    std::uint16_t format = 1; // 1=PCM, 3=float
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bits = 0;

    // Percorre chunks: "fmt " e "data" são os que importam.
    std::size_t offset = kHeaderSize;
    const std::byte* dataStart = nullptr;
    std::size_t dataSize = 0;
    while (offset + 8 <= bytes.size()) {
        const std::byte* chunkId = bytes.data() + offset;
        const std::uint32_t chunkSize = readU32(bytes.data() + offset + 4);
        const std::size_t body = offset + 8;
        if (body + chunkSize > bytes.size()) {
            break; // chunk truncado → ignora o rabicho
        }
        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                return makeUnexpected(wavError(StatusCode::ParseError,
                                               "fmt chunk curto"));
            }
            format = readU16(bytes.data() + body);
            channels = readU16(bytes.data() + body + 2);
            sampleRate = readU32(bytes.data() + body + 4);
            bits = readU16(bytes.data() + body + 14);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataStart = bytes.data() + body;
            dataSize = chunkSize;
        }
        // Chunks extras (LIST etc.) são PULSADOS.
        offset = body + chunkSize + (chunkSize & 1u); // padding par
    }

    if (dataStart == nullptr) {
        return makeUnexpected(
            wavError(StatusCode::ParseError, "sem chunk data"));
    }
    if (channels == 0 || sampleRate == 0) {
        return makeUnexpected(
            wavError(StatusCode::ParseError, "fmt ausente antes de data"));
    }
    if (format != 1 && format != 3) {
        return makeUnexpected(wavError(StatusCode::NotSupported,
                                       "formato " + std::to_string(format) +
                                           " (só PCM e float)"));
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        // Bug C-10 da auditoria final: `bits + " literal"` somava uint16_t a
        // ponteiro (aritmética + leitura OOB p/ bits >= 20). std::to_string.
        return makeUnexpected(wavError(StatusCode::NotSupported,
                                       std::to_string(bits) +
                                           " bits não suportado"));
    }

    WavData out;
    out.sampleRate = sampleRate;
    out.channels = static_cast<std::uint16_t>(channels);
    const std::size_t bytesPerSample = bits / 8;
    const std::size_t totalSamples = dataSize / bytesPerSample;
    out.samples.reserve(totalSamples);
    for (std::size_t i = 0; i < totalSamples; ++i) {
        out.samples.push_back(
            sampleToFloat(dataStart + i * bytesPerSample, bits, format));
    }
    return out;
}

}  // namespace eng::audio
