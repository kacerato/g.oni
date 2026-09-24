#include "eng/render/SpriteMaterial.hpp"

/// eng::render — codec do asset .mat.json.

#include "eng/serial/Json.hpp"

namespace eng::render {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error materialError(std::string message)
{
    return Error{StatusCode::InvalidArgument,
                 "material: " + std::move(message)};
}

}  // namespace

Result<MaterialAsset> materialDecode(std::string_view json)
{
    auto parsed = eng::serial::parseJson(json);
    if (parsed.isError()) {
        return makeUnexpected(
            materialError("JSON inválido: " + parsed.error().message));
    }
    const eng::serial::JsonValue root = parsed.value();
    if (!root.isObject()) {
        return makeUnexpected(materialError("raiz não é objeto"));
    }

    MaterialAsset asset;

    // name (obrigatório — o seletor do Inspector indexa por ele).
    const auto name = root.find("name");
    if (name.has_value() && name->isString()) {
        asset.name = name->asString();
    } else {
        return makeUnexpected(materialError("'name' é obrigatório"));
    }
    if (asset.name.empty()) {
        return makeUnexpected(materialError("'name' vazio"));
    }

    // shader (obrigatório — nome INVÁLIDO é erro, não fallback silencioso:
    // o material não pode prometer o que a ShaderLibrary não registra).
    const auto shader = root.find("shader");
    if (shader.has_value() && shader->isString()) {
        asset.material.shader = shader->asString();
    } else {
        return makeUnexpected(materialError("'shader' é obrigatório"));
    }
    if (!asset.material.hasValidShader()) {
        return makeUnexpected(materialError(
            "'shader' desconhecido: '" + asset.material.shader +
            "' (registrados: unlit, lit)"));
    }

    // tint [r, g, b, a] (opcional — neutro quando ausente).
    const auto tint = root.find("tint");
    if (tint.has_value()) {
        if (!tint->isArray() || tint->size() != 4u) {
            return makeUnexpected(
                materialError("'tint' deve ser [r, g, b, a]"));
        }
        for (std::size_t i = 0; i < 4u; ++i) {
            const auto channel = tint->at(i);
            if (!channel.isNumber()) {
                return makeUnexpected(
                    materialError("'tint' canal não-numérico"));
            }
            const float value = static_cast<float>(channel.asF64());
            if (value < 0.f) {
                return makeUnexpected(
                    materialError("'tint' negativo (canal " +
                                  std::to_string(i) + ")"));
            }
            (&asset.material.tintR)[i] = value;
        }
    }
    return asset;
}

Result<std::string> materialEncode(const MaterialAsset& asset)
{
    using eng::serial::JsonValue;

    JsonValue root = JsonValue::object();
    root.raw()["name"] = asset.name;
    root.raw()["shader"] = asset.material.shader;
    root.raw()["tint"] = nlohmann::json::array(
        {asset.material.tintR, asset.material.tintG, asset.material.tintB,
         asset.material.tintA});
    return root.dump();
}

}  // namespace eng::render
