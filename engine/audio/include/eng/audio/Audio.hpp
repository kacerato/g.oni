#pragma once

/// eng::audio — vozes, buses, mixer software e backend abstraído
///.
///
/// Conceitos: Sound (buffer decodificado — SFX), Music (leitura
/// progressiva do arquivo — streaming real), Voice (instância tocando),
/// AudioBus (ganho de grupo), fonte = Sound|Music.
///
/// Lifetime: handles de voz são VALORES geracionais (obsoletos =
/// no-op seguro); vozes terminadas são coletadas no tick() do jogo; o
/// mixer NÃO retém o Sound (shared_ptr do chamador); Music fecha o cursor
/// no fim/stop.
///
/// Threads: mix() roda na THREAD DE ÁUDIO (callback do backend);
/// todo o resto roda na thread do jogo. Um único mutex protege a lista de
/// vozes — janelas curtas (mix por frame, sem alocação no caminho quente
/// após a voz existir). Padrão pull.

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eng/audio/Wav.hpp"
#include "eng/core/Result.hpp"
#include "eng/fs/FileSystem.hpp"
#include "eng/fs/Path.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::audio {

// =============================================================================
// Bus
// =============================================================================

struct AudioBus {
    std::string name{"master"};
    float gain{1.f};
};

// =============================================================================
// Sound — buffer decodificado
// =============================================================================

class Sound final {
public:
    Sound() = default;

    /// Decodifica WAV completo (SFX — curto).
    [[nodiscard]] static eng::core::Result<Sound> fromWav(
        const WavData& data);

    [[nodiscard]] std::shared_ptr<const WavData> data() const noexcept
    {
        return data_;
    }
    [[nodiscard]] std::uint32_t frames() const noexcept;

private:
    std::shared_ptr<const WavData> data_;
};

// =============================================================================
// Mixer + vozes
// =============================================================================

/// Handle de voz: índice + geração (obsoleto = no-op — §6.10).
struct VoiceHandle {
    std::uint64_t value{0};
    [[nodiscard]] bool isValid() const noexcept { return value != 0; }
};

struct MixerStats {
    std::uint64_t voicesPlayed{0};
    std::uint64_t voicesFinished{0};
    std::uint64_t framesMixed{0};
    std::uint64_t underruns{0}; ///< pull sem tick (música sem dados)
};

class AudioMixer final {
public:
    explicit AudioMixer(std::uint32_t sampleRate = 48000,
                        std::uint16_t channels = 2);

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // --- buses ----------------------------------------------------------

    std::uint32_t createBus(std::string_view name, float gain);
    void setBusGain(std::uint32_t busId, float gain);

    // --- vozes (thread do JOGO) ---------------------------------------------------

    /// Toca um Sound (loop opcional). O mixer retém o shared_ptr — o
    /// chamador pode soltar o dele.
    [[nodiscard]] eng::core::Result<VoiceHandle> playSound(
        const Sound& sound, std::uint32_t busId = 0, float volume = 1.f,
        bool loop = false);

    /// Toca música com LEITURA PROGRESSIVA (streaming §6.10): o arquivo é
    /// lido uma vez (bytes), decodificado sob demanda por cursor.
    [[nodiscard]] eng::core::Result<VoiceHandle> playMusic(
        const std::shared_ptr<eng::fs::FileSystem>& fs,
        const eng::fs::Path& path, std::uint32_t busId = 0,
        float volume = 1.f, bool loop = false);

    void stop(VoiceHandle handle);
    void pause(VoiceHandle handle);
    void resume(VoiceHandle handle);
    void setVolume(VoiceHandle handle, float volume);
    [[nodiscard]] bool isPlaying(VoiceHandle handle) const;
    [[nodiscard]] bool isPaused(VoiceHandle handle) const;

    /// Lifecycle do app: pausa/retoma TUDO (onPause/onResume).
    void pauseAll() noexcept;
    void resumeAll() noexcept;
    void stopAll() noexcept;

    /// Manutenção da thread do JOGO: recolhe vozes encerradas, avança o
    /// decode sob demanda do streaming.
    void tick();

    // --- mix (THREAD DE ÁUDIO — AAudio) ----------------------------------------------

    /// Soma `frames` frames no out (f32 interleaved; NÃO zera: adiciona).
    void mix(float* out, std::uint32_t frames);

    [[nodiscard]] std::uint32_t sampleRate() const noexcept
    {
        return sampleRate_;
    }
    [[nodiscard]] std::uint16_t channels() const noexcept
    {
        return channels_;
    }
    [[nodiscard]] std::size_t liveVoices() const noexcept;
    [[nodiscard]] const MixerStats& stats() const noexcept
    {
        return stats_;
    }

    static constexpr std::uint32_t kMasterBus = 0;

private:
    struct Voice {
        std::uint64_t id{0};       // handle value
        std::uint32_t generation{1};
        bool active{false};
        bool paused{false};
        bool loop{false};
        float volume{1.f};
        std::uint32_t busId{0};

        // Fonte A: Sound (buffer pronto).
        std::shared_ptr<const WavData> buffer;
        std::size_t cursorFrames{0};

        // Fonte B: Music (bytes WAV + cursor de decode sob demanda).
        std::vector<std::byte> streamBytes;
        std::size_t streamDataOffset{0};  ///< início do chunk data
        std::size_t streamDataSize{0};
        std::uint32_t streamRate{0};
        std::uint16_t streamChannels{0};
        std::uint16_t streamBits{0};
        std::size_t streamCursor{0};      ///< bytes consumidos do data
        std::vector<float> decodeWindow;  ///< janela atual decodificada
        std::size_t windowCursor{0};
    };

    void decodeNextWindow(Voice& voice);
    [[nodiscard]] float busGain(std::uint32_t busId) const noexcept;
    /// Busca por handle (ativos apenas); obsoleto → nullptr.
    [[nodiscard]] static Voice* findVoice(
        std::vector<std::unique_ptr<Voice>>& voices, VoiceHandle handle);
    [[nodiscard]] static const Voice* findVoice(
        const std::vector<std::unique_ptr<Voice>>& voices,
        VoiceHandle handle);

    std::uint32_t sampleRate_;
    std::uint16_t channels_;
    mutable std::mutex mutex_;
    std::vector<AudioBus> buses_;
    std::vector<std::unique_ptr<Voice>> voices_;
    std::uint64_t nextVoiceId_{1};
    MixerStats stats_{};
};

// =============================================================================
// Backend — pull
// =============================================================================

/// Nomes dos estágios granulares da inicialização do backend de
/// áudio. Contrato com o host: o hook de progresso recebe EXATAMENTE
/// estas strings e o diagnóstico as persiste 1:1 (ver
/// docs/p34-aaudio-fix.md). A sequência canônica da inicialização AAudio
/// está documentada em AAudioBackend.cpp.
namespace backend_stage {
inline constexpr char Dlopen[] = "AUDIO_DLOPEN";
inline constexpr char Symbols[] = "AUDIO_SYMBOLS";
inline constexpr char BuilderCreate[] = "AUDIO_BUILDER_CREATE";
inline constexpr char BuilderConfig[] = "AUDIO_BUILDER_CONFIG";
inline constexpr char StreamOpen[] = "AUDIO_STREAM_OPEN";
inline constexpr char StreamParamsVerify[] = "AUDIO_STREAM_PARAMS_VERIFY";
inline constexpr char StreamStart[] = "AUDIO_STREAM_START";
inline constexpr char CallbackFirstFrame[] = "AUDIO_CALLBACK_FIRST_FRAME";
}  // namespace backend_stage

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    /// Conecta o mixer ao dispositivo e inicia o pull.
    [[nodiscard]] virtual eng::core::Result<void> start(AudioMixer& mixer) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /// Descrição estável do device/stream EFETIVAMENTE aberto para
    /// diagnóstico persistido (marcos STARTUP_AUDIO do host). Vazio quando
    /// não aplicável. Não usar em contexto de sinal.
    [[nodiscard]] virtual std::string describeDevice() const
    {
        return {};
    }
    /// True quando o callback do backend entregou ao menos um
    /// bloco ao device (o AAudio marca isso na própria thread de áudio;
    /// o host observa da thread dele e persiste o marco
    /// backend_stage::CallbackFirstFrame). Default: false.
    [[nodiscard]] virtual bool hasFirstCallbackFired() const noexcept
    {
        return false;
    }
};

/// Backend de TESTES/CI: nenhum dispositivo — o teste puxa mix() à mão.
/// (Contadores de operação; usado também quando não há áudio no host.)
class NullAudioBackend final : public IAudioBackend {
public:
    eng::core::Result<void> start(AudioMixer& mixer) override;
    void stop() override;
    bool isRunning() const noexcept override { return running_; }
    std::string_view name() const noexcept override { return "null"; }
    std::string describeDevice() const override;

    std::uint64_t startCount{0};
    std::uint64_t stopCount{0};

private:
    bool running_{false};
};

/// Estágios granulares do backend OpenSL ES (fallback
/// do Unisoc). Mesmo contrato do hook (backend_stage acima).
namespace opensl_stage {
inline constexpr char Dlopen[] = "AUDIO_OSLE_DLOPEN";
inline constexpr char Symbols[] = "AUDIO_OSLE_SYMBOLS";
inline constexpr char EngineCreate[] = "AUDIO_OSLE_ENGINE";
inline constexpr char OutputMixCreate[] = "AUDIO_OSLE_MIX";
inline constexpr char PlayerCreate[] = "AUDIO_OSLE_PLAYER";
inline constexpr char PlayerRealize[] = "AUDIO_OSLE_REALIZE";
inline constexpr char CallbackFirstFrame[] = "AUDIO_OSLE_FIRST_FRAME";
}  // namespace opensl_stage

/// Backend REAL alternativo — OpenSL ES (NDK, API 9+,
/// dlopen de libOpenSLES.so — mesmo padrão ADR-037/038). É o caminho
/// Legacy do framework: quando o AAudio do HAL recusa (builder null no
/// Unisoc T612 do Realme C33), o OpenSL ES abre. Saída i16 (formato
/// universal do caminho Legacy); conversão/resampler via AudioAdapt.
/// Android apenas (Linux/testes: fábrica não compilada — guard).
[[nodiscard]] std::unique_ptr<IAudioBackend> createOpenSlEsBackend();

/// Backend AAudio real (dlopen libaaudio.so — API 26+, mesma
/// disciplina ADR-037/038). Android apenas (Linux/testes: TU vazio —
/// guard __ANDROID__). Declaração pública porque o Auto backend
/// (cadeia P4.1) consulta a fábrica ANTES do OpenSL ES; nenhum outro
/// chamador — no Linux/testes a definição não existe (TU vazio) e a
/// cadeia Auto nem chega a ela (guard do AutoBackend.cpp).
[[nodiscard]] std::unique_ptr<IAudioBackend> createAAudioBackend();

/// CADEIA de seleção automática — tenta AAudio; recusado,
/// tenta OpenSL ES; o backend vencedor é logado (marco
/// AUDIO_BACKEND_SELECTED) e exposto ao autor (HUD do editor). Nunca
/// fallback silencioso: a escolha fica registrada em estágio + status.
class AutoAudioBackend final : public IAudioBackend {
public:
    ~AutoAudioBackend() override;
    [[nodiscard]] eng::core::Result<void> start(AudioMixer& mixer) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::string describeDevice() const override;
    [[nodiscard]] bool hasFirstCallbackFired() const noexcept override;

    /// Backend EFETIVO (nullptr antes do start bem-sucedido).
    [[nodiscard]] const IAudioBackend* active() const noexcept
    {
        return active_.get();
    }

private:
    std::unique_ptr<IAudioBackend> active_{};
};

/// Fábrica do backend padrão da plataforma (P4.1: CADEIA Auto no
/// Android — AAudio → OpenSL ES; null no Linux/testes). Dono é o
/// chamador (host).
[[nodiscard]] std::unique_ptr<IAudioBackend> createDefaultBackend();

// =============================================================================
// Hook de progresso da inicialização do backend
// =============================================================================

/// Callback de progresso instalado pelo HOST antes de
/// createDefaultBackend(): os backends emitem os estágios granulares
/// (backend_stage::*) DURANTE start() — é o que cercar a janela de morte
/// súbita do Realme C33 (as chamadas AAudio são as mais opacas do
/// startup). A engine de áudio NÃO depende do módulo editor (grafo
/// acíclico): quem sabe persistir é quem instalou o hook.
///
/// Contratos:
/// - chamado na MESMA thread de start() (nunca da thread de áudio,
///   nunca de signal handler);
/// - `stage` é um backend_stage::*; `status` "begin"/"ok"/"failed";
///   `detail` é texto curto ou nullptr;
/// - o hook NÃO pode chamar a API de áudio (reentrância proibida) e
/// - não pode lançar (função C).
using BackendProgressHook = void (*)(void* userdata, const char* stage,
                                     const char* status,
                                     const char* detail);

/// Registra (ou limpa com nullptr) o hook de progresso.
void setBackendProgressHook(BackendProgressHook hook, void* userdata);

/// (uso interno dos backends) Emite um estágio pelo hook instalado
/// (no-op sem hook). `detail` pode ser nullptr.
void reportBackendStage(const char* stage, const char* status,
                        const char* detail) noexcept;

}  // namespace eng::audio
