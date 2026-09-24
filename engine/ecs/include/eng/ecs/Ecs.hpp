#pragma once

/// eng::ecs — world de entidades com sparse-sets e handles geracionais
///.
///
/// Modelo (decisões completas em ADR-024):
///   - `Entity = { index, generation }`. Índices são reciclados via free-list;
///     a geração do slot incrementa a cada destruição — handles antigos
///     (geração velha) tornam-se obsoletos.
///   - Operações com handle obsoleto são NO-OP SEGURO: `destroy`/`remove`
///     devolvem false, `get`/`emplace` devolvem nullptr, `has` devolve false,
///     `valid` devolve false. Nunca crash, nunca abort.
///   - Storage por tipo de componente: **sparse-set** (dense packed +
///     sparse índice→posição). Remoção por swap-and-pop com move-CONSTRUCT
///     (T precisa ser move-constructible; noexcept preferido — ADR-024).
///   - `emplace<T>` em entidade que já tem T: SUBSTITUI o componente
///     (documentado; devolve o ponteiro do novo valor).
///   - `each<Ts...>`: visita entidades que possuem TODOS os Ts, NA ORDEM de
///     inserção do menor pool. CONST-CORRECT: `World` mutável entrega
///     `fn(Entity, Ts&...)`; `const World` entrega `fn(Entity, const Ts&...)`.
///   - `each` usa snapshot do menor pool: mutação DURANTE a iteração é segura
///     — destruir a entidade corrente ou outra, remover componentes, criar
///     entidades novas (estas não aparecem na rodada corrente). Nenhuma
///     entidade é visitada duas vezes (detalhes e custo em ADR-024).
///   - Sem dependência de `eng::reflect` no código (missão §B.4): a aresta
///     do grafo é declarada no CMake para integrações futuras.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eng::ecs {

/// Handle de entidade: índice do slot + geração (detecção de obsolescência).
struct Entity {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] friend bool operator==(const Entity&, const Entity&) = default;
    [[nodiscard]] friend bool operator!=(const Entity&, const Entity&) = default;
};

} // namespace eng::ecs

/// Hash de Entity (POD trivial) — habilita uso em unordered containers.
template<>
struct std::hash<eng::ecs::Entity> {
    [[nodiscard]] std::size_t operator()(const eng::ecs::Entity& e) const noexcept
    {
        const std::uint64_t combined =
            (static_cast<std::uint64_t>(e.generation) << 32) | e.index;
        return std::hash<std::uint64_t>{}(combined);
    }
};

namespace eng::ecs {

class World;

namespace detail {

/// Chave por tipo SEM RTTI (mesma técnica de eng::events/ADR-022):
/// endereço de membro estático inline por instanciação — único por T.
template<typename T>
struct TypeTag {
    static constexpr char token = 0;
};

template<typename T>
[[nodiscard]] const void* typeKey() noexcept
{
    return &TypeTag<T>::token;
}

// =============================================================================
// PoolBase — interface não-template dos pools (remoção tipada é virtual:
// swap-and-pop precisa mover T; contagem e entidade-por-posição não-template).
// =============================================================================

class PoolBase {
public:
    PoolBase() = default;
    virtual ~PoolBase() = default;
    PoolBase(const PoolBase&) = delete;
    PoolBase& operator=(const PoolBase&) = delete;

    /// Remove o componente de `e` (no-op se ausente). Chamado por
    /// World::destroy para limpar todos os pools.
    virtual void remove(Entity e) = 0;

    [[nodiscard]] virtual std::size_t count() const noexcept = 0;
    [[nodiscard]] virtual Entity entityAt(std::size_t densePos) const = 0;

protected:
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);
};

// =============================================================================
// Pool<T> — sparse-set: dense_ (componentes packed) + entities_ (paralelo) +
// sparse_ (e.index → posição no dense; kNpos = ausente). Validação de geração
// por comparação completa do Entity armazenado — slot reciclado produz
// Entity diferente e o lookup falha (no-op seguro).
// Pré-condição: World::emplace só encaminha entidades VÁLIDAS.
// =============================================================================

template<typename T>
class Pool final : public PoolBase {
    static_assert(std::is_move_constructible_v<T>,
                  "componente precisa ser move-constructible (swap-and-pop, ADR-024)");

public:
    /// Constrói T(args...) na entidade. Já possui T → substitui
    /// (destroy + construct in place — só move-ctor é exigido de T).
    template<typename... Args>
    T* emplace(Entity e, Args&&... args)
    {
        const std::size_t pos = densePos(e);
        if (pos != kNpos) {
            T& slot = dense_[pos];
            slot.~T();
            new (&slot) T(std::forward<Args>(args)...);
            entities_[pos] = e;
            return &slot;
        }
        growSparseTo(static_cast<std::size_t>(e.index) + 1);
        sparse_[e.index] = dense_.size();
        dense_.emplace_back(std::forward<Args>(args)...);
        entities_.push_back(e);
        return &dense_.back();
    }

    [[nodiscard]] T* get(Entity e)
    {
        const std::size_t pos = densePos(e);
        return pos != kNpos ? &dense_[pos] : nullptr;
    }

    [[nodiscard]] const T* get(Entity e) const
    {
        const std::size_t pos = densePos(e);
        return pos != kNpos ? &dense_[pos] : nullptr;
    }

    [[nodiscard]] bool has(Entity e) const { return densePos(e) != kNpos; }

    void remove(Entity e) override
    {
        const std::size_t pos = densePos(e);
        if (pos == kNpos) {
            return; // ausente: no-op
        }
        sparse_[e.index] = kNpos;

        // Swap-and-pop com move-CONSTRUCT (move-assign não é exigido de T).
        const std::size_t last = dense_.size() - 1;
        if (pos != last) {
            T& slot = dense_[pos];
            slot.~T();
            new (&slot) T(std::move(dense_[last]));
            entities_[pos] = entities_[last];
        }
        dense_.pop_back();
        entities_.pop_back();
        if (pos != last) {
            sparse_[entities_[pos].index] = pos; // dono do componente movido
        }
    }

    [[nodiscard]] std::size_t count() const noexcept override { return dense_.size(); }

    [[nodiscard]] Entity entityAt(std::size_t densePos) const override
    {
        return entities_[densePos];
    }

private:
    /// Posição no dense_ para `e`, comparando o Entity COMPLETO (geração
    /// divergente — slot reciclado — falha). kNpos se ausente.
    [[nodiscard]] std::size_t densePos(Entity e) const
    {
        if (e.index >= sparse_.size()) {
            return kNpos;
        }
        const std::size_t pos = sparse_[e.index];
        if (pos == kNpos || pos >= dense_.size()) {
            return kNpos;
        }
        return entities_[pos] == e ? pos : kNpos;
    }

    void growSparseTo(std::size_t size)
    {
        if (sparse_.size() < size) {
            sparse_.resize(size, kNpos);
        }
    }

    std::vector<T> dense_;          ///< componentes packed
    std::vector<Entity> entities_;  ///< entidade dona de cada dense_[i]
    std::vector<std::size_t> sparse_; ///< e.index → posição em dense_ (kNpos = sem)
};

} // namespace detail

// =============================================================================
// World — contêiner de entidades + componentes. Não é singleton.
// =============================================================================

class World final {
public:
    World() = default;
    ~World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /// Cria uma entidade viva. Índices reciclados quando disponíveis.
    [[nodiscard]] Entity create();

    /// Destrói a entidade e TODOS os seus componentes. Retorna false se o
    /// handle é obsoleto (no-op seguro). A geração do slot incrementa —
    /// handles antigos jamais resurrectam a entidade.
    bool destroy(Entity e);

    /// Handle aponta para uma entidade viva com a geração corrente?
    [[nodiscard]] bool valid(Entity e) const noexcept;

    /// Quantidade de entidades vivas.
    [[nodiscard]] std::size_t size() const noexcept { return aliveCount_; }

    /// Constrói (ou substitui) o componente T na entidade.
    /// Handle obsoleto → nullptr (no-op seguro).
    template<typename T, typename... Args>
    [[nodiscard]] T* emplace(Entity e, Args&&... args)
    {
        if (!valid(e)) {
            return nullptr;
        }
        return poolFor<T>().emplace(e, std::forward<Args>(args)...);
    }

    /// Remove o componente T. false: ausente ou handle obsoleto (no-op).
    template<typename T>
    bool remove(Entity e)
    {
        detail::Pool<T>* pool = findPool<T>();
        if (pool == nullptr || !pool->has(e)) {
            return false;
        }
        pool->remove(e);
        return true;
    }

    /// Ponteiro para o componente (nullptr se ausente/obsoleto).
    template<typename T>
    [[nodiscard]] T* get(Entity e)
    {
        detail::Pool<T>* pool = findPool<T>();
        return pool != nullptr ? pool->get(e) : nullptr;
    }

    template<typename T>
    [[nodiscard]] const T* get(Entity e) const
    {
        const detail::Pool<T>* pool = findPool<T>();
        return pool != nullptr ? pool->get(e) : nullptr;
    }

    template<typename T>
    [[nodiscard]] bool has(Entity e) const
    {
        const detail::Pool<T>* pool = findPool<T>();
        return pool != nullptr && pool->has(e);
    }

    /// Componentes do tipo T armazenados.
    template<typename T>
    [[nodiscard]] std::size_t componentCount() const
    {
        const detail::Pool<T>* pool = findPool<T>();
        return pool != nullptr ? pool->count() : 0;
    }

    /// Visita entidades que possuem TODOS os Ts, na ordem de inserção do
    /// menor pool. fn(Entity, Ts&...). Snapshot: mutação durante a iteração
    /// é segura (ver cabeçalho do módulo e ADR-024).
    template<typename... Ts, typename Fn>
    void each(Fn&& fn)
    {
        static_assert(sizeof...(Ts) > 0, "each<Ts...> requer ao menos um componente");
        static_assert(std::is_invocable_v<Fn&, Entity, Ts&...>,
                      "fn deve ser invocável como fn(Entity, Ts&...)");
        for (const Entity e : eachCandidates<Ts...>()) {
            if (((get<Ts>(e) != nullptr) && ...)) {
                fn(e, *get<Ts>(e)...);
            }
        }
    }

    /// Versão const: fn(Entity, const Ts&...).
    template<typename... Ts, typename Fn>
    void each(Fn&& fn) const
    {
        static_assert(sizeof...(Ts) > 0, "each<Ts...> requer ao menos um componente");
        static_assert(std::is_invocable_v<Fn&, Entity, const Ts&...>,
                      "fn deve ser invocável como fn(Entity, const Ts&...)");
        for (const Entity e : eachCandidates<Ts...>()) {
            if (((get<Ts>(e) != nullptr) && ...)) {
                fn(e, *get<Ts>(e)...);
            }
        }
    }

private:
    template<typename T>
    [[nodiscard]] detail::Pool<T>& poolFor()
    {
        const void* key = detail::typeKey<T>();
        if (const auto it = pools_.find(key); it != pools_.end()) {
            return static_cast<detail::Pool<T>&>(*it->second);
        }
        auto pool = std::make_unique<detail::Pool<T>>();
        auto* raw = pool.get();
        pools_.emplace(key, std::move(pool));
        return *raw;
    }

    template<typename T>
    [[nodiscard]] detail::Pool<T>* findPool() noexcept
    {
        const auto it = pools_.find(detail::typeKey<T>());
        return it != pools_.end() ? static_cast<detail::Pool<T>*>(it->second.get()) : nullptr;
    }

    template<typename T>
    [[nodiscard]] const detail::Pool<T>* findPool() const noexcept
    {
        const auto it = pools_.find(detail::typeKey<T>());
        return it != pools_.end() ? static_cast<const detail::Pool<T>*>(it->second.get()) : nullptr;
    }

    /// Snapshot das entidades do MENOR pool entre Ts (vazio se algum tipo
    /// não tem pool). Ordem de inserção preservada.
    template<typename... Ts>
    [[nodiscard]] std::vector<Entity> eachCandidates() const
    {
        const detail::PoolBase* pools[] = {findPool<Ts>()...};
        const detail::PoolBase* smallest = nullptr;
        for (const detail::PoolBase* pool : pools) {
            if (pool == nullptr) {
                return {}; // tipo nunca usado → conjunto vazio
            }
            if (smallest == nullptr || pool->count() < smallest->count()) {
                smallest = pool;
            }
        }
        std::vector<Entity> snapshot;
        snapshot.reserve(smallest->count());
        for (std::size_t i = 0; i < smallest->count(); ++i) {
            snapshot.push_back(smallest->entityAt(i));
        }
        return snapshot;
    }

    struct Slot {
        std::uint32_t generation = 0;
        bool alive = false;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeList_;
    std::unordered_map<const void*, std::unique_ptr<detail::PoolBase>> pools_;
    std::size_t aliveCount_ = 0;
};

} // namespace eng::ecs
