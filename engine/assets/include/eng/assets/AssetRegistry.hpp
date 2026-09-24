#pragma once

/// eng::assets::AssetRegistry — catálogo persistente AssetId ↔ AssetMeta
/// (FASE 3; ADR-029).
///
/// - Renomear/mover asset = atualizar sourcePath do meta — as referências
///   por AssetId permanecem válidas (a identidade mora no id, ADR-028).
/// - Serialização JSON determinística: entradas ordenadas por AssetId
///   (hi, lo) — bytes estáveis para o mesmo conteúdo (round-trip testado).
/// - Formato (asset_registry.json):
///     {"formatVersion":1,"assets":[
///        {"id":"…","type":"Scene","sourcePath":"scenes/a.json"}, … ]}
/// - Thread-safety: single-threaded — acesso concorrente à
///   MESMA instância requer sincronização externa.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "eng/assets/AssetId.hpp"
#include "eng/assets/AssetMeta.hpp"
#include "eng/core/Result.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::assets {

inline constexpr std::uint32_t kAssetRegistryFormatVersion = 1;

class AssetRegistry final {
public:
    /// Insere ou ATUALIZA o meta do id (upsert — renomeação usa isto).
    /// id nulo → InvalidArgument.
    eng::core::Result<void> upsert(AssetMeta meta);

    /// Remove o id do catálogo. Ausente → NotFound.
    eng::core::Result<void> remove(AssetId id);

    /// Meta do id (nullptr se ausente).
    [[nodiscard]] const AssetMeta* find(AssetId id) const;

    /// Todas as entradas, ORDENADAS por AssetId (determinístico).
    [[nodiscard]] std::vector<AssetMeta> all() const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // --- persistência ---------------------------------------------------

    /// JSON do catálogo completo (determinístico).
    [[nodiscard]] eng::core::Result<std::string> serialize() const;

    /// Parse do JSON do catálogo. Valida: formatVersion suportado, ids
    /// canônicos, tipos nomeados, paths presentes. NUNCA lança.
    [[nodiscard]] static eng::core::Result<AssetRegistry> deserialize(
        std::string_view text);

    /// Composição sem tocar em texto (usado por serial ↔ JsonValue).
    [[nodiscard]] eng::core::Result<eng::serial::JsonValue> toJson() const;
    [[nodiscard]] static eng::core::Result<AssetRegistry> fromJson(
        const eng::serial::JsonValue& value);

private:
    /// Ordenado por AssetId (invariante mantida pelo upsert).
    std::vector<AssetMeta> entries_;
};

} // namespace eng::assets
