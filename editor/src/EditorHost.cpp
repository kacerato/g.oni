#include "eng/editor/EditorHost.hpp"

#include "eng/editor/Diagnostics.hpp"

/// EditorHost — host Android do editor (FASE 8). Espelha o AndroidRuntime
/// (FASE 7) com o MESMO contrato de surface/lifecycle (ADR-039/040), mas
/// renderiza o viewport do EditorDocument.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "eng/audio/Audio.hpp"
#include "eng/log/Macros.hpp"
#include "eng/rhi/Renderer.hpp"
#include "eng/rhi/gles/GlesBackend.hpp"
#include "eng/rhi/vulkan/VulkanBackend.hpp"

#ifdef __ANDROID__
#include <android/native_window.h>
#include "LogcatSink.hpp"
#endif

namespace eng::editor {

namespace {

ENG_LOG_CATEGORY("editor");

/// Registro de fábricas UMA vez por processo (idempotente — ADR-036).
void registerBackendFactories()
{
    static const bool registered = [] {
        using eng::rhi::BackendType;
        (void)eng::rhi::Renderer::registerBackend(
            BackendType::Vulkan, &eng::rhi::vulkan::createBackend);
        (void)eng::rhi::Renderer::registerBackend(
            BackendType::OpenGLES, &eng::rhi::gles::createBackend);
        return true;
    }();
    (void)registered;
}

[[nodiscard]] eng::rhi::BackendType backendFromName(const char* name)
{
    if (name == nullptr) {
        return eng::rhi::BackendType::Auto;
    }
    if (std::strcmp(name, "vulkan") == 0) {
        return eng::rhi::BackendType::Vulkan;
    }
    if (std::strcmp(name, "gles") == 0) {
        return eng::rhi::BackendType::OpenGLES;
    }
    return eng::rhi::BackendType::Auto;
}

[[nodiscard]] const char* backendToName(eng::rhi::BackendType type) noexcept
{
    switch (type) {
    case eng::rhi::BackendType::Vulkan: return "vulkan";
    case eng::rhi::BackendType::OpenGLES: return "gles";
    case eng::rhi::BackendType::Auto: break;
    }
    return "auto";
}

/// Frames apresentados para declarar uma sessão SAUDÁVEL (promove o
/// backend "trying" → "good"). ~0.5s a 60Hz: suficiente p/ atravessar a
/// criação de surface + primeiros draws (onde ocorre a morte súbita).
constexpr std::uint64_t kWatchdogHealthyFrames = 30;
/// Arquivo do watchdog na RAIZ do workspace (oculto, fora dos projetos).
constexpr std::string_view kWatchdogFile{".goni_backend_watchdog"};

/// P3.4 — trampoline do hook de progresso do backend de áudio: os
/// estágios granulares (backend_stage::*) viram marcos do diagnóstico
/// persistido 1:1 (mesma thread de start(); o mirror P3.2 copia
/// para Download/GONI a cada estágio — a janela de morte do C33 fica
/// cercada estágio a estágio). Sem userdata: diag é global do processo.
void audioBackendProgress(void* /*userdata*/, const char* stage,
                           const char* status, const char* detail)
{
    eng::editor::diag::mark(stage, status, detail);
}

/// P3.5 — trampoline do hook de progresso do RHI (micro-marks
/// RHI_BACKEND_SELECT/INSTANCE/DEVICE/SURFACE/SWAPCHAIN da janela
/// resume→surface): o eng::rhi não conhece o diagnóstico (grafo
/// acíclico) — quem sabe persistir é quem instalou o hook.
void rhiBackendProgress(void* /*userdata*/, const char* stage,
                        const char* status, const char* detail)
{
    eng::editor::diag::mark(stage, status, detail);
}

}  // namespace

// =============================================================================
// Criação/destruição
// =============================================================================

eng::core::Result<EditorHost*> EditorHost::create(const char* backend,
                                                  const char* workspaceRootCStr)
{
    registerBackendFactories();
#ifdef __ANDROID__
    // Reuso do sink da FASE 7 (android/runtime/src/LogcatSink.hpp — no APK
    // ambos os módulos compilam no mesmo alvo goni; no Linux o include é
    // eliminado pelo preprocessor).
    static const bool logcatAttached = [] {
        static eng::android::LogcatSink sink;
        eng::log::Logger::get().addSink(sink);
        return true;
    }();
    (void)logcatAttached;
#endif

    EditorHost* host = new EditorHost{};
    diag::mark("STARTUP_EDITOR_HOST", "begin");
    // P3.5 (T2): micro-marks do RHI — instalados ANTES de qualquer
    // Renderer::create (a criação de surface é o próximo sub-passo
    // invisível após STARTUP_RESUME no device).
    eng::rhi::setProgressHook(&rhiBackendProgress, nullptr);
    // FRONTEIRA DO WORKSPACE (RECOVERY P0 — bug do APK: "destino absoluto
    // é proibido" / "caminho absoluto proibido"): o root físico (absoluto
    // no Android) é absorvido AQUI, no RootedFileSystem. O documento vê
    // apenas paths RELATIVOS ao workspace — as validações anti-absoluto do
    // editor continuam intactas (nada foi enfraquecido; a conversão que
    // faltava na arquitetura foi adicionada na fronteira certa).
    const eng::fs::Path workspaceRoot{
        workspaceRootCStr == nullptr ? std::string_view(".")
                                    : std::string_view(workspaceRootCStr)};
    host->rooted_ = std::make_unique<eng::fs::RootedFileSystem>(
        host->fs_, workspaceRoot);
    diag::mark("STARTUP_FILESYSTEM", "ok", workspaceRoot.str().c_str());
    auto document = EditorDocument::create(*host->rooted_, eng::fs::Path{"."});
    if (document.isError()) {
        diag::mark("STARTUP_EDITOR_DOCUMENT", "failed",
                   document.error().message.c_str());
        delete host;
        return eng::core::makeUnexpected(document.error());
    }
    diag::mark("STARTUP_EDITOR_DOCUMENT", "ok");
    host->requested_ = backendFromName(backend);
    host->document_ = std::move(document.value());
    // P4.5.1 (R1): o begin acima era o ÚNICO mark sem par "ok" — a
    // janela entre begin e o próximo mark do Kotlin nunca fechava (a
    // morte "pós-host" era indistinguível de "morrendo a criar o
    // host"). Host + documento PRONTOS: fase de criação FECHADA.
    diag::mark("STARTUP_EDITOR_HOST", "ok", "host + documento prontos");
    ENG_INFO("Editor host criado (backend '{}', workspace root '{}')",
             backend == nullptr ? "auto" : backend, workspaceRoot.str());
    return host;
}

EditorHost::~EditorHost()
{
    stopAudio();  // P2 §12: device audio morre ANTES do mixer (documento)
    destroyRendererAndWindow();
}

// =============================================================================
// Surface (ADR-040 — contrato idêntico ao AndroidRuntime)
// =============================================================================

void EditorHost::acquireWindow(void* window) noexcept
{
#ifdef __ANDROID__
    if (window != nullptr) {
        ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
    }
#else
    (void)window; // Linux: ponteiro-marker (testes) — sem NDK
#endif
}

void EditorHost::releaseWindow(void* window) noexcept
{
#ifdef __ANDROID__
    if (window != nullptr) {
        ANativeWindow_release(static_cast<ANativeWindow*>(window));
    }
#else
    (void)window;
#endif
}

void EditorHost::destroyRendererAndWindow() noexcept
{
    // Texturas GPU MORREM ANTES do renderer (handles pertencem ao renderer
    // vivo — ADR-035); o cache fica vazio para o próximo renderer.
    if (eng::rhi::Renderer* renderer =
            viewportRenderer_.has_value() ? viewportRenderer_->renderer() : nullptr;
        renderer != nullptr) {
        textureCache_.clear(*renderer);
    } else {
        textureCache_.discardAll();
    }
    // Renderer PRIMEIRO (backend libera a VkSurfaceKHR/EGLSurface antes da
    // janela morrer — ADR-040); depois o release da referência própria.
    viewportRenderer_.reset();
    if (window_ != nullptr) {
        releaseWindow(window_);
        window_ = nullptr;
    }
    windowKind_ = eng::rhi::NativeWindowKind::None;
}

void EditorHost::invalidateTextureCache()
{
    if (eng::rhi::Renderer* renderer =
            viewportRenderer_.has_value() ? viewportRenderer_->renderer() : nullptr;
        renderer != nullptr) {
        textureCache_.clear(*renderer);
    } else {
        textureCache_.discardAll();
    }
}

void EditorHost::surfaceCreated(void* window,
                                eng::rhi::NativeWindowKind kind,
                                std::uint32_t width, std::uint32_t height)
{
    if (window_ != nullptr) {
        // surfaceCreated duplicado (defensivo): descarta o anterior.
        destroyRendererAndWindow();
    }
    acquireWindow(window);
    window_ = window;
    windowKind_ = kind;
    pendingWidth_ = width;
    pendingHeight_ = height;
    diag::mark("STARTUP_SURFACE", "acquired",
               "referência própria do ANativeWindow");

    if (width > 0 && height > 0 && createRendererForWindow(width, height)) {
        state_ = HostSurfaceState::Available;
    } else {
        // Tamanho desconhecido (surfaceChanged trará) — janela retida.
        state_ = HostSurfaceState::NoSurface;
    }
    ++stats_.surfaceCreations;
    ENG_INFO("Editor surface created ({}x{})", width, height);
}

void EditorHost::surfaceChanged(std::uint32_t width, std::uint32_t height)
{
    if (state_ == HostSurfaceState::Available ||
        state_ == HostSurfaceState::ChangedPending) {
        pendingWidth_ = width;
        pendingHeight_ = height;
        state_ = HostSurfaceState::ChangedPending; // aplica no próximo frame
    } else if (state_ == HostSurfaceState::NoSurface && window_ != nullptr &&
               width > 0 && height > 0) {
        // surfaceChanged chegou sem tamanho na created — cria agora.
        pendingWidth_ = width;
        pendingHeight_ = height;
        if (createRendererForWindow(width, height)) {
            state_ = HostSurfaceState::Available;
        }
    }
}

void EditorHost::surfaceDestroyed()
{
    destroyRendererAndWindow();
    state_ = HostSurfaceState::Destroyed;
    ++stats_.surfaceDestructions;
    ENG_INFO("Editor surface destroyed");
}

// =============================================================================
// Lifecycle
// =============================================================================

void EditorHost::onPause()
{
    paused_ = true; // flag apenas — robusto a qualquer ordem (§VI FASE 7)
    // P3.5 (T4): a época invalida ciclos de retry em voo (o worker
    // abandona no próximo checkpoint). A MAIN THREAD nunca mais para o
    // device aqui — o stream segue puxando silêncio (vozes pausadas
    // abaixo); o teardown definitivo é do destrutor, no worker, sob
    // audioOpMutex_. Um pause NUNCA pode bloquear em binder do AAudio.
    audioEpoch_.fetch_add(1);
    audioMixerLifecycle("onPause");  // P2 §12: vozes pausam com o app
}

void EditorHost::onResume()
{
    diag::mark("STARTUP_RESUME", "begin");
    paused_ = false;
    audioMixerLifecycle("onResume"); // P2 §12: vozes retomam com o app
    // P3.5 (T4): o áudio NUNCA mais é síncrono no onResume — o AAudio do
    // Unisoc já provou hostil (builder null em janela opaca). O worker
    // dedicado tenta (backoff 1/2/4 s) e o resume RETORNA IMEDIATAMENTE.
    scheduleAudioRetry();
    diag::mark("STARTUP_RESUME", "ok", "audio agendado async");
}

// =============================================================================
// Áudio P3.5 (T0/T4) — worker dedicado + posse serializada
// =============================================================================

std::shared_ptr<eng::audio::IAudioBackend>
EditorHost::audioBackendSnapshot() const
{
    const std::lock_guard<std::mutex> ptrLock{audioPtrMutex_};
    return audioBackend_;
}

void EditorHost::setAudioBackend(
    std::shared_ptr<eng::audio::IAudioBackend> backend)
{
    const std::lock_guard<std::mutex> ptrLock{audioPtrMutex_};
    audioBackend_ = std::move(backend);
}

void EditorHost::scheduleAudioRetry()
{
    {
        std::lock_guard<std::mutex> lock{audioRetryMutex_};
        audioResumeRequested_ = true;
        if (audioRetryActive_) {
            audioRetryCv_.notify_one();
            return;  // worker vivo: o pedido será consumido no próximo ciclo
        }
        audioRetryActive_ = true;
        audioRetryCancelled_ = false;
    }
    audioRetryCv_.notify_one();
    // worker anterior já terminou (active=false é o ÚLTIMO ato dele):
    // join imediato e spawna outro. Nunca há mais de UMA worker viva.
    if (audioRetryThread_.joinable()) {
        audioRetryThread_.join();
    }
    audioRetryThread_ = std::thread{[this] { audioRetryWorker(); }};
}

void EditorHost::audioRetryWorker()
{
    for (;;) {
        std::unique_lock<std::mutex> lock{audioRetryMutex_};
        audioRetryCv_.wait(lock, [this] {
            return audioRetryCancelled_ || audioResumeRequested_;
        });
        if (audioRetryCancelled_) {
            break;
        }
        audioResumeRequested_ = false;  // pedido consumido — novo ciclo
        lock.unlock();
        audioRetryRunCycle();
    }
    std::lock_guard<std::mutex> lock{audioRetryMutex_};
    audioRetryActive_ = false;
}

void EditorHost::audioRetryRunCycle()
{
    // Backoff P3.5: tentativa imediata + retries a +1 s/+2 s/+4 s
    // (4 tentativas totais; interpretação do "backoff 1/2/4, máx 3
    // tentativas de retry" — documentada em docs/p35-hang-audio.md).
    constexpr int kBackoffMs[] = {0, 1000, 2000, 4000};
    constexpr int kTotalAttempts = 4;
    const std::uint32_t epoch = audioEpoch_.load();
    bool started = false;
    for (int attempt = 1; attempt <= kTotalAttempts; ++attempt) {
        if (kBackoffMs[attempt - 1] > 0) {
            std::unique_lock<std::mutex> lock{audioRetryMutex_};
            audioRetryCv_.wait_for(
                lock, std::chrono::milliseconds{kBackoffMs[attempt - 1]},
                [this] { return audioRetryCancelled_; });
        }
        if (audioEpoch_.load() != epoch) {
            return;  // pausa/destruição mudou a geração — ciclo abortado
        }
        {
            const std::lock_guard<std::mutex> opLock{audioOpMutex_};
            const std::shared_ptr<eng::audio::IAudioBackend> current =
                audioBackendSnapshot();
            if (current != nullptr && current->isRunning() &&
                !audioNullFallback_) {
                started = true;  // já ativo (resume duplo etc.)
                break;
            }
            {
                char detail[96];
                std::snprintf(detail, sizeof detail, "tentativa %d/%d",
                              attempt, kTotalAttempts);
                diag::mark("STARTUP_AUDIO_RETRY", "begin", detail);
            }
            started = startAudioLocked();
        }
        if (started && audioEpoch_.load() == epoch) {
            break;
        }
        if (audioEpoch_.load() != epoch) {
            return;
        }
        if (!started && attempt < kTotalAttempts) {
            char detail[96];
            std::snprintf(detail, sizeof detail,
                          "falhou — proxima em %d ms",
                          kBackoffMs[attempt]);
            diag::mark("STARTUP_AUDIO_RETRY", "failed", detail);
        }
    }
    // Falha DEFINITIVA (época intacta durante todo o ciclo): NullBackend
    // gracioso — o app segue VIVO, previews/Play seguem funcionando SEM
    // som; o próximo resume descarta o null e tenta o device de novo.
    if (!started && audioEpoch_.load() == epoch) {
        const std::lock_guard<std::mutex> opLock{audioOpMutex_};
        const std::shared_ptr<eng::audio::IAudioBackend> current =
            audioBackendSnapshot();
        if ((current == nullptr || !current->isRunning() ||
             audioNullFallback_) &&
            document_ != nullptr) {
            auto nullBackend =
                std::make_shared<eng::audio::NullAudioBackend>();
            if (!nullBackend->start(document_->audioMixer()).isError()) {
                setAudioBackend(std::move(nullBackend));
                audioNullFallback_ = true;
                diag::mark("STARTUP_AUDIO", "null-fallback",
                           "NullBackend gracioso — app vivo, sem som "
                           "(nova tentativa no proximo resume)");
            }
        }
    }
}

void EditorHost::cancelAudioRetry() noexcept
{
    // Sem try/catch: a engine compila -fno-exceptions (ADR-004). As
    // operações (lock/notify/join) em falha catastrófica de std::level
    // terminariam o processo de qualquer forma — o contrato noexcept é
    // honrado pelo caminho normal; worker presa em AAudio é a limitação
    // documentada do HAL (ver docs/p35-hang-audio.md).
    {
        std::lock_guard<std::mutex> lock{audioRetryMutex_};
        audioRetryCancelled_ = true;
    }
    audioRetryCv_.notify_all();
    if (audioRetryThread_.joinable()) {
        audioRetryThread_.join();
    }
}

bool EditorHost::startAudio()
{
    // P3.5: caminho de manutenção SÍNCRONO (testes/legacy). O app usa o
    // worker (scheduleAudioRetry). Serializado pela posse do AAudio.
    const std::lock_guard<std::mutex> opLock{audioOpMutex_};
    return startAudioLocked();
}

bool EditorHost::startAudioLocked()
{
    // P3.4 — granular: cercar TODO o caminho de áudio com estágios
    // persistidos (o open do AAudio envolve dlopen + binder + HAL do
    // dispositivo — as chamadas de sistema mais opacas da janela de
    // morte súbita do C33). O backend emite os seus próprios marcos
    // (AUDIO_*) via hook — instalado ANTES de criar o backend.
    if (document_ == nullptr) {
        return false;
    }
    // Resume após null-fallback: descarta o nulo e tenta o device REAL
    // de novo ("nova tentativa no próximo resume" — missão P3.5 T4).
    if (audioNullFallback_) {
        stopAudioLocked();
    }
    {
        const std::shared_ptr<eng::audio::IAudioBackend> current =
            audioBackendSnapshot();
        if (current != nullptr && current->isRunning()) {
            return true;  // idempotente
        }
    }
    diag::mark("STARTUP_AUDIO", "begin");
    eng::audio::setBackendProgressHook(&audioBackendProgress, nullptr);
    auto backend = eng::audio::createDefaultBackend();
    if (backend == nullptr) {
        diag::mark("STARTUP_AUDIO", "failed", "createDefaultBackend = null");
        return false;
    }
    {
        const std::string backendName{backend->name()};
        diag::mark("STARTUP_AUDIO", "backend", backendName.c_str());
    }
    audioFirstFrameMarked_ = false;
    auto started = backend->start(document_->audioMixer());
    if (started.isError()) {
        ENG_WARN("audio backend: {} — previews/Play continuam sem device",
                 started.error().message);
        diag::mark("STARTUP_AUDIO", "failed",
                   started.error().message.c_str());
        return false;  // objeto local morre aqui (nada publicado)
    }
    setAudioBackend(std::move(backend));
    {
        const std::shared_ptr<eng::audio::IAudioBackend> published =
            audioBackendSnapshot();
        const std::string device = published->describeDevice();
        diag::mark("STARTUP_AUDIO", "started", device.c_str());
    }
    ENG_INFO("audio backend ativo");
    // P3.4: o primeiro callback pode disparar já DENTRO do requestStart
    // (o AAudio começa a puxar antes de retornar) — observa agora.
    checkAudioFirstCallbackFrame();
    return true;
}

void EditorHost::stopAudio() noexcept
{
    // P3.5 (T4): teardown completo — aborta retries, espera a worker
    // (join) e só então para o backend sob a posse serializada. É o
    // ÚNICO caminho que bloqueia em AAudio — destrutor apenas (risco
    // residual documentado: um binder do HAL preso seguraria o join —
    // exatamente o cenário que o watchdog T3 captura com evidência).
    audioEpoch_.fetch_add(1);
    cancelAudioRetry();
    const std::lock_guard<std::mutex> opLock{audioOpMutex_};
    stopAudioLocked();
}

void EditorHost::stopAudioLocked() noexcept
{
    const std::shared_ptr<eng::audio::IAudioBackend> backend =
        audioBackendSnapshot();
    if (backend != nullptr) {
        backend->stop();
    }
    setAudioBackend(nullptr);
    audioFirstFrameMarked_ = false;
    audioNullFallback_ = false;
    if (document_ != nullptr) {
        document_->audioMixer().stopAll();
    }
}

bool EditorHost::audioRunning() const noexcept
{
    const std::shared_ptr<eng::audio::IAudioBackend> backend =
        audioBackendSnapshot();
    return backend != nullptr && backend->isRunning();
}

std::string EditorHost::audioStatusLine() const
{
    // P4.1 (T3/D6) — VERDADE para o autor: qual backend está no ar (ou
    // por que não há som). O NullBackend gracioso do P3.5 deixa de ser
    // silêncio: vira "null: <último motivo do diagnóstico>".
    const std::shared_ptr<eng::audio::IAudioBackend> backend =
        audioBackendSnapshot();
    if (backend == nullptr || !backend->isRunning()) {
        return "off";
    }
    const std::string name{backend->name()};
    if (name == "null") {
        return audioNullFallback_
                   ? "null: device sem AAudio/OpenSL — som indisponível "
                     "(nova tentativa no próximo resume)"
                   : "null: backend de teste (sem device)";
    }
    return "running: " + name + " — " + backend->describeDevice();
}

void EditorHost::checkAudioFirstCallbackFrame()
{
    // P3.4 — evidência de vida do pull: o callback do AAudio marca um
    // átomo na própria thread de áudio; persistimos o marco UMA vez,
    // da UI thread (o hook/mirror do diagnóstico nunca roda na thread
    // de áudio — reentrância proibida por contrato). P3.5: lê um
    // SNAPSHOT do backend (o worker pode trocê-lo enquanto isto roda).
    if (audioFirstFrameMarked_) {
        return;
    }
    const std::shared_ptr<eng::audio::IAudioBackend> backend =
        audioBackendSnapshot();
    if (backend == nullptr || !backend->isRunning() ||
        !backend->hasFirstCallbackFired()) {
        return;
    }
    audioFirstFrameMarked_ = true;
    const std::string device = backend->describeDevice();
    diag::mark(eng::audio::backend_stage::CallbackFirstFrame, "ok",
               device.c_str());
}

void EditorHost::audioMixerLifecycle(const char* reason) noexcept
{
    if (document_ == nullptr) {
        return;
    }
    // P3.5: o lifecycle do MIXER apenas (vozes). O backend NÃO é mais
    // parado no pause: a main thread nunca toca AAudio em lifecycle —
    // um binder do HAL preso não pode congelar o pause do app (o stream
    // segue puxando silêncio; teardown é do destrutor, no worker).
    if (std::string_view(reason) == "onPause") {
        document_->audioMixer().pauseAll();
    } else {
        document_->audioMixer().resumeAll();
    }
}

void EditorHost::setBackend(const char* backend)
{
    requested_ = backendFromName(backend);
    if (state_ == HostSurfaceState::Available ||
        state_ == HostSurfaceState::ChangedPending) {
        // Recria AGORA com o novo backend (mesma janela).
        const std::uint32_t w = pendingWidth_;
        const std::uint32_t h = pendingHeight_;
        viewportRenderer_.reset();
        if (createRendererForWindow(w, h)) {
            state_ = HostSurfaceState::Available;
            ENG_INFO("backend trocado a quente");
        } else {
            state_ = HostSurfaceState::NoSurface;
        }
    }
    // Sem surface: aplica na próxima surfaceCreated (§XV FASE 7).
}

// =============================================================================
// Frame
// =============================================================================

bool EditorHost::createRendererForWindow(std::uint32_t width,
                                         std::uint32_t height)
{
    if (window_ == nullptr) {
        return false;
    }
    eng::rhi::SurfaceDesc surface;
    surface.window = eng::rhi::NativeWindowHandle{window_, windowKind_};
    surface.width = width;
    surface.height = height;
    // Watchdog (P3 §0): Auto é ajustado pelo histórico da SESSÃO
    // anterior (backend que morreu é pulado; comprovadamente bom é
    // preferido). Escolha explícita do usuário passa intacta.
    const eng::rhi::BackendType effective = effectiveBackend();
    if (effective != requested_) {
        ENG_WARN(
            "watchdog: Auto ajustado {} -> {} (sessão anterior deixou "
            "'{}')",
            backendToName(requested_), backendToName(effective),
            watchdogState_.empty() ? "-" : watchdogState_);
    }
    diag::mark("STARTUP_SURFACE", "renderer",
               backendToName(effective));
    auto renderer = ViewportRenderer::create(surface, effective);
    if (renderer.isError()) {
        ENG_ERROR("viewport renderer não criado: {}", renderer.error().message);
        return false;
    }
    viewportRenderer_ = std::move(renderer.value());
    document_->viewport().setScreenSize(static_cast<float>(width),
                                       static_cast<float>(height));
    watchdogOnRendererCreated();
    logSelection();
    return true;
}

void EditorHost::logSelection() const noexcept
{
    const auto* caps =
        viewportRenderer_.has_value() ? viewportRenderer_->capabilities()
                                      : nullptr;
    if (caps == nullptr) {
        return;
    }
    ENG_INFO("Backend selected: {}", caps->backendName);
    ENG_INFO("API version: {}", caps->apiVersion);
    ENG_INFO("GPU vendor: {}", caps->device.vendor);
    ENG_INFO("GPU renderer: {}", caps->device.name);
    ENG_INFO("Driver: {}", caps->device.driver);
    ENG_INFO("Software rendering: {}", caps->softwareRendering ? "yes" : "no");
}

bool EditorHost::renderFrame(float deltaSeconds)
{
    // P3.4 — observa o primeiro callback de áudio mesmo em frames
    // pulados (sem surface/paused): a evidência não depende do render.
    if (!audioFirstFrameMarked_) {
        checkAudioFirstCallbackFrame();
    }

    if (paused_ || state_ == HostSurfaceState::NoSurface ||
        state_ == HostSurfaceState::Destroyed ||
        !viewportRenderer_.has_value()) {
        ++stats_.framesSkippedNoSurface;
        return false;
    }

    if (state_ == HostSurfaceState::ChangedPending && window_ != nullptr) {
        auto resized =
            viewportRenderer_->resize(pendingWidth_, pendingHeight_);
        if (resized.isError()) {
            ++stats_.framesSkippedNoSurface;
            return false;
        }
        document_->viewport().setScreenSize(
            static_cast<float>(pendingWidth_),
            static_cast<float>(pendingHeight_));
        state_ = HostSurfaceState::Available;
    }

    // 1) tick do runtime (Play) — FASE 8: contrato; FASES 9/10 preenchem.
    document_->tick(deltaSeconds);

    // P4.7.0 B6: o CÉREBRO observa o frame (EMA + térmico → preset com
    // histerese). Roda mesmo sem surface? NÃO — sem surface não há
    // frame (return acima): métrica honesta mede frames reais.
    const int thermalRaw =
        thermalFn_ != nullptr ? thermalFn_(thermalUser_) : -1;
    const auto thermal = thermalRaw < 0
                             ? ThermalLevel::Unknown
                             : static_cast<ThermalLevel>(
                                   std::clamp(thermalRaw, 0, 6));
    governor_.onFrame(deltaSeconds * 1000.f, thermal);

    // 2) render do foco (edição em Edit; clone em Play — §8.7). Sprites
    // com textura real via TextureCache (evolução P0-3 — o documento é a
    // fonte dos dados; o host é o dono do renderer/upload). Sem projeto →
    // assets nulos: sprites caem no caminho de cor (honesto). Gizmo P1:
    // desenhado por cima (tool ativa + seleção; Edit apenas).
    const eng::scene::Scene* scene = document_->sceneInFocus();

    // P4.7.0 B6 (pooling): buffers REUTILIZADOS — clear() preserva a
    // capacidade conquistada; o frame quente não realoca.
    quadsScratch_.clear();
    particlesScratch_.clear();

    // P4.7.0 B6: CULLING por câmera — SÓ no Play (no Edit o autor vê a
    // cena INTEIRA por definição; culling de editor seria ferramenta
    // mentirosa). Rect = vista do frame (câmera de jogo ativa) + margem
    // conservadora para sprites maiores que a escala (documentado).
    std::uint32_t culled = 0;
    if (document_->isPlaying()) {
        auto cull = document_->viewport().worldViewRect();
        cull.margin = Viewport::kCullMarginWorld;
        document_->viewport().buildQuadsInto(
            quadsScratch_, *scene, document_->selection(), &cull, &culled);
    } else {
        document_->viewport().buildQuadsInto(
            quadsScratch_, *scene, document_->selection(), nullptr,
            nullptr);
    }
    lastCulled_ = culled;
    lastQuads_ = static_cast<std::uint32_t>(quadsScratch_.size());
    auto& quads = quadsScratch_;

    // P3 §3: material do sprite → shader/tint do quad (cache do documento;
    // sem projeto/default os quads já saem "lit" com tint intacto).
    document_->resolveMaterials(quads);
    const auto particles = document_->viewport().buildParticleQuads(*scene);
    gizmoDraw_ = document_->gizmoDraw(&textureCache_);
    // P4.6 (L2): a grade consome a config do PROJETO (passo em unidades,
    // primary-every, cores, show/hide — Grid v2).
    const eng::project::GridConfig& gridConfig = document_->gridConfig();
    const bool drew = viewportRenderer_->renderFrame(
        document_->viewport(), quads, particles, document_->isPlaying(),
        document_->assets(), textureCache_, &gizmoDraw_, &gridConfig);

    if (drew) {
        if (!stats_.startupComplete) {
            stats_.startupComplete = true;
            // P3.5 (T2): micro-mark de primeiro frame SUBMETIDO (o
            // STARTUP_COMPLETE abaixo confirma a APRESENTAÇÃO).
            diag::mark("FIRST_FRAME", "ok", "frame submetido ao viewport");
            diag::mark("STARTUP_COMPLETE", "ok", "first frame presented");
        }
        ++stats_.framesSubmitted;
        stats_.framesPresented = viewportRenderer_->framesPresented();
        watchdogOnFramePresented();  // P3 §0: promove trying → good
        stats_.firstFrameSubmitted = stats_.firstFrameSubmitted ||
                                     viewportRenderer_->framesSubmitted() > 0;
        stats_.firstFramePresented = stats_.firstFramePresented ||
                                     viewportRenderer_->framesPresented() > 0;
    }
    return drew;
}

eng::rhi::BackendType EditorHost::selectedBackend() const noexcept
{
    return viewportRenderer_.has_value() ? viewportRenderer_->activeBackend()
                                        : eng::rhi::BackendType::Auto;
}

// P4.7.0 B6: uma linha de performance para o HUD do Play (o padrão
// tab-separated dos getters — o Kotlin faz split('\t')).
std::string EditorHost::perfSummaryLine() const
{
    const PerfPreset preset = governor_.preset();
    const char* thermalName = "unknown";
    switch (governor_.thermal()) {
    case ThermalLevel::None: thermalName = "ok"; break;
    case ThermalLevel::Light: thermalName = "leve"; break;
    case ThermalLevel::Moderate: thermalName = "moderado"; break;
    case ThermalLevel::Severe: thermalName = "severo"; break;
    case ThermalLevel::Critical: thermalName = "crítico"; break;
    case ThermalLevel::Emergency: thermalName = "emergência"; break;
    case ThermalLevel::Shutdown: thermalName = "shutdown"; break;
    case ThermalLevel::Unknown: thermalName = "unknown"; break;
    }
    const char* presetName =
        governor_.presetIndex() == 0
            ? "High"
            : (governor_.presetIndex() == 1 ? "Med" : "Low");
    const float emaMs = governor_.emaMs();
    const float fps = emaMs > 0.0001f ? 1000.f / emaMs : 0.f;
    const std::size_t drawCalls =
        viewportRenderer_.has_value()
            ? viewportRenderer_->lastFrameDrawCalls()
            : 0;
    // "perf:\t<emaMs>\t<fps>\t<drawCalls>\t<culled>/<quads>\t<thermal>\t
    //  <preset>\t<renderScale>" — 7 campos tab-separated (padrão JNI).
    std::string line = "perf:";
    line += '\t';
    line += std::to_string(emaMs);
    line += '\t';
    line += std::to_string(fps);
    line += '\t';
    line += std::to_string(drawCalls);
    line += '\t';
    line += std::to_string(lastCulled_) + "/" + std::to_string(lastQuads_);
    line += '\t';
    line += thermalName;
    line += '\t';
    line += presetName;
    line += '\t';
    line += std::to_string(preset.renderScale);
    return line;
}

const eng::rhi::RendererCapabilities* EditorHost::capabilities() const noexcept
{
    return viewportRenderer_.has_value() ? viewportRenderer_->capabilities()
                                        : nullptr;
}

// =============================================================================
// Startup/diagnóstico (P3 §0 — bug Android "AlreadyExists" + "fecha
// rapidamente")
// =============================================================================

eng::core::Result<std::string> EditorHost::ensureStartupProject()
{
    // Origem registrada: a Activity chama UM ponto (nativeEditorEnsureProject)
    // — a política inteira (listar/decidir/criar/abrir) vive no documento,
    // testável no Linux sem Android.
    auto opened = document_->ensureStartupProject();
    if (opened.isError()) {
        diag::mark("STARTUP_PROJECT", "failed",
                   opened.error().message.c_str());
    } else {
        diag::mark("STARTUP_PROJECT", "ok", opened.value().c_str());
        // STARTUP_MATERIAL: o sistema de materiais inicializa junto ao
        // projeto (assets/materials + cache de resolução — P3 §3).
        diag::mark("STARTUP_MATERIAL", "ok", opened.value().c_str());
    }
    return opened;
}

void EditorHost::dumpState(const char* origin) const noexcept
{
    // Formato estável "state: <campo> = <valor>" — grep-ável no logcat.
    ENG_INFO("state: origem = {}", origin == nullptr ? "-" : origin);
    ENG_INFO("state: backend pedido = {}", backendToName(requested_));
    ENG_INFO("state: backend ativo = {}",
             viewportRenderer_.has_value()
                 ? backendToName(viewportRenderer_->activeBackend())
                 : "-");
    ENG_INFO("state: surface = {}", [this] {
        switch (state_) {
        case HostSurfaceState::NoSurface: return "no_surface";
        case HostSurfaceState::Available: return "available";
        case HostSurfaceState::ChangedPending: return "changed_pending";
        case HostSurfaceState::Destroyed: return "destroyed";
        }
        return "?";
    }());
    ENG_INFO("state: frames submetidos = {} | apresentados = {}",
             stats_.framesSubmitted, stats_.framesPresented);
    ENG_INFO("state: paused = {} | watchdog = '{}'", paused_,
             watchdogState_.empty() ? "-" : watchdogState_);
    if (document_ == nullptr) {
        ENG_INFO("state: documento = <nulo>");
        return;
    }
    const EditorDocument& doc = *document_;
    ENG_INFO("state: documento.hasProject = {} | projeto = '{}'",
             doc.hasProject(), doc.projectName());
    ENG_INFO("state: documento.projectRoot = '{}'",
             doc.hasProject() ? doc.projectRoot().str() : "-");
    ENG_INFO("state: modo = {} | sceneDirty = {} | projectDirty = {}",
             doc.isPlaying() ? "play" : "edit", doc.sceneDirty(),
             doc.projectDirty());
}

// --- watchdog de backend ------------------------------------------------------

std::string EditorHost::watchdogRead() const noexcept
{
    if (rooted_ == nullptr) {
        return {};
    }
    auto text = rooted_->readAllText(
        eng::fs::Path{kWatchdogFile});
    if (text.isError()) {
        return {};
    }
    // Trim defensivo (mesma política do .goni_last_project).
    std::string_view state{text.value()};
    while (!state.empty() &&
           (state.front() == '\n' || state.front() == '\r' ||
            state.front() == ' ')) {
        state.remove_prefix(1);
    }
    while (!state.empty() &&
           (state.back() == '\n' || state.back() == '\r' ||
            state.back() == ' ')) {
        state.remove_suffix(1);
    }
    return std::string{state};
}

void EditorHost::watchdogWrite(std::string_view state) noexcept
{
    if (rooted_ == nullptr) {
        return;
    }
    // Best-effort por design: o watchdog NUNCA pode derrubar a sessão.
    (void)rooted_->writeAllText(eng::fs::Path{kWatchdogFile}, state);
}

void EditorHost::watchdogOnRendererCreated() noexcept
{
    if (viewportRenderer_.has_value()) {
        watchdogBaseline_ = viewportRenderer_->framesPresented();
        const char* name =
            backendToName(viewportRenderer_->activeBackend());
        watchdogWrite(std::string{"trying:"} + name);
        watchdogState_ = std::string{"trying:"} + name;
        ENG_INFO("watchdog: sessão iniciando com backend '{}' (marcador "
                 "'trying:{}')",
                 name, name);
    }
}

void EditorHost::watchdogOnFramePresented() noexcept
{
    if (!viewportRenderer_.has_value()) {
        return;
    }
    const std::uint64_t presented =
        viewportRenderer_->framesPresented() - watchdogBaseline_;
    if (presented < kWatchdogHealthyFrames) {
        return;
    }
    // Sessão saudável: o backend ATIVO sobreviveu — promove a "good".
    const char* name = backendToName(viewportRenderer_->activeBackend());
    const std::string good = std::string{"good:"} + name;
    if (watchdogState_ != good) {
        watchdogWrite(good);
        watchdogState_ = good;
        ENG_INFO("watchdog: backend '{}' saudável ({} frames) — marcador "
                 "'good:{}'",
                 name, presented, name);
    }
}

eng::rhi::BackendType EditorHost::effectiveBackend() const noexcept
{
    if (requested_ != eng::rhi::BackendType::Auto) {
        return requested_;  // escolha explícita: watchdog não mexe
    }
    watchdogState_ = watchdogRead();
    const std::string_view state = watchdogState_;
    if (state == "trying:vulkan") {
        // Sessão anterior com Vulkan morreu antes de 30 frames.
        return eng::rhi::BackendType::OpenGLES;
    }
    if (state == "trying:gles") {
        return eng::rhi::BackendType::Vulkan;
    }
    if (state == "good:gles") {
        return eng::rhi::BackendType::OpenGLES;
    }
    if (state == "good:vulkan") {
        return eng::rhi::BackendType::Vulkan;
    }
    return eng::rhi::BackendType::Auto;  // sem histórico: ordem padrão
}

}  // namespace eng::editor
