#pragma once

#include <optional>

#include "eng/math/Vec3.hpp"
#include "eng/math/Vec4.hpp"

namespace eng::math {

/// Matriz 4x4 de floats, column-major: `m[col * 4 + row]`.
/// Vetores são colunas: `v' = M * v` com `v'(r) = Σ_k M(k, r) * v(k)`.
/// Convenção right-handed, câmera olhando para -Z, ângulos em radianos.
///
/// Faixa de profundidade (ENG_MATH_VULKAN_DEPTH, CMake):
///   OFF (padrão) → GL:      z_ndc ∈ [-1, 1]
///   ON           → Vulkan:  z_ndc ∈ [0, 1]
struct Mat4 {
    float m[16]{}; // column-major, zerada por padrão (identity() para a identidade)

    [[nodiscard]] static constexpr Mat4 identity() noexcept;

    /// Matriz de translação pura.
    [[nodiscard]] static constexpr Mat4 translation(const Vec3& t) noexcept;

    /// Matriz de escala pura.
    [[nodiscard]] static constexpr Mat4 scale(const Vec3& s) noexcept;

    /// Rotações elementares em radianos (sentido anti-horário olhando do
    /// eixo positivo para a origem).
    [[nodiscard]] static Mat4 rotationX(float radians) noexcept;
    [[nodiscard]] static Mat4 rotationY(float radians) noexcept;
    [[nodiscard]] static Mat4 rotationZ(float radians) noexcept;

    /// Projeção perspectiva (fovY em radianos; nearZ/farZ são distâncias
    /// positivas; câmera olha para -Z).
    [[nodiscard]] static Mat4 perspective(float fovYRadians, float aspect,
                                          float nearZ, float farZ) noexcept;

    /// Projeção ortográfica (nearZ/farZ positivos; câmera olha para -Z).
    [[nodiscard]] static Mat4 orthographic(float left, float right, float bottom,
                                           float top, float nearZ, float farZ) noexcept;

    /// Base de câmera: eye → target com up como referência de verticalidade.
    /// Degenerada quando forward ∥ up (chamador deve evitar).
    [[nodiscard]] static Mat4 lookAt(const Vec3& eye, const Vec3& target,
                                     const Vec3& up) noexcept;

    // --- acesso ---------------------------------------------------------------

    /// Elemento por (coluna, linha) — consistente com o layout column-major.
    [[nodiscard]] constexpr float at(int col, int row) const noexcept { return m[col * 4 + row]; }
    constexpr float& at(int col, int row) noexcept { return m[col * 4 + row]; }

    /// Coluna `c` como Vec4.
    [[nodiscard]] constexpr Vec4 column(int c) const noexcept {
        return {m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]};
    }

    // --- álgebra ----------------------------------------------------------------

    /// Composição: (A * B) aplica B antes de A sobre vetores-coluna.
    [[nodiscard]] Mat4 operator*(const Mat4& o) const noexcept;

    /// Transforma um Vec4 homogêneo.
    [[nodiscard]] Vec4 operator*(const Vec4& v) const noexcept;

    /// Transforma um ponto (w=1) e divide por w (projeções inclusas).
    [[nodiscard]] Vec3 transformPoint(const Vec3& p) const noexcept;

    /// Transforma uma direção (w=0) — ignora translação.
    [[nodiscard]] Vec3 transformDirection(const Vec3& d) const noexcept;

    [[nodiscard]] Mat4 transposed() const noexcept;
    [[nodiscard]] float determinant() const noexcept;

    /// Inversa via adjugata; nullopt quando singular (|det| < 1e-12).
    [[nodiscard]] std::optional<Mat4> inverted() const noexcept;
};

[[nodiscard]] constexpr Mat4 Mat4::identity() noexcept {
    Mat4 out;
    out.m[0] = 1.0f;
    out.m[5] = 1.0f;
    out.m[10] = 1.0f;
    out.m[15] = 1.0f;
    return out;
}

[[nodiscard]] constexpr Mat4 Mat4::translation(const Vec3& t) noexcept {
    Mat4 out = identity();
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

[[nodiscard]] constexpr Mat4 Mat4::scale(const Vec3& s) noexcept {
    Mat4 out = identity();
    out.m[0] = s.x;
    out.m[5] = s.y;
    out.m[10] = s.z;
    return out;
}

} // namespace eng::math
