#pragma once

/// eng::scene::LayerRegistry — camadas GAME/SUBGAME/nomeadas com
/// participação (update/física/render) e timeScale (evolução P0-5; ADR-051).
///
/// Modelo (decisões completas em ADR-051):
///   - Camadas são ESTRUTURA DE CENA (não estado de runtime): a registry é
///     de propriedade da Scene e persiste no arquivo da cena. Física,
///     animação, partículas e viewport — que já recebem Scene& — consultam
///     participação sem nova dependência de módulo.
///   - Built-ins: GAME (default — tudo participante) e SUBGAME (segundo
///     grupo de simulação independente: mundo do pause-menu/minigame).
///     Nomeadas via addLayer().
///   - Participação por camada: update/physics/render. timeScale por
///     camada (0 = pausada) — animação/partículas escalam o dt POR
///     ENTIDADE; física NÃO escala por camada em P0-5 (timestep global).
///   - Remoção de camada em USO é REJEITADA (varredura each<LayerMember>
///     no chamador que tem o World — Scene::removeLayer). Sem fallback
///     silencioso.
///   - Ausência de LayerMember = GAME (consulta via Scene::participatesIn /
///     Scene::timeScaleOf — cachos de fallback NÃO existem aqui).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/reflect/Reflect.hpp"

namespace eng::scene {

/// Participação de uma camada nos estágios do frame.
struct LayerParticipation {
    bool update = true;
    bool physics = true;
    bool render = true;

    [[nodiscard]] friend bool operator==(
        const LayerParticipation&, const LayerParticipation&) = default;
};

/// Definição de camada: participação + escala de tempo.
struct LayerDefinition {
    std::string name;
    LayerParticipation participation{};
    float timeScale = 1.f;
};

/// Estágios consultáveis por entidade (via Scene).
enum class LayerStage : std::uint8_t {
    Update,
    Physics,
    Render,
};

/// Componente: entidade membro da camada `layer` (default GAME).
/// Molde: a entidade (e seus filhos) não roda, não colide e não aparece no
/// jogo. `spawn("nome")` nos scripts cria cópias ATIVAS dela.
struct Template {
    bool active = false;  ///< reservado (sempre false no molde)
};

struct LayerMember {
    std::string layer{"GAME"};
};

/// Registro de camadas — GAME/SUBGAME built-in + nomeadas.
class LayerRegistry final {
public:
    LayerRegistry();
    ~LayerRegistry() = default;
    LayerRegistry(const LayerRegistry&) = delete;
    LayerRegistry& operator=(const LayerRegistry&) = delete;
    LayerRegistry(LayerRegistry&&) noexcept = default;
    LayerRegistry& operator=(LayerRegistry&&) noexcept = default;

    static constexpr std::string_view kGame = "GAME";
    static constexpr std::string_view kSubgame = "SUBGAME";

    /// Camada nomeada nova. Erros: nome vazio, duplicado (built-ins
    /// incluídos). A camada nasce com participação total e timeScale 1.
    [[nodiscard]] eng::core::Result<void> addLayer(std::string_view name);

    /// Remove uma camada NOMEADA (built-ins são permanentes). Erros:
    /// inexistente ou built-in. O chamador (Scene) valida uso antes.
    [[nodiscard]] eng::core::Result<void> remove(std::string_view name);

    /// Definição da camada (nullptr se não existe — built-ins SEMPRE
    /// existem). Consulta por nome.
    [[nodiscard]] const LayerDefinition* find(std::string_view name) const;

    [[nodiscard]] bool has(std::string_view name) const
    {
        return find(name) != nullptr;
    }

    /// Define a participação da camada. Erro: camada inexistente.
    [[nodiscard]] eng::core::Result<void> setParticipation(
        std::string_view name, LayerParticipation participation);

    /// Define o timeScale (>= 0; 0 = pausada). Erro: camada inexistente
    /// ou valor inválido.
    [[nodiscard]] eng::core::Result<void> setTimeScale(
        std::string_view name, float timeScale);

    /// Todas as definições em ordem de criação (GAME, SUBGAME, depois
    /// nomeadas na ordem de adição — determinístico).
    [[nodiscard]] const std::vector<LayerDefinition>& definitions() const
    {
        return layers_;
    }

    /// A definição é igual ao default? (serialização de diagnóstico).
    [[nodiscard]] static bool isDefault(const LayerDefinition& definition);

    void clear() noexcept;  // volta ao estado construído (built-ins)

private:
    [[nodiscard]] LayerDefinition* findMutable(std::string_view name);

    std::vector<LayerDefinition> layers_;
};

}  // namespace eng::scene

ENG_REFLECT_BEGIN(eng::scene::Template)
    ENG_REFLECT_FIELD(active)
ENG_REFLECT_END()

ENG_REFLECT_BEGIN(eng::scene::LayerMember)
    ENG_REFLECT_FIELD(layer)
ENG_REFLECT_END()
