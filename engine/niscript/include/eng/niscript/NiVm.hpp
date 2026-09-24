#pragma once

/// eng::ni — VM do NI-Script.
///
/// Execução: stack machine com ORÇAMENTO GLOBAL de instruções (§6.2 —
/// determinístico, sem threads); regiões `repair`/`timeout`;
/// Faults reparáveis (nunca exceções — ADR-004).
///
/// Instância (NiScriptState) = programa compartilhado + entidade self +
/// globais + links + lastFault. O CONJUNTO de instâncias (NiInstanceSet)
/// vive no host/consumidor e dá a propagação BFS de `emit`.
///
/// Determinismo: sem relógio/RNG/threads/hash-ordem; f64 IEEE sem
/// fast-math; iteração por ORDEM DE INSERÇÃO. Mesma cena + mesmos scripts +
/// mesma sequência de eventos ⇒ mesmos efeitos (testado).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eng/ecs/Ecs.hpp"
#include "eng/niscript/NiBindings.hpp"
#include "eng/niscript/NiProgram.hpp"
#include "eng/niscript/NiValue.hpp"

namespace eng::ni {

class NiVm;
class NiInstanceSet;
class NiScriptState;
struct NiExecOps; ///< helpers internos de execução (definidos em Vm.cpp)

/// Contexto de execução — estado interno de UMA execução de evento.
/// Nativos recebem `NiExecContext&` (host/bindings/instância corrente).
class NiExecContext final {
public:
    /// Parâmetros de UMA execução de evento (imutáveis durante a execução).
    struct Params {
        const NiNativeTable* natives = nullptr;
        NiHost* host = nullptr;                ///< nulos só se não usados
        const NiBindingTable* bindings = nullptr;
        NiInstanceSet* set = nullptr;          ///< propagação de emit
        std::uint64_t budget = kDefaultBudget; ///< instruções
    };

    /// Informação de trace (hook de depuração — §6.5).
    struct TraceInfo {
        std::uint32_t pc = 0;
        std::uint32_t line = 0;
        std::uint32_t col = 0;
        OpCode op = OpCode::CONST;
        std::size_t stackDepth = 0;
        std::size_t frameDepth = 0;
    };

    // --- acesso de NATIVOS (público por design: superfície dos bindings) ---
    [[nodiscard]] NiHost* host() noexcept { return params_.host; }
    [[nodiscard]] const NiBindingTable* bindings() const noexcept
    {
        return params_.bindings;
    }
    /// Instância cujo código está no TOPO da pilha (self/links/globais).
    [[nodiscard]] NiScriptState* currentInstance() noexcept
    {
        return frames_.empty() ? nullptr : frames_.back().owner;
    }

private:
    friend class NiVm;
    friend struct NiExecOps; // helpers internos (Vm.cpp)

    struct Region {
        enum class Kind : std::uint8_t { Repair, Timeout };
        Kind kind = Kind::Repair;
        std::size_t stackDepth = 0;   ///< topo da pilha na entrada
        std::size_t frameDepth = 0;   ///< frames.size() NA entrada (dono incluso)
        std::uint32_t contPc = 0;    ///< Repair: pc após a região
        std::uint64_t savedDeadline = 0; ///< deadline vigente ANTES da entrada
    };

    struct Frame {
        const NiProgram* program = nullptr;
        const NiFunc* func = nullptr;
        NiScriptState* owner = nullptr; ///< globais/self/links desta instância
        std::uint32_t pc = 0;
        std::uint32_t base = 0;          ///< base dos args+locais na pilha
        std::size_t regionBase = 0;      ///< regions.size() na entrada
        bool fromEmit = false;           ///< criado por emit
    };

    Params params_{};
    std::vector<NiValue> stack_;
    std::vector<Frame> frames_;
    std::vector<Region> regions_;
    std::uint64_t count_ = 0;       ///< contador global (monotônico — §6.2)
    std::uint64_t deadline_ = 0;    ///< deadline corrente (timeout §5.4)
    std::uint32_t emitDepth_ = 0;
    std::optional<NiFault> fault_;  ///< fault ATIVO (unwind) ou registrado
};

/// Estado de UMA instância de script (uma entidade dona).
class NiScriptState final {
public:
    NiScriptState(std::shared_ptr<const NiProgram> program,
                  eng::ecs::Entity self);

    [[nodiscard]] const NiProgram& program() const noexcept
    {
        return *program_;
    }
    [[nodiscard]] eng::ecs::Entity self() const noexcept { return self_; }
    [[nodiscard]] const std::optional<NiFault>& lastFault() const noexcept
    {
        return lastFault_;
    }
    void clearLastFault() noexcept { lastFault_.reset(); }

    /// Globais da instância (inspeção de testes — leitura).
    [[nodiscard]] const NiValue* global(std::string_view name) const noexcept;
    [[nodiscard]] const std::vector<ecs::Entity>& links() const noexcept
    {
        return links_;
    }

private:
    friend class NiVm;

    std::shared_ptr<const NiProgram> program_;
    eng::ecs::Entity self_{};
    std::vector<NiValue> globals_; ///< cópia dos zero-values do programa
    std::vector<ecs::Entity> links_; ///< ordem de inserção, sem duplicatas
    std::optional<NiFault> lastFault_;
};

/// Conjunto de instâncias do consumidor + propagação BFS de `emit`.
/// Ordem determinística: ordem de CRIAÇÃO (insertion order — §7.4).
class NiInstanceSet final {
public:
    NiScriptState& create(std::shared_ptr<const NiProgram> program,
                          ecs::Entity self);
    void clear() noexcept { instances_.clear(); }
    /// Remove as instâncias cujo `self` não passa em `keep` (entidades
    /// destruídas durante o jogo). Devolve quantas saíram.
    std::size_t removeIf(bool (*drop)(void* user, ecs::Entity self), void* user);
    [[nodiscard]] bool empty() const noexcept { return instances_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }

    /// Instâncias cujo self é `entity`, na ordem de criação (para o BFS do
    /// emit — chamado APENAS pelo VM).
    [[nodiscard]] std::vector<NiScriptState*> instancesOf(
        ecs::Entity entity) const;

    /// Dispara um evento em TODAS as instâncias, na ordem de criação.
    /// Devolve faults registrados (uma entrada por instância que falhou —
    /// instâncias SEM o evento não executam e não falham).
    [[nodiscard]] std::vector<std::optional<NiFault>> broadcast(
        const char* event, NiVm& vm, const NiExecContext::Params& params);

    /// Visão CONST determinística (ordem de criação) — uso do host/editor.
    [[nodiscard]] const std::vector<std::unique_ptr<NiScriptState>>&
    asVector() const noexcept
    {
        return instances_;
    }

private:
    friend class NiVm;
    std::vector<std::unique_ptr<NiScriptState>> instances_;
};

/// Máquina virtual — executa UM evento em UMA instância (design §7.4).
class NiVm final {
public:
    /// Hook de depuração: chamado a cada `stride` instruções.
    /// stride == 0 desliga. O hook vê o estado (const) — não pode mutar.
    void setTraceHook(std::function<void(const NiExecContext::TraceInfo&)> hook,
                      std::uint32_t stride) noexcept
    {
        traceHook_ = std::move(hook);
        traceStride_ = stride;
    }

    /// Executa `event` na instância. Sem handler → nullopt (nada a fazer).
    /// Com handler: roda até o fim, até o orçamento, ou até Fault fora de
    /// região (§5.3.5 — fault registrado em state.lastFault e devolvido).
    /// Parâmetros inválidos (tabela de nativos divergente) = erro fatal do
    /// HOST: devolve fault NativeError com mensagem explícita.
    [[nodiscard]] std::optional<NiFault> run(NiScriptState& state,
                                             std::string_view event,
                                             const NiExecContext::Params& params);

private:
    std::function<void(const NiExecContext::TraceInfo&)> traceHook_;
    std::uint32_t traceStride_ = 0;
};

} // namespace eng::ni
