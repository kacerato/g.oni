#include "eng/audio/Audio.hpp"
#include "eng/audio/AudioAdapt.hpp"

/// OpenSlEsBackend — saída REAL alternativa no Android.
///
/// O AAudio do Unisoc T612 (Realme C33) recusa abrir (builder=null /
/// openStream falha — evidência nos marcos AUDIO_* do P3.4/P3.5). O
/// AAudio é UMA fachada do framework; o caminho LEGACY (AudioTrack via
/// OpenSL ES) existe desde a API 9 e atravessa o mesmo HAL — quando o
/// MMAP/binder do AAudio recusa, o OpenSL ES abre (é o que o Oboe faz
/// como fallback de indústria).
///
/// Modelo: AudioPlayer com BUFFER QUEUE (push de PCM i16 pronto). O
/// callback da fila corre na thread INTERNA do OpenSL: converte o mix
/// float → i16 via AudioAdapt (convertFrames) com resampler linear de
/// fase exata quando a taxa do device diverge do mixer. Saída estéreo
/// 48000 Hz pedida; o caminho Legacy honra o formato pedido (é quem
/// alimenta o AudioTrack do framework).
///
/// Mesma disciplina do AAudioBackend:
///  - dlopen("libOpenSLES.so") + dlsym — sem link edit;
///  - marcos granulares (opensl_stage::*) via hook do host;
///  - CallbackGate: nenhum callback toca o mixer após o stop() do dono;
///  - stop(): parada pedida → drenagem do callback → destruição dos
///    objetos (engine/mix/player) na thread do dono.
///
/// Linux/testes: TU VAZIO (a fábrica só existe no Android — o Auto
/// backend do Linux é o NullAudioBackend).

#ifdef __ANDROID__

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "eng/log/Macros.hpp"

namespace eng::audio {

namespace {

ENG_LOG_CATEGORY("audio.opensl");

/// Entrada da biblioteca (o ÚNICO símbolo de função público do OpenSL
/// ES; todo o resto navega pelas Itf v-tables).
using SlCreateEngineFn = SLresult (*)(SLObjectItf*, SLuint32,
                                      const SLEngineOption*, SLuint32,
                                      const SLInterfaceID*,
                                      const SLboolean*);

struct OpenSlApi {
    void* library{nullptr};
    SlCreateEngineFn createEngine{nullptr};
    SLInterfaceID iidEngine{nullptr};
    SLInterfaceID iidPlay{nullptr};
    SLInterfaceID iidBufferQueue{nullptr};
};

/// SLresult → texto estável. O OpenSL ES NÃO expõe resultToText como o
/// AAudio — tabela dos códigos do especificação Khronos (OpenSLES.h);
/// "?" para código fora da tabela, NUNCA nullptr (o marco persiste o
/// texto literal — contrato do hook). P4.1.1: o TU chamava um
/// formatResult que nunca existiu aqui (o do AAudioBackend.cpp tem
/// outra assinatura e vive noutro TU) — o clang do NDK recusou.
[[nodiscard]] const char* formatResult(SLresult result) noexcept
{
    switch (result) {
    case SL_RESULT_SUCCESS: return "SUCCESS";
    case SL_RESULT_PRECONDITIONS_VIOLATED: return "PRECONDITIONS_VIOLATED";
    case SL_RESULT_PARAMETER_INVALID: return "PARAMETER_INVALID";
    case SL_RESULT_MEMORY_FAILURE: return "MEMORY_FAILURE";
    case SL_RESULT_RESOURCE_ERROR: return "RESOURCE_ERROR";
    case SL_RESULT_RESOURCE_LOST: return "RESOURCE_LOST";
    case SL_RESULT_IO_ERROR: return "IO_ERROR";
    case SL_RESULT_BUFFER_INSUFFICIENT: return "BUFFER_INSUFFICIENT";
    case SL_RESULT_CONTENT_CORRUPTED: return "CONTENT_CORRUPTED";
    case SL_RESULT_CONTENT_UNSUPPORTED: return "CONTENT_UNSUPPORTED";
    case SL_RESULT_CONTENT_NOT_FOUND: return "CONTENT_NOT_FOUND";
    case SL_RESULT_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case SL_RESULT_FEATURE_UNSUPPORTED: return "FEATURE_UNSUPPORTED";
    case SL_RESULT_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case SL_RESULT_UNKNOWN_ERROR: return "UNKNOWN_ERROR";
    case SL_RESULT_OPERATION_ABORTED: return "OPERATION_ABORTED";
    case SL_RESULT_CONTROL_LOST: return "CONTROL_LOST";
    default: return "?";
    }
}

/// Resolve a tabela por dlsym (função + os SLInterfaceID exportados).
/// false = dlopen falhou (libOpenSLES.so ausente — impossível em API
/// ≥ 9) ou símbolo ausente (HAL quebrado — erro preciso no marco).
///
/// Os SL_IID_* são VARIÁVEIS globais do tipo
/// `const SLInterfaceID` (= `const SLInterfaceID_ *const` — PONTEIRO
/// const para o struct do IID de 16 bytes). O dlsym devolve O ENDEREÇO
/// da variável; o VALOR (o IID em si) vem da desreferência — o código
/// antigo fazia static_cast do endereço direto para o ponteiro e
/// desreferenciava nos chamados (conversão inválida que o clang do NDK
/// recusou). Null-check ANTES da desreferência (dlsym pode devolver
/// nullptr).
[[nodiscard]] bool loadOpenSlApi(OpenSlApi& api)
{
    api.library = dlopen("libOpenSLES.so", RTLD_NOW | RTLD_LOCAL);
    if (api.library == nullptr) {
        return false;
    }
    api.createEngine = reinterpret_cast<SlCreateEngineFn>(
        dlsym(api.library, "slCreateEngine"));
    const void* symEngine = dlsym(api.library, "SL_IID_ENGINE");
    const void* symPlay = dlsym(api.library, "SL_IID_PLAY");
    const void* symQueue =
        dlsym(api.library, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    if (api.createEngine == nullptr || symEngine == nullptr ||
        symPlay == nullptr || symQueue == nullptr) {
        return false;
    }
    api.iidEngine = *static_cast<const SLInterfaceID*>(symEngine);
    api.iidPlay = *static_cast<const SLInterfaceID*>(symPlay);
    api.iidBufferQueue = *static_cast<const SLInterfaceID*>(symQueue);
    return true;
}

/// Frames por buffer da fila (10 ms @ 48 kHz — latência previsível e
/// margem de CPU confortável no C33).
constexpr std::uint32_t kBufferFrames = 480;
/// Buffers em voo (3×10 ms = 30 ms de amortecimento contra underrun).
constexpr std::uint32_t kBufferCount = 3;

// =============================================================================
// Backend
// =============================================================================

class OpenSlEsBackend final : public IAudioBackend {
public:
    ~OpenSlEsBackend() override { stop(); }

    eng::core::Result<void> start(AudioMixer& mixer) override
    {
        if (running_.load()) {
            return {};
        }
        mixer_.store(&mixer, std::memory_order_relaxed);
        gate_.reopen();
        firstCallback_.store(false, std::memory_order_relaxed);

        // ---- AUDIO_OSLE_DLOPEN / SYMBOLS --------------------------------
        reportBackendStage(opensl_stage::Dlopen, "begin",
                           "dlopen libOpenSLES.so");
        OpenSlApi api{};
        if (!loadOpenSlApi(api)) {
            const char* err = dlerror();
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "dlopen/símbolos falharam: %s",
                          err != nullptr ? err : "?");
            reportBackendStage(opensl_stage::Dlopen, "failed", detail);
            return eng::core::makeUnexpected(eng::core::Error{
                eng::core::StatusCode::NotSupported,
                std::string("OpenSL ES indisponível (") + detail + ")"});
        }
        reportBackendStage(opensl_stage::Dlopen, "ok",
                           "libOpenSLES.so carregada");
        reportBackendStage(opensl_stage::Symbols, "ok",
                           "slCreateEngine + IIDs resolvidos");

        // ---- AUDIO_OSLE_ENGINE ------------------------------------------
        reportBackendStage(opensl_stage::EngineCreate, "begin", "");
        SLObjectItf engineObject = nullptr;
        const SLresult engineResult =
            api.createEngine(&engineObject, 0, nullptr, 0, nullptr, nullptr);
        if (engineResult != SL_RESULT_SUCCESS || engineObject == nullptr) {
            reportBackendStage(opensl_stage::EngineCreate, "failed",
                               formatResult(engineResult));
            return refuse(engineResult, "slCreateEngine falhou");
        }
        reportBackendStage(opensl_stage::EngineCreate, "ok", "");
        if (const SLresult r =
                (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
            r != SL_RESULT_SUCCESS) {
            reportBackendStage(opensl_stage::PlayerRealize, "failed",
                               formatResult(r));
            (*engineObject)->Destroy(engineObject);
            return refuse(r, "engine Realize falhou");
        }
        SLEngineItf engine = nullptr;
        (void)(*engineObject)->GetInterface(engineObject, api.iidEngine,
                                            &engine);
        engine_ = engine;
        api_ = api;             // IIDs vivos até o stop
        engineObject_ = engineObject;

        // ---- AUDIO_OSLE_MIX ---------------------------------------------
        reportBackendStage(opensl_stage::OutputMixCreate, "begin", "");
        // SLEngineItf é DUPLO ponteiro
        // (const SLEngineItf_ *const *) — a chamada segue o MESMO
        // idioma dos objetos: (*itf)->Função(itf, ...).
        if (engine == nullptr ||
            (*engine)->CreateOutputMix(engine, &mixObject_, 0, nullptr,
                                       nullptr) != SL_RESULT_SUCCESS) {
            reportBackendStage(opensl_stage::OutputMixCreate, "failed",
                               "CreateOutputMix recusado");
            return cleanupAndRefuse("CreateOutputMix falhou");
        }
        if (const SLresult r =
                (*mixObject_)->Realize(mixObject_, SL_BOOLEAN_FALSE);
            r != SL_RESULT_SUCCESS) {
            reportBackendStage(opensl_stage::OutputMixCreate, "failed",
                               formatResult(r));
            return cleanupAndRefuse("mix Realize falhou");
        }
        reportBackendStage(opensl_stage::OutputMixCreate, "ok", "");

        // ---- AUDIO_OSLE_PLAYER ------------------------------------------
        // Fonte: PCM i16 estéreo na taxa do MIXER (buffer queue); sink:
        // o OutputMix acima. A taxa pedida é a do MIXER — divergência
        // com o device é coberta pelo resampler (o buffer É o formato
        // pedido; o caminho Legacy honra).
        reportBackendStage(opensl_stage::PlayerCreate, "begin", "");
        SLDataLocator_AndroidSimpleBufferQueue locator{};
        locator.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
        locator.numBuffers = kBufferCount;
        SLDataFormat_PCM format{};
        format.formatType = SL_DATAFORMAT_PCM;
        format.numChannels = 2;
        format.samplesPerSec = mixer.sampleRate() * 1000u;  // mHz
        format.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
        format.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
        format.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
        format.endianness = SL_BYTEORDER_LITTLEENDIAN;
        SLDataSource source{};
        source.pFormat = &format;
        source.pLocator = &locator;
        SLDataLocator_OutputMix sinkLocator{};
        sinkLocator.locatorType = SL_DATALOCATOR_OUTPUTMIX;
        sinkLocator.outputMix = mixObject_;
        SLDataSink sink{};
        sink.pFormat = nullptr;
        sink.pLocator = &sinkLocator;
        const SLInterfaceID ids[1] = {api.iidBufferQueue};
        const SLboolean req[1] = {SL_BOOLEAN_TRUE};
        if ((*engine)->CreateAudioPlayer(engine, &playerObject_, &source,
                                         &sink, 1, ids,
                                         req) != SL_RESULT_SUCCESS) {
            reportBackendStage(opensl_stage::PlayerCreate, "failed",
                               "CreateAudioPlayer recusado");
            return cleanupAndRefuse("CreateAudioPlayer falhou");
        }
        reportBackendStage(opensl_stage::PlayerCreate, "ok",
                           "player criado (i16 estéreo)");

        reportBackendStage(opensl_stage::PlayerRealize, "begin", "");
        if (const SLresult r =
                (*playerObject_)->Realize(playerObject_, SL_BOOLEAN_FALSE);
            r != SL_RESULT_SUCCESS) {
            reportBackendStage(opensl_stage::PlayerRealize, "failed",
                               formatResult(r));
            playerObject_ = nullptr;
            return cleanupAndRefuse("player Realize falhou");
        }
        (void)(*playerObject_)
            ->GetInterface(playerObject_, api.iidPlay, &play_);
        (void)(*playerObject_)
            ->GetInterface(playerObject_, api.iidBufferQueue, &queue_);
        if (play_ == nullptr || queue_ == nullptr) {
            reportBackendStage(opensl_stage::PlayerRealize, "failed",
                               "interfaces PLAY/BUFFERQUEUE ausentes");
            return cleanupAndRefuse("interfaces do player ausentes");
        }
        reportBackendStage(opensl_stage::PlayerRealize, "ok", "");

        // ---- buffers + start --------------------------------------------
        streamChannels_ = 2;
        streamRate_ = mixer.sampleRate();
        mixChannels_ = mixer.channels();
        outScratch_.assign(static_cast<std::size_t>(kBufferFrames) *
                               streamChannels_,
                           0.f);
        stage_.assign(static_cast<std::size_t>(kBufferFrames) * mixChannels_,
                      0.f);
        // O caminho Legacy HONRA a taxa pedida (nós alimentamos os
        // buffers) — não há divergência a reamostrar por construção;
        // o resampler fica fora do caminho quente de propósito.
        {
            char detail[128];
            std::snprintf(detail, sizeof detail,
                          "%uHz %uch i16 fila=%ux%u frames",
                          static_cast<unsigned>(streamRate_),
                          static_cast<unsigned>(streamChannels_),
                          static_cast<unsigned>(kBufferCount),
                          static_cast<unsigned>(kBufferFrames));
            diagInfo_ = detail;
        }
        (*queue_)->RegisterCallback(queue_, &OpenSlEsBackend::queueEntry,
                                    this);
        (*play_)->SetPlayState(play_, SL_PLAYSTATE_STOPPED);
        // Enche a fila ANTES do PLAY (ordem do NDK — sem underrun inicial).
        for (std::uint32_t i = 0; i < kBufferCount; ++i) {
            enqueueBuffer();
        }
        if (const SLresult r =
                (*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING);
            r != SL_RESULT_SUCCESS) {
            reportBackendStage(opensl_stage::PlayerRealize, "failed",
                               formatResult(r));
            return cleanupAndRefuse("SetPlayState(PLAYING) falhou");
        }
        running_.store(true, std::memory_order_release);
        ENG_INFO("OpenSL ES ativo ({} Hz, i16 estéreo, fila {}x{})",
                 streamRate_, streamChannels_, kBufferCount, kBufferFrames);
        return {};
    }

    void stop() override
    {
        if (!running_.load(std::memory_order_acquire) && playerObject_ == nullptr) {
            return;
        }
        // P3.4 disciplina: fecha o portão ANTES da parada — nenhum
        // callback toca o mixer daqui em diante; drena e destrói.
        gate_.close();
        if (play_ != nullptr) {
            (void)(*play_)->SetPlayState(play_, SL_PLAYSTATE_STOPPED);
        }
        if (queue_ != nullptr) {
            (void)(*queue_)->Clear(queue_);
            (void)(*queue_)->RegisterCallback(queue_, nullptr, nullptr);
        }
        if (!gate_.waitDrained(200)) {
            ENG_WARN("OpenSL ES: callback não drenou em 200 ms (segue)");
        }
        running_.store(false, std::memory_order_release);
        destroyObjects();
        ENG_INFO("OpenSL ES parado — objetos destruídos");
    }

    [[nodiscard]] bool isRunning() const noexcept override
    {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "opensl";
    }

    [[nodiscard]] std::string describeDevice() const override
    {
        return diagInfo_;
    }

    [[nodiscard]] bool hasFirstCallbackFired() const noexcept override
    {
        return firstCallback_.load(std::memory_order_acquire);
    }

private:
    /// O contrato de start() é Result<void> —
    /// o erro devolve-se via makeUnexpected (Result NÃO converte Error
    /// implicitamente; o clang do NDK recusou a conversão com -Werror).
    [[nodiscard]] eng::core::Result<void> refuse(SLresult r,
                                                 const char* what) const
    {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::Unknown,
            std::string(what) + " (código " +
                std::to_string(static_cast<int>(r)) + ")"});
    }

    /// Destrói player/mix/engine (ordem folha→raiz) e devolve erro.
    [[nodiscard]] eng::core::Result<void> cleanupAndRefuse(const char* what)
    {
        destroyObjects();
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::Unknown,
            std::string("OpenSL ES: ") + what});
    }

    void destroyObjects() noexcept
    {
        if (playerObject_ != nullptr) {
            (void)(*playerObject_)->Destroy(playerObject_);
            playerObject_ = nullptr;
        }
        if (mixObject_ != nullptr) {
            (void)(*mixObject_)->Destroy(mixObject_);
            mixObject_ = nullptr;
        }
        if (engineObject_ != nullptr) {
            (void)(*engineObject_)->Destroy(engineObject_);
            engineObject_ = nullptr;
        }
        play_ = nullptr;
        queue_ = nullptr;
        engine_ = nullptr;
    }

    /// Mistura UM buffer (float do mixer → i16 do device) e enfileira.
    /// Corre na thread DO CHAMADOR (start enche; callback re-enzouve).
    void enqueueBuffer() noexcept
    {
        AudioMixer* mixer = mixer_.load(std::memory_order_relaxed);
        if (mixer == nullptr || gate_.isClosed()) {
            return;
        }
        const auto frames = static_cast<std::uint32_t>(kBufferFrames);
        std::memset(stage_.data(), 0,
                    static_cast<std::size_t>(frames) * mixChannels_ *
                        sizeof(float));
        mixer->mix(stage_.data(), frames);
        convertFrames(stage_.data(), outScratch_.data(), frames,
                      mixChannels_, streamChannels_,
                      DeviceSampleFormat::Int16);
        (void)(*queue_)->Enqueue(
            queue_, outScratch_.data(),
            static_cast<SLuint32>(frames * streamChannels_ *
                                  bytesPerSample(DeviceSampleFormat::Int16)));
    }

    /// Entrada da fila (thread INTERNA do OpenSL): re-enzouve o buffer
    /// consumido. Guard: portão fechado → NÃO toca em nada (P3.4 causa 3).
    static void SLAPIENTRY queueEntry(SLBufferQueueItf bq, void* context)
    {
        auto* self = static_cast<OpenSlEsBackend*>(context);
        if (!self->gate_.tryEnter()) {
            return;
        }
        CallbackGate::Scope scope{self->gate_};
        if (!self->firstCallback_.exchange(true,
                                           std::memory_order_acq_rel)) {
            reportBackendStage(opensl_stage::CallbackFirstFrame, "ok",
                               "primeiro buffer consumido pelo device");
        }
        self->enqueueBuffer();
        (void)bq;  // interface redundante com o contexto
    }

    OpenSlApi api_{};
    SLObjectItf engineObject_{nullptr};
    SLObjectItf mixObject_{nullptr};
    SLObjectItf playerObject_{nullptr};
    SLEngineItf engine_{nullptr};
    SLPlayItf play_{nullptr};
    SLBufferQueueItf queue_{nullptr};
    std::atomic<AudioMixer*> mixer_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> firstCallback_{false};
    CallbackGate gate_{};
    std::uint32_t streamChannels_{2};
    std::uint32_t streamRate_{48000};
    std::uint32_t mixChannels_{2};
    std::vector<float> outScratch_{};  ///< buffer i16 (staging float→convert)
    std::vector<float> stage_{};       ///< lote do mixer (layout do mixer)
    std::string diagInfo_{"?"};
};

}  // namespace

std::unique_ptr<IAudioBackend> createOpenSlEsBackend()
{
    return std::make_unique<OpenSlEsBackend>();
}

}  // namespace eng::audio

#else  // Linux/testes: TU vazio (a fábrica vive apenas no Android).

namespace eng::audio {

// O Auto backend no Linux é o NullAudioBackend direto (AudioTests puxam
// o mix manualmente) — nunca referencia esta fábrica.

}  // namespace eng::audio

#endif
