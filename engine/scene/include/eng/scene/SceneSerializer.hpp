#pragma once

/// eng::scene::SceneSerializer — persistência determinística do grafo de
/// nós (FASE 3, missão §2.7; ADR-033 — desvio D3: vive DENTRO de scene).
///
/// ComponentContract: cada componente do catálogo pode
/// declarar `requires`/`conflicts`/`single`/`category`/`scriptAlias` e
/// hooks de ciclo de vida (`onAttach`/`onDetach`/`onValidate`). Contratos
/// são APLICAÇÃO DE AUTORIA (add/remove do Inspector — erros precisos);
/// o LOAD não re-injeta dependências (cenas salvas já as satisfazem —
/// cenas editadas à mão abrem com WARN, nunca falham por contrato).
/// Registro NATIVO de efeitos colaterais (física/materiais/luz) passa a
/// viver SOMENTE nos hooks — zero caso especial espalhado pelo editor.
///
/// Formato (formatVersion 1):
/// {
///   "formatVersion": 1,
///   "sceneEntityIds": ["uuid", ...],            // ordenadas (hi,lo)
///   "entities": [
///     { "id": "uuid", "parent": "uuid"|null,
///       "components": [ {"type": "eng::math::Transform", "data": {…}} ] }
///   ]
/// }
///
/// Decisões:
///   - Entidades ordenadas por SceneEntityId; componentes por nome de tipo
///     (determinismo byte-a-byte: serialize(deserialize(x)) == x).
///   - Dados de componentes via reflect (nome estável + PropertyInfo por
///     offset) — NUNCA typeid/índice; enums por NOME de enumerador.
///   - TIPOS DE COMPONENTE são registrados explicitamente
///     (registerComponentType<T>) porque World não expõe enumeração
///     dinâmica de pools (achado crítico 1 da auditoria) — built-in:
///     eng::math::Transform.
///   - Hierarchy vira o campo "parent" (ordem de anexação NÃO é persistida;
///     pós-load ela é a canônica por SceneEntityId); WorldMatrix é cache
///     derivado e não é persistido; entradas SceneIdentity/Hierarchy/
///     WorldMatrix no JSON são toleradas com WARN (escritores externos).
///   - Pai obsoleto (bypass do world na origem) → serializado como RAIZ
///     (mesma política de ADR-025).
///   - Referência de asset quebrada NÃO impede o load (parse só valida
///     forma); a checagem contra o AssetRegistry é da camada de composição
///     (runtime/editor/tests) — documentado em ADR-033/D4.
///   - load ADICIONA nós aos existentes; ids duplicados → ParseError.
///
/// Thread-safety: save/load não são concorrentes sobre a mesma
/// Scene; o registro de componentes é single-threaded (init), leitura
/// concorrente após registro é segura.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/scene/SceneIdentity.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::scene {

// Declarações ANTECIPADAS do detail — o contrato/hooks são parte da
// assinatura pública de registerComponentType e o
// arquivo define o detail DEPOIS da classe.
namespace detail {
/// Contrato de autoria de um componente. Tudo OPCIONAL
/// exceto `category` (vazio = grupo "Outros" do Inspector).
struct ComponentContract {
    /// Tipos (nome canônico) que PRECISAM estar presentes na entidade
    /// antes deste componente ser adicionado. ("requires" do contrato —
    /// o identificador `requires` é palavra-chave C++20, por isso o
    /// membro chama `required`.)
    std::vector<std::string> required;
    /// Tipos que NÃO podem coexistir com este na mesma entidade.
    std::vector<std::string> conflicts;
    /// true: instância ÚNICA na cena inteira (ex.: pós-processamento no
    /// P4.7.1). false: quantas entidades quiserem.
    bool single = false;
    /// Grupo do Inspector: "Transform"|"Render"|"Física"|"Lógica"|
    /// "Áudio"|"Câmera"|"FX" (vazio = "Outros").
    std::string category;
    /// Apelido NI-Script do componente ("" = sem apelido; scripts usam o
    /// nome canônico). Gerado do MESMO registro que alimenta o Inspector.
    std::string scriptAlias;
};

struct ComponentEntry;
using HookValidate = eng::core::Result<void>(*)
    (eng::scene::Scene&, eng::ecs::Entity, const ComponentEntry&);
using HookAttach = void(*)(eng::scene::Scene&, eng::ecs::Entity,
                           const ComponentEntry&, void* hookUser);
using HookDetach = void(*)(eng::scene::Scene&, eng::ecs::Entity,
                           const ComponentEntry&, void* hookUser);
} // namespace detail

class SceneSerializer final {
public:
    SceneSerializer() = delete;

    /// Registra um tipo de componente como serializável. `typeName` é o
    /// NOME ESTÁVEL registrado no reflect (o mesmo do ENG_REFLECT_BEGIN).
    /// O tipo precisa estar registrado no reflect ANTES (senão erro).
    /// Built-ins registrados no próprio módulo: eng::math::Transform.
    /// Contrato + hooks opcionais (registrar no MESMO
    /// chamada — o contrato é parte da entrada do catálogo).
    template<typename T>
    [[nodiscard]] static eng::core::Result<void> registerComponentType(
        std::string_view typeName,
        detail::ComponentContract contract = {},
        detail::HookAttach onAttach = nullptr,
        detail::HookDetach onDetach = nullptr,
        detail::HookValidate onValidate = nullptr,
        void* hookUser = nullptr);

    /// Scene → texto JSON determinístico. EFEITO: atribui SceneEntityId a
    /// nós que ainda não têm (componente SceneIdentity emplantado).
    [[nodiscard]] static eng::core::Result<std::string> save(Scene& scene);

    /// Texto JSON → nós/components ANEXADOS à cena. Erros claros (formato,
    /// uuid, componente desconhecido, parent ausente, ciclo) — nunca throw.
    [[nodiscard]] static eng::core::Result<void> load(Scene& scene,
                                                      std::string_view text);

    /// Versão do formato de cena escrito/lido.
    static constexpr std::uint32_t kFormatVersion = 1;
};

} // namespace eng::scene

// =============================================================================
// Template (header) — entradas tipadas sobre a API pública de World
// =============================================================================

namespace eng::scene::detail {

// (ComponentContract, HookValidate/Attach/Detach e ComponentEntry já
// declarados ANTES de SceneSerializer — ver topo do arquivo; aqui ficam
// as DEFINIÇÕES completas.)


/// Hooks de ciclo de vida — registrados pelo AUTOR do componente
/// (ComponentRegistration.cpp no editor; built-ins no próprio scene).
/// `hookUser` é contexto fornecido no registro (ex.: ponteiro do
/// documento para consulta de materiais).
struct ComponentEntry;
using HookValidate = eng::core::Result<void>(*)(
    eng::scene::Scene&, eng::ecs::Entity, const ComponentEntry&);
using HookAttach = void(*)(eng::scene::Scene&, eng::ecs::Entity,
                           const ComponentEntry&, void* hookUser);
using HookDetach = void(*)(eng::scene::Scene&, eng::ecs::Entity,
                           const ComponentEntry&, void* hookUser);

/// Entrada de componente serializável (type-erased via lambdas tipadas;
/// a própria entrada é parâmetro das funções — sem capturas, conversível
/// para function pointer, mesmo padrão de LoaderEntry em eng::assets).
struct ComponentEntry {
    const eng::reflect::TypeInfo* info = nullptr;
    bool (*has)(const eng::ecs::World&, eng::ecs::Entity) = nullptr;
    /// Constrói T{} no world (FASE 8, auditoria D2: "Add Component" do
    /// editor sem segundo registry — o catálogo continua ÚNICO aqui).
    eng::core::Result<void> (*emplaceDefault)(
        const ComponentEntry&, eng::ecs::World&, eng::ecs::Entity) = nullptr;
    /// Ponteiro do componente (leitura — Inspector do editor; FASE 8 D2).
    /// nullptr se a entidade não o possui.
    const void* (*get)(const eng::ecs::World&, eng::ecs::Entity) = nullptr;
    /// Ponteiro mutável (escrita de campos por offset — Inspector).
    void* (*getMutable)(eng::ecs::World&, eng::ecs::Entity) = nullptr;
    /// Remove o componente da entidade (type-erased — Inspector/"Remove").
    /// false se a entidade não o possui.
    bool (*removeFrom)(eng::ecs::World&, eng::ecs::Entity) = nullptr;
    eng::core::Result<eng::serial::JsonValue> (*encode)(
        const ComponentEntry&, const eng::ecs::World&,
        eng::ecs::Entity) = nullptr;
    eng::core::Result<void> (*decodeAndEmplace)(
        const ComponentEntry&, eng::ecs::World&, eng::ecs::Entity,
        const eng::serial::JsonValue&) = nullptr;
    /// Número de instâncias vivas do componente no world (P4.7.0 —
    /// contratos `single` + métricas do catálogo; World expõe
    /// componentCount<T>() por tipo).
    std::size_t (*count)(const eng::ecs::World&) = nullptr;

    // --- P4.7.0 Bloco 1: contrato + hooks ---------------------------
    ComponentContract contract;
    HookValidate onValidate = nullptr;
    HookAttach onAttach = nullptr;
    HookDetach onDetach = nullptr;
    void* hookUser = nullptr;
};

/// Registro global de componentes (não-template, no .cpp).
void registerComponentEntry(std::string typeName, ComponentEntry entry);
[[nodiscard]] const std::map<std::string, ComponentEntry>&
componentEntries();

/// Anexa/atualiza contrato + hooks de um componente JÁ REGISTRADO
/// (caminho dos built-ins — eng::math::Transform registra o seu no
/// próprio módulo scene, sem re-registrar o tipo).
void registerComponentContract(
    std::string_view typeName, ComponentContract contract,
    HookAttach onAttach = nullptr, HookDetach onDetach = nullptr,
    HookValidate onValidate = nullptr, void* hookUser = nullptr);

} // namespace eng::scene::detail

namespace eng::scene {

template<typename T>
eng::core::Result<void> SceneSerializer::registerComponentType(
    std::string_view typeName,
    detail::ComponentContract contract,
    detail::HookAttach onAttach,
    detail::HookDetach onDetach,
    detail::HookValidate onValidate,
    void* hookUser)
{
    using eng::core::Error;
    using eng::core::StatusCode;

    const eng::reflect::TypeInfo* info =
        eng::reflect::TypeRegistry::global().find(typeName);
    if (info == nullptr) {
        return eng::core::makeUnexpected(Error{
            StatusCode::NotSupported,
            "SceneSerializer::registerComponentType: tipo '" +
                std::string(typeName) +
                "' não está registrado no reflect (ENG_REFLECT?)"});
    }

    detail::ComponentEntry entry;
    entry.info = info;
    entry.has = [](const eng::ecs::World& world, eng::ecs::Entity e) {
        return world.has<T>(e);
    };
    entry.emplaceDefault = [](const detail::ComponentEntry& /*self*/,
                               eng::ecs::World& world,
                               eng::ecs::Entity e)
        -> eng::core::Result<void> {
        if (world.emplace<T>(e, T{}) == nullptr) {
            return eng::core::makeUnexpected(Error{
                StatusCode::InvalidArgument,
                "SceneSerializer: emplaceDefault falhou (entidade obsoleta?)"});
        }
        return {};
    };
    entry.get = [](const eng::ecs::World& world, eng::ecs::Entity e) {
        return static_cast<const void*>(world.get<T>(e));
    };
    entry.getMutable = [](eng::ecs::World& world, eng::ecs::Entity e) {
        return static_cast<void*>(world.get<T>(e));
    };
    entry.removeFrom = [](eng::ecs::World& world, eng::ecs::Entity e) {
        return world.remove<T>(e);
    };
    entry.encode = [](const detail::ComponentEntry& self,
                      const eng::ecs::World& world,
                      eng::ecs::Entity e)
        -> eng::core::Result<eng::serial::JsonValue> {
        const T* component = world.get<T>(e);
        if (component == nullptr) {
            return eng::core::makeUnexpected(Error{
                StatusCode::NotFound,
                "SceneSerializer: componente sumiu entre has() e get()"});
        }
        return eng::serial::encodeStruct(component, *self.info);
    };
    entry.decodeAndEmplace = [](const detail::ComponentEntry& self,
                                eng::ecs::World& world,
                                eng::ecs::Entity e,
                                const eng::serial::JsonValue& data)
        -> eng::core::Result<void> {
        T component{};
        const auto decoded =
            eng::serial::decodeStruct(data, &component, *self.info);
        if (decoded.isError()) {
            return decoded;
        }
        if (world.emplace<T>(e, std::move(component)) == nullptr) {
            return eng::core::makeUnexpected(Error{
                StatusCode::InvalidArgument,
                "SceneSerializer: emplace falhou (entidade obsoleta?)"});
        }
        return {};
    };
    entry.count = [](const eng::ecs::World& world) {
        return world.componentCount<T>();
    };
    entry.contract = std::move(contract);
    entry.onAttach = onAttach;
    entry.onDetach = onDetach;
    entry.onValidate = onValidate;
    entry.hookUser = hookUser;

    detail::registerComponentEntry(std::string(typeName), entry);
    return {};
}

} // namespace eng::scene
