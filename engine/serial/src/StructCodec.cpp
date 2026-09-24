#include "eng/serial/StructCodec.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

namespace eng::serial {

namespace {

using eng::core::Error;
using eng::core::StatusCode;
using eng::core::makeUnexpected;
using eng::reflect::PropertyInfo;
using eng::reflect::TypeInfo;
using eng::reflect::TypeKind;
using eng::reflect::TypeRegistry;

/// Registro de codecs de campo por typeName. Escrita sob lock exclusivo,
/// leitura sob lock compartilhado (mesma política do TypeRegistry —
/// ADR-021). Valores em deque: endereços estáveis.
class FieldTypeCodecRegistry {
public:
    static FieldTypeCodecRegistry& instance() noexcept
    {
        static FieldTypeCodecRegistry registry; // magic static
        return registry;
    }

    void upsert(std::string_view typeName, FieldTypeCodec codec)
    {
        std::unique_lock lock(mutex_);
        entries_[std::string(typeName)] = codec;
    }

    [[nodiscard]] const FieldTypeCodec* find(std::string_view typeName)
    {
        std::shared_lock lock(mutex_);
        const auto it = entries_.find(std::string(typeName));
        return it == entries_.end() ? nullptr : &it->second;
    }

private:
    FieldTypeCodecRegistry() = default;

    mutable std::shared_mutex mutex_;
    std::map<std::string, FieldTypeCodec> entries_;
};

} // namespace

void registerFieldTypeCodec(std::string_view typeName, FieldTypeCodec codec)
{
    FieldTypeCodecRegistry::instance().upsert(typeName, codec);
}

const FieldTypeCodec* findFieldTypeCodec(std::string_view typeName)
{
    return FieldTypeCodecRegistry::instance().find(typeName);
}

namespace {

[[nodiscard]] const TypeInfo* findType(std::string_view name)
{
    return TypeRegistry::global().find(name);
}

[[nodiscard]] Error unregisteredType(const std::string& where,
                                     const std::string& typeName)
{
    return Error{StatusCode::ParseError,
                 where + ": tipo de campo não registrado: '" + typeName + "'"};
}

[[nodiscard]] Error wrongType(const std::string& where,
                              const std::string& fieldName,
                              const char* expected)
{
    return Error{StatusCode::ParseError,
                 where + ": campo '" + fieldName + "' esperava " + expected};
}

/// Leitura com sinal por tamanho (member aponta para o valor bruto).
[[nodiscard]] std::int64_t readSigned(const void* member,
                                      std::size_t size) noexcept
{
    switch (size) {
    case 1: return *static_cast<const std::int8_t*>(member);
    case 2: return *static_cast<const std::int16_t*>(member);
    case 4: return *static_cast<const std::int32_t*>(member);
    default: return *static_cast<const std::int64_t*>(member);
    }
}

/// Leitura sem sinal por tamanho.
[[nodiscard]] std::uint64_t readUnsigned(const void* member,
                                         std::size_t size) noexcept
{
    switch (size) {
    case 1: return *static_cast<const std::uint8_t*>(member);
    case 2: return *static_cast<const std::uint16_t*>(member);
    case 4: return *static_cast<const std::uint32_t*>(member);
    default: return *static_cast<const std::uint64_t*>(member);
    }
}

/// Escrita de inteiro por tamanho (trunca para a largura do campo).
void writeInteger(void* member, std::size_t size, bool isSigned,
                  std::int64_t value) noexcept
{
    if (isSigned) {
        switch (size) {
        case 1: *static_cast<std::int8_t*>(member) = static_cast<std::int8_t>(value); return;
        case 2: *static_cast<std::int16_t*>(member) = static_cast<std::int16_t>(value); return;
        case 4: *static_cast<std::int32_t*>(member) = static_cast<std::int32_t>(value); return;
        default: *static_cast<std::int64_t*>(member) = value; return;
        }
    }
    const auto raw = static_cast<std::uint64_t>(value);
    switch (size) {
    case 1: *static_cast<std::uint8_t*>(member) = static_cast<std::uint8_t>(raw); return;
    case 2: *static_cast<std::uint16_t*>(member) = static_cast<std::uint16_t>(raw); return;
    case 4: *static_cast<std::uint32_t*>(member) = static_cast<std::uint32_t>(raw); return;
    default: *static_cast<std::uint64_t*>(member) = raw; return;
    }
}

// --- encode: um campo -------------------------------------------------------

[[nodiscard]] eng::core::Result<JsonValue> encodeMember(
    const void* member, const PropertyInfo& prop)
{
    // Codec de campo por NOME de tipo tem PRECEDÊNCIA: tipos de
    // identidade (UUID como string) são codificados por quem os conhece.
    if (const FieldTypeCodec* codec = findFieldTypeCodec(prop.typeName)) {
        return codec->encode(member);
    }

    const TypeInfo* type = findType(prop.typeName);
    if (type == nullptr) {
        return makeUnexpected(
            unregisteredType("encodeStruct", prop.typeName));
    }

    switch (type->kind) {
    case TypeKind::Primitive: {
        if (prop.typeName == "bool") {
            return JsonValue::boolean(*static_cast<const bool*>(member));
        }
        if (prop.typeName == "string") {
            return JsonValue::string(
                *static_cast<const std::string*>(member));
        }
        if (prop.typeName == "f32") {
            const float v = *static_cast<const float*>(member);
            if (!std::isfinite(v)) {
                return makeUnexpected(Error{
                    StatusCode::ParseError,
                    "encodeStruct: campo '" + prop.name +
                        "' tem valor não finito (NaN/Inf) — JSON não "
                        "representa"});
            }
            return JsonValue::real(v);
        }
        if (prop.typeName == "f64") {
            const double v = *static_cast<const double*>(member);
            if (!std::isfinite(v)) {
                return makeUnexpected(Error{
                    StatusCode::ParseError,
                    "encodeStruct: campo '" + prop.name +
                        "' tem valor não finito (NaN/Inf)"});
            }
            return JsonValue::real(v);
        }
        if (prop.typeName == "u64" || prop.typeName == "u32" ||
            prop.typeName == "u16" || prop.typeName == "u8") {
            return JsonValue::uinteger(readUnsigned(member, type->size));
        }
        return JsonValue::integer(readSigned(member, type->size));
    }
    case TypeKind::Struct:
        return encodeStruct(member, *type);
    case TypeKind::Enum: {
        const TypeInfo* under =
            TypeRegistry::global().find(type->underlyingTypeId);
        const bool unsignedUnder = under != nullptr && !under->name.empty() &&
                                   under->name[0] == 'u';
        const std::int64_t raw =
            unsignedUnder
                ? static_cast<std::int64_t>(readUnsigned(member, type->size))
                : readSigned(member, type->size);
        for (const auto& enumerator : type->enumerators) {
            if (enumerator.value == raw) {
                return JsonValue::string(enumerator.name);
            }
        }
        return makeUnexpected(Error{
            StatusCode::ParseError,
            "encodeStruct: campo '" + prop.name + "' (enum '" +
                prop.typeName + "') tem valor " + std::to_string(raw) +
                " sem enumerador nomeado"});
    }
    }
    return makeUnexpected(
        unregisteredType("encodeStruct", prop.typeName));
}

// --- decode: um campo -------------------------------------------------------

[[nodiscard]] eng::core::Result<void> decodeMember(const JsonValue& field,
                                                   void* member,
                                                   const PropertyInfo& prop)
{
    // Codec de campo por NOME de tipo tem PRECEDÊNCIA.
    if (const FieldTypeCodec* codec = findFieldTypeCodec(prop.typeName)) {
        return codec->decode(field, member);
    }

    const TypeInfo* type = findType(prop.typeName);
    if (type == nullptr) {
        return makeUnexpected(
            unregisteredType("decodeStruct", prop.typeName));
    }

    switch (type->kind) {
    case TypeKind::Primitive: {
        if (prop.typeName == "bool") {
            if (!field.isBool()) {
                return makeUnexpected(
                    wrongType("decodeStruct", prop.name, "bool"));
            }
            *static_cast<bool*>(member) = field.asBool();
            return {};
        }
        if (prop.typeName == "string") {
            if (!field.isString()) {
                return makeUnexpected(
                    wrongType("decodeStruct", prop.name, "string"));
            }
            *static_cast<std::string*>(member) = field.asString();
            return {};
        }
        if (prop.typeName == "f32") {
            if (!field.isNumber()) {
                return makeUnexpected(
                    wrongType("decodeStruct", prop.name, "número"));
            }
            const double v = field.asF64();
            if (v > 3.402823466e38 || v < -3.402823466e38) {
                return makeUnexpected(Error{
                    StatusCode::ParseError,
                    "decodeStruct: campo '" + prop.name +
                        "' fora da faixa de f32"});
            }
            *static_cast<float*>(member) = static_cast<float>(v);
            return {};
        }
        if (prop.typeName == "f64") {
            if (!field.isNumber()) {
                return makeUnexpected(
                    wrongType("decodeStruct", prop.name, "número"));
            }
            *static_cast<double*>(member) = field.asF64();
            return {};
        }
        if (!field.isInteger() && !field.isUnsigned()) {
            return makeUnexpected(
                wrongType("decodeStruct", prop.name, "inteiro"));
        }
        const bool unsignedField = prop.typeName[0] == 'u';
        if (unsignedField) {
            if (field.isInteger()) {
                if (field.asI64() < 0) {
                    return makeUnexpected(Error{
                        StatusCode::ParseError,
                        "decodeStruct: campo '" + prop.name +
                            "' é sem sinal mas o JSON tem negativo"});
                }
                writeInteger(member, type->size, /*isSigned=*/false,
                             field.asI64());
            } else {
                writeInteger(member, type->size, /*isSigned=*/false,
                             static_cast<std::int64_t>(field.asU64()));
            }
        } else {
            if (field.isUnsigned() &&
                field.asU64() > 0x7FFF'FFFF'FFFF'FFFFull) {
                return makeUnexpected(Error{
                    StatusCode::ParseError,
                    "decodeStruct: campo '" + prop.name +
                        "' com sinal não comporta o valor"});
            }
            writeInteger(member, type->size, /*isSigned=*/true,
                         field.asI64());
        }
        return {};
    }
    case TypeKind::Struct:
        if (!field.isObject()) {
            return makeUnexpected(
                wrongType("decodeStruct", prop.name, "objeto"));
        }
        return decodeStruct(field, member, *type);
    case TypeKind::Enum: {
        if (!field.isString()) {
            return makeUnexpected(
                wrongType("decodeStruct", prop.name,
                          "nome de enumerador (string)"));
        }
        const std::string& name = field.asString();
        for (const auto& enumerator : type->enumerators) {
            if (enumerator.name == name) {
                const TypeInfo* under =
                    TypeRegistry::global().find(type->underlyingTypeId);
                const bool unsignedUnder =
                    under != nullptr && !under->name.empty() &&
                    under->name[0] == 'u';
                writeInteger(member, type->size, !unsignedUnder,
                             enumerator.value);
                return {};
            }
        }
        return makeUnexpected(Error{
            StatusCode::ParseError,
            "decodeStruct: campo '" + prop.name + "': enumerador '" + name +
                "' não existe em '" + prop.typeName + "'"});
    }
    }
    return makeUnexpected(unregisteredType("decodeStruct", prop.typeName));
}

} // namespace

eng::core::Result<JsonValue> encodeStruct(const void* obj,
                                          const TypeInfo& type)
{
    if (type.kind != TypeKind::Struct) {
        return makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "encodeStruct: tipo '" + type.name + "' não é Struct"});
    }

    JsonValue out = JsonValue::object();
    for (const PropertyInfo& prop : type.properties) {
        const void* member =
            static_cast<const char*>(obj) + prop.offset;
        auto encoded = encodeMember(member, prop);
        if (encoded.isError()) {
            return makeUnexpected(encoded.error());
        }
        out.set(prop.name, std::move(encoded.value()));
    }
    return out;
}

eng::core::Result<void> decodeStruct(const JsonValue& value, void* obj,
                                     const TypeInfo& type)
{
    if (type.kind != TypeKind::Struct) {
        return makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "decodeStruct: tipo '" + type.name + "' não é Struct"});
    }
    if (!value.isObject()) {
        return makeUnexpected(
            Error{StatusCode::ParseError,
                  "decodeStruct: valor para '" + type.name +
                      "' não é objeto"});
    }

    for (const PropertyInfo& prop : type.properties) {
        // ESTRITO com campo ausente (schema v1 — ADR-031); chaves
        // desconhecidas do JSON são IGNORADAS (evolução aditiva).
        const auto field = value.find(prop.name);
        if (!field.has_value()) {
            return makeUnexpected(Error{
                StatusCode::ParseError,
                "decodeStruct: campo obrigatório ausente: '" + prop.name +
                    "' em '" + type.name + "'"});
        }
        void* member = static_cast<char*>(obj) + prop.offset;
        const auto decoded = decodeMember(*field, member, prop);
        if (decoded.isError()) {
            return makeUnexpected(decoded.error());
        }
    }
    return {};
}

} // namespace eng::serial
