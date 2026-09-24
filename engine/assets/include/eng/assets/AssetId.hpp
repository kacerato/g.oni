#pragma once

/// eng::assets::AssetId — identidade ESTÁVEL de asset (FASE 3; ADR-028).
///
/// Tipo FORTE sobre core::Uuid128 — não conversível implicitamente para
/// SceneEntityId/ProjectId (missão §2.3: "distinto por tipo forte").
/// Decisão de identidade: UUIDv4 de 128 bits; NÃO path-hash
/// (renomear quebraria tudo), NÃO content-hash (mudar 1 pixel quebraria
/// tudo), NÃO hash truncado (colisão silenciosa é inaceitável).
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "eng/core/Result.hpp"
#include "eng/core/Uuid.hpp"

namespace eng::assets {

struct AssetId {
    eng::core::Uuid128 uuid{};

    [[nodiscard]] bool isNil() const noexcept { return uuid.isNil(); }

    /// AssetId novo (UUIDv4).
    [[nodiscard]] static AssetId generate()
    {
        return AssetId{eng::core::Uuid128::generate()};
    }

    /// Parse estrito da forma canônica.
    [[nodiscard]] static eng::core::Result<AssetId> fromString(
        std::string_view text)
    {
        const auto uuid = eng::core::Uuid128::fromString(text);
        if (uuid.isError()) {
            return eng::core::makeUnexpected(uuid.error());
        }
        return AssetId{uuid.value()};
    }

    /// 16 bytes big-endian.
    [[nodiscard]] static eng::core::Result<AssetId> fromBytesBE(
        std::span<const std::byte> bytes)
    {
        const auto uuid = eng::core::Uuid128::fromBytesBE(bytes);
        if (uuid.isError()) {
            return eng::core::makeUnexpected(uuid.error());
        }
        return AssetId{uuid.value()};
    }

    [[nodiscard]] std::string toString() const { return uuid.toString(); }
    [[nodiscard]] std::array<std::byte, 16> toBytesBE() const
    {
        return uuid.toBytesBE();
    }

    [[nodiscard]] friend bool operator==(const AssetId&,
                                         const AssetId&) noexcept = default;
    [[nodiscard]] friend std::strong_ordering operator<=>(
        const AssetId& a, const AssetId& b) noexcept
    {
        return a.uuid <=> b.uuid;
    }
};

} // namespace eng::assets

/// Hash de AssetId — habilita uso em unordered containers.
template<>
struct std::hash<eng::assets::AssetId> {
    [[nodiscard]] std::size_t operator()(
        const eng::assets::AssetId& id) const noexcept
    {
        return std::hash<eng::core::Uuid128>{}(id.uuid);
    }
};
