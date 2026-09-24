#pragma once

namespace eng::math {

/// Vetor 3D de floats (posições, direções, cores lineares).
/// Convenção: right-handed, ângulos em radianos.
struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    // --- aritmética -----------------------------------------------------------

    [[nodiscard]] constexpr Vec3 operator+(const Vec3& o) const noexcept {
        return {x + o.x, y + o.y, z + o.z};
    }
    [[nodiscard]] constexpr Vec3 operator-(const Vec3& o) const noexcept {
        return {x - o.x, y - o.y, z - o.z};
    }
    [[nodiscard]] constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }
    [[nodiscard]] constexpr Vec3 operator*(float s) const noexcept {
        return {x * s, y * s, z * s};
    }
    [[nodiscard]] constexpr Vec3 operator/(float s) const noexcept {
        return {x / s, y / s, z / s};
    }

    constexpr Vec3& operator+=(const Vec3& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr Vec3& operator-=(const Vec3& o) noexcept {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    constexpr Vec3& operator*=(float s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    /// Produto escalar.
    [[nodiscard]] constexpr float dot(const Vec3& o) const noexcept {
        return x * o.x + y * o.y + z * o.z;
    }

    /// Produto vetorial (right-handed: x cross y = z).
    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const noexcept {
        return {
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x,
        };
    }

    [[nodiscard]] constexpr float lengthSquared() const noexcept { return dot(*this); }
    [[nodiscard]] float length() const noexcept;

    /// Versão normalizada; vetor nulo devolve vetor nulo (garantia sem NaN).
    [[nodiscard]] Vec3 normalized() const noexcept;

    [[nodiscard]] constexpr bool operator==(const Vec3& o) const noexcept = default;
};

[[nodiscard]] constexpr Vec3 operator*(float s, const Vec3& v) noexcept { return v * s; }

} // namespace eng::math
