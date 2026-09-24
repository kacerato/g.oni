#pragma once

/// eng::math — biblioteca matemática própria.
/// Convenções: column-major, right-handed, ângulos em radianos.
/// Profundidade: GL [-1,1] por padrão; Vulkan [0,1] com ENG_MATH_VULKAN_DEPTH.
#include "eng/math/Mat4.hpp"
#include "eng/math/Quat.hpp"
#include "eng/math/Transform.hpp"
#include "eng/math/Vec2.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/math/Vec4.hpp"
