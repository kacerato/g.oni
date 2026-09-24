#include "eng/audio/AudioAdapt.hpp"

/// Implementação da adaptação mixer→device (P3.4 — ver AudioAdapt.hpp
/// para o problema e o contrato). Funções puras + resampler + portão —
/// testadas no Linux pelo suite de áudio (sem NDK).

#include <algorithm>
#include <numeric>

namespace eng::audio {

// =============================================================================
// convertFrames — formato + canais
// =============================================================================

namespace {

[[nodiscard]] float clampUnity(float v) noexcept
{
    return v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
}

/// Escreve UMA amostra já clampada no formato alvo.
void writeSample(void* dst, std::uint64_t index, float v,
                 DeviceSampleFormat format) noexcept
{
    switch (format) {
    case DeviceSampleFormat::Float: {
        static_cast<float*>(dst)[index] = v;
        return;
    }
    case DeviceSampleFormat::Int16: {
        static_cast<std::int16_t*>(dst)[index] = static_cast<std::int16_t>(
            v >= 1.f ? 32767 : (v <= -1.f ? -32768 : v * 32767.f));
        return;
    }
    case DeviceSampleFormat::Int32: {
        static_cast<std::int32_t*>(dst)[index] = static_cast<std::int32_t>(
            v >= 1.f
                ? 2147483647
                : (v <= -1.f ? -2147483647 - 1
                             : static_cast<std::int64_t>(v * 2147483647.f)));
        return;
    }
    }
}

}  // namespace

void convertFrames(const float* src, void* dst, std::uint32_t frames,
                   std::uint32_t srcChannels, std::uint32_t dstChannels,
                   DeviceSampleFormat format) noexcept
{
    if (dst == nullptr || dstChannels == 0 || frames == 0 ||
        bytesPerSample(format) == 0) {
        return;
    }
    // Sem fonte legível → silêncio (defensivo: nunca deixa o buffer do
    // device com lixo — é o que o HAL toca no fone).
    if (src == nullptr || srcChannels == 0) {
        std::memset(dst, 0,
                    static_cast<std::size_t>(frames) * dstChannels *
                        bytesPerSample(format));
        return;
    }

    for (std::uint32_t f = 0; f < frames; ++f) {
        const float* in = src + static_cast<std::size_t>(f) * srcChannels;
        const std::uint64_t base =
            static_cast<std::uint64_t>(f) * dstChannels;

        if (dstChannels == srcChannels) {
            for (std::uint32_t c = 0; c < dstChannels; ++c) {
                writeSample(dst, base + c, clampUnity(in[c]), format);
            }
            continue;
        }
        if (dstChannels > srcChannels) {
            // Extras duplicam o ÚLTIMO canal de origem (mono→N).
            const float last = in[srcChannels - 1];
            for (std::uint32_t c = 0; c < srcChannels; ++c) {
                writeSample(dst, base + c, clampUnity(in[c]), format);
            }
            for (std::uint32_t c = srcChannels; c < dstChannels; ++c) {
                writeSample(dst, base + c, clampUnity(last), format);
            }
            continue;
        }
        // dstChannels < srcChannels: primeiros passam, último SOMA o
        // restante (downmix 2→1 = L+R, clampado).
        for (std::uint32_t c = 0; c + 1 < dstChannels; ++c) {
            writeSample(dst, base + c, clampUnity(in[c]), format);
        }
        float sum = 0.f;
        for (std::uint32_t c = dstChannels - 1; c < srcChannels; ++c) {
            sum += in[c];
        }
        writeSample(dst, base + dstChannels - 1, clampUnity(sum), format);
    }
}

// =============================================================================
// LinearResampler — push/pull com histórico interno
// =============================================================================

bool LinearResampler::configure(std::uint32_t inRate,
                                 std::uint32_t outRate,
                                 std::uint32_t channels,
                                 std::uint32_t maxOutFrames) noexcept
{
    // Teto anti-HAL-insano (taxas reais de áudio ficam <= 384 kHz).
    constexpr std::uint32_t kMaxSaneRate = 1u << 19;
    if (inRate == 0 || outRate == 0 || inRate > kMaxSaneRate ||
        outRate > kMaxSaneRate || channels == 0 ||
        channels > kMaxChannels || maxOutFrames == 0) {
        return false;
    }
    const std::uint32_t g = std::gcd(inRate, outRate);
    num_ = inRate / g;
    den_ = outRate / g;
    if (num_ > kMaxRateRatio * den_ || den_ > kMaxRateRatio * num_) {
        return false;  // razão fora de [1/8, 8] — device inconsistente
    }
    channels_ = channels;
    totalOut_ = 0;
    pushed_ = 0;
    base_ = 0;
    capacity_ = maxPushFrames(maxOutFrames) + kMaxRateRatio;
    // Alocação única na thread do HOST (regra do callback: zero alocação
    // no caminho quente). Sob -fno-exceptions uma falha aqui
    // termina o processo — o mesmo contrato de qualquer container do
    // engine; o tamanho é modesto (~dezenas de KB).
    buf_.assign(static_cast<std::size_t>(capacity_) * channels, 0.f);
    return true;
}

std::uint32_t LinearResampler::framesToPush(
    std::uint32_t outFrames) const noexcept
{
    if (outFrames == 0) {
        return 0;
    }
    // A última saída do lote interpola os frames de entrada
    // floor((T+N-1)*num/den) e +1 — o histórico precisa cobrir o +1:
    const std::uint64_t nLast = totalOut_ + outFrames - 1;
    const std::uint64_t need = positionOf(nLast) + 2;
    if (need <= pushed_) {
        return 0;  // razão densa: o histórico retido já cobre o lote
    }
    const std::uint64_t missing = need - pushed_;
    return static_cast<std::uint32_t>(missing > 0xffffffffull
                                          ? 0xffffffffu
                                          : missing);
}

std::uint32_t LinearResampler::maxPushFrames(
    std::uint32_t maxOutFrames) const noexcept
{
    // Teto do próximo lote: maxOutFrames*num_/den_ + folgas de seam e
    // arredondamento.
    const std::uint64_t worst =
        (static_cast<std::uint64_t>(maxOutFrames) * num_) / den_ + 4;
    return static_cast<std::uint32_t>(
        worst > 0xffffffffull ? 0xffffffffu
                               : static_cast<std::uint32_t>(worst));
}

void LinearResampler::push(const float* src, std::uint32_t srcCount) noexcept
{
    if (srcCount == 0 || channels_ == 0) {
        return;
    }
    if (src == nullptr) {
        srcCount = 0;
    }
    // Compacta o histórico à frente quando o append não couber: o
    // frame mais antigo que a PRÓXIMA saída pode ler é a posição da
    // saída `totalOut_` (o `a` do próximo lerp) — descarta o que vem
    // antes disso.
    const std::uint64_t keepFrom = std::min(pushed_, positionOf(totalOut_));
    if (keepFrom > base_) {
        const std::uint64_t keepCount = pushed_ - keepFrom;
        if (keepCount > 0) {
            std::memmove(buf_.data(),
                         buf_.data() + static_cast<std::size_t>(
                                           (keepFrom - base_) * channels_),
                         static_cast<std::size_t>(keepCount) * channels_ *
                             sizeof(float));
        }
        base_ = keepFrom;
    }
    // Defensivo: srcCount maior que a capacidade do anel (o backend
    // limita os lotes a maxOutFrames — não ocorre por contrato): aceita
    // apenas o que cabe (degrada sem corromper).
    const std::uint64_t room = static_cast<std::uint64_t>(capacity_) -
                               (pushed_ - base_);
    if (srcCount > room) {
        srcCount = static_cast<std::uint32_t>(room);
    }
    if (srcCount > 0 && src != nullptr) {
        std::memcpy(buf_.data() + static_cast<std::size_t>(
                                      (pushed_ - base_) * channels_),
                    src, static_cast<std::size_t>(srcCount) * channels_ *
                             sizeof(float));
        pushed_ += srcCount;
    }
}

void LinearResampler::produce(std::uint32_t outFrames, float* dst) noexcept
{
    if (dst == nullptr || outFrames == 0) {
        return;
    }
    if (channels_ == 0) {
        return;
    }
    const std::uint32_t ch = channels_;
    const std::uint64_t lastAvail =
        pushed_ > 0 ? pushed_ - 1 : 0;  // último frame com dado real

    for (std::uint32_t o = 0; o < outFrames; ++o) {
        const std::uint64_t n = totalOut_ + o;
        const std::uint64_t p = positionOf(n);         // floor da posição
        const std::uint64_t rem = (n * num_) % den_;    // fase exata
        const double frac =
            static_cast<double>(rem) / static_cast<double>(den_);

        // Par de interpolação (p, p+1) — clampado ao histórico disponível
        // (defensivo: chamador que under-deliver vê a última amostra
        // segurada, nunca leitura fora do buffer).
        std::uint64_t a = p;
        std::uint64_t b = p + 1;
        if (a < base_ || pushed_ == 0) {
            a = base_;
        }
        if (a > lastAvail) {
            a = lastAvail;
        }
        if (b < a) {
            b = a;
        }
        if (b > lastAvail) {
            b = lastAvail;
        }
        const float* fa =
            buf_.data() + static_cast<std::size_t>((a - base_) * ch);
        const float* fb =
            buf_.data() + static_cast<std::size_t>((b - base_) * ch);
        float* out = dst + static_cast<std::size_t>(o) * ch;
        for (std::uint32_t c = 0; c < ch; ++c) {
            out[c] = static_cast<float>(fa[c] + (fb[c] - fa[c]) * frac);
        }
    }
    totalOut_ += outFrames;
}

}  // namespace eng::audio
