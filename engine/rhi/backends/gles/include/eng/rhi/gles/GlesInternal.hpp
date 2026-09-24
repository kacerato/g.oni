#pragma once

/// Internos compartilhados do backend OpenGL ES. NÃO é API pública.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "eng/core/Error.hpp"
#include "eng/rhi/Types.hpp"

#include "eng/rhi/gles/GlesLoader.hpp"

namespace eng::rhi::gles {

[[nodiscard]] inline eng::core::Error makeError(eng::core::StatusCode code,
                                                std::string_view message) {
    return eng::core::Error{code, std::string{message}};
}

/// Mapeamento Format (abstraction) → internalFormat/basically GL enums.
/// `0` quando não existe em GLES (o chamador valida antes).
[[nodiscard]] constexpr GLenum toGlFormat(eng::rhi::Format format) noexcept {
    using F = eng::rhi::Format;
    switch (format) {
    case F::R8G8B8A8Unorm: return GL_RGBA8;
    case F::B8G8R8A8Unorm: return GL_RGBA8;  // GLES é RGBA-first (sem BGRA nativo)
    case F::R8G8B8A8Srgb: return GL_SRGB8_ALPHA8;
    case F::B8G8R8A8Srgb: return GL_SRGB8_ALPHA8;
    case F::R32G32Sfloat: return GL_RG32F;
    case F::R32G32B32A32Sfloat: return GL_RGBA32F;
    case F::R16G16B16A16Sfloat: return GL_RGBA16F;
    case F::D32Sfloat: return GL_DEPTH_COMPONENT32F;
    case F::D24UnormS8Uint: return GL_DEPTH24_STENCIL8;
    case F::Undefined: return 0;
    }
    return 0;
}

/// GLSL ES declara atributos com `vec4` (layout da abstraction usa
/// R32G32B32A32Sfloat — paridade com o fixture do Vulkan, missão §40).
[[nodiscard]] constexpr GLenum toGlVertexType(eng::rhi::Format format) noexcept {
    using F = eng::rhi::Format;
    switch (format) {
    case F::R8G8B8A8Unorm:
    case F::B8G8R8A8Unorm: return GL_UNSIGNED_BYTE;
    case F::R8G8B8A8Srgb:
    case F::B8G8R8A8Srgb: return GL_UNSIGNED_BYTE;
    case F::R32G32Sfloat: return GL_FLOAT;
    case F::R32G32B32A32Sfloat: return GL_FLOAT;
    case F::R16G16B16A16Sfloat: return GL_HALF_FLOAT;
    case F::D32Sfloat: return GL_FLOAT;
    case F::D24UnormS8Uint: return GL_UNSIGNED_INT;
    case F::Undefined: return 0;
    }
    return 0;
}

/// Formatos inteiros SEM SINAL mapeiam para [0,1] em GLSL (`normalized`
/// do glVertexAttribPointer). Bug C-15 da auditoria final: Unorm era lido
/// como 0-255 bruto (GL_FALSE), violando a semântica do formato da
/// abstraction. Srgb também é byte sem sinal normalizado (a conversão
/// gama é função do formato, não do atributo).
[[nodiscard]] constexpr GLboolean isGlNormalizedFormat(
    eng::rhi::Format format) noexcept {
    using F = eng::rhi::Format;
    switch (format) {
    case F::R8G8B8A8Unorm:
    case F::B8G8R8A8Unorm:
    case F::R8G8B8A8Srgb:
    case F::B8G8R8A8Srgb: return GL_TRUE;
    default: return GL_FALSE;
    }
}

/// Tamanho por componente dos formatos de vértice (em bytes).
[[nodiscard]] constexpr std::uint32_t formatComponents(eng::rhi::Format format) noexcept {
    using F = eng::rhi::Format;
    switch (format) {
    case F::R8G8B8A8Unorm:
    case F::B8G8R8A8Unorm:
    case F::R8G8B8A8Srgb:
    case F::B8G8R8A8Srgb:
    case F::R32G32B32A32Sfloat:
    case F::R16G16B16A16Sfloat: return 4;
    case F::R32G32Sfloat: return 2;  // RG — 2 componentes no atributo
    case F::D32Sfloat: return 1;
    case F::D24UnormS8Uint: return 2;
    case F::Undefined: return 0;
    }
    return 0;
}

/// Nome do estado GL de comparação de profundidade.
[[nodiscard]] constexpr GLenum toGlDepthFunc(eng::rhi::CompareOp op) noexcept {
    using O = eng::rhi::CompareOp;
    switch (op) {
    case O::Never: return GL_NEVER;
    case O::Less: return GL_LESS;
    case O::Equal: return GL_EQUAL;
    case O::LessOrEqual: return GL_LEQUAL;
    case O::Greater: return GL_GREATER;
    case O::NotEqual: return GL_NOTEQUAL;
    case O::GreaterOrEqual: return GL_GEQUAL;
    case O::Always: return GL_ALWAYS;
    }
    return GL_LESS;
}

/// Fatores de blend (src/dst) — missão §37 (intenção traduzida).
[[nodiscard]] constexpr GLenum toGlBlendFactor(eng::rhi::BlendFactor factor) noexcept {
    using F = eng::rhi::BlendFactor;
    switch (factor) {
    case F::Zero: return GL_ZERO;
    case F::One: return GL_ONE;
    case F::SrcAlpha: return GL_SRC_ALPHA;
    case F::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
    case F::DstAlpha: return GL_DST_ALPHA;
    case F::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

[[nodiscard]] constexpr GLenum toGlBlendEquation(eng::rhi::BlendOp op) noexcept {
    using O = eng::rhi::BlendOp;
    switch (op) {
    case O::Add: return GL_FUNC_ADD;
    case O::Subtract: return GL_FUNC_SUBTRACT;
    case O::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
    case O::Min: return GL_MIN;
    case O::Max: return GL_MAX;
    }
    return GL_FUNC_ADD;
}

/// Tabela de recursos com handles generacionais — mesmo contrato do
/// Vulkan: índice nos 32 bits baixos (+1), geração nos altos.
template <typename Entry>
class HandleTable {
public:
    HandleTable() = default;

    [[nodiscard]] std::uint64_t insert(Entry entry) {
        std::uint32_t index = 0;
        std::uint32_t generation = 1;
        if (!freeSlots_.empty()) {
            index = freeSlots_.back().first;
            generation = freeSlots_.back().second;
            freeSlots_.pop_back();
        } else {
            index = nextIndex_++;
        }
        entries_.emplace(index, std::move(entry));
        generations_[index] = generation;
        return (static_cast<std::uint64_t>(generation) << 32) |
               (static_cast<std::uint64_t>(index) + 1u);
    }

    [[nodiscard]] Entry* find(std::uint64_t handle) {
        if (handle == 0u) {
            return nullptr;
        }
        const std::uint32_t index = static_cast<std::uint32_t>((handle & 0xFFFFFFFFu) - 1u);
        const std::uint32_t generation = static_cast<std::uint32_t>(handle >> 32);
        if (generations_[index] != generation) {
            return nullptr;
        }
        const auto it = entries_.find(index);
        return it == entries_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Entry* find(std::uint64_t handle) const {
        return const_cast<HandleTable*>(this)->find(handle);
    }

    [[nodiscard]] bool remove(std::uint64_t handle, Entry& outEntry) {
        if (handle == 0u) {
            return false;
        }
        const std::uint32_t index = static_cast<std::uint32_t>((handle & 0xFFFFFFFFu) - 1u);
        const std::uint32_t generation = static_cast<std::uint32_t>(handle >> 32);
        if (generations_[index] != generation) {
            return false;
        }
        const auto it = entries_.find(index);
        if (it == entries_.end()) {
            return false;
        }
        outEntry = std::move(it->second);
        entries_.erase(it);
        freeSlots_.emplace_back(index, generation + 1);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] std::vector<Entry> drainAll() {
        std::vector<Entry> out{};
        out.reserve(entries_.size());
        for (auto& [index, entry] : entries_) {
            out.push_back(std::move(entry));
        }
        entries_.clear();
        return out;
    }

private:
    std::uint32_t nextIndex_{0};
    std::map<std::uint32_t, Entry> entries_{};
    std::map<std::uint32_t, std::uint32_t> generations_{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> freeSlots_{};
};

} // namespace eng::rhi::gles
