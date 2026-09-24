#pragma once

/// eng::jobs — sistema de tarefas com work stealing.
///
/// Modelo (detalhes e decisões em ADR-023):
///   - N workers (default: `std::thread::hardware_concurrency()`, mínimo 1),
///     cada um com deque próprio protegido por mutex.
///   - Envio: round-robin entre deques. Execução local: LIFO (pop do fim —
///     localidade de cache). Roubo: FIFO (início do deque de OUTRO worker),
///     varredura circular — work stealing REAL, não simulado.
///   - `submit(f)` devolve `JobHandle`; `handle.wait()` bloqueia até concluir
///     (e repropaga exceção do job quando ENG_JOBS_CATCH_EXCEPTIONS).
///   - `waitAll()` bloqueia até zerar jobs enfileirados + em execução.
///   - Destrutor = `shutdown()`: para de aceitar envios, DRENA as filas
///     (todo job pendente executa), encerra e faz join de todos os workers.
///     `shutdown()` é idempotente (segunda chamada: no-op). Nenhum thread
///     vaza; o destrutor é o ponto de sincronização definitivo.
///   - Envio APÓS shutdown: devolve handle INVÁLIDO (wait: no-op) — sem
///     crash; comportamento documentado e testado.
///
/// Exceções: quando `ENG_JOBS_CATCH_EXCEPTIONS=1` (default ON em debug; OFF
/// em release — ver ADR-023 e CMakePresets), o módulo captura exceções do
/// job, armazena via `std::exception_ptr` e as repropaga em `wait()`. Com a
/// opção OFF, exceção dentro de job propaga no worker e encerra o processo
/// (`std::terminate`) — política de release.
///
/// Limitações documentadas (deliberadas, FASE 2):
///   - `wait()`/`waitAll()` chamados DE DENTRO de um job não são suportados
///     (podem deadlockar o pool) — aguardam JobGroup/dependências futuras.
///   - JobGroup, dependências entre jobs, futures e corrotinas: interface
///     EVOLUÇÃO documentada em ADR-023 — SEM código stub nesta fase.
///   - `submit()` concorrente com `shutdown()` a partir de threads externas
///     deve ser serializado pelo chamador (ver ADR-023, seção "ordas").
///   - Ciclo de vida e contabilidade LINEARIZÁVEL: `queued_`
///     conta o job ANTES da publicação na fila; a transferência
///     fila→execução incrementa `inFlight_` ANTES de decrementar `queued_`
///     — a soma `queued_ + inFlight_` nunca cai a zero durante uma
///     transferência, logo `waitAll` jamais retorna cedo.
///   - Workers dormem em `std::counting_semaphore` (contagem persistente:
///     release antes de acquire não se perde — sem categoria de lost-wakeup;
///     sem spin de predicado). Envio não disputa o mutex de wake.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(ENG_JOBS_CATCH_EXCEPTIONS) && ENG_JOBS_CATCH_EXCEPTIONS != 0
#define ENG_JOBS_DETAIL_CATCH 1
#else
#define ENG_JOBS_DETAIL_CATCH 0
#endif

namespace eng::jobs {

class JobSystem;

namespace detail {

// =============================================================================
// Callable — void() type-erased com small-buffer (sem std::function, ADR-023).
// Inline quando cabe no buffer de 48 bytes, respeita o alinhamento e é
// nothrow-movable; senão heap. Movimento SEMPRE noexcept (inline = move-
// construct + destroy; heap = transferência de ponteiro).
// =============================================================================

class Callable {
public:
    Callable() = default;
    Callable(const Callable&) = delete;
    Callable& operator=(const Callable&) = delete;
    Callable& operator=(Callable&&) = delete;
    Callable(Callable&& other) noexcept;
    ~Callable();

    template<typename F>
    void emplaceFrom(F&& functor)
    {
        using G = std::decay_t<F>;
        static_assert(std::is_invocable_v<G&>,
                      "job deve ser invocável como f()");

        constexpr bool kInline = sizeof(G) <= kSmallSize
                                 && alignof(G) <= alignof(Storage)
                                 && std::is_nothrow_move_constructible_v<G>;
        if constexpr (kInline) {
            obj_ = &storage_;
        } else {
            obj_ = ::operator new(sizeof(G));
            heap_ = true;
        }
        new (obj_) G(std::forward<F>(functor));
        invoke_ = &invokeImpl<G>;
        destroy_ = &destroyImpl<G>;
        moveCtor_ = &moveCtorImpl<G>;
    }

    /// Invoca o callable armazenado. Vazio → no-op defensivo.
    void invoke() { if (invoke_ != nullptr) { invoke_(obj_); } }

    [[nodiscard]] bool empty() const noexcept { return invoke_ == nullptr; }

private:
    struct Storage {
        alignas(std::max_align_t) std::byte bytes[48];
    };

    template<typename G>
    static void invokeImpl(void* obj)
    {
        (*static_cast<G*>(obj))();
    }

    template<typename G>
    static void destroyImpl(void* obj) noexcept
    {
        static_cast<G*>(obj)->~G();
    }

    template<typename G>
    static void moveCtorImpl(void* dst, void* src) noexcept
    {
        new (dst) G(std::move(*static_cast<G*>(src)));
    }

    static constexpr std::size_t kSmallSize = sizeof(Storage);

    Storage storage_{};
    void* obj_ = nullptr;                       ///< &storage_ ou heap
    void (*invoke_)(void*) = nullptr;
    void (*destroy_)(void*) noexcept = nullptr;
    void (*moveCtor_)(void* dst, void* src) noexcept = nullptr;
    bool heap_ = false;
};

// =============================================================================
// Estado de conclusão de um job — propriedade COMPARTILHADA de verdade
// (fila/worker + handle do usuário): shared_ptr é a escolha explícita
// (missão §B.0, justificativa em ADR-023). O estado sobrevive ao JobSystem —
// wait() em handle de job já concluído funciona mesmo pós-destruição.
// =============================================================================

struct JobState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;             ///< protegido por mutex
    std::exception_ptr exception;  ///< protegido por mutex (quando ENG_JOBS_DETAIL_CATCH)
};

/// Tarefa enfileirada: estado + callable. Move-only.
struct Task {
    std::shared_ptr<JobState> state;
    Callable callable;
};

/// Deque de um worker (também alvo de roubo). front = mais antigo (roubo),
/// back = mais recente (pop local LIFO).
struct WorkerQueue {
    std::mutex mutex;
    std::deque<Task> jobs;
};

} // namespace detail

// =============================================================================
// JobHandle — RAII move-only sobre o estado de conclusão. A destruição NÃO
// espera (o job continua: o estado é compartilhado com a fila/worker); use
// wait()/waitAll()/~JobSystem como pontos de sincronização. wait() em handle
// inválido (pós-shutdown) é no-op.
// =============================================================================

class JobHandle final {
public:
    JobHandle() noexcept = default;
    JobHandle(const JobHandle&) = delete;
    JobHandle& operator=(const JobHandle&) = delete;
    JobHandle(JobHandle&&) noexcept = default;
    JobHandle& operator=(JobHandle&&) noexcept = default;
    ~JobHandle() = default;

    /// Bloqueia até o job concluir; repropaga exceção capturada (quando
    /// habilitado). Handle inválido → retorna imediatamente.
    void wait();

    /// Handle obtido de um submit aceito?
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

private:
    friend class JobSystem;
    explicit JobHandle(std::shared_ptr<detail::JobState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::JobState> state_;
};

// =============================================================================
// JobSystem — pool de workers com work stealing. Não é singleton: cada
// subsistema cria o seu. Cycle de vida: construção inicia workers; shutdown()
// (ou destrutor) drena e encerra.
// =============================================================================

class JobSystem final {
public:
    /// `workerCount == 0` → `hardware_concurrency()` (mínimo 1).
    /// Lança std::invalid_argument se workerCount > hardware? NÃO: aceita
    /// qualquer valor ≥ 1 (oversubscription é decisão do chamador); 0 é o
    /// default automático.
    explicit JobSystem(unsigned workerCount = 0);

    /// shutdown() — drena, encerra workers, join. Nunca vaza threads.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Enfileira `task` (invocável como f()). Devolve handle inválido se o
    /// sistema já está desligado (sem crash — documentado). Complexidade:
    /// O(1) (deque do worker alvo + possível heap do callable).
    /// Contabilidade: o job é CONTADO antes de ser publicado — waitAll é
    /// linearizável em relação a submits concorrentes.
    template<typename F>
    [[nodiscard]] JobHandle submit(F&& task)
    {
        if (!running_.load(std::memory_order_acquire)) {
            return JobHandle{}; // pós-shutdown: comportamento definido
        }
        auto state = std::make_shared<detail::JobState>();
        detail::Task job;
        job.state = state;
        job.callable.emplaceFrom(std::forward<F>(task));

        const unsigned target =
            nextQueue_.fetch_add(1, std::memory_order_relaxed) % workerCount_;
        queued_.fetch_add(1, std::memory_order_release); // conta ANTES de publicar
        {
            detail::WorkerQueue& queue = *queues_[target];
            std::lock_guard<std::mutex> lock(queue.mutex);
            queue.jobs.push_back(std::move(job));
        }
        workAvailable_.release(); // acorda um worker estacionado
        return JobHandle(std::move(state));
    }

    /// Bloqueia até todos os jobs enfileirados E em execução concluírem.
    /// Não chamar de dentro de um job (deadlock — ADR-023).
    void waitAll();

    /// Para de aceitar envios, DRENA as filas e faz join dos workers.
    /// Idempotente: segunda chamada (ou destrutor após chamada) é no-op.
    void shutdown();

    [[nodiscard]] unsigned workerCount() const noexcept { return workerCount_; }
    [[nodiscard]] bool isShutdown() const noexcept
    {
        return !running_.load(std::memory_order_acquire);
    }

private:
    void workerLoop(unsigned id);
    void executeTask(detail::Task&& task);
    /// Transferência fila→execução SEM janela zero-zero: inFlight_ sobe ANTES
    /// de queued_ cair — a soma nunca toca zero durante a transferência.
    void transferToInFlight() noexcept
    {
        inFlight_.fetch_add(1, std::memory_order_relaxed);
        queued_.fetch_sub(1, std::memory_order_release);
    }
    [[nodiscard]] std::optional<detail::Task> tryPopOwn(unsigned id);
    [[nodiscard]] std::optional<detail::Task> trySteal(unsigned id);

    unsigned workerCount_ = 0;
    std::vector<std::unique_ptr<detail::WorkerQueue>> queues_;
    std::vector<std::thread> threads_;

    /// Estacionamento de workers: contagem persistente de jobs publicados —
    /// release antes de acquire é seguro por construção (sem lost-wakeup).
    std::counting_semaphore<> workAvailable_{0};
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_; ///< esperas de waitAll
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownDone_{false};
    std::atomic<int> queued_{0};    ///< jobs contados (ainda não publicados + nas filas)
    std::atomic<int> inFlight_{0};  ///< jobs em execução
    std::atomic<unsigned> nextQueue_{0};
};

} // namespace eng::jobs
