#pragma once

/// eng::scene::SceneIdentity — identidade PERSISTENTE de nós de cena
///.
///
/// Entity{index,generation} da FASE 2 é handle de RUNTIME — gravá-lo em
/// disco seria frágil (índices mudam entre builds, gerações dependem da
/// ordem de criação). SceneEntityId é a identidade estável: UUIDv4 forte
/// sobre core::Uuid128 (distinto de AssetId/ProjectId por tipo — ADR-028),
/// atribuído na PRIMEIRA serialização e gravado no próprio arquivo via
/// componente SceneIdentity.
///
/// O registro do codec de campo (UUID como string canônica em JSON) é
/// INLINE neste header: todo TU que inclui registra (idempotente) — mesma
/// estratégia dos macros ENG_REFLECT.
#include <string_view>

#include "eng/core/Uuid.hpp"
#include "eng/ecs/Ecs.hpp"
#include "eng/serial/StructCodec.hpp"

namespace eng::scene {

struct SceneEntityId {
    eng::core::Uuid128 uuid{};

    [[nodiscard]] bool isNil() const noexcept { return uuid.isNil(); }

    [[nodiscard]] static SceneEntityId generate()
    {
        return SceneEntityId{eng::core::Uuid128::generate()};
    }

    [[nodiscard]] static eng::core::Result<SceneEntityId> fromString(
        std::string_view text)
    {
        const auto uuid = eng::core::Uuid128::fromString(text);
        if (uuid.isError()) {
            return eng::core::makeUnexpected(uuid.error());
        }
        return SceneEntityId{uuid.value()};
    }

    [[nodiscard]] std::string toString() const { return uuid.toString(); }

    [[nodiscard]] friend bool operator==(
        const SceneEntityId&, const SceneEntityId&) noexcept = default;
    [[nodiscard]] friend std::strong_ordering operator<=>(
        const SceneEntityId& a, const SceneEntityId& b) noexcept
    {
        return a.uuid <=> b.uuid;
    }
};

/// Componente que carrega a identidade persistente do nó. Atribuído pelo
/// SceneSerializer na primeira serialização; lido de volta no load.
struct SceneIdentity {
    SceneEntityId id;
};

namespace detail {

/// Forma canônica do UUID nulo — aceita como "sem referência" nos codecs
/// de CAMPO (identidade ausente é legítima em componentes).
inline constexpr std::string_view kNilUuidText =
    "00000000-0000-0000-0000-000000000000";

/// Codec de campo: SceneEntityId em componentes de usuário vira STRING
/// UUID canônica no JSON (ADR-033 — referências entre entidades por
/// identidade estável, nunca por handle de runtime). A forma NULA é
/// aceita como "sem referência" (Uuid128::fromString puro exige v4).
inline const bool eng_scene_entity_id_codec_registered = [] {
    eng::serial::FieldTypeCodec codec;
    codec.encode = [](const void* member) {
        const auto* id = static_cast<const SceneEntityId*>(member);
        return eng::core::Result<eng::serial::JsonValue>(
            eng::serial::JsonValue::string(id->toString()));
    };
    codec.decode = [](const eng::serial::JsonValue& field,
                      void* member) -> eng::core::Result<void> {
        if (!field.isString()) {
            return eng::core::makeUnexpected(eng::core::Error{
                eng::core::StatusCode::ParseError,
                "SceneEntityId: campo esperava string UUID canônica"});
        }
        const std::string& text = field.asString();
        if (text == kNilUuidText) {
            *static_cast<SceneEntityId*>(member) = SceneEntityId{};
            return {};
        }
        auto parsed = SceneEntityId::fromString(text);
        if (parsed.isError()) {
            return eng::core::makeUnexpected(parsed.error());
        }
        *static_cast<SceneEntityId*>(member) = parsed.value();
        return {};
    };
    eng::serial::registerFieldTypeCodec("eng::scene::SceneEntityId", codec);
    return true;
}();

} // namespace detail

} // namespace eng::scene

/// Hash de SceneEntityId — habilita uso em unordered containers.
template<>
struct std::hash<eng::scene::SceneEntityId> {
    [[nodiscard]] std::size_t operator()(
        const eng::scene::SceneEntityId& id) const noexcept
    {
        return std::hash<eng::core::Uuid128>{}(id.uuid);
    }
};
