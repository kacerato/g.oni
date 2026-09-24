#include "eng/jobs/JobSystem.hpp"

#include <exception>
#include <optional>
#include <utility>

namespace eng::jobs::detail {

// --- Callable ----------------------------------------------------------------

Callable::Callable(Callable&& other) noexcept
    : storage_{}, obj_(other.obj_),
      invoke_(other.invoke_),
      destroy_(other.destroy_),
      moveCtor_(other.moveCtor_),
      heap_(other.heap_)
{
    if (other.obj_ == nullptr) {
        obj_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        moveCtor_ = nullptr;
        heap_ = false;
        return;
    }
    if (other.heap_) {
        // Heap: transferência de ponteiro — o objeto não se move.
    } else {
        // Inline: move-construct no novo buffer + destrói a origem.
        // (pré-condição do inline: G é nothrow-move-constructible)
        moveCtor_(&storage_, &other.storage_);
        destroy_(&other.storage_);
        obj_ = &storage_;
    }
    other.obj_ = nullptr;
    other.invoke_ = nullptr;
    other.destroy_ = nullptr;
    other.moveCtor_ = nullptr;
    other.heap_ = false;
}

Callable::~Callable()
{
    if (obj_ != nullptr) {
        destroy_(obj_);
        if (heap_) {
            ::operator delete(obj_);
        }
    }
}

} // namespace eng::jobs::detail

namespace eng::jobs {

// --- JobHandle ----------------------------------------------------------------

void JobHandle::wait()
{
    if (state_ == nullptr) {
        return; // handle inválido (pós-shutdown): no-op documentado
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->cv.wait(lock, [this] { return state_->done; });
#if ENG_JOBS_DETAIL_CATCH
    if (state_->exception) {
        std::rethrow_exception(state_->exception);
    }
#endif
}

// --- JobSystem ----------------------------------------------------------------

JobSystem::JobSystem(unsigned workerCount)
{
    if (workerCount == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        workerCount = hardware > 0 ? hardware : 1;
    }
    workerCount_ = workerCount;

    queues_.reserve(workerCount_);
    for (unsigned i = 0; i < workerCount_; ++i) {
        queues_.push_back(std::make_unique<detail::WorkerQueue>());
    }

    running_.store(true, std::memory_order_release);
    threads_.reserve(workerCount_);
    for (unsigned i = 0; i < workerCount_; ++i) {
        threads_.emplace_back([this, i] { workerLoop(i); });
    }
}

JobSystem::~JobSystem()
{
    shutdown(); // drena filas, encerra e faz join — nenhum thread vaza
}

void JobSystem::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        if (shutdownDone_.exchange(true, std::memory_order_acq_rel)) {
            return; // idempotente: segunda chamada é no-op
        }
        running_.store(false, std::memory_order_release);
        wakeCv_.notify_all(); // desperta esperas de waitAll (avaliação do predicado)
    }
    // acorda TODOS os estacionados para drenar e encerrar — a contagem
    // persistente do semáforo garante que nenhum fique bloqueado
    workAvailable_.release(workerCount_);
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void JobSystem::waitAll()
{
    std::unique_lock<std::mutex> lock(wakeMutex_);
    wakeCv_.wait(lock, [this] {
        return queued_.load(std::memory_order_acquire) == 0
               && inFlight_.load(std::memory_order_acquire) == 0;
    });
}

std::optional<detail::Task> JobSystem::tryPopOwn(unsigned id)
{
    detail::WorkerQueue& queue = *queues_[id];
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.jobs.empty()) {
        return std::nullopt;
    }
    detail::Task task = std::move(queue.jobs.back()); // LIFO local
    queue.jobs.pop_back();
    transferToInFlight(); // inFlight++ antes de queued--
    return task;
}

std::optional<detail::Task> JobSystem::trySteal(unsigned id)
{
    // Varredura circular a partir do vizinho: front = mais antigo (FIFO).
    for (unsigned offset = 1; offset <= workerCount_; ++offset) {
        detail::WorkerQueue& queue = *queues_[(id + offset) % workerCount_];
        std::lock_guard<std::mutex> lock(queue.mutex);
        if (queue.jobs.empty()) {
            continue;
        }
        detail::Task task = std::move(queue.jobs.front());
        queue.jobs.pop_front();
        transferToInFlight(); // inFlight++ antes de queued--
        return task;
    }
    return std::nullopt;
}

void JobSystem::executeTask(detail::Task&& task)
{
    // inFlight_ já foi incrementado na transferência (tryPopOwn/trySteal).

#if ENG_JOBS_DETAIL_CATCH
    try {
        task.callable.invoke();
    } catch (...) {
        std::lock_guard<std::mutex> lock(task.state->mutex);
        task.state->exception = std::current_exception();
    }
#else
    task.callable.invoke(); // sem captura: propaga no worker → terminate
#endif

    {
        std::lock_guard<std::mutex> lock(task.state->mutex);
        task.state->done = true;
    }
    task.state->cv.notify_all();
    task.state.reset();

    if (inFlight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Último job concluiu: acorda esperas de waitAll.
        std::lock_guard<std::mutex> lock(wakeMutex_);
        wakeCv_.notify_all();
    }
}

void JobSystem::workerLoop(unsigned id)
{
    for (;;) {
        if (auto task = tryPopOwn(id)) {
            executeTask(std::move(*task));
            continue;
        }
        if (auto task = trySteal(id)) {
            executeTask(std::move(*task));
            continue;
        }

        if (!running_.load(std::memory_order_acquire)
            && queued_.load(std::memory_order_acquire) == 0) {
            return; // desligado e drenado: encerra o worker
        }
        // Estaciona no semáforo: a contagem é persistente (release antes de
        // acquire é seguro — sem categoria de lost-wakeup; ADR-023). Acorda
        // com novo trabalho ou com os releases do shutdown.
        workAvailable_.acquire();
        // Volta ao topo: pop/steal de novo.
    }
}

} // namespace eng::jobs
