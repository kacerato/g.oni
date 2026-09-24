#pragma once

/// eng::editor::AnimationAssets — codec JSON de AnimationClip.
///
/// Formato do asset .anim.json (assets/animations/<nome>.anim.json):
///
/// {
///   "name": "spin",                // nome do clip (banco do Play)
///   "fps": 8,                      // cadência de authoring de frames
///   "loop": true,                  // default do Animator ao atribuir
///   "position": [[t, x, y, z], ...],
///   "rotation": [[t, degX, degY, degZ], ...],   // Euler GRAUS (author)
///   "scale":    [[t, x, y, z], ...],
///   "frames":   [[t, "textura.png", u0, v0, u1, v1], ...]
/// }
///
/// Decisões:
/// - Rotação em GRAUS Euler (a convenção do AUTOR — Inspector/gizmo); a
///   conversão para Quat acontece no DECODE (a engine continua em Quat).
/// - Tracks ausentes simplesmente não existem no JSON (clip parcial).
/// - O mesmo formato é usado no Play (banco) e no preview (editor).

#include <string>
#include <string_view>
#include <vector>

#include "eng/animation/Animation.hpp"
#include "eng/core/Result.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::editor {

/// Metadados de authoring que acompanham o clip (fora da engine).
struct AnimationAssetMeta {
    float fps{8.f};     ///< cadência sugerida p/ adicionar frames
    bool loop{true};    ///< default de loop ao atribuir ao Animator
};

/// Diagnóstico de validação (1-based, mesma forma do scriptCompile).
struct AnimDiag {
    std::uint32_t line = 0;
    std::string message;
};

/// Clip decodificado + metadados.
struct AnimationAsset {
    eng::animation::AnimationClip clip{};
    AnimationAssetMeta meta{};

    /// Lista plana de frames (espelho de clip.frames para a UI editar).
    [[nodiscard]] std::size_t frameCount() const noexcept
    {
        return clip.frames.size();
    }
};

/// JSON → AnimationAsset. Falha com TODOS os diagnósticos coletados
/// (formato é Result: erros internos de parse; conteúdo inválido entra
/// em `diags` — chamador decide veredicto por diags vazios).
[[nodiscard]] eng::core::Result<AnimationAsset> animationDecode(
    std::string_view json, std::vector<AnimDiag>* diags = nullptr);

/// AnimationAsset → JSON (dump estável — round-trip testado).
[[nodiscard]] eng::core::Result<std::string> animationEncode(
    const AnimationAsset& asset);

} // namespace eng::editor
