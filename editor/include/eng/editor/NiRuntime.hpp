#pragma once

/// eng::editor::NiRuntime — runtime de scripts do editor (FASE 11).
///
/// Posse e ciclo (ADR-044): vive DENTRO do EditorDocument e aponta para o
/// CLONE de Play. play(): compila os NiScriptComponent do clone, cria
/// instâncias (ordem determinística de varredura do World), roda @init e
/// `up start`. tick(): `up update` por instância (orçamento por chamada
/// — design §6.2). stop(): `up destroy` best-effort e DESCARTE (o
/// bindings aponta para o world do clone — nunca sobrevive ao stop).
///
/// Host: delta do frame corrente + ações do runtimeInput_ + spawn/find/
/// despawn no clone. Bindings: açúcar de nó (position/rotation/scale/
/// name) + componentes do catálogo — REFLETIDOS por offset (mesmo
/// mecanismo do Inspector, auditoria D2).

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "eng/events/Events.hpp"
#include "eng/niscript/NiScript.hpp"
#include "eng/niscript/NiVm.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/scene/SceneEvents.hpp"

namespace eng::editor {

/// P4.1 (T2/D5) — diagnóstico do runtime de scripts, VISÍVEL ao autor.
/// O D5 device-verificado: script com erro compilava "em silêncio" (só
/// log) e o autor via "nada acontece" no Play. Estes contadores são a
/// fonte da UI (toast/painel do editor) e do diagnóstico persistido.
struct NiScriptStats {
    std::uint32_t scriptsFound{0};      ///< componentes com source não-vazio
    std::uint32_t scriptsCompiled{0};   ///< compilação OK
    std::uint32_t scriptsFailed{0};     ///< compilação FALHOU (desabilitado)
    std::uint32_t instances{0};         ///< instâncias vivas (== compiled)
    std::uint64_t ticks{0};             ///< eventos `up update` executados
    std::uint64_t firstUpdateTick{0};   ///< nº do tick da 1ª update (0=nenhum)
    std::uint32_t faults{0};            ///< faults de runtime registrados
    /// Primeiro erro de compilação ("linha:col: mensagem") — vazio se OK.
    std::string firstCompileError;
    /// Entidade (index) da 1ª falha de compilação — para a UI apontar o nó.
    std::uint32_t firstFailedEntity{0xFFFFFFFFu};
    /// Último fault de runtime ("mensagem @ entidade") — vazio se nenhum.
    std::string lastFaultMessage;

    [[nodiscard]] bool healthy() const noexcept
    {
        return scriptsFailed == 0;
    }
};

class NiRuntime final {
public:
    NiRuntime();            // HostImpl é interno do .cpp (Pimpl leve)
    ~NiRuntime();
    NiRuntime(const NiRuntime&) = delete;
    NiRuntime& operator=(const NiRuntime&) = delete;

    /// Prepara o runtime sobre o CLONE (chamado no play(), depois do
    /// clone existir). Compila todos os scripts (erros: log + STATS +
    /// diagnóstico persistido — P4.1: nunca mais silêncio), cria
    /// instâncias e roda @init na ordem de criação.
    void start(eng::scene::Scene& runtimeScene);

    /// `up start` em todas as instâncias (após todos @init — ordem
    /// determinística documentada em docs/ni-script/07).
    void fireStart();

    /// `up update` em todas as instâncias.
    /// `deltaSeconds` alimenta delta(); o orçamento é por EVENTO
    /// (kDefaultBudget — design §6.2).
    void tick(float deltaSeconds);

    /// `up destroy` best-effort + descarte de TODO o estado.
    void shutdown() noexcept;

    [[nodiscard]] bool empty() const noexcept { return set_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return set_.size(); }

    /// Faults registrados do último tick (diagnóstico — null quando ok).
    [[nodiscard]] std::vector<const eng::ni::NiScriptState*> instances()
        const;

    /// P4.1 (T2/D5): estatística VISÍVEL do runtime (compilação, ticks,
    /// faults) — o editor exibe ao autor (toast/painel), não só no log.
    [[nodiscard]] const NiScriptStats& stats() const noexcept
    {
        return stats_;
    }

    /// Liga a fonte de AÇÕES (o EditorDocument conecta o runtimeInput_
    /// do clone). `phase`: 0=down, 1=pressed, 2=released.
    void setActionQuery(bool (*query)(std::string_view, int, void*),
                        void* user) noexcept;

    /// P4.7.0 B5: kinematic_sweep — quando ON (default), o verbo `move`
    /// de um KINEMATIC com collider é VARRIDO (TOI+slide). O documento
    /// liga com o setting da cena no play().
    void setKinematicSweep(bool enabled) noexcept
    {
        kinematicSweep_ = enabled;
    }
    [[nodiscard]] bool kinematicSweep() const noexcept
    {
        return kinematicSweep_;
    }

    /// P4.7.0 B6: LOGIC LOD — filtro de visibilidade por script (nullptr =
    /// sempre roda, comportamento pré-P4.7). O filtro retorna "roda?";
    /// a política (off-screen pula; opt-out sempre roda) é do HOST do
    /// filtro — o runtime só obedece. Instalado no play() com a cena do
    /// clone; morre com o stop (o ponteiro aponta para o documento).
    void setLodFilter(bool (*filter)(void* user, eng::ecs::Entity self),
                      void* user) noexcept
    {
        lodFilter_ = filter;
        lodFilterUser_ = user;
    }

private:
    struct HostImpl;

    /// P4.7.0 Bloco 1: inscreve os eventos de gameplay no barramento da
    /// cena (on_hit/on_enter/on_exit/on_visible/on_invisible). A inscrição
    /// é RAII e vive APENAS entre start/shutdown (nunca sobrevive à cena).
    template<typename E>
    void subscribeGameEvent()
    {
        eventSubscriptions_.push_back(scene_->events().subscribe<E>(
            [this](const E& event) {
                if constexpr (std::is_same_v<E, eng::scene::HitEvent>) {
                    dispatchHitEvent(event);
                } else if constexpr (std::is_same_v<E,
                                         eng::scene::TriggerEvent>) {
                    dispatchTriggerEvent(event);
                } else {
                    static_assert(std::is_same_v<E,
                                      eng::scene::VisibilityEvent>,
                                  "evento de gameplay não suportado");
                    dispatchVisibilityEvent(event);
                }
            }));
    }

    /// Handlers: roda `up <nome>` nas instâncias cujo self == entidade do
    /// evento (inline durante o publish — física/ordem determinística).
    void dispatchHitEvent(const eng::scene::HitEvent& event);
    void dispatchTriggerEvent(const eng::scene::TriggerEvent& event);
    void dispatchVisibilityEvent(const eng::scene::VisibilityEvent& event);
    void runHandlerOn(eng::ecs::Entity self, std::string_view handler);

    [[nodiscard]] bool queryAction_(std::string_view action, int phase) const;
    [[nodiscard]] eng::ni::NiExecContext::Params params() const;

    eng::ni::NiNativeTable natives_;
    eng::ni::NiBindingTable bindings_;
    /// Estado dos adaptadores do catálogo (o binding guarda ponteiro cru).
    std::vector<std::shared_ptr<void>> bindingState_;
    eng::ni::NiInstanceSet set_;
    eng::ni::NiVm vm_;
    std::unique_ptr<HostImpl> host_;
    eng::scene::Scene* scene_ = nullptr;
    float delta_ = 0.f;
    bool (*actionQuery_)(std::string_view, int, void*) = nullptr;
    void* actionQueryUser_ = nullptr;
    NiScriptStats stats_{};
    /// P4.7.0 Bloco 1: inscrições RAII nos eventos da cena (canceladas no
    /// shutdown — antes do bus morrer com a cena de Play).
    std::vector<eng::events::Subscription> eventSubscriptions_;
    /// Recursão: handler de evento pode emitir contato (spawn/move) →
    /// publicar de novo? Guarda de profundidade (um nível de script).
    bool dispatching_ = false;
    /// P4.7.0 B5: varredura do kinematic no `move` (default ON).
    bool kinematicSweep_ = true;
    /// P4.7.0 B6: filtro do logic LOD (nullptr = sempre roda).
    bool (*lodFilter_)(void* user, eng::ecs::Entity self) = nullptr;
    void* lodFilterUser_ = nullptr;
};

} // namespace eng::editor
