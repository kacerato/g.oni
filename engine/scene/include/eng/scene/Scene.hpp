#pragma once

/// eng::scene — hierarquia de nós sobre o ECS com transforms (FASE 2, missão
/// §B.5).
///
/// Modelo (decisões completas em ADR-025):
///   - `Scene` é DONA de um `eng::ecs::World` (composição — ownership único).
///     Nó = entidade com os componentes `Hierarchy` (pai + filhos, ordem de
///     anexação) e `eng::math::Transform` (TRS local, identidade na criação).
///   - Floresta de raízes: múltiplas raízes são permitidas (`parent == kNoEntity`).
///   - `attach(child, parent)`: move o nó (desanexa do pai atual); idempotente
///     para o MESMO pai; devolve false para entidades inválidas, self-attach
///     ou **CICLO** (child ancestral de parent — detecção subindo a cadeia).
///   - `destroyNode(node)`: destrói o nó E TODOS os descendentes (cascata,
///     ordem determinística: folhas primeiro). Desanexa o nó do pai — o pai
///     (e irmãos) sobrevivem.
///   - Transform local: mutável via `localTransform(node)` (ponteiro direto).
///   - Mundo: DOIS caminhos explícitos —
///       * `updateWorldTransforms()` recomputa e CACHEIA todas as matrizes
///         (iterativo, sem recursão; pula referências obsoletas);
///         `worldMatrix(node)` lê o cache do ÚLTIMO update (nullptr se o nó
///         nunca foi coberto por um update).
///       * `computeWorldMatrix(node)` calcula NA HORA subindo a cadeia de
///         pais (O(profundidade)) — sempre corrente, sem cache.
///   - Entidades destruídas DIRETAMENTE via `world().destroy()` (bypass da
///     Scene) deixam referências obsoletas nas listas de filhos — elas são
///     PULADAS (eachChild/childCount/update) e limpas OPORTUNISTICAMENTE nas
///     operações de desanexação. O caminho suportado é `destroyNode`.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/ecs/Ecs.hpp"
#include "eng/events/Events.hpp"
#include "eng/math/Mat4.hpp"
#include "eng/math/Transform.hpp"
#include "eng/scene/Layers.hpp"
#include "eng/scene/Links.hpp"

namespace eng::scene {

/// Sentinela "sem entidade/pai": índice e geração máximos — nunca um handle
/// real (um slot só alcança esse índice após 2^32 criações).
inline constexpr eng::ecs::Entity kNoEntity{0xFFFFFFFFu, 0xFFFFFFFFu};

/// Componente interno da Scene: pai + filhos em ordem de anexação.
/// Acesso público apenas para fins de integração/teste — prefira a API de Scene.
struct Hierarchy {
    eng::ecs::Entity parent = kNoEntity;
    std::vector<eng::ecs::Entity> children;
};

/// Componente interno da Scene: cache da matriz local→mundo do último
/// `updateWorldTransforms()`.
struct WorldMatrix {
    eng::math::Mat4 matrix = eng::math::Mat4::identity();
};

class Scene final {
public:
    Scene() = default;
    ~Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Cria um nó RAIZ (sem pai) com transform local identidade.
    [[nodiscard]] eng::ecs::Entity createNode();

    /// Anexa `child` sob `parent` (desanexa do pai atual; ordem de anexação
    /// registrada). false: inválidos, child==parent, ou criaria ciclo.
    /// true idempotente quando já está sob `parent`.
    bool attach(eng::ecs::Entity child, eng::ecs::Entity parent);

    /// Solta `node` do pai atual (vira raiz). false: inválido ou já raiz.
    bool detach(eng::ecs::Entity node);

    /// Destrói `node` e TODOS os descendentes (folhas primeiro). Desanexa o
    /// nó do seu próprio pai. false: handle obsoleto (no-op seguro).
    bool destroyNode(eng::ecs::Entity node);

    /// Pai do nó — kNoEntity para raízes/inválidos.
    [[nodiscard]] eng::ecs::Entity parentOf(eng::ecs::Entity node) const noexcept;

    /// Filhos VÁLIDOS (referências obsoletas não contam — ADR-025).
    [[nodiscard]] std::size_t childCount(eng::ecs::Entity parent) const;

    /// Itera os filhos válidos de `parent` NA ORDEM de anexação.
    /// Snapshot interno: mutar a hierarquia durante a iteração é seguro.
    template<typename Fn>
    void eachChild(eng::ecs::Entity parent, Fn&& fn)
    {
        static_assert(std::is_invocable_v<Fn&, eng::ecs::Entity>,
                      "fn deve ser invocável como fn(eng::ecs::Entity)");
        const Hierarchy* hierarchy = hierarchyOf(parent);
        if (hierarchy == nullptr) {
            return;
        }
        const std::vector<eng::ecs::Entity> snapshot = hierarchy->children;
        for (const eng::ecs::Entity child : snapshot) {
            if (isNode(child)) {
                fn(child);
            }
        }
    }

    /// Versão CONST (FASE 8: caminhos de leitura do editor — viewport e
    /// snapshot de hierarquia — não podem mutar a cena). Mesma semântica.
    template<typename Fn>
    void eachChild(eng::ecs::Entity parent, Fn&& fn) const
    {
        static_assert(std::is_invocable_v<Fn&, eng::ecs::Entity>,
                      "fn deve ser invocável como fn(eng::ecs::Entity)");
        const Hierarchy* hierarchy = hierarchyOf(parent);
        if (hierarchy == nullptr) {
            return;
        }
        const std::vector<eng::ecs::Entity> snapshot = hierarchy->children;
        for (const eng::ecs::Entity child : snapshot) {
            if (isNode(child)) {
                fn(child);
            }
        }
    }

    /// Transform TRS local do nó (mutável). nullptr: handle obsoleto.
    [[nodiscard]] eng::math::Transform* localTransform(eng::ecs::Entity node);

    /// Transform TRS local do nó (somente leitura). nullptr: obsoleto.
    [[nodiscard]] const eng::math::Transform* localTransform(eng::ecs::Entity node) const;

    /// Recomputa e cacheia as matrizes mundo de TODOS os nós (top-down,
    /// iterativo). Chamar após editar transforms locais.
    void updateWorldTransforms();

    /// Matriz local→mundo do ÚLTIMO updateWorldTransforms() — nullptr se o nó
    /// não era coberto por nenhum update (ou handle obsoleto).
    [[nodiscard]] const eng::math::Mat4* worldMatrix(eng::ecs::Entity node) const;

    /// Matriz local→mundo calculada AGORA (sobe a cadeia de pais; O(prof.)).
    /// Nó inválido → identidade. Pai obsoleto (bypass) → tratado como raiz.
    [[nodiscard]] eng::math::Mat4 computeWorldMatrix(eng::ecs::Entity node) const;

    /// Nós vivos na cena.
    [[nodiscard]] std::size_t nodeCount() const noexcept;

    /// Handle aponta para um nó vivo da cena?
    [[nodiscard]] bool isNode(eng::ecs::Entity node) const noexcept;

    /// Mundo ECS da cena (para componentes de gameplay: meshes, física...).
    /// Destruição DIRETA de entidades no world contorna a Scene — ver
    /// cabeçalho do módulo (limpeza oportunista).
    [[nodiscard]] eng::ecs::World& world() noexcept { return world_; }
    [[nodiscard]] const eng::ecs::World& world() const noexcept { return world_; }

    // --- links tipados (evolução P0-5, ADR-051) ------------------------------

    /// Registry de links da cena (tipos, criação, consultas).
    [[nodiscard]] LinkRegistry& links() noexcept { return links_; }
    [[nodiscard]] const LinkRegistry& links() const noexcept { return links_; }

    /// Cria um link tipado `from → to`. Valida AMBAS as pontas (nós vivos)
    /// ANTES de tocar a registry — erros da registry (tipo não registrado,
    /// duplicado, ciclo) são propagados com contexto.
    [[nodiscard]] eng::core::Result<LinkId> createLink(std::string_view type,
                                                       eng::ecs::Entity from,
                                                       eng::ecs::Entity to);

    /// Destrói um link (no-op seguro com handle obsoleto).
    bool destroyLink(LinkId id);

    /// Remove links com ponta morta (bypass do world). Retorna quantos.
    std::size_t sweepLinks();

    // --- camadas (evolução P0-5, ADR-051) -------------------------------------

    /// Registry de camadas da cena (GAME/SUBGAME + nomeadas).
    [[nodiscard]] LayerRegistry& layers() noexcept { return layers_; }
    [[nodiscard]] const LayerRegistry& layers() const noexcept
    {
        return layers_;
    }

    /// Remove uma camada — REJEITADA enquanto qualquer entidade a
    /// referencia (varredura de LayerMember; sem fallback silencioso).
    [[nodiscard]] eng::core::Result<void> removeLayer(
        std::string_view name);

    /// Nome da camada de `node` (LayerMember → registry; GAME quando sem
    /// componente). string_view para o storage VIVO — não reter além da
    /// consulta. Camada referenciada mas ausente → GAME (defensivo; o
    /// serializer valida no load e removeLayer impede o órfão).
    [[nodiscard]] std::string_view layerOf(eng::ecs::Entity node) const noexcept;

    /// `node` participa do estágio na sua camada? Ausência de LayerMember
    /// = GAME (tudo participante). Consulta canônica de física/animação/
    /// partículas/viewport — ADR-051.
    /// true se o nó ou algum ancestral é um Molde (eng::scene::Template).
    [[nodiscard]] bool isTemplated(eng::ecs::Entity node) const noexcept;
    /// Participação só pela camada (ignora moldes) — o editor usa para
    /// desenhar moldes esmaecidos fora do Play.
    [[nodiscard]] bool layerParticipates(eng::ecs::Entity node,
                                         LayerStage stage) const noexcept;
    [[nodiscard]] bool participatesIn(eng::ecs::Entity node,
                                       LayerStage stage) const noexcept;

    /// timeScale da camada de `node` (GAME default = 1.f).
    [[nodiscard]] float timeScaleOf(eng::ecs::Entity node) const noexcept;

    /// Barramento de eventos da cena — gameplay tipado
    /// (SceneEvents.hpp: HitEvent, TriggerEvent, VisibilityEvent). Não é
    /// thread-safe: publish/subscribe na thread do jogo. Vive
    /// NA cena — morre com ela (inscrições são RAII e devem ser
    /// canceladas antes; ADR-022: Subscription não sobrevive ao bus).
    [[nodiscard]] eng::events::EventBus& events() noexcept { return events_; }
    [[nodiscard]] const eng::events::EventBus& events() const noexcept
    {
        return events_;
    }

private:
    [[nodiscard]] Hierarchy* hierarchyOf(eng::ecs::Entity node) noexcept;
    [[nodiscard]] const Hierarchy* hierarchyOf(eng::ecs::Entity node) const noexcept;

    /// Remove `child` da lista do `parent`, apagando também entradas
    /// obsoletas (limpeza oportunista — ADR-025).
    void removeFromChildren(eng::ecs::Entity parent, eng::ecs::Entity child);

    /// Descendentes válidos de `node` (inclusive o próprio), ordem
    /// determinística por nível (BFS sobre as listas de filhos).
    [[nodiscard]] std::vector<eng::ecs::Entity> collectSubtree(eng::ecs::Entity node) const;

    eng::ecs::World world_;
    LinkRegistry links_{};    ///< links tipados
    LayerRegistry layers_{};  ///< GAME/SUBGAME/nomeadas
    eng::events::EventBus events_{}; ///< barramento de gameplay (P4.7.0 B1)
};

} // namespace eng::scene
