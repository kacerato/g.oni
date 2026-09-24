#pragma once

/// eng::serial::JsonValue — wrapper MÍNIMO verificado sobre nlohmann::json
///.
///
/// Por que wrapper (e não alias direto): o runtime compila -fno-exceptions
/// — o uso de nlohmann fica CONFINADO às vias que não lançam:
///   - parse via allow_exceptions=false + is_discarded();
///   - leitura SEMPRE pré-checada (isString/isNumber/... antes de get);
///   - dump com error_handler replace (UTF-8 inválido → U+FFFD, sem abort).
/// A fachada torna essas regras IMPOSSÍVEIS de contornar por engano.
///
/// Determinismo: objetos são std::map internamente → chaves
/// ordenadas no dump, independente da ordem de inserção; floats saem na
/// forma mais curta que preserva o valor (round-trip exato).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "eng/core/Result.hpp"

namespace eng::serial {

class JsonValue {
public:
    // --- fábricas -------------------------------------------------------------

    [[nodiscard]] static JsonValue null();
    [[nodiscard]] static JsonValue boolean(bool value);
    [[nodiscard]] static JsonValue integer(std::int64_t value);
    [[nodiscard]] static JsonValue uinteger(std::uint64_t value);
    [[nodiscard]] static JsonValue real(double value);
    [[nodiscard]] static JsonValue string(std::string_view value);
    [[nodiscard]] static JsonValue array();
    [[nodiscard]] static JsonValue object();

    // --- consultas ------------------------------------------------------------

    [[nodiscard]] bool isNull() const;
    [[nodiscard]] bool isBool() const;
    [[nodiscard]] bool isInteger() const;  // inteiro com sinal
    [[nodiscard]] bool isUnsigned() const; // inteiro sem sinal
    [[nodiscard]] bool isNumber() const;   // qualquer número
    [[nodiscard]] bool isString() const;
    [[nodiscard]] bool isArray() const;
    [[nodiscard]] bool isObject() const;

    /// Elementos de array OU entradas de objeto.
    [[nodiscard]] std::size_t size() const;

    // --- leitura (pré-checada: UB se o tipo não corresponder — os as* só
    //     são válidos após o is* correspondente; o StructCodec é o cliente
    //     canônico e sempre pré-checa) -----------------------------------------

    [[nodiscard]] bool asBool() const;
    [[nodiscard]] std::int64_t asI64() const;  // inteiro (sinal ou não)
    [[nodiscard]] std::uint64_t asU64() const; // inteiro (sinal ou não)
    [[nodiscard]] double asF64() const;        // qualquer número
    [[nodiscard]] const std::string& asString() const;

    /// Elemento de array (cópia do valor). Pré-condição: isArray e
    /// index < size(); fora do contrato → null (nunca UB).
    [[nodiscard]] JsonValue at(std::size_t index) const;

    /// Campo de objeto (cópia do valor); nullopt se ausente ou se `this`
    /// não for objeto. Cópia proposital: mantém o wrapper livre de casts
    /// entre layouts (o codec interno lê via raw(), sem cópia).
    [[nodiscard]] std::optional<JsonValue> find(std::string_view key) const;

    /// Itera as chaves de um objeto em ordem lexicográfica (std::map).
    /// Pré-condição: isObject.
    void eachKey(const std::function<void(std::string_view)>& fn) const;

    // --- construção (mutação) --------------------------------------------------

    /// Acrescenta ao array (pré-condição: isArray).
    void append(JsonValue value);

    /// Define/substitui campo do objeto (pré-condição: isObject).
    void set(std::string_view key, JsonValue value);

    // --- (de)serialização de texto ----------------------------------------------

    /// Texto JSON compacto (determinístico — ver cabeçalho do módulo).
    [[nodiscard]] std::string dump() const;

    /// Acesso ao valor nlohmann subjacente (uso interno de eng::serial;
    /// consumidores externos não devem depender disto).
    [[nodiscard]] const nlohmann::json& raw() const noexcept { return value_; }
    [[nodiscard]] nlohmann::json& raw() noexcept { return value_; }

    /// Ponte interna com o valor nlohmann (parse do módulo e uso por
    /// engenharia — consumidores preferem as fábricas/leituras pré-checadas).
    explicit JsonValue(nlohmann::json value) : value_(std::move(value)) {}

    nlohmann::json value_ = nlohmann::json::value_t::null;
};

} // namespace eng::serial
