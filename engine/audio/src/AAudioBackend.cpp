#include "eng/audio/Audio.hpp"
#include "eng/audio/AudioAdapt.hpp"

/// AAudioBackend — saída REAL no Android.
///
/// Este backend é a
/// frente de batalha da missão. A janela de morte foi pinada pelo
/// diagnóstico P3.3 DENTRO de start() — entre o marco "STARTUP_AUDIO
/// backend AAudio" e "started/failed". As correções (ver
/// docs/p34-aaudio-fix.md):
///
/// - CAUSA 1 (parâmetros divergem): os valores do builder são
///   SUGESTÕES. O P3.3 RECUSAVA divergências (áudio morto); o P3.4
///   ADAPTA: o callback mistura no layout do MIXER e a camada
///   AudioAdapt converte para o layout EFETIVO do stream (canais,
///   formato float/i16/i32 e TAXA com reamostragem exata).
/// - CAUSA 2 (requestStart em stream inválido): estado verificado com
///   AAudioStream_getState — só parte de AAUDIO_STREAM_STATE_OPEN;
///   null-stream pós-openStream é recusado.
/// - CAUSA 3 (callback após destruição): sem global — o callback usa
///   userData=this + CallbackGate (flag + drenagem limitada); stop()
///   pede parada, espera o estado assentar, DRENA o callback e só
///   então fecha o stream e solta o mixer.
/// - CAUSA 4 (race onResume×surfaceCreated): ambos rodam na UI
///   thread do Android (looper único) — auditoria documentada no
///   relatório P3.4; a única concorrência real (thread de áudio) é
///   coberta pelo gate + átomos + mutex do mixer.
///
/// Marcos granulares (backend_stage::*) emitem-se via hook instalado
/// pelo host ANTES de createDefaultBackend() — a engine de áudio não
/// depende do editor. Sequência canônica:
///
///   AUDIO_DLOPEN → AUDIO_SYMBOLS → AUDIO_BUILDER_CREATE →
///   AUDIO_BUILDER_CONFIG → AUDIO_STREAM_OPEN →
///   AUDIO_STREAM_PARAMS_VERIFY → AUDIO_STREAM_START →
///   [AUDIO_CALLBACK_FIRST_FRAME — observado pelo host]
///
/// TODO estágio que FALHA emite "failed" com detalhe preciso e o app
/// segue VIVO sem device de áudio (fallback NullBackend do host — o
/// editor continua 100% funcional, previews/Play seguem sem som).
///
/// Padrão dos backends gráficos: dlopen("libaaudio.so")
/// em runtime + dlsym — SEM link edit e SEM include de headers Android
/// no engine (o include existe apenas sob __ANDROID__ neste TU).
/// Dispositivo < API 26: libaaudio.so não existe → start() falha com
/// erro preciso. O handle NÃO é dlclose'd (bibliotecas de sistema
/// retêm estado/threads — ver ADR-037).

#ifdef __ANDROID__

#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <errno.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "eng/log/Macros.hpp"

namespace eng::audio {

namespace {

ENG_LOG_CATEGORY("audio.backend");

// Funções resolvidas por dlsym (assinaturas do header AAudio).
using CreateStreamBuilderFn = AAudioStreamBuilder* (*)();
using BuilderSetIntFn = void (*)(AAudioStreamBuilder*, int32_t);
using BuilderSetFormatFn = void (*)(AAudioStreamBuilder*, aaudio_format_t);
using BuilderSetCallbackFn = void (*)(AAudioStreamBuilder*,
                                      AAudioStream_dataCallback, void*);
using OpenStreamFn = aaudio_result_t (*)(AAudioStreamBuilder*,
                                         AAudioStream**);
using StreamControlFn = aaudio_result_t (*)(AAudioStream*);
using StreamCloseFn = aaudio_result_t (*)(AAudioStream*);
using BuilderDeleteFn = void (*)(AAudioStreamBuilder*);
using StreamGetIntFn = int32_t (*)(AAudioStream*);
using StreamGetFormatFn = aaudio_format_t (*)(AAudioStream*);
using StreamGetStateFn = aaudio_stream_state_t (*)(AAudioStream*);
using WaitForStateChangeFn = aaudio_result_t (*)(AAudioStream*,
                                                 aaudio_stream_state_t,
                                                 aaudio_stream_state_t*,
                                                 int64_t);
using ResultTextFn = const char* (*)(aaudio_result_t);
using BuilderSetPerformanceModeFn = void (*)(AAudioStreamBuilder*,
                                              aaudio_performance_mode_t);

struct AAudioApi {
    void* library{nullptr};
    CreateStreamBuilderFn createStreamBuilder{nullptr};
    BuilderSetIntFn setSampleRate{nullptr};
    BuilderSetIntFn setChannelCount{nullptr};
    BuilderSetFormatFn setFormat{nullptr};
    BuilderSetCallbackFn setDataCallback{nullptr};
    OpenStreamFn openStream{nullptr};
    StreamControlFn requestStart{nullptr};
    StreamControlFn requestStop{nullptr};
    StreamCloseFn closeStream{nullptr};
    BuilderDeleteFn deleteBuilder{nullptr};
    // Leitura dos parâmetros EFETIVOS do stream aberto.
    StreamGetIntFn getChannelCount{nullptr};
    StreamGetIntFn getSampleRate{nullptr};
    StreamGetFormatFn getFormat{nullptr};
    // Estado do stream (causa 2), parada determinística (causa
    // 3) e dimensionamento do scratch do callback.
    StreamGetStateFn getState{nullptr};
    WaitForStateChangeFn waitForStateChange{nullptr};
    StreamGetIntFn getBufferCapacityInFrames{nullptr};
    // Texto legível do código de erro nos marcos "failed".
    ResultTextFn resultText{nullptr};
    // SEM MMAP: PERFORMANCE_MODE_NONE força o caminho Legacy
    // (AudioTrack do framework) — o MMAP do AAudio só é suportado em
    // devices selecionados (Pixel, S10, Mate20…); em HALs budget como o
    // Unisoc T612 o path MMAP falha ou crasha. Padrão da indústria (Oboe).
    BuilderSetPerformanceModeFn setPerformanceMode{nullptr};
};

/// Resolve a tabela por dlsym. `missing` recebe o nome do símbolo
/// ausente (nullptr quando foi o dlopen que falhou).
[[nodiscard]] bool loadAAudioApi(AAudioApi& api, const char** missing)
{
    *missing = nullptr;
    api.library = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
    if (api.library == nullptr) {
        return false;
    }
    struct SymbolSlot {
        const char* name;
        void** slot;
    };
    const SymbolSlot slots[] = {
        {"AAudio_createStreamBuilder",
         reinterpret_cast<void**>(&api.createStreamBuilder)},
        {"AAudioStreamBuilder_setSampleRate",
         reinterpret_cast<void**>(&api.setSampleRate)},
        {"AAudioStreamBuilder_setChannelCount",
         reinterpret_cast<void**>(&api.setChannelCount)},
        {"AAudioStreamBuilder_setFormat",
         reinterpret_cast<void**>(&api.setFormat)},
        {"AAudioStreamBuilder_setDataCallback",
         reinterpret_cast<void**>(&api.setDataCallback)},
        {"AAudioStreamBuilder_openStream",
         reinterpret_cast<void**>(&api.openStream)},
        {"AAudioStream_requestStart",
         reinterpret_cast<void**>(&api.requestStart)},
        {"AAudioStream_requestStop",
         reinterpret_cast<void**>(&api.requestStop)},
        {"AAudioStream_close",
         reinterpret_cast<void**>(&api.closeStream)},
        {"AAudioStreamBuilder_delete",
         reinterpret_cast<void**>(&api.deleteBuilder)},
        {"AAudioStream_getChannelCount",
         reinterpret_cast<void**>(&api.getChannelCount)},
        {"AAudioStream_getSampleRate",
         reinterpret_cast<void**>(&api.getSampleRate)},
        {"AAudioStream_getFormat",
         reinterpret_cast<void**>(&api.getFormat)},
        {"AAudioStream_getState", reinterpret_cast<void**>(&api.getState)},
        {"AAudioStream_waitForStateChange",
         reinterpret_cast<void**>(&api.waitForStateChange)},
        {"AAudioStream_getBufferCapacityInFrames",
         reinterpret_cast<void**>(&api.getBufferCapacityInFrames)},
        {"AAudio_convertResultToText",
         reinterpret_cast<void**>(&api.resultText)},
        {"AAudioStreamBuilder_setPerformanceMode",
         reinterpret_cast<void**>(&api.setPerformanceMode)},
    };
    for (const SymbolSlot& s : slots) {
        *s.slot = dlsym(api.library, s.name);
        if (*s.slot == nullptr) {
            *missing = s.name;
            return false;
        }
    }
    return true;
}

/// Formata "código N (TEXTO)" para os detalhes dos marcos.
void formatResult(char* buf, std::size_t cap, aaudio_result_t code,
                  const AAudioApi& api)
{
    const char* text = api.resultText != nullptr ? api.resultText(code)
                                                 : nullptr;
    if (text == nullptr) {
        text = "?";
    }
    std::snprintf(buf, cap, "código %d (%s)", static_cast<int>(code), text);
}

[[nodiscard]] const char* formatName(DeviceSampleFormat format) noexcept
{
    switch (format) {
    case DeviceSampleFormat::Int16: return "i16";
    case DeviceSampleFormat::Float: return "float";
    case DeviceSampleFormat::Int32: return "i32";
    }
    return "?";
}

/// Converte aaudio_format_t → formato do conversor. false = sem
/// conversor (o stream é recusado com erro preciso).
[[nodiscard]] bool formatToConvert(aaudio_format_t format,
                                   DeviceSampleFormat& out) noexcept
{
    switch (format) {
    case AAUDIO_FORMAT_PCM_I16: out = DeviceSampleFormat::Int16; return true;
    case AAUDIO_FORMAT_PCM_FLOAT: out = DeviceSampleFormat::Float; return true;
    case AAUDIO_FORMAT_PCM_I32: out = DeviceSampleFormat::Int32; return true;
    default: return false;
    }
}

// =============================================================================
// Backend
// =============================================================================

/// Teto de frames por LOTE de saída do callback (o callback faz
/// chunking nesse valor — o scratch é fixo, alocado no start).
constexpr std::uint32_t kMaxOutChunk = 512;
/// Padrão quando a capacidade do stream não é legível (HAL exótico).
constexpr std::uint32_t kDefaultOutChunk = 256;

class AAudioBackend final : public IAudioBackend {
public:
    ~AAudioBackend() override { stop(); }

    eng::core::Result<void> start(AudioMixer& mixer) override
    {
        if (running_.load()) {
            return {};
        }
        mixer_.store(&mixer);
        gate_.reopen();
        firstCallback_.store(false, std::memory_order_relaxed);
        firstCallbackFrames_.store(0, std::memory_order_relaxed);

        // ---- AUDIO_DLOPEN / AUDIO_SYMBOLS --------------------------------
        reportStage(backend_stage::Dlopen, "begin", "dlopen libaaudio.so");
        const char* missing = nullptr;
        if (!loadAAudioApi(api_, &missing)) {
            if (missing == nullptr) {
                const char* err = dlerror();
                char detail[160];
                std::snprintf(detail, sizeof detail,
                              "dlopen libaaudio.so falhou: %s",
                              err != nullptr ? err : "?");
                reportStage(backend_stage::Dlopen, "failed", detail);
                return refuseStart(
                    eng::core::StatusCode::NotSupported,
                    "AAudio indisponível (libaaudio.so ausente — API < 26?)");
            }
            reportStage(backend_stage::Dlopen, "ok", "libaaudio.so carregada");
            char detail[128];
            std::snprintf(detail, sizeof detail, "símbolo ausente: %s",
                          missing);
            reportStage(backend_stage::Symbols, "failed", detail);
            return refuseStart(eng::core::StatusCode::NotSupported,
                              std::string("AAudio: símbolo ausente: ") +
                                  missing);
        }
        reportStage(backend_stage::Dlopen, "ok", "libaaudio.so carregada");
        reportStage(backend_stage::Symbols, "ok",
                    "18 símbolos resolvidos (dlsym)");

        // ---- AUDIO_BUILDER_CREATE ---------------------------------------
        reportStage(backend_stage::BuilderCreate, "begin", "");
        // AAudio_createStreamBuilder NÃO devolve aaudio_result_t —
        // um null aqui é falha de alocação/estado interno da libaaudio.
        // O errno no momento da chamada é a ÚNICA evidência disponível;
        // em device REAL isto é ERRO grave (não "esperado" — mensagem
        // P3.4 corrigida). Emulador -no-audio: documentado esperado.
        errno = 0;
        AAudioStreamBuilder* builder = api_.createStreamBuilder();
        if (builder == nullptr) {
            const int savedErrno = errno;
            char errbuf[128];
            errbuf[0] = '\0';
            if (savedErrno != 0) {
                // Portátil entre bionic (POSIX, devolve int) e glibc
                // (GNU, devolve char*): o (void) aceita ambos; o texto
                // fica em errbuf.
                (void)strerror_r(savedErrno, errbuf, sizeof errbuf);
            }
            char detail[256];
            std::snprintf(detail, sizeof detail,
                          "builder=null ERRO: errno=%d (%s)"
                          " [emulador -no-audio: esperado]",
                          savedErrno,
                          errbuf[0] != '\0' ? errbuf : "sem detalhe");
            reportStage(backend_stage::BuilderCreate, "failed", detail);
            return refuseStart(
                eng::core::StatusCode::Unknown,
                std::string("AAudio_createStreamBuilder devolveu null (") +
                    detail + ")");
        }
        reportStage(backend_stage::BuilderCreate, "ok", "builder criado");

        // ---- AUDIO_BUILDER_CONFIG ---------------------------------------
        // Pedimos os parâmetros do MIXER — mas são SUGESTÕES: o que
        // vale é o que o HAL abriu (verificado/adaptado adiante).
        // 
        //  - PERFORMANCE_MODE_NONE explícito — SEM MMAP no Unisoc (o
        //    caminho Legacy do framework é o comprovadamente seguro);
        //  - taxa 48000 (default do mixer) + canais/formato EXPLÍCITOS.
        api_.setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
        api_.setSampleRate(
            builder, static_cast<std::int32_t>(mixer.sampleRate()));
        api_.setChannelCount(
            builder, static_cast<std::int32_t>(mixer.channels()));
        api_.setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        api_.setDataCallback(builder, &AAudioBackend::callbackEntry, this);
        {
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "req %uHz %uch float perf=NONE(sem mmap)",
                          static_cast<unsigned>(mixer.sampleRate()),
                          static_cast<unsigned>(mixer.channels()));
            reportStage(backend_stage::BuilderConfig, "ok", detail);
        }

        // ---- AUDIO_STREAM_OPEN -------------------------------------------
        // A chamada mais opaca do startup: binder → AAudioService →
        // política de áudio → HAL do dispositivo (MMAP quando
        // disponível). Qualquer coisa que o vendor fizer de errado
        // aqui era invisível até o P3.3.
        reportStage(backend_stage::StreamOpen, "begin",
                    "binder→AAudioService→HAL");
        AAudioStream* stream = nullptr;
        const aaudio_result_t opened = api_.openStream(builder, &stream);
        api_.deleteBuilder(builder);
        if (opened != AAUDIO_OK) {
            char detail[128];
            formatResult(detail, sizeof detail, opened, api_);
            reportStage(backend_stage::StreamOpen, "failed", detail);
            return refuseStart(eng::core::StatusCode::Unknown,
                               std::string("AAudio openStream falhou (") +
                                   detail + ")");
        }
        if (stream == nullptr) {
            // Defensivo (causa 2): resultado OK + ponteiro nulo = HAL
            // inconsistente — requestStart nele seria SIGSEGV dentro
            // de libaaudio.
            reportStage(backend_stage::StreamOpen, "failed",
                        "openStream=OK mas stream=null (HAL inconsistente)");
            return refuseStart(eng::core::StatusCode::Unknown,
                              "AAudio openStream devolveu stream nulo");
        }
        // Parâmetros EFETIVOS (o que o HAL ABRIU de verdade).
        const int32_t eCh = api_.getChannelCount(stream);
        const int32_t eRate = api_.getSampleRate(stream);
        const aaudio_format_t eFmt = api_.getFormat(stream);
        const int32_t eCap = api_.getBufferCapacityInFrames(stream);
        {
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "ch=%d rate=%d fmt=%d cap=%d", eCh, eRate,
                          static_cast<int>(eFmt), eCap);
            reportStage(backend_stage::StreamOpen, "ok", detail);
        }

        // ---- AUDIO_STREAM_PARAMS_VERIFY (causas 1 e 2) -------------------
        reportStage(backend_stage::StreamParamsVerify, "begin", "");
        const auto refuseStream =
            [&](const char* stageDetail) -> eng::core::Result<void> {
            reportStage(backend_stage::StreamParamsVerify, "failed",
                        stageDetail);
            (void)api_.closeStream(stream);
            return refuseStart(eng::core::StatusCode::NotSupported,
                               std::string("AAudio stream recusado: ") +
                                   stageDetail);
        };

        // Causa 2: estado do stream — só requestStart a partir de OPEN.
        const aaudio_stream_state_t state = api_.getState(stream);
        if (state != AAUDIO_STREAM_STATE_OPEN) {
            char detail[128];
            std::snprintf(detail, sizeof detail,
                          "estado %d após open (esperado OPEN=%d)",
                          static_cast<int>(state),
                          static_cast<int>(AAUDIO_STREAM_STATE_OPEN));
            return refuseStream(detail);
        }
        // Causa 1: canais — adaptamos qualquer layout dentro de [1, 8].
        if (eCh <= 0 || eCh > 8) {
            char detail[128];
            std::snprintf(detail, sizeof detail, "canais %d fora de [1, 8]",
                          eCh);
            return refuseStream(detail);
        }
        if (mixer.channels() == 0 || mixer.channels() > 8) {
            char detail[128];
            std::snprintf(detail, sizeof detail,
                          "mixer com %u canais (máx 8)",
                          static_cast<unsigned>(mixer.channels()));
            return refuseStream(detail);
        }
        // Causa 1: formato — float/i16/i32 são convertidos; o resto é
        // recusado com precisão (i24 packed e i8 existem no AAudio).
        DeviceSampleFormat convertFormat{};
        if (!formatToConvert(eFmt, convertFormat)) {
            char detail[128];
            std::snprintf(detail, sizeof detail,
                          "fmt %d sem conversor (suportados: float=2 i16=1 "
                          "i32=4)",
                          static_cast<int>(eFmt));
            return refuseStream(detail);
        }
        // Causa 1: taxa — reamostragem exata quando divergir (razão
        // sanitizada; fora de [1/8, 8] o HAL está inconsistente).
        const bool rateMatches =
            eRate == static_cast<std::int32_t>(mixer.sampleRate());
        if (!rateMatches &&
            !resampler_.configure(mixer.sampleRate(),
                                  static_cast<std::uint32_t>(eRate),
                                  mixer.channels(), kMaxOutChunk)) {
            char detail[128];
            std::snprintf(detail, sizeof detail, "taxa %u→%d fora de [1/8,8]",
                          static_cast<unsigned>(mixer.sampleRate()), eRate);
            return refuseStream(detail);
        }

        // Dimensionamento do callback (thread do HOST, UMA vez):
        streamChannels_ = static_cast<std::uint32_t>(eCh);
        streamFormat_ = convertFormat;
        std::uint32_t chunk = kDefaultOutChunk;
        if (eCap > 0) {
            const std::uint32_t cap = static_cast<std::uint32_t>(eCap);
            chunk = cap < kMaxOutChunk ? cap : kMaxOutChunk;
        }
        outChunk_ = chunk;
        outScratch_.assign(
            static_cast<std::size_t>(outChunk_) * mixer.channels(), 0.f);
        if (!resampler_.isPassthrough()) {
            stage_.assign(static_cast<std::size_t>(
                              resampler_.maxPushFrames(outChunk_)) *
                              mixer.channels(),
                          0.f);
        } else {
            stage_.clear();
        }

        {
            char detail[160];
            const bool exact =
                rateMatches &&
                eCh == static_cast<std::int32_t>(mixer.channels()) &&
                eFmt == AAUDIO_FORMAT_PCM_FLOAT;
            if (exact) {
                std::snprintf(detail, sizeof detail,
                              "exact ch=%d rate=%d fmt=%s", eCh, eRate,
                              formatName(convertFormat));
            } else {
                std::snprintf(detail, sizeof detail,
                              "adapt ch %u→%d fmt float→%s rate %u→%d (%s)",
                              static_cast<unsigned>(mixer.channels()), eCh,
                              formatName(convertFormat),
                              static_cast<unsigned>(mixer.sampleRate()), eRate,
                              resampler_.isPassthrough() ? "igual"
                                                         : "reamostrado");
            }
            reportStage(backend_stage::StreamParamsVerify, "ok", detail);
            // Descrição estável do device p/ o host (marcos started e
            // first-frame do diagnóstico): parâmetros EFETIVOS + o que
            // a adaptação está cobrindo.
            std::string adapt;
            if (!rateMatches) {
                adapt += "resample";
            }
            if (eCh != static_cast<std::int32_t>(mixer.channels())) {
                if (!adapt.empty()) {
                    adapt += '+';
                }
                adapt += "ch";
            }
            if (eFmt != AAUDIO_FORMAT_PCM_FLOAT) {
                if (!adapt.empty()) {
                    adapt += '+';
                }
                adapt += "fmt";
            }
            streamInfo_ = "ch=" + std::to_string(eCh) +
                         " rate=" + std::to_string(eRate) +
                         " fmt=" + formatName(convertFormat) +
                         " cap=" + std::to_string(eCap) +
                         (adapt.empty() ? std::string{" exact"}
                                       : " adapt[" + adapt + "]");
        }

        // ---- AUDIO_STREAM_START -------------------------------------------
        reportStage(backend_stage::StreamStart, "begin", "requestStart (async)");
        const aaudio_result_t started = api_.requestStart(stream);
        if (started != AAUDIO_OK) {
            char detail[128];
            formatResult(detail, sizeof detail, started, api_);
            reportStage(backend_stage::StreamStart, "failed", detail);
            (void)api_.closeStream(stream);
            return refuseStart(eng::core::StatusCode::Unknown,
                               std::string("AAudio requestStart falhou (") +
                                   detail + ")");
        }
        stream_ = stream;
        running_.store(true);
        reportStage(backend_stage::StreamStart, "ok",
                     "pull ativo — callback na thread do AAudio");
        return {};
    }

    void stop() override
    {
        if (!running_.exchange(false)) {
            // start() nunca completou: ainda assim fechamos o portão e
            // soltamos o mixer (algum caminho de recusa deixou apontado).
            gate_.close();
            mixer_.store(nullptr);
            return;
        }
        // CAUSA 3 — sequência de parada determinística:
        // 1) portão fechado: novos callbacks recusados na entrada;
        gate_.close();
        AAudioStream* const stream = stream_;
        if (stream != nullptr) {
            // 2) parada assíncrona pedida;
            (void)api_.requestStop(stream);
            // 3) espera limitada do stream assentar (STOPPING→*):
            //    falha não é fatal — o close faz o join final;
            aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
            (void)api_.waitForStateChange(
                stream, AAUDIO_STREAM_STATE_STOPPING, &next,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::milliseconds{50})
                    .count());
            // 4) drenagem do NOSSO callback (nenhum mix() em andamento);
            if (!gate_.waitDrained(150)) {
                ENG_WARN("audio: callback não drenou em 150 ms — "
                         "closeStream fará o join final da thread");
            }
            // 5) join autoritativo + liberação.
            (void)api_.closeStream(stream);
        }
        stream_ = nullptr;
        mixer_.store(nullptr);
    }

    [[nodiscard]] bool isRunning() const noexcept override
    {
        return running_.load();
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "AAudio";
    }
    [[nodiscard]] std::string describeDevice() const override
    {
        // Depois do primeiro callback acrescenta a evidência de vida do
        // pull (número de frames do primeiro bloco entregue).
        if (firstCallback_.load(std::memory_order_acquire)) {
            return streamInfo_ + " first=" +
                   std::to_string(firstCallbackFrames_.load(
                       std::memory_order_relaxed));
        }
        return streamInfo_;
    }
    [[nodiscard]] bool hasFirstCallbackFired() const noexcept override
    {
        return firstCallback_.load(std::memory_order_acquire);
    }

private:
    /// Caminho comum de TODA recusa: o app segue VIVO (sem device de
    /// áudio); o mixer é solto e o portão fechado — nenhum callback
    /// pode tocar estado pela metade.
    eng::core::Result<void> refuseStart(eng::core::StatusCode code,
                                        std::string message)
    {
        gate_.close();
        mixer_.store(nullptr);
        return eng::core::makeUnexpected(
            eng::core::Error{code, std::move(message)});
    }

    static void reportStage(const char* stage, const char* status,
                            const char* detail)
    {
        eng::audio::reportBackendStage(stage, status, detail);
    }

    // --- callback (thread do AAudio) --------------------------------------

    static aaudio_data_callback_result_t callbackEntry(
        AAudioStream* /*stream*/, void* userData, void* audioData,
        int32_t numFrames)
    {
        auto* self = static_cast<AAudioBackend*>(userData);
        if (self == nullptr) {
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        if (!self->gate_.tryEnter()) {
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        const CallbackGate::Scope scope{self->gate_};
        return self->deliver(audioData, numFrames);
    }

    /// Entrega um bloco: mistura no layout do MIXER e converte para o
    /// layout EFETIVO do stream. NUNCA lança exceção, NUNCA aloca,
    /// NUNCA loga — qualquer anomalia vira silêncio + continuidade
    /// (o app permanece vivo; o diagnóstico de anomalias vem dos
    /// marcos do start, não do caminho quente).
    aaudio_data_callback_result_t deliver(void* audioData,
                                           int32_t numFrames) noexcept
    {
        if (!firstCallback_.load(std::memory_order_relaxed)) {
            firstCallbackFrames_.store(numFrames,
                                       std::memory_order_relaxed);
            firstCallback_.store(true, std::memory_order_release);
        }
        if (audioData == nullptr || numFrames <= 0) {
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        const std::uint32_t frames =
            static_cast<std::uint32_t>(numFrames);
        const std::uint32_t outCh = streamChannels_;
        AudioMixer* const mixer = mixer_.load(std::memory_order_acquire);
        if (mixer == nullptr || outCh == 0) {
            fillSilence(audioData, frames, outCh);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        const std::uint32_t mixCh = mixer->channels();
        const std::uint32_t sampleBytes =
            bytesPerSample(streamFormat_);
        auto* dst = static_cast<std::uint8_t*>(audioData);

        for (std::uint32_t done = 0; done < frames;) {
            const std::uint32_t outN =
                outChunk_ < frames - done ? outChunk_ : frames - done;
            if (resampler_.isPassthrough()) {
                // Caminho comum (taxas iguais): mistura e converte.
                std::memset(outScratch_.data(), 0,
                            static_cast<std::size_t>(outN) * mixCh *
                                sizeof(float));
                mixer->mix(outScratch_.data(), outN);
            } else {
                // Taxas divergentes: puxa do mixer o que o histórico
                // do resampler ainda não cobre e interpola a fase
                // exata (0 frames novos é normal em razões densas).
                const std::uint32_t push = resampler_.framesToPush(outN);
                if (push > 0) {
                    std::memset(stage_.data(), 0,
                                static_cast<std::size_t>(push) * mixCh *
                                    sizeof(float));
                    mixer->mix(stage_.data(), push);
                    resampler_.push(stage_.data(), push);
                }
                resampler_.produce(outN, outScratch_.data());
            }
            convertFrames(outScratch_.data(),
                          dst + static_cast<std::size_t>(done) * outCh *
                                    sampleBytes,
                          outN, mixCh, outCh, streamFormat_);
            done += outN;
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void fillSilence(void* audioData, std::uint32_t frames,
                     std::uint32_t channels) const noexcept
    {
        if (audioData == nullptr) {
            return;
        }
        // Zeros são silêncio em float E nos inteiros PCM (código 0).
        std::memset(audioData, 0,
                    static_cast<std::size_t>(frames) * channels *
                        bytesPerSample(streamFormat_));
    }

    // --- estado --------------------------------------------------------

    AAudioApi api_{};
    AAudioStream* stream_{nullptr};        ///< só na thread do host
    std::atomic<AudioMixer*> mixer_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> firstCallback_{false};
    std::atomic<std::int32_t> firstCallbackFrames_{0};
    CallbackGate gate_{};
    LinearResampler resampler_{};
    DeviceSampleFormat streamFormat_{DeviceSampleFormat::Float};
    std::uint32_t streamChannels_{0};
    std::uint32_t outChunk_{kDefaultOutChunk};
    std::vector<float> outScratch_{};  ///< lote de saída (mixCh intercalado)
    std::vector<float> stage_{};       ///< lote do mixer p/ o resampler
    std::string streamInfo_{"?"};
};

}  // namespace

/// Fábrica EXPOSTA do AAudio — o Auto backend (cadeia de
/// seleção automática, AutoBackend.cpp) tenta esta PRIMEIRO e cai para o
/// OpenSL ES quando o HAL recusa. O createDefaultBackend mudou de dono:
/// vive no AutoBackend.cpp nos dois builds.
std::unique_ptr<IAudioBackend> createAAudioBackend()
{
    return std::make_unique<AAudioBackend>();
}

}  // namespace eng::audio

#else  // Linux/testes: sem device de áudio — null (pull manual do teste).

#include <memory>

namespace eng::audio {

// No Linux a cadeia Auto NÃO existe (não há device) — a
// fábrica do AAudio também não; AutoBackend.cpp dá o createDefaultBackend.

}  // namespace eng::audio

#endif
