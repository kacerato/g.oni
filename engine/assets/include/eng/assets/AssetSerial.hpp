#pragma once

/// eng::assets::AssetSerial — codec de campo para AssetId em componentes
/// serializados.
///
/// "AssetId em componentes é serializado como string UUID": o StructCodec
/// consulta codecs por NOME de tipo — este header registra o de
/// "eng::assets::AssetId". Registro INLINE (toda TU que inclui registra;
/// idempotente) — mesma estratégia de ENG_REFLECT/SceneIdentity. Sem isto,
/// eng::scene precisaria de aresta para eng::assets (proibida pelo
/// layering — desvios D1/D4 da auditoria).
#include "eng/assets/AssetId.hpp"
#include "eng/serial/StructCodec.hpp"

namespace eng::assets::detail {

/// Forma canônica do UUID nulo — aceita como "sem referência" no codec de
/// campo (identidade ausente é legítima em componentes).
inline constexpr std::string_view kNilAssetIdText =
    "00000000-0000-0000-0000-000000000000";

inline const bool eng_asset_id_field_codec_registered = [] {
    eng::serial::FieldTypeCodec codec;
    codec.encode = [](const void* member)
        -> eng::core::Result<eng::serial::JsonValue> {
        const auto* id = static_cast<const AssetId*>(member);
        return eng::serial::JsonValue::string(id->toString());
    };
    codec.decode = [](const eng::serial::JsonValue& field,
                      void* member) -> eng::core::Result<void> {
        if (!field.isString()) {
            return eng::core::makeUnexpected(eng::core::Error{
                eng::core::StatusCode::ParseError,
                "AssetId: campo esperava string UUID canônica"});
        }
        const std::string& text = field.asString();
        if (text == kNilAssetIdText) {
            *static_cast<AssetId*>(member) = AssetId{};
            return {};
        }
        auto parsed = AssetId::fromString(text);
        if (parsed.isError()) {
            return eng::core::makeUnexpected(parsed.error());
        }
        *static_cast<AssetId*>(member) = parsed.value();
        return {};
    };
    eng::serial::registerFieldTypeCodec("eng::assets::AssetId", codec);
    return true;
}();

} // namespace eng::assets::detail
