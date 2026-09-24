#include "eng/audio/Audio.hpp"

/// AudioMixer + vozes + buses. Mix f32 por software; pull.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace eng::audio {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

/// Frames decodificados por janela de streaming (troca rara, custo baixo).
constexpr std::size_t kStreamWindowFrames = 16384;

[[nodiscard]] Error mixerError(StatusCode code, std::string message)
{
    return Error{code, "audio: " + std::move(message)};
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

/// Uma amostra do stream WAV → float (mesma tabela do Wav.cpp — pequena e
/// estável; duplicar aqui mantém o cursor de bytes simples).
[[nodiscard]] float streamSample(const std::byte* p, std::uint16_t bits,
                                bool isFloat) noexcept
{
    if (bits == 16) {
        const std::int16_t raw = static_cast<std::int16_t>(readU16(p));
        return static_cast<float>(raw) / 32768.f;
    }
    if (bits == 8) {
        return (static_cast<float>(static_cast<std::uint8_t>(*p)) - 128.f) /
               128.f;
    }
    if (bits == 24) {
        const std::uint32_t raw =
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16);
        const std::int32_t s = static_cast<std::int32_t>(raw << 8) >> 8;
        return static_cast<float>(s) / 8388608.f;
    }
    if (isFloat) {
        std::uint32_t raw = readU32(p);
        float out = 0.f;
        std::memcpy(&out, &raw, sizeof(out));
        return out;
    }
    const std::int32_t raw = static_cast<std::int32_t>(readU32(p));
    return static_cast<float>(raw) / 2147483648.f;
}

}  // namespace

// =============================================================================
// Sound
// =============================================================================

Result<Sound> Sound::fromWav(const WavData& data)
{
    if (data.channels == 0 || data.samples.empty()) {
        return makeUnexpected(mixerError(StatusCode::InvalidArgument,
                                         "WAV vazio/sem canais"));
    }
    Sound sound;
    sound.data_ = std::make_shared<WavData>(data);
    return sound;
}

std::uint32_t Sound::frames() const noexcept
{
    if (data_ == nullptr || data_->channels == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(data_->samples.size() /
                                      data_->channels);
}

// =============================================================================
// AudioMixer — construção/buses
// =============================================================================

AudioMixer::AudioMixer(std::uint32_t sampleRate, std::uint16_t channels)
    : sampleRate_(sampleRate > 0 ? sampleRate : 48000),
      channels_(channels > 0 ? channels : 2)
{
    buses_.push_back(AudioBus{"master", 1.f});
}

std::uint32_t AudioMixer::createBus(std::string_view name, float gain)
{
    std::lock_guard<std::mutex> lock(mutex_);
    buses_.push_back(AudioBus{std::string(name), gain});
    return static_cast<std::uint32_t>(buses_.size() - 1);
}

void AudioMixer::setBusGain(std::uint32_t busId, float gain)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (busId < buses_.size()) {
        buses_[busId].gain = gain;
    }
}

float AudioMixer::busGain(std::uint32_t busId) const noexcept
{
    if (busId < buses_.size()) {
        return buses_[busId].gain;
    }
    return 0.f;
}

AudioMixer::Voice* AudioMixer::findVoice(
    std::vector<std::unique_ptr<Voice>>& voices, VoiceHandle handle)
{
    for (auto& voice : voices) {
        if (voice->id == handle.value && voice->active) {
            return voice.get();
        }
    }
    return nullptr;
}

const AudioMixer::Voice* AudioMixer::findVoice(
    const std::vector<std::unique_ptr<Voice>>& voices, VoiceHandle handle)
{
    for (const auto& voice : voices) {
        if (voice->id == handle.value && voice->active) {
            return voice.get();
        }
    }
    return nullptr;
}

// =============================================================================
// Vozes (thread do jogo)
// =============================================================================

Result<VoiceHandle> AudioMixer::playSound(const Sound& sound,
                                         std::uint32_t busId, float volume,
                                         bool loop)
{
    if (sound.data() == nullptr) {
        return makeUnexpected(
            mixerError(StatusCode::InvalidArgument, "Sound vazio"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto voice = std::make_unique<Voice>();
    voice->id = nextVoiceId_++;
    voice->active = true;
    voice->loop = loop;
    voice->volume = volume;
    voice->busId = busId;
    voice->buffer = sound.data();
    voices_.push_back(std::move(voice));
    ++stats_.voicesPlayed;
    return VoiceHandle{voices_.back()->id};
}

Result<VoiceHandle> AudioMixer::playMusic(
    const std::shared_ptr<eng::fs::FileSystem>& fs, const eng::fs::Path& path,
    std::uint32_t busId, float volume, bool loop)
{
    if (fs == nullptr) {
        return makeUnexpected(
            mixerError(StatusCode::InvalidArgument, "fs obrigatório"));
    }
    // Leitura única (bytes) — o DECODE é progressivo (cursor abaixo).
    auto bytes = fs->readAllBytes(path);
    if (bytes.isError()) {
        return makeUnexpected(bytes.error());
    }
    const std::span<const std::byte> all = bytes.value();

    // Header mínimo + fmt + data (validação reaproveitada do Wav).
    auto parsed = Wav::parse(all);
    if (parsed.isError()) {
        return makeUnexpected(parsed.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto voice = std::make_unique<Voice>();
    voice->id = nextVoiceId_++;
    voice->active = true;
    voice->loop = loop;
    voice->volume = volume;
    voice->busId = busId;

    voice->streamBytes = std::move(bytes.value());
    voice->streamRate = parsed.value().sampleRate;
    voice->streamChannels = parsed.value().channels;
    voice->streamBits = 16; // (re-derivado abaixo junto ao offset data)
    voice->streamCursor = 0;
    voice->windowCursor = 0;
    voice->decodeWindow.clear();

    // Re-localiza fmt/data nos bytes BRUTOS (offsets para o cursor).
    std::size_t offset = 12;
    while (offset + 8 <= voice->streamBytes.size()) {
        const std::byte* chunkId = voice->streamBytes.data() + offset;
        const std::uint32_t chunkSize = readU32(voice->streamBytes.data() +
                                              offset + 4);
        const std::size_t body = offset + 8;
        if (body + chunkSize > voice->streamBytes.size()) {
            break;
        }
        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            voice->streamBits = readU16(voice->streamBytes.data() + body + 14);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            voice->streamDataOffset = body;
            voice->streamDataSize = chunkSize;
        }
        offset = body + chunkSize + (chunkSize & 1u);
    }

    voices_.push_back(std::move(voice));
    ++stats_.voicesPlayed;
    return VoiceHandle{voices_.back()->id};
}

void AudioMixer::stop(VoiceHandle handle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* voice = findVoice(voices_, handle)) {
        voice->active = false;
        voice->buffer.reset();
        voice->streamBytes.clear();
        voice->streamBytes.shrink_to_fit();
    }
}

void AudioMixer::pause(VoiceHandle handle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* voice = findVoice(voices_, handle)) {
        voice->paused = true;
    }
}

void AudioMixer::resume(VoiceHandle handle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* voice = findVoice(voices_, handle)) {
        voice->paused = false;
    }
}

void AudioMixer::setVolume(VoiceHandle handle, float volume)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* voice = findVoice(voices_, handle)) {
        voice->volume = std::clamp(volume, 0.f, 4.f);
    }
}

bool AudioMixer::isPlaying(VoiceHandle handle) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Voice* voice = findVoice(voices_, handle);
    return voice != nullptr && !voice->paused;
}

bool AudioMixer::isPaused(VoiceHandle handle) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Voice* voice = findVoice(voices_, handle);
    return voice != nullptr && voice->paused;
}

void AudioMixer::pauseAll() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& voice : voices_) {
        voice->paused = true;
    }
}

void AudioMixer::resumeAll() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& voice : voices_) {
        voice->paused = false;
    }
}

void AudioMixer::stopAll() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& voice : voices_) {
        voice->active = false;
        voice->buffer.reset();
        voice->streamBytes.clear();
        voice->streamBytes.shrink_to_fit();
    }
}

std::size_t AudioMixer::liveVoices() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t live = 0;
    for (const auto& voice : voices_) {
        if (voice->active) {
            ++live;
        }
    }
    return live;
}

// =============================================================================
// Streaming — janelas decodificadas sob demanda
// =============================================================================

void AudioMixer::decodeNextWindow(Voice& voice)
{
    // Chunk de bytes ainda não consumidos a partir do cursor.
    const std::size_t bytesPerSample = voice.streamBits / 8;
    const std::size_t remainingBytes =
        voice.streamDataSize > voice.streamCursor
            ? voice.streamDataSize - voice.streamCursor
            : 0;
    if (remainingBytes == 0) {
        voice.decodeWindow.clear();
        return;
    }
    const std::size_t maxWindowBytes =
        kStreamWindowFrames * voice.streamChannels * bytesPerSample;
    const std::size_t windowBytes = std::min(remainingBytes, maxWindowBytes);
    const std::byte* base =
        voice.streamBytes.data() + voice.streamDataOffset +
        voice.streamCursor;
    const bool isFloat = voice.streamBits == 32; // (format 3 presumido em 32f)

    voice.decodeWindow.resize(windowBytes / bytesPerSample);
    for (std::size_t i = 0; i < voice.decodeWindow.size(); ++i) {
        voice.decodeWindow[i] =
            streamSample(base + i * bytesPerSample, voice.streamBits,
                         isFloat);
    }
    voice.streamCursor += windowBytes;
    voice.windowCursor = 0;
}

// =============================================================================
// tick (jogo) + mix (áudio)
// =============================================================================

void AudioMixer::tick()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto dead = [](const std::unique_ptr<Voice>& voice) {
        return !voice->active;
    };
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), dead),
                  voices_.end());
}

void AudioMixer::mix(float* out, std::uint32_t frames)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t outChannels = channels_;
    std::size_t mixed = 0;

    for (auto& voice : voices_) {
        if (!voice->active || voice->paused) {
            continue;
        }
        const float gain = voice->volume * busGain(voice->busId);
        if (gain <= 0.f) {
            continue;
        }

        for (std::uint32_t frame = 0; frame < frames;) {
            if (voice->buffer != nullptr) {
                // --- Sound: buffer pronto --------------------------------
                const WavData& data = *voice->buffer;
                const std::uint32_t channels =
                    std::min<std::uint32_t>(data.channels, outChannels);
                const std::size_t totalFrames = data.samples.size() /
                                                data.channels;
                if (voice->cursorFrames >= totalFrames) {
                    if (voice->loop && totalFrames > 0) {
                        voice->cursorFrames = 0;
                    } else {
                        voice->active = false;
                        ++stats_.voicesFinished;
                        break;
                    }
                }
                const std::size_t take =
                    std::min<std::size_t>(frames - frame,
                                          totalFrames - voice->cursorFrames);
                for (std::size_t i = 0; i < take; ++i) {
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[(frame + i) * outChannels + c] +=
                            data.samples[(voice->cursorFrames + i) *
                                             data.channels +
                                         c] *
                            gain;
                    }
                    // Canal faltante: duplica o último (mono→estéreo).
                    for (std::uint32_t c = channels; c < outChannels; ++c) {
                        out[(frame + i) * outChannels + c] +=
                            data.samples[(voice->cursorFrames + i) *
                                             data.channels +
                                         channels - 1] *
                            gain;
                    }
                }
                voice->cursorFrames += take;
                frame += static_cast<std::uint32_t>(take);
                mixed += take;
            } else {
                // --- Music: janela decodificada sob demanda ----------------
                if (voice->windowCursor >= voice->decodeWindow.size()) {
                    if (voice->streamCursor >= voice->streamDataSize) {
                        if (voice->loop) {
                            voice->streamCursor = 0;
                            voice->windowCursor = 0;
                            voice->decodeWindow.clear();
                        } else {
                            voice->active = false;
                            ++stats_.voicesFinished;
                            break;
                        }
                    }
                    decodeNextWindow(*voice);
                    if (voice->decodeWindow.empty()) {
                        break;
                    }
                }
                const std::uint32_t channels = std::min<std::uint32_t>(
                    voice->streamChannels, outChannels);
                const std::size_t windowFrames =
                    voice->decodeWindow.size() / voice->streamChannels;
                const std::size_t take =
                    std::min<std::size_t>(frames - frame,
                                          windowFrames - voice->windowCursor);
                for (std::size_t i = 0; i < take; ++i) {
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[(frame + i) * outChannels + c] +=
                            voice->decodeWindow[(voice->windowCursor + i) *
                                                    voice->streamChannels +
                                                c] *
                            gain;
                    }
                    for (std::uint32_t c = channels; c < outChannels; ++c) {
                        out[(frame + i) * outChannels + c] +=
                            voice->decodeWindow[(voice->windowCursor + i) *
                                                    voice->streamChannels +
                                                channels - 1] *
                            gain;
                    }
                }
                voice->windowCursor += take;
                frame += static_cast<std::uint32_t>(take);
                mixed += take;
            }
        }
    }

    // Clamp final (sweetener barato; vozes raramente somam >1).
    for (std::uint32_t i = 0; i < frames * outChannels; ++i) {
        out[i] = std::clamp(out[i], -1.f, 1.f);
    }
    stats_.framesMixed += mixed;
}

// =============================================================================
// NullAudioBackend
// =============================================================================

eng::core::Result<void> NullAudioBackend::start(AudioMixer& /*mixer*/)
{
    ++startCount;
    running_ = true;
    return {};
}

void NullAudioBackend::stop()
{
    ++stopCount;
    running_ = false;
}

std::string NullAudioBackend::describeDevice() const
{
    return "null (sem device — testes/Linux)";
}

// =============================================================================
// Hook de progresso — armazenamento global do processo
// =============================================================================

namespace {
BackendProgressHook g_progressHook = nullptr;
void* g_progressUserdata = nullptr;
}  // namespace

void setBackendProgressHook(BackendProgressHook hook, void* userdata)
{
    g_progressHook = hook;
    g_progressUserdata = hook != nullptr ? userdata : nullptr;
}

void reportBackendStage(const char* stage, const char* status,
                        const char* detail) noexcept
{
    if (g_progressHook != nullptr && stage != nullptr) {
        g_progressHook(g_progressUserdata, stage,
                       status != nullptr ? status : "ok", detail);
    }
}

}  // namespace eng::audio
