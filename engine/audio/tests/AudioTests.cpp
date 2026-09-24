/// Testes de eng::audio: load (WAV), play/stop/
/// pause/resume/volume/loop, cleanup, streaming de Music, buses, pull
/// concorrente (stress thread-safe) e NullBackend.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "eng/audio/Audio.hpp"
#include "eng/audio/Wav.hpp"
#include "eng/fs/MemoryFileSystem.hpp"

namespace {

using namespace eng::audio;

/// Constrói um WAV PCM16 em bytes (header + N amostras interleaved).
std::vector<std::byte> makeWav16(std::uint32_t sampleRate,
                                 std::uint16_t channels,
                                 const std::vector<std::int16_t>& samples,
                                 bool withListChunk = false,
                                 std::uint16_t bitsOverride = 16)
{
    std::vector<std::byte> out;
    const auto push = [&out](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    const char riff[] = "RIFF";
    const char wave[] = "WAVE";
    push(riff, 4);
    // Corpo do LIST: "INFO" + filler de 10 bytes (14 total, par).
    const std::uint32_t listBodySize = withListChunk ? 14 : 0;
    const std::uint32_t dataSize =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riffSize =
        4 + (8 + 16) + (8 + listBodySize) + (8 + dataSize);
    push(&riffSize, 4);
    push(wave, 4);
    if (withListChunk) {
        const char list[] = "LIST";
        push(list, 4);
        push(&listBodySize, 4);
        const char info[] = "INFO";
        push(info, 4);
        const char filler[] = "IGTR ABC ";
        push(filler, 10);
    }
    const char fmt[] = "fmt ";
    push(fmt, 4);
    const std::uint32_t fmtSize = 16;
    push(&fmtSize, 4);
    const std::uint16_t format = 1;
    const std::uint16_t blockAlign = channels * 2;
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint16_t bits = bitsOverride;
    push(&format, 2);
    push(&channels, 2);
    push(&sampleRate, 4);
    push(&byteRate, 4);
    push(&blockAlign, 2);
    push(&bits, 2);
    const char data[] = "data";
    push(data, 4);
    push(&dataSize, 4);
    for (const std::int16_t sample : samples) {
        push(&sample, 2);
    }
    return out;
}

}  // namespace

// =============================================================================
// WAV
// =============================================================================

TEST_CASE("audio: WAV PCM16 parse com valores exatos", "[audio]")
{
    const std::vector<std::int16_t> samples{0, 16384, -16384, 32767, -32768};
    const auto bytes = makeWav16(44100, 1, samples);
    auto parsed = Wav::parse(bytes);
    REQUIRE(parsed.ok());
    CHECK(parsed.value().sampleRate == 44100);
    CHECK(parsed.value().channels == 1);
    REQUIRE(parsed.value().samples.size() == samples.size());
    CHECK_THAT(parsed.value().samples[1],
               Catch::Matchers::WithinAbs(16384.f / 32768.f, 1e-6f));
    CHECK_THAT(parsed.value().samples[2],
               Catch::Matchers::WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(parsed.value().samples[4],
               Catch::Matchers::WithinAbs(-1.f, 1e-6f));
}

TEST_CASE("audio: WAV com chunk LIST é pulSado; estéreo ok", "[audio]")
{
    const std::vector<std::int16_t> samples{100, -100, 200, -200};
    const auto bytes = makeWav16(48000, 2, samples, /*withListChunk=*/true);
    auto parsed = Wav::parse(bytes);
    REQUIRE(parsed.ok());
    CHECK(parsed.value().channels == 2);
    CHECK(parsed.value().samples.size() == 4);
}

TEST_CASE("audio: WAV rejeita lixo com erro preciso", "[audio]")
{
    CHECK(Wav::parse({}).isError());
    std::vector<std::byte> shortFile(20, std::byte{0});
    CHECK(Wav::parse(shortFile).isError());
    std::vector<std::byte> notRiff(100, std::byte{0});
    CHECK(Wav::parse(notRiff).isError());
}

// =============================================================================
// Sound/Voice/mixer (§6.11: play/stop/pause/resume/volume/loop/cleanup)
// =============================================================================

TEST_CASE("audio: play de Sound mixa com volume e loop", "[audio]")
{
    WavData data;
    data.sampleRate = 48000;
    data.channels = 1;
    data.samples = {0.5f, 0.5f, 0.5f, 0.5f}; // 4 frames
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 1);
    auto voice = mixer.playSound(sound.value(), 0, /*volume=*/0.5f,
                                 /*loop=*/false);
    REQUIRE(voice.ok());
    CHECK(mixer.isPlaying(voice.value()));

    // Mixa 4 frames → soma 0.5*0.5 por amostra.
    std::vector<float> out(8, 0.f);
    mixer.mix(out.data(), 4);
    CHECK_THAT(out[0], Catch::Matchers::WithinAbs(0.25f, 1e-5f));
    CHECK_THAT(out[3], Catch::Matchers::WithinAbs(0.25f, 1e-5f));

    // Sem loop: voz termina e é coletada no tick.
    std::vector<float> out2(4, 0.f);
    mixer.mix(out2.data(), 4);
    CHECK_FALSE(mixer.isPlaying(voice.value()));
    mixer.tick();
    CHECK(mixer.liveVoices() == 0);
    CHECK(mixer.stats().voicesFinished == 1);
}

TEST_CASE("audio: loop de Sound reinicia o buffer", "[audio]")
{
    WavData data;
    data.channels = 1;
    data.samples = {1.f, -1.f};
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 1);
    auto voice = mixer.playSound(sound.value(), 0, 1.f, /*loop=*/true);
    REQUIRE(voice.ok());
    std::vector<float> out(6, 0.f);
    mixer.mix(out.data(), 6);
    // Padrão 1,-1,1,-1,1,-1 se o loop reiniciou.
    CHECK_THAT(out[4], Catch::Matchers::WithinAbs(1.f, 1e-5f));
    CHECK_THAT(out[5], Catch::Matchers::WithinAbs(-1.f, 1e-5f));
    CHECK(mixer.isPlaying(voice.value()));
}

TEST_CASE("audio: stop/pause/resume e handle obsoleto é no-op", "[audio]")
{
    WavData data;
    data.channels = 1;
    data.samples = std::vector<float>(100, 0.5f);
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 1);
    auto voice = mixer.playSound(sound.value());
    REQUIRE(voice.ok());

    mixer.pause(voice.value());
    CHECK(mixer.isPaused(voice.value()));
    CHECK_FALSE(mixer.isPlaying(voice.value()));
    std::vector<float> out(8, 0.f);
    mixer.mix(out.data(), 8); // pausado: NÃO mixa
    CHECK_THAT(out[0], Catch::Matchers::WithinAbs(0.f, 1e-6f));

    mixer.resume(voice.value());
    mixer.mix(out.data(), 8);
    CHECK_THAT(out[0], Catch::Matchers::WithinAbs(0.5f, 1e-5f));

    mixer.stop(voice.value());
    CHECK_FALSE(mixer.isPlaying(voice.value()));

    // Handle obsoleto: todas as ops são no-op seguras.
    mixer.stop(voice.value());
    mixer.pause(voice.value());
    mixer.setVolume(voice.value(), 2.f);
    CHECK_FALSE(mixer.isPlaying(voice.value()));
}

TEST_CASE("audio: volume por voz e ganho por bus multiplicam", "[audio]")
{
    WavData data;
    data.channels = 1;
    data.samples = {1.f, 1.f}; // 2 frames — 1 antes, 1 depois da troca
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 1);
    const auto sfx = mixer.createBus("sfx", 0.25f);
    auto voice = mixer.playSound(sound.value(), sfx, /*volume=*/0.5f);
    REQUIRE(voice.ok());
    std::vector<float> out(1, 0.f);
    mixer.mix(out.data(), 1);
    CHECK_THAT(out[0], Catch::Matchers::WithinAbs(0.125f, 1e-5f)); // .5*.25

    // Bus muda em voo.
    mixer.setBusGain(sfx, 1.f);
    std::vector<float> out2(1, 0.f);
    mixer.mix(out2.data(), 1);
    CHECK_THAT(out2[0], Catch::Matchers::WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("audio: mono é duplicado para estéreo", "[audio]")
{
    WavData data;
    data.channels = 1;
    data.samples = {0.5f};
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 2); // SAÍDA estéreo
    auto voice = mixer.playSound(sound.value());
    REQUIRE(voice.ok());
    std::vector<float> out(2, 0.f);
    mixer.mix(out.data(), 1);
    CHECK_THAT(out[0], Catch::Matchers::WithinAbs(0.5f, 1e-5f));
    CHECK_THAT(out[1], Catch::Matchers::WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("audio: pauseAll/resumeAll/stopAll (lifecycle §6.10)", "[audio]")
{
    WavData data;
    data.channels = 1;
    data.samples = std::vector<float>(50, 0.5f);
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 1);
    auto a = mixer.playSound(sound.value());
    auto b = mixer.playSound(sound.value());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(mixer.liveVoices() == 2);

    mixer.pauseAll();
    CHECK(mixer.isPaused(a.value()));
    CHECK(mixer.isPaused(b.value()));
    mixer.resumeAll();
    CHECK(mixer.isPlaying(a.value()));
    mixer.stopAll();
    CHECK(mixer.liveVoices() == 0);
}

// =============================================================================
// Music (streaming real — §6.10)
// =============================================================================

TEST_CASE("audio: música toca por janelas (streaming) e termina", "[audio]")
{
    // 1 segundo de "áudio" PCM16 mono, 8 kHz → 8000 amostras.
    std::vector<std::int16_t> samples(8000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = 16384;
    }
    auto fs = std::make_shared<eng::fs::MemoryFileSystem>();
    REQUIRE(fs->mkdirs(eng::fs::Path{"music"}).ok());
    REQUIRE(fs->writeAllBytes(eng::fs::Path{"music/x.wav"},
                              makeWav16(8000, 1, samples))
                .ok());

    AudioMixer mixer(8000, 1);
    auto voice = mixer.playMusic(fs, eng::fs::Path{"music/x.wav"});
    REQUIRE(voice.ok());
    CHECK(mixer.isPlaying(voice.value()));

    // Consome 7900 dos 8000 frames (ainda tocando)...
    std::vector<float> out(7900, 0.f);
    mixer.mix(out.data(), 7900);
    CHECK(mixer.isPlaying(voice.value()));
    // ...e termina no pull seguinte (janela cruzou o fim do arquivo).
    std::vector<float> out2(200, 0.f);
    mixer.mix(out2.data(), 200);
    CHECK_FALSE(mixer.isPlaying(voice.value())); // terminou
    // Conteúdo correto: 16384/32768 = 0.5.
    CHECK_THAT(out[100], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
    CHECK_THAT(out2[10], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("audio: música em loop recarrega o início", "[audio]")
{
    std::vector<std::int16_t> samples{20000, -20000};
    auto fs = std::make_shared<eng::fs::MemoryFileSystem>();
    REQUIRE(fs->writeAllBytes(eng::fs::Path{"loop.wav"},
                              makeWav16(8000, 1, samples))
                .ok());

    AudioMixer mixer(8000, 1);
    auto voice = mixer.playMusic(fs, eng::fs::Path{"loop.wav"}, 0, 1.f,
                                 /*loop=*/true);
    REQUIRE(voice.ok());
    std::vector<float> out(5, 0.f);
    mixer.mix(out.data(), 5); // 2 + 2 + 1 → reiniciou
    CHECK_THAT(out[4], Catch::Matchers::WithinAbs(20000.f / 32768.f, 1e-4f));
    CHECK(mixer.isPlaying(voice.value()));
}

// =============================================================================
// Stress concorrente (pull × play/stop — §D7)
// =============================================================================

TEST_CASE("audio: pull concorrente com play/stop (thread-safety)", "[audio]")
{
    WavData data;
    data.channels = 1;
    data.samples = std::vector<float>(256, 0.25f);
    auto sound = Sound::fromWav(data);
    REQUIRE(sound.ok());

    AudioMixer mixer(48000, 2);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> mixedFrames{0};

    // "Thread de áudio": puxa mix continuamente.
    std::thread audioThread([&] {
        std::vector<float> buffer(512, 0.f);
        while (!stop.load()) {
            std::fill(buffer.begin(), buffer.end(), 0.f);
            mixer.mix(buffer.data(), 256);
            mixedFrames += 256;
        }
    });

    // Garante que a thread de áudio puxou AO MENOS UMA janela antes da
    // rajada (scheduling: em -O3 a rajada termina antes do primeiro pull).
    for (int spin = 0; spin < 2000 && mixedFrames.load() == 0; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(mixedFrames.load() > 0);

    // "Thread do jogo": cria/pausa/para vozes em rajada.
    for (int i = 0; i < 400; ++i) {
        auto voice = mixer.playSound(sound.value());
        if (voice.ok()) {
            mixer.pause(voice.value());
            mixer.resume(voice.value());
            if (i % 2 == 0) {
                mixer.stop(voice.value());
            }
        }
        mixer.tick();
    }
    stop.store(true);
    audioThread.join();

    CHECK(mixedFrames.load() > 0);
    // Nenhum crash/UB — o ASan/TSan do preset valida a corrida.
}

// =============================================================================
// NullBackend
// =============================================================================

TEST_CASE("audio: NullBackend conta start/stop", "[audio]")
{
    AudioMixer mixer;
    auto backend = eng::audio::createDefaultBackend();
    REQUIRE(backend != nullptr);
    CHECK(backend->name() == std::string_view{"null"});
    auto started = backend->start(mixer);
    REQUIRE(started.ok());
    CHECK(backend->isRunning());
    backend->stop();
    CHECK_FALSE(backend->isRunning());
}

// =============================================================================
// Correção da auditoria final FASES 4–10 (remediação C-10)
// =============================================================================

TEST_CASE("audio: bits não suportados produzem mensagem válida (C-10)", "[audio]")
{
    // Antes: `bits + " literal"` fazia aritmética de ponteiro — mensagem
    // garbage e leitura OOB p/ bits >= 20 (UB capturável pelo ASan).
    const std::vector<std::int16_t> samples{0, 16384, -16384};
    auto parsed = Wav::parse(makeWav16(44100, 1, samples, false,
                                       /*bitsOverride=*/64));
    REQUIRE(parsed.isError());
    CHECK(parsed.error().message.find("64 bits") != std::string::npos);
    CHECK(parsed.error().message.find("não suportado") != std::string::npos);

    auto parsed12 = Wav::parse(makeWav16(44100, 1, samples, false,
                                         /*bitsOverride=*/12));
    REQUIRE(parsed12.isError());
    CHECK(parsed12.error().message.find("12 bits") != std::string::npos);
}

// =============================================================================
// Camada de adaptação mixer→device (Realme C33 SIGSEGV)
// =============================================================================

#include "eng/audio/AudioAdapt.hpp"

#include <atomic>
#include <string>

TEST_CASE("audio P3.4: convertFrames mapeia canais e formatos", "[audio]")
{
    using F = DeviceSampleFormat;

    SECTION("mesmos canais float→float é cópia direta")
    {
        const std::vector<float> src{0.25f, -0.5f, 0.75f, 1.f};
        float dst[4] = {};
        convertFrames(src.data(), dst, 2, 2, 2, F::Float);
        CHECK(dst[0] == 0.25f);
        CHECK(dst[1] == -0.5f);
        CHECK(dst[2] == 0.75f);
        CHECK(dst[3] == 1.f);
    }

    SECTION("downmix 2→1 soma e clampa (i16)")
    {
        const std::vector<float> src{0.5f, 0.25f, -0.9f, -0.9f};
        std::int16_t dst[2] = {};
        convertFrames(src.data(), dst, 2, 2, 1, F::Int16);
        // 0.5+0.25=0.75 → 0.75*32767
        CHECK(dst[0] == 24575);
        // -0.9-0.9=-1.8 → clamp -1 → -32768
        CHECK(dst[1] == -32768);
    }

    SECTION("upmix 1→2 duplica o canal (i16)")
    {
        const std::vector<float> src{0.5f, -0.25f};
        std::int16_t dst[4] = {};
        convertFrames(src.data(), dst, 2, 1, 2, F::Int16);
        CHECK(dst[0] == 16383);
        CHECK(dst[1] == 16383);
        CHECK(dst[2] == -8191);
        CHECK(dst[3] == -8191);
    }

    SECTION("downmix 6→2: primeiros passam, último soma o resto")
    {
        std::vector<float> src(6);
        src[0] = 0.25f;   // → dst L
        src[1] = 0.1f;    // → soma no dst R
        src[2] = 0.2f;    // soma
        src[3] = 0.3f;    // soma
        src[4] = 0.15f;   // soma
        src[5] = 0.25f;   // soma (total R = 1.0)
        float dst[2] = {};
        convertFrames(src.data(), dst, 1, 6, 2, F::Float);
        CHECK(dst[0] == 0.25f);
        CHECK(dst[1] == 1.f);  // 0.1+0.2+0.3+0.15+0.25
    }

    SECTION("float→i32 escala com clamp nos extremos")
    {
        const std::vector<float> src{1.f, -1.f, 1.5f, -1.5f};
        std::int32_t dst[4] = {};
        convertFrames(src.data(), dst, 4, 1, 1, F::Int32);
        CHECK(dst[0] == 2147483647);
        CHECK(dst[1] == -2147483647 - 1);
        CHECK(dst[2] == 2147483647);   // clamp +∞ do range
        CHECK(dst[3] == -2147483647 - 1);
    }

    SECTION("fonte nula escreve silêncio (nunca lixo no device)")
    {
        std::int16_t dst[4] = {7, 7, 7, 7};
        convertFrames(nullptr, dst, 2, 2, 2, F::Int16);
        for (std::int16_t v : dst) {
            CHECK(v == 0);
        }
    }
}

TEST_CASE("audio P3.4: convertFrames clamp i16 nos limites", "[audio]")
{
    using F = DeviceSampleFormat;
    const std::vector<float> src{1.f, -1.f, 0.9999695f, -0.9999695f};
    std::int16_t dst[4] = {};
    convertFrames(src.data(), dst, 4, 1, 1, F::Int16);
    CHECK(dst[0] == 32767);
    CHECK(dst[1] == -32768);
    CHECK(dst[2] == 32766);
    CHECK(dst[3] == -32766);  // trunc p/ zero: -32766.0006 → -32766
}

// -----------------------------------------------------------------------------
// LinearResampler — fase exata
// -----------------------------------------------------------------------------

/// Roda o resampler em "callbacks" de batchSize saídas sobre uma rampa
/// unitária e devolve TODAS as saídas produzidas.
/// A rampa tem valor n na posição n — a interpolação linear exata de
/// uma rampa é a PRÓPRIA posição: saída k == k*num/den (exato).
static std::vector<double> resampleRamp(std::uint32_t inRate,
                                        std::uint32_t outRate,
                                        std::uint32_t batches,
                                        std::uint32_t batchSize,
                                        std::uint64_t* pushedTotal)
{
    LinearResampler r;
    if (!r.configure(inRate, outRate, 1, batchSize)) {
        return {};
    }
    std::vector<double> out;
    std::vector<float> stage;
    std::vector<float> dst(batchSize);
    std::uint64_t nextInput = 0;
    for (std::uint32_t b = 0; b < batches; ++b) {
        const std::uint32_t push = r.framesToPush(batchSize);
        if (push > 0) {
            stage.assign(push, 0.f);
            for (std::uint32_t i = 0; i < push; ++i) {
                stage[i] = static_cast<float>(nextInput + i);
            }
            r.push(stage.data(), push);
            nextInput += push;
        }
        r.produce(batchSize, dst.data());
        for (std::uint32_t i = 0; i < batchSize; ++i) {
            out.push_back(dst[i]);
        }
    }
    if (pushedTotal != nullptr) {
        *pushedTotal = nextInput;
    }
    return out;
}

TEST_CASE("audio P3.4: LinearResampler recusa config inválida", "[audio]")
{
    LinearResampler r;
    CHECK_FALSE(r.configure(0, 48000, 2, 256));
    CHECK_FALSE(r.configure(48000, 0, 2, 256));
    CHECK_FALSE(r.configure(48000, 48000, 0, 256));
    CHECK_FALSE(r.configure(48000, 48000, 2, 0));
    // razão 16 fora de [1/8, 8]
    CHECK_FALSE(r.configure(8000, 128000, 2, 256));
    CHECK(r.configure(48000, 48000, 2, 256));
    CHECK(r.isPassthrough());
    CHECK(r.configure(44100, 48000, 2, 256));
    CHECK_FALSE(r.isPassthrough());
}

TEST_CASE("audio P3.4: LinearResampler upsample 2x é exato na rampa",
          "[audio]")
{
    // in=48000 → out=96000: saída k na posição k/2 → valor k/2.
    const auto out = resampleRamp(48000, 96000, 8, 96, nullptr);
    REQUIRE(out.size() == 8 * 96);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const double expected = static_cast<double>(k) / 2.0;
        CHECK(out[k] == Catch::Approx(expected).margin(1e-3));
    }
}

TEST_CASE("audio P3.4: LinearResampler downsample 2x é exato na rampa",
          "[audio]")
{
    // in=96000 → out=48000: saída k na posição 2k → valor 2k.
    const auto out = resampleRamp(96000, 48000, 8, 96, nullptr);
    REQUIRE(out.size() == 8 * 96);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const double expected = static_cast<double>(2 * k);
        CHECK(out[k] == Catch::Approx(expected).margin(1e-2));
    }
}

TEST_CASE("audio P3.4: LinearResampler 48000→44100 exato com seams",
          "[audio]")
{
    // O caso real do C33 se o HAL abrir 44100 com o mixer a 48000
    // (in=mixer=48000, out=stream=44100 — razão irredutível 160/147).
    // Lotes NÃO alinhados forçam seams do histórico interno. Saída k
    // fica na posição k×160/147 da rampa → valor k×160/147 exatamente.
    const auto out = resampleRamp(48000, 44100, 12, 128, nullptr);
    REQUIRE(out.size() == 12 * 128);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const double expected =
            static_cast<double>(k) * 160.0 / 147.0;
        CHECK(out[k] == Catch::Approx(expected).margin(1e-3));
    }
}

TEST_CASE("audio P3.4: LinearResampler razão densa (1/6) exata em lotes "
          "de 1 — regressão do seam", "[audio]")
{
    // Regressão do bug de design encontrado no desenvolvimento do
    // Razões densas com lotes mínimos precisavam de lookback
    // mais fundo que um único frame de tail — o modelo push/pull com
    // histórico interno corrige. Saída k == k/6 exatamente.
    const auto out = resampleRamp(48000, 288000, 40, 1, nullptr);
    REQUIRE(out.size() == 40);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const double expected = static_cast<double>(k) / 6.0;
        CHECK(out[k] == Catch::Approx(expected).margin(1e-3));
    }
    // O mesmo com lotes não alinhados ao período 6.
    const auto out13 = resampleRamp(48000, 288000, 7, 13, nullptr);
    REQUIRE(out13.size() == 7 * 13);
    for (std::size_t k = 0; k < out13.size(); ++k) {
        const double expected = static_cast<double>(k) / 6.0;
        CHECK(out13[k] == Catch::Approx(expected).margin(1e-3));
    }
}

TEST_CASE("audio P3.4: LinearResampler contabilidade push/produce",
          "[audio]")
{
    // Padrão do callback real: por lote, framesToPush() → mix → push →
    // produce. Taxas divergentes (160/147) com 2 canais.
    LinearResampler r;
    REQUIRE(r.configure(48000, 44100, 2, 128));
    std::vector<float> stage;
    std::vector<float> dst(128 * 2);
    constexpr std::uint32_t kRounds = 5;
    for (std::uint32_t round = 0; round < kRounds; ++round) {
        const std::uint32_t push = r.framesToPush(128);
        if (push > 0) {
            stage.assign(static_cast<std::size_t>(push) * 2, 0.f);
            for (std::uint32_t i = 0; i < push; ++i) {
                stage[i * 2] = 0.5f;    // DC constante (L)
                stage[i * 2 + 1] = -0.5f;  // DC constante (R)
            }
            r.push(stage.data(), push);
        }
        r.produce(128, dst.data());
        // DC atravessa o lerp SEM mudar de valor, em qualquer fase.
        for (std::uint32_t i = 0; i < 128; ++i) {
            CHECK(dst[i * 2] == Catch::Approx(0.5f).margin(1e-6));
            CHECK(dst[i * 2 + 1] == Catch::Approx(-0.5f).margin(1e-6));
        }
    }
    CHECK(r.produced() == kRounds * 128);
    // Conservação aproximada de frames: produziu 640 saídas ≈ 640×160/147
    // frames de entrada (+ o frame de look-ahead do último lote).
    const std::uint64_t expectedFrames =
        (static_cast<std::uint64_t>(kRounds) * 128) * 160 / 147;
    CHECK(r.pushed() >= expectedFrames);
    CHECK(r.pushed() <= expectedFrames + 2);
    // Defensivo: produzir sem push novo suficiente segura a última
    // amostra — nunca lê fora, nunca NaN.
    const std::uint32_t push = r.framesToPush(128);
    if (push > 0) {
        stage.assign(static_cast<std::size_t>(push) * 2, 0.25f);
        r.push(stage.data(), push);
    }
    r.produce(128, dst.data());
    for (std::uint32_t i = 0; i < 128; ++i) {
        CHECK(std::isfinite(dst[i * 2]));
        CHECK(std::isfinite(dst[i * 2 + 1]));
    }
}

// -----------------------------------------------------------------------------
// CallbackGate — protocolo de drenagem (causa 3)
// -----------------------------------------------------------------------------

TEST_CASE("audio P3.4: CallbackGate recusa entrada após close", "[audio]")
{
    CallbackGate gate;
    gate.reopen();
    CHECK(gate.tryEnter());
    gate.exit();
    gate.close();
    CHECK(gate.isClosed());
    CHECK_FALSE(gate.tryEnter());
    gate.reopen();
    CHECK(gate.tryEnter());
    gate.exit();
}

TEST_CASE("audio P3.4: CallbackGate drena imediatamente sem entrantes",
          "[audio]")
{
    CallbackGate gate;
    gate.reopen();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(gate.waitDrained(50));
    CHECK(std::chrono::steady_clock::now() - t0 <
          std::chrono::milliseconds{20});
}

TEST_CASE("audio P3.4: CallbackGate timeout com entrante preso", "[audio]")
{
    CallbackGate gate;
    gate.reopen();
    REQUIRE(gate.tryEnter());  // "callback" em andamento: nunca sai
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(gate.waitDrained(50));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed >= std::chrono::milliseconds{45});
    CHECK(elapsed < std::chrono::milliseconds{2000});
    gate.exit();  // libera
    CHECK(gate.waitDrained(50));
}

TEST_CASE("audio P3.4: CallbackGate drena quando o callback sai",
          "[audio]")
{
    CallbackGate gate;
    gate.reopen();
    std::atomic<bool> inCallback{false};
    std::atomic<bool> entered{false};
    std::thread callback([&] {
        if (gate.tryEnter()) {
            entered.store(true);
            inCallback.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            inCallback.store(false);
            gate.exit();
        }
    });
    while (!entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    gate.close();
    CHECK(gate.waitDrained(500));
    CHECK_FALSE(inCallback.load());
    callback.join();
    // Depois de drenado e fechado, nenhuma nova entrada.
    CHECK_FALSE(gate.tryEnter());
}

// -----------------------------------------------------------------------------
// Estágios P3.4 + hook de progresso
// -----------------------------------------------------------------------------

TEST_CASE("audio P3.4: nomes dos estágios do backend são o contrato do "
          "diagnóstico", "[audio]")
{
    // Essas strings são lidas do goni_startup.log exportado no
    // Download/GONI do Realme C33 — um typo aqui destrói o protocolo
    // de diagnóstico da missão.
    CHECK(std::string_view{backend_stage::Dlopen} == "AUDIO_DLOPEN");
    CHECK(std::string_view{backend_stage::Symbols} == "AUDIO_SYMBOLS");
    CHECK(std::string_view{backend_stage::BuilderCreate} ==
          "AUDIO_BUILDER_CREATE");
    CHECK(std::string_view{backend_stage::BuilderConfig} ==
          "AUDIO_BUILDER_CONFIG");
    CHECK(std::string_view{backend_stage::StreamOpen} ==
          "AUDIO_STREAM_OPEN");
    CHECK(std::string_view{backend_stage::StreamParamsVerify} ==
          "AUDIO_STREAM_PARAMS_VERIFY");
    CHECK(std::string_view{backend_stage::StreamStart} ==
          "AUDIO_STREAM_START");
    CHECK(std::string_view{backend_stage::CallbackFirstFrame} ==
          "AUDIO_CALLBACK_FIRST_FRAME");
}

TEST_CASE("audio P3.4: hook de progresso recebe estágios 1:1", "[audio]")
{
    struct Record {
        std::string stage;
        std::string status;
        std::string detail;
    };
    std::vector<Record> records;
    const auto hook = [](void* userdata, const char* stage,
                          const char* status, const char* detail) {
        auto* out = static_cast<std::vector<Record>*>(userdata);
        out->push_back(Record{stage, status != nullptr ? status : "",
                              detail != nullptr ? detail : ""});
    };
    setBackendProgressHook(hook, &records);
    reportBackendStage(backend_stage::StreamOpen, "begin", "binder");
    reportBackendStage(backend_stage::StreamOpen, "ok", nullptr);
    reportBackendStage(backend_stage::StreamOpen, nullptr, nullptr);
    setBackendProgressHook(nullptr, nullptr);
    reportBackendStage(backend_stage::StreamStart, "ok", "pós-limpeza");

    REQUIRE(records.size() == 3);
    CHECK(records[0].stage == "AUDIO_STREAM_OPEN");
    CHECK(records[0].status == "begin");
    CHECK(records[0].detail == "binder");
    CHECK(records[1].status == "ok");
    CHECK(records[1].detail.empty());
    CHECK(records[2].status == "ok");  // default
}

TEST_CASE("audio P3.4: NullBackend não reporta primeiro callback",
          "[audio]")
{
    // O observador do host (marco AUDIO_CALLBACK_FIRST_FRAME) nunca
    // dispara para o backend null — sem device não há pull real.
    AudioMixer mixer;
    auto backend = eng::audio::createDefaultBackend();
    REQUIRE(backend != nullptr);
    CHECK(backend->name() == std::string_view{"null"});
    REQUIRE(backend->start(mixer).ok());
    CHECK(backend->isRunning());
    CHECK_FALSE(backend->hasFirstCallbackFired());
    backend->stop();
    CHECK_FALSE(backend->isRunning());
}
