#include "eng/editor/AnimationAssets.hpp"

/// eng::editor::AnimationAssets — implementação do codec JSON.

#include <algorithm>
#include <cmath>

#include "eng/math/Mat4.hpp"
#include "eng/math/Quat.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/serial/Json.hpp"

namespace eng::editor {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error animError(std::string message)
{
    return Error{StatusCode::InvalidArgument,
                 "animation: " + std::move(message)};
}

/// Extrai um float de um JsonValue numérico (rejeita lixo com diag).
[[nodiscard]] bool numberOr(const eng::serial::JsonValue& v, float& out,
                            std::vector<AnimDiag>* diags,
                            const char* what)
{
    if (!v.isNumber()) {
        if (diags != nullptr) {
            diags->push_back({0u, std::string(what) + " não é número"});
        }
        return false;
    }
    out = static_cast<float>(v.asF64());
    return true;
}

/// Graus Euler (convenção do autor) → Quat (convenção da engine — a
/// MESMA fórmula de EditorDocument::quatFromDegrees).
[[nodiscard]] eng::math::Quat quatFromDegrees(
    const eng::math::Vec3& degrees) noexcept
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
    return eng::math::Quat::fromEulerAngles(
        degrees.x * kDegToRad, degrees.y * kDegToRad,
        degrees.z * kDegToRad);
}

/// Uma track [ [t, ...componentes], ... ] genérica (N floats por key).
template <std::size_t N>
[[nodiscard]] bool decodeVecTrack(const eng::serial::JsonValue& array,
                                  std::vector<float>& times,
                                  std::vector<std::array<float, N>>& values,
                                  std::vector<AnimDiag>* diags,
                                  const char* what)
{
    if (!array.isArray()) {
        if (diags != nullptr) {
            diags->push_back({0u, std::string(what) + " não é array"});
        }
        return false;
    }
    for (std::size_t i = 0; i < array.size(); ++i) {
        const eng::serial::JsonValue key = array.at(i);
        if (!key.isArray() || key.size() < N + 1u) {
            if (diags != nullptr) {
                diags->push_back({0u, std::string(what) + " key " +
                                          std::to_string(i) +
                                          " precisa de [t, " +
                                          std::to_string(N) + " valores]"});
            }
            return false;
        }
        float t = 0.f;
        if (!numberOr(key.at(0), t, diags, what)) {
            return false;
        }
        std::array<float, N> value{};
        for (std::size_t c = 0; c < N; ++c) {
            if (!numberOr(key.at(c + 1u), value[c], diags, what)) {
                return false;
            }
        }
        times.push_back(t);
        values.push_back(value);
    }
    return true;
}

}  // namespace

Result<AnimationAsset> animationDecode(std::string_view json,
                                       std::vector<AnimDiag>* diags)
{
    auto parsed = eng::serial::parseJson(json);
    if (parsed.isError()) {
        return makeUnexpected(animError("JSON inválido: " +
                                       parsed.error().message));
    }
    const eng::serial::JsonValue root = parsed.value();
    if (!root.isObject()) {
        return makeUnexpected(animError("raiz não é objeto"));
    }

    AnimationAsset asset;

    // name (obrigatório — o banco do Play indexa por ele).
    const auto name = root.find("name");
    if (name.has_value() && name->isString()) {
        asset.clip.name = name->asString();
    } else {
        if (diags != nullptr) {
            diags->push_back({0u, "'name' ausente ou não-string"});
        }
        return makeUnexpected(animError("'name' é obrigatório"));
    }

    // Metadados de authoring (fps/loop).
    if (const auto fps = root.find("fps");
        fps.has_value() && fps->isNumber() && fps->asF64() > 0.0) {
        asset.meta.fps = static_cast<float>(fps->asF64());
    }
    if (const auto loop = root.find("loop"); loop.has_value() && loop->isBool()) {
        asset.meta.loop = loop->asBool();
    }
    // Hold do último frame (default = 1/fps do meta).
    asset.clip.frameHold =
        asset.meta.fps > 0.f ? 1.f / asset.meta.fps : 0.f;
    if (const auto hold = root.find("frameHold");
        hold.has_value() && hold->isNumber() && hold->asF64() >= 0.0) {
        asset.clip.frameHold = static_cast<float>(hold->asF64());
    }

    bool ok = true;

    // position: [t, x, y, z]
    if (const auto track = root.find("position"); track.has_value()) {
        std::vector<float> times;
        std::vector<std::array<float, 3>> values;
        ok = decodeVecTrack<3>(*track, times, values, diags,
                               "'position'") && ok;
        if (ok) {
            for (std::size_t i = 0; i < times.size(); ++i) {
                asset.clip.position.push_back(
                    eng::animation::PositionKey{
                        times[i], eng::math::Vec3{values[i][0],
                                                 values[i][1],
                                                 values[i][2]}});
            }
        }
    }

    // rotation: [t, grausX, grausY, grausZ] — Euler do AUTOR.
    if (ok) {
        if (const auto track = root.find("rotation"); track.has_value()) {
            std::vector<float> times;
            std::vector<std::array<float, 3>> values;
            ok = decodeVecTrack<3>(*track, times, values, diags,
                                   "'rotation'") &&
                 ok;
            if (ok) {
                for (std::size_t i = 0; i < times.size(); ++i) {
                    asset.clip.rotation.push_back(
                        eng::animation::RotationKey{
                            times[i], quatFromDegrees(eng::math::Vec3{
                                          values[i][0], values[i][1],
                                          values[i][2]})});
                }
            }
        }
    }

    // scale: [t, x, y, z]
    if (ok) {
        if (const auto track = root.find("scale"); track.has_value()) {
            std::vector<float> times;
            std::vector<std::array<float, 3>> values;
            ok = decodeVecTrack<3>(*track, times, values, diags,
                                   "'scale'") &&
                 ok;
            if (ok) {
                for (std::size_t i = 0; i < times.size(); ++i) {
                    asset.clip.scale.push_back(
                        eng::animation::ScaleKey{
                            times[i], eng::math::Vec3{values[i][0],
                                                      values[i][1],
                                                      values[i][2]}});
                }
            }
        }
    }

    // frames: [t, "textura", u0, v0, u1, v1]
    if (ok) {
        if (const auto track = root.find("frames"); track.has_value()) {
        if (!track->isArray()) {
            if (diags != nullptr) {
                diags->push_back({0u, "'frames' não é array"});
            }
            ok = false;
        } else {
            for (std::size_t i = 0; i < track->size(); ++i) {
                const eng::serial::JsonValue key = track->at(i);
                if (!key.isArray() || key.size() < 2u ||
                    !key.at(1).isString()) {
                    if (diags != nullptr) {
                        diags->push_back({0u, "'frames' key " +
                                                  std::to_string(i) +
                                                  " precisa de [t, "
                                                  "\"textura\", u0, v0, "
                                                  "u1, v1]"});
                    }
                    ok = false;
                    break;
                }
                eng::animation::SpriteFrameKey frame;
                if (!numberOr(key.at(0), frame.time, diags, "'frames'")) {
                    ok = false;
                    break;
                }
                frame.textureAsset = key.at(1).asString();
                if (key.size() >= 6u) {
                    ok = numberOr(key.at(2), frame.u0, diags, "'frames'") &&
                         ok;
                    ok = numberOr(key.at(3), frame.v0, diags, "'frames'") &&
                         ok;
                    ok = numberOr(key.at(4), frame.u1, diags, "'frames'") &&
                         ok;
                    ok = numberOr(key.at(5), frame.v1, diags, "'frames'") &&
                         ok;
                }
                if (!ok) {
                    break;
                }
                asset.clip.frames.push_back(std::move(frame));
            }
        }
        }
    }

    if (!ok) {
        return makeUnexpected(
            animError("conteúdo inválido (ver diagnósticos)"));
    }
    return asset;
}

Result<std::string> animationEncode(const AnimationAsset& asset)
{
    using eng::serial::JsonValue;

    JsonValue root = JsonValue::object();
    root.raw()["name"] = asset.clip.name;
    root.raw()["fps"] = static_cast<std::uint64_t>(
        std::max(1, static_cast<int>(std::round(asset.meta.fps))));
    root.raw()["loop"] = asset.meta.loop;
    root.raw()["frameHold"] = asset.clip.frameHold;

    {
        JsonValue track = JsonValue::array();
        for (const auto& key : asset.clip.position) {
            track.raw().push_back(nlohmann::json::array(
                {key.time, key.value.x, key.value.y, key.value.z}));
        }
        root.raw()["position"] = track.raw();
    }
    {
        // Quat → graus Euler do AUTOR (round-trip do decode) — MESMA
        // extração YXZ do EditorDocument (uma convenção só).
        constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;
        JsonValue track = JsonValue::array();
        for (const auto& key : asset.clip.rotation) {
            const eng::math::Mat4 mat = key.value.toMatrix();
            const float sinPitch = std::clamp(-mat.at(2, 1), -1.f, 1.f);
            const float pitch = std::asin(sinPitch);
            float yaw = 0.f;
            float roll = 0.f;
            if (std::abs(std::cos(pitch)) > 1e-4f) {
                yaw = std::atan2(mat.at(2, 0), mat.at(2, 2));
                roll = std::atan2(mat.at(0, 1), mat.at(1, 1));
            } else {
                roll = std::atan2(-mat.at(1, 0), mat.at(0, 0));
            }
            track.raw().push_back(nlohmann::json::array(
                {key.time, pitch * kRadToDeg, yaw * kRadToDeg,
                 roll * kRadToDeg}));
        }
        root.raw()["rotation"] = track.raw();
    }
    {
        JsonValue track = JsonValue::array();
        for (const auto& key : asset.clip.scale) {
            track.raw().push_back(nlohmann::json::array(
                {key.time, key.value.x, key.value.y, key.value.z}));
        }
        root.raw()["scale"] = track.raw();
    }
    {
        JsonValue track = JsonValue::array();
        for (const auto& key : asset.clip.frames) {
            track.raw().push_back(nlohmann::json::array(
                {key.time, key.textureAsset, key.u0, key.v0, key.u1,
                 key.v1}));
        }
        root.raw()["frames"] = track.raw();
    }
    return root.dump();
}

}  // namespace eng::editor
