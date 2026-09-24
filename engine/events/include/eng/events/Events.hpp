#pragma once

/// eng::events — barramento de eventos com lifetime RAII.
///
/// Semânticas garantidas (testadas, ver ADR-022):
///   - `publish` executa handlers NA ORDEM de inscrição (determinístico).
///   - `Subscription` é move-only RAII: destruição/movida-de cancela a
///     inscrição; cancelamento é idempotente.
///   - Desinscrição durante um dispatch:
///       * handler ainda não chamado nesta rodada → NÃO será chamado;
///       * handler corrente (chamando o cancelamento) → seguro; o handler
///         termina e o slot limpa a entrada após a rodada.
///   - Inscrição durante um dispatch → NÃO é chamada na rodada corrente;
///     entra em rodadas subsequentes.
///   - publish aninhado (reentrante), do mesmo ou de outro tipo de evento →
///     suportado; remoção física de entradas é adiada até a profundidade de
///     dispatch do slot voltar a zero.
///
/// O que este módulo NÃO faz (por decisão, ver ADR-022):
///   - NÃO é thread-safe: todas as chamadas a um mesmo EventBus
///     (subscribe/publish/cancelamento) devem vir da mesma thread ou ser
///     serializadas externamente. Reentrância NA MESMA thread é suportada.
///   - handlers NÃO recebem eventos por valor mutável (sempre `const E&`).
///
/// Hot path: publish = lookup O(1) no mapa de slots + travessia da
/// lista de handlers + chamada por ponteiro de função. SEM std::function —
/// type-erasure com small-buffer (48 bytes) + ponteiros de função; handlers
/// maiores que o buffer ou com alinhamento excedente vão para o heap.
/// publish NUNCA aloca.

#include <cstddef>
#include <list>
#include <memory>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace eng::events {

class Subscription;

namespace detail {

// =============================================================================
// Handler — callable type-erased void(const E&) com small-buffer.
// Construído IN-PLACE (emplaceFrom); cópia E movimento deletados: o Handler
// vive em um nó de std::list (endereço estável) e nunca precisa migrar.
// =============================================================================

class Handler {
public:
    Handler() = default;
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;
    ~Handler();

    /// Constrói F in-place, tipado para eventos E. Pré-condição: !invoke_
    /// (handler vazio). sizeof(G) > buffer ou alignof(G) > alinhamento do
    /// buffer → heap (assinado em tempo de compilação — sem custo no caminho
    /// inline).
    template<typename F, typename E>
    void emplaceFrom(F&& functor)
    {
        using G = std::decay_t<F>;
        static_assert(std::is_invocable_v<G&, const E&>,
                      "handler de evento deve ser invocável como f(const E&)");

        constexpr bool kInline = sizeof(G) <= kSmallSize
                                 && alignof(G) <= alignof(HandlerStorage);
        if constexpr (kInline) {
            obj_ = &storage_;
        } else {
            obj_ = ::operator new(sizeof(G));
            heap_ = true;
        }
        new (obj_) G(std::forward<F>(functor));
        invoke_ = &invokeImpl<G, E>;
        destroy_ = &destroyImpl<G>;
    }

    /// Invoca o handler armazenado para `event` (apontado como void — o tipo
    /// real foi fixado na construção). Handler vazio → no-op defensivo.
    void invoke(const void* event);

    [[nodiscard]] bool empty() const noexcept { return invoke_ == nullptr; }

private:
    struct HandlerStorage {
        alignas(std::max_align_t) std::byte bytes[48];
    };

    template<typename G, typename E>
    static void invokeImpl(void* obj, const void* event)
    {
        (*static_cast<G*>(obj))(*static_cast<const E*>(event));
    }

    template<typename G>
    static void destroyImpl(void* obj) noexcept
    {
        static_cast<G*>(obj)->~G();
    }

    static constexpr std::size_t kSmallSize = sizeof(HandlerStorage);

    HandlerStorage storage_{};
    void* obj_ = nullptr;                    ///< &storage_ ou heap
    void (*invoke_)(void*, const void*) = nullptr;
    void (*destroy_)(void*) noexcept = nullptr;
    bool heap_ = false;
};

// =============================================================================
// Entrada e slot — Entry vive em std::list (endereços estáveis sob
// inserção/remoção). `dead` é tombstone: remoção física é adiada até o fim
// do dispatch (profundidade 0) para que handlers em execução nunca acessem
// memória liberada.
// =============================================================================

struct Entry {
    Handler handler;
    bool dead = false;
};

class SlotBase {
public:
    SlotBase() = default;
    virtual ~SlotBase() = default;
    SlotBase(const SlotBase&) = delete;
    SlotBase& operator=(const SlotBase&) = delete;

    /// Cancela a inscrição da entrada. Durante dispatch (depth > 0): apenas
    /// marca dead — a remoção física ocorre quando a última rodada terminar.
    /// Chamado SOMENTE por Subscription (que esquece o ponteiro em seguida —
    /// invariante: dead ⇒ nenhuma referência externa viva).
    void release(Entry* entry) noexcept;

    [[nodiscard]] std::size_t liveCount() const noexcept;

protected:
    /// Guarda de profundidade: incrementa na entrada da rodada, decrementa na
    /// saída (inclusive por exceção em builds de teste) e varre as entradas
    /// mortas quando a profundidade volta a zero.
    struct DispatchGuard {
        explicit DispatchGuard(SlotBase& slot) noexcept : slot_(slot) { ++slot_.depth_; }
        ~DispatchGuard()
        {
            if (--slot_.depth_ == 0) {
                slot_.sweepDead();
            }
        }
        DispatchGuard(const DispatchGuard&) = delete;
        DispatchGuard& operator=(const DispatchGuard&) = delete;

    private:
        SlotBase& slot_;
    };

    std::list<Entry> entries_;
    int depth_ = 0;

private:
    void sweepDead();
};

/// Chave por tipo de evento SEM RTTI: endereço de um membro estático inline
/// por instanciação — único por E em todo o processo, estável entre TUs.
template<typename E>
struct EventKeyTag {
    static constexpr char token = 0;
};

template<typename E>
[[nodiscard]] const void* eventKey() noexcept
{
    return &EventKeyTag<E>::token;
}

/// Slot tipado por evento: inscreve, publica na ordem de inscrição.
template<typename E>
class Slot final : public SlotBase {
public:
    /// Inscreve `functor` (construção in-place no fim da lista). Retorna a
    /// entrada — a Subscription devolvida ao chamador é a única dona do
    /// cancelamento.
    template<typename F>
    Entry* subscribe(F&& functor)
    {
        entries_.emplace_back();
        Entry& entry = entries_.back();
        entry.handler.emplaceFrom<F, E>(std::forward<F>(functor));
        return &entry;
    }

    /// Rodada de dispatch: percorre da primeira até a ÚLTIMA ENTRADA
    /// COMPROMETIDA no início da rodada (inclusive). Inscrições feitas
    /// durante a rodada ficam além desse limite — esperam a próxima.
    /// Entradas mortas são puladas (não invocadas), nunca removidas aqui.
    void publish(const E& event)
    {
        if (entries_.empty()) {
            return;
        }
        Entry* lastCommitted = &entries_.back();
        DispatchGuard guard{*this};
        for (auto it = entries_.begin();; ++it) {
            Entry& entry = *it;
            if (!entry.dead) {
                entry.handler.invoke(&event);
            }
            if (&entry == lastCommitted) {
                break; // para ANTES de tocar entradas anexadas nesta rodada
            }
        }
    }
};

} // namespace detail

// =============================================================================
// Subscription — RAII move-only. Destruição (ou unsubscribe()) cancela a
// inscrição. Pré-condição documentada: NÃO sobreviver ao EventBus
// que a criou.
// =============================================================================

class Subscription final {
public:
    Subscription() noexcept = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : slot_(std::exchange(other.slot_, nullptr)),
          entry_(std::exchange(other.entry_, nullptr)) {}

    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other) {
            unsubscribe();
            slot_ = std::exchange(other.slot_, nullptr);
            entry_ = std::exchange(other.entry_, nullptr);
        }
        return *this;
    }

    ~Subscription() { unsubscribe(); }

    /// Cancelamento idempotente — mesmo efeito da destruição.
    void unsubscribe() noexcept
    {
        if (entry_ != nullptr) {
            slot_->release(entry_);
            entry_ = nullptr; // invariante: dead ⇒ Subscription esqueceu
            slot_ = nullptr;
        }
    }

    /// Inscrição ainda ativa (não cancelada/não movida-de)?
    [[nodiscard]] bool alive() const noexcept { return entry_ != nullptr; }

private:
    friend class EventBus;

    Subscription(detail::SlotBase* slot, detail::Entry* entry) noexcept
        : slot_(slot), entry_(entry) {}

    detail::SlotBase* slot_ = nullptr;
    detail::Entry* entry_ = nullptr;
};

// =============================================================================
// EventBus — barramento por processo de jogo (não é singleton; cada mundo/scene
// cria o seu). Não thread-safe (ver cabeçalho do módulo e ADR-022).
// =============================================================================

class EventBus final {
public:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    ~EventBus() = default;

    /// Inscreve `handler` para eventos do tipo E (invocável como
    /// `f(const E&)`). Aloca a entrada; devolve Subscription RAII.
    /// Complexidade: O(1) amortizado.
    template<typename E, typename F>
    [[nodiscard]] Subscription subscribe(F&& handler)
    {
        detail::Slot<E>& slot = slotFor<E>();
        detail::Entry* entry = slot.subscribe(std::forward<F>(handler));
        return Subscription(&slot, entry);
    }

    /// Publica `event` para todas as inscrições vivas comprometidas no início
    /// da rodada, na ordem de inscrição. Sem subscribers → no-op. NUNCA aloca.
    /// Reentrante na mesma thread (política de rodadas: ADR-022).
    template<typename E>
    void publish(const E& event)
    {
        if (const auto it = slots_.find(detail::eventKey<E>()); it != slots_.end()) {
            static_cast<detail::Slot<E>&>(*it->second).publish(event);
        }
    }

    /// Inscrições vivas para E. Entradas canceladas durante um dispatch ativo
    /// já não contam: o cancelamento é imediato na semântica observável (a
    /// remoção física é que é adiada — ver ADR-022).
    template<typename E>
    [[nodiscard]] std::size_t subscriberCount() const
    {
        if (const auto it = slots_.find(detail::eventKey<E>()); it != slots_.end()) {
            return it->second->liveCount();
        }
        return 0;
    }

private:
    template<typename E>
    [[nodiscard]] detail::Slot<E>& slotFor()
    {
        const void* key = detail::eventKey<E>();
        if (const auto it = slots_.find(key); it != slots_.end()) {
            return static_cast<detail::Slot<E>&>(*it->second);
        }
        auto slot = std::make_unique<detail::Slot<E>>();
        auto* raw = slot.get();
        slots_.emplace(key, std::move(slot));
        return *raw;
    }

    std::unordered_map<const void*, std::unique_ptr<detail::SlotBase>> slots_;
};

} // namespace eng::events
