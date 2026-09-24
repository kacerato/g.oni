#ifndef ENG_EDITOR_DIAGNOSTICS_HPP
#define ENG_EDITOR_DIAGNOSTICS_HPP

/// \file Diagnostics.hpp
/// \brief P3.1–P3.5 — Diagnóstico persistente + crash handler + watchdog.
///
/// O PROBLEMA REAL (missão P3.1): no Realme C33 o app fecha após o splash,
/// SEM logcat disponível. O diagnóstico precisa SOBREVIVER à morte do
/// processo e ser extraível sem depender do usuário.
///
/// Evolução:
/// - P3.1: cada estágio é gravado IMEDIATAMENTE (append+fflush+fsync) em
///   filesDir/goni_startup.log; crash handler nativo grava goni_crash.log
///   e RE-ENTREGA ao handler anterior (tombstone/debuggerd preservados).
/// - P3.2: callback de espelho (Download/GONI via MediaStore) após cada
///   estágio persistido.
/// - P3.3: módulo+backtrace (frame-pointer) dentro do handler, sem
///   dladdr (locks do linker deadlockam em crash durante dlopen).
/// - P3.5 (esta revisão):
///   * mark() é THREAD-SAFE e carrega timestamps duplos (wallclock ms +
///     monotônico ms) — os micro-marks da janela resume→surface precisam
///     de resolução de milissegundos;
///   * o espelho NUNCA roda na thread que marcou: os pedidos vão para uma
///     FILA drenada por UMA thread dedicada do próprio diag (o trampoline
///     JNI só existe lá — marks de threads nativas não-attachadas, como
///     o retry de áudio, nunca tocam a VM);
///   * watchdog de hang: arm() captura a "main thread"; heartbeat() é a
///     prova de vida (Handler.post da Activity); evaluate() (pinger de 1 s)
///     dispara pthread_kill(main, SIGUSR1) após 8 s sem resposta — o
///     handler despeja o CONTEXTO EXATO onde a main está presa;
///   * captura forense completa em qualquer sinal handled: TODAS as
///     threads (tid+comm+pilhas [fp] e [scan]), maps CRU verbatim e
///     si_signo/si_code/si_pid/gettid.
///
/// Contrato de thread: mark/init/setMirrorCallback podem ser
/// chamados de QUALQUER thread (mutexes internos); o callback de espelho
/// é invocado EXCLUSIVAMENTE na thread de despacho interna; o crash
/// handler e os handlers de SIGUSR1/SIGUSR2 usam apenas
/// open/read/write/getdents64/snprintf (trade-off documentado) — sem
/// malloc, sem locks, sem dladdr.

#include <cstddef>
#include <cstdint>

namespace eng::editor::diag {

/// Inicializa o diagnóstico persistente (dir = filesDir no Android; dir de
/// teste no Linux). Idempotente: a segunda chamada é no-op. Abre
/// <dir>/goni_startup.log em append e instala o crash handler escrevendo
/// <dir>/goni_crash.log.
void init(const char* dir);

/// Marca um estágio de startup (THREAD-SAFE desde P3.5 — qualquer thread).
/// `status`: "ok" | "failed" | "begin" | outro rótulo curto.
/// `detail`: informação extra opcional (backend, projeto, erro).
/// Persiste na hora (flush + fsync) com timestamps duplos e enfileira o
/// espelho (a cópia pública é feita pela thread de despacho — nunca na
/// thread do chamador).
void mark(const char* stage, const char* status = "ok",
          const char* detail = nullptr);

/// Espelho assíncrono: o callback é registrado como antes (antes
/// do init), mas passa a ser invocado por UMA thread de despacho interna
/// (uma notificação por mark, ordem FIFO, sem coalescing neste nível —
/// o coalescing pesado é do detentor, no Android).
///
/// Contratos:
/// - chamada SEMPRE na thread de despacho (nunca na thread de mark;
///   nunca em signal handler);
/// - o callback NÃO pode chamar mark()/init() (recursão proibida);
/// - exceções não atravessam (função C): o detentor engole tudo.
using MirrorCallback = void (*)(void* userdata);

/// Registra (ou limpa com nullptr) o callback de espelhamento.
void setMirrorCallback(MirrorCallback callback, void* userdata);

/// Enfileira um pedido de espelho AGORA (no-op sem callback). Usado pela
/// Activity para forçar um export e pelos testes Linux. Retorna na hora.
void requestMirror();

/// Bloqueia até a fila do espelho drenar (máx `timeoutMs) — usado por
/// testes e pelo export do crash log na execução seguinte (garante a
/// cópia pública ANTES de voltar a arriscar morrer). Retorna false no
/// timeout (a fila segue drenando no fundo — nada é perdido).
bool waitMirrorIdle(std::uint32_t timeoutMs);

/// Último estágio marcado com sucesso ("-" quando nenhum).
/// É usado pelo crash handler (goni_crash.log) e por dumpState.
const char* lastStage() noexcept;

/// Instala handlers de crash (idempotente). Sem init() prévio: no-op.
/// O handler grava goni_crash.log e encadeia o handler ANTERIOR
/// (tombstone/debuggerd preservados — nada é mascarado).
void installCrashHandler();

/// true se a execução ANTERIOR deixou um crash REAL — linha "[crash]"
/// escrita pelo signal handler em goni_crash.log. A nota benigna
/// "[handler] instalado" NÃO conta.
bool hasPreviousCrashReport();

/// Caminho EFETIVO do log de startup ("-" quando não inicializado).
const char* startupLogPath() noexcept;
/// Caminho EFETIVO do log de crash ("-" quando não inicializado).
const char* crashLogPath() noexcept;

/// Descreve um endereço do PRÓPRIO processo como
/// "<modulo>+0x<offset>" (lê /proc/self/maps NA HORA). NÃO usar dentro
/// de signal handler (o handler tem a própria via, async-signal-safe).
bool describeAddress(std::uintptr_t address, char* out, std::size_t cap);

// =============================================================================
// Watchdog de hang da main thread (auto-ANR do G.ONI)
// =============================================================================

/// Hang sem ADB é invisível: /data/anr e /data/tombstones exigem root. O
/// watchdog do G.ONI substitui o ANR que o dispositivo não entrega:
///
///  1. arm()      — chamado NA main thread (captura pthread_t/tid e
///                  instala os handlers de SIGUSR1/SIGUSR2);
///  2. heartbeat()— chamado pela RUNNABLE postada na main (Handler.post
///                  da Activity, 1 s) — prova que a main processa
///                  mensagens;
///  3. evaluate() — chamado pelo pinger (1 s): sem resposta há
///                  `threshold` ms → grava [watchdog] no goni_crash.log e
///                  pthread_kill(main, SIGUSR1). O handler roda NA main
///                  travada e despeja o CONTEXTO EXATO do travamento
///                  (pc/pilhas [fp]/[scan]/threads/maps) — o processo
///                  CONTINUA VIVO (SIGUSR1 aqui é diagnóstico, não fatal;
///                  o suspend do ART é preservado por encadeamento).
namespace watchdog {

/// Captura a thread atual como "main" e instala os handlers diagnósticos.
/// Idempotente. `thresholdMs` (default 8000) e `graceMs` (default 15000)
/// são configuráveis para testes — a graça cobre o startup pesado
/// (onCreate do editor) antes do primeiro heartbeat.
void arm(std::int64_t thresholdMs = 8000, std::int64_t graceMs = 15000);

/// Prova de vida da main thread (atualiza o relógio interno).
void heartbeat() noexcept;

/// Verifica expiração; dispara o poke (uma vez por episódio) quando
/// vencido. Retorna true quando disparou (para o logcat do pinger).
bool evaluate() noexcept;

/// Limiares ativos (eco para log/diagnóstico).
std::int64_t thresholdMs() noexcept;
std::int64_t graceMs() noexcept;

}  // namespace watchdog

}  // namespace eng::editor::diag

#endif  // ENG_EDITOR_DIAGNOSTICS_HPP
