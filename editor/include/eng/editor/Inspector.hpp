#pragma once

/// eng::editor::Inspector — acesso genérico a componentes/campos via
/// reflection.
///
/// - Catálogo: `eng::scene::detail::componentEntries()` (ÚNICO registry —
///   auditoria D2; inclui os built-ins Name/Transform e tudo que as fases
///   futuras registrarem — física/animação/etc aparecem automaticamente).
/// - Campos: `TypeInfo::properties` (offset + typeName + hint). Valores
///   trafegam como STRING (boundary neutra para JNI — audit §4):
///     bool → "true"/"false"; inteiros → decimal; f32/f64 → %g;
///     string → como está; enum → NOME do enumerador;
///     struct conhecida (Vec3/Quat) → subcampos por caminho "position.x".
/// - Escrita: parse por tipo → escrita por offset. Erros precisos (campo
///   desconhecido, valor inválido, entidade obsoleta, componente ausente).
///
/// Evolução P0-6: cada Field carrega um KIND semântico + options
/// para o host renderizar editores REAIS (não EditText livre):
///     "bool"    → Switch/checkbox
///     "enum"    → opções legais em `options` (separadas por '|')
///     "number"  → campo numérico (f32/f64)
///     "int"     → campo inteiro (i8..u64)
///     "color"   → grupo de canais hint color:<grupo>:<r|g|b|a>; o path é a
///                 lista de membros por vírgula ("tintR,tintG,tintB") e o
///                 valor é hex "#RRGGBB" ou "#RRGGBBAA" (alpha = 4º canal
///                 quando presente)
///     "texture" → string com hint "texture" (asset de textura do projeto)
///     "text"    → texto livre
///
/// Nada aqui conhece componentes específicos: Transform é lido como
/// QUALQUER struct refletida — o Inspector não tem um único if de
/// "position" (missão §8.4: não hard-code). Kind/color/texture derivam de
/// METADADOS (hint), nunca de nomes de campos.

#include <string>
#include <string_view>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/ecs/Ecs.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/scene/SceneSerializer.hpp"

namespace eng::editor {

class Inspector final {
public:
    Inspector() = delete;

    /// Um campo achatado do Inspector (primitivo folha ou grupo de cor).
    struct Field {
        std::string path;     ///< "position.x", "value", "tintR,tintG,tintB"
        std::string typeName; ///< "f32", "string", nome do enum, "color"...
        std::string value;    ///< representação textual (cor: "#RRGGBB[AA]")
        std::string kind;     ///< semântico p/ UI: ver header
        std::string options;  ///< enums: enumeradores por '|' ("" se não-enum)
    };

    /// Catálogo completo de componentes registrados (ordenado por nome).
    [[nodiscard]] static std::vector<std::string> catalog();

    /// Entrada do catálogo com metadados do contrato —
    /// categoria do Inspector + apelido NI-Script, ambos gerados do MESMO
    /// registro (ComponentContract no catálogo do serializer).
    struct CatalogEntry {
        std::string name;       ///< nome canônico do tipo
        std::string category;   ///< "Transform".."FX" ("" = "Outros")
        std::string scriptAlias; ///< apelido NI-Script ("" = nome canônico)
    };
    /// Catálogo com metadados, ordenado por (ordem de categoria fixa,
    /// nome) — Transform, Render, Física, Lógica, Áudio, Câmera, FX,
    /// Outros. É a ÚNICA fonte do agrupamento do painel Add.
    [[nodiscard]] static std::vector<CatalogEntry> catalogEntries();

    /// Contrato do componente (nullptr se fora do catálogo).
    [[nodiscard]] static const eng::scene::detail::ComponentContract*
    contractOf(std::string_view component);

    /// Componentes PRESENTES na entidade (ordenados por nome).
    [[nodiscard]] static std::vector<std::string> componentsOf(
        const eng::scene::Scene& scene, eng::ecs::Entity entity);

    /// Campos achatados do componente na entidade. Erro: componente ausente.
    [[nodiscard]] static eng::core::Result<std::vector<Field>> fieldsOf(
        const eng::scene::Scene& scene, eng::ecs::Entity entity,
        std::string_view component);

    /// Lê um campo por caminho ("position.x"; grupos de cor aceitam o path
    /// comma-junto emitido por fieldsOf). Erros precisos.
    [[nodiscard]] static eng::core::Result<std::string> getField(
        const eng::scene::Scene& scene, eng::ecs::Entity entity,
        std::string_view component, std::string_view fieldPath);

    /// Escreve um campo por caminho (parse por tipo; cor aceita hex). Marca
    /// a cena suja. Erros precisos; NUNCA escreve parcialmente.
    [[nodiscard]] static eng::core::Result<void> setField(
        eng::scene::Scene& scene, eng::ecs::Entity entity,
        std::string_view component, std::string_view fieldPath,
        std::string_view value);

    /// Componentes protegidos de remoção (integridade da cena/serializer).
    /// Transform é obrigatório (nó); Name é o rótulo mínimo do editor.
    [[nodiscard]] static bool isRemovable(std::string_view component);

    /// Adiciona componente default-construído via catálogo (D2).
    /// Valida o ComponentContract (requires/conflicts/
    /// single) com erro PRECISO, executa onAttach (registro nativo de
    /// efeitos colaterais — luz/física/materiais) e onValidate pós-anexo.
    /// `attachUser` é contexto do chamador (ex.: EditorDocument) entregue
    /// ao hook.
    [[nodiscard]] static eng::core::Result<void> addComponent(
        eng::scene::Scene& scene, eng::ecs::Entity entity,
        std::string_view component, void* attachUser = nullptr);

    /// Remove componente (recusa protegidos). P4.7.0 Bloco 1: recusa
    /// também quando OUTRO componente presente exige o removido (erro
    /// com o nome do dependente); roda onDetach pós-remoção com
    /// `detachUser`.
    [[nodiscard]] static eng::core::Result<void> removeComponent(
        eng::scene::Scene& scene, eng::ecs::Entity entity,
        std::string_view component, void* detachUser = nullptr);
};

} // namespace eng::editor
