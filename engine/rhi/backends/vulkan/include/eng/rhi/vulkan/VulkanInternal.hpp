#pragma once

/// Internos compartilhados do backend Vulkan. NÃO é API pública —
/// usado entre as unidades de tradução do backend e pelos testes unitários
/// das funções puras (sem loader).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "eng/core/Error.hpp"
#include "eng/rhi/Types.hpp"

// Idem VulkanLoader.hpp — a macro precisa estar definida ANTES do
// PRIMEIRO include de vulkan.h em QUALQUER TU (guards tornam os demais
// includes no-ops).
#ifdef __ANDROID__
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace eng::rhi::vulkan {

[[nodiscard]] inline eng::core::Error makeError(eng::core::StatusCode code,
                                                std::string_view message) {
    return eng::core::Error{code, std::string{message}};
}

/// Mapeamento Format (abstraction) → VkFormat. `VK_FORMAT_UNDEFINED` quando
/// o formato não existe em Vulkan (o chamador valida antes).
[[nodiscard]] constexpr VkFormat toVkFormat(eng::rhi::Format format) noexcept {
    using F = eng::rhi::Format;
    switch (format) {
    case F::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case F::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case F::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case F::R32G32Sfloat: return VK_FORMAT_R32G32_SFLOAT;
    case F::R32G32B32A32Sfloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case F::R16G16B16A16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::D32Sfloat: return VK_FORMAT_D32_SFLOAT;
    case F::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case F::Undefined: return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

/// Mapeamento VkFormat → Format da abstraction (para reportar o REAL da
/// swapchain). `Format::Undefined` quando não mapeado.
[[nodiscard]] constexpr eng::rhi::Format fromVkFormat(VkFormat format) noexcept {
    using F = eng::rhi::Format;
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return F::R8G8B8A8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM: return F::B8G8R8A8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return F::R8G8B8A8Srgb;
    case VK_FORMAT_B8G8R8A8_SRGB: return F::B8G8R8A8Srgb;
    case VK_FORMAT_R32G32_SFLOAT: return F::R32G32Sfloat;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return F::R32G32B32A32Sfloat;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return F::R16G16B16A16Sfloat;
    case VK_FORMAT_D32_SFLOAT: return F::D32Sfloat;
    case VK_FORMAT_D24_UNORM_S8_UINT: return F::D24UnormS8Uint;
    default: return F::Undefined;
    }
}

/// Valida um blob SPIR-V: tamanho múltiplo de 4, >= 20 bytes (cabeçalho) e
/// magic 0x07230203 (little-endian na ordem de bytes do stream).
/// Mensagem precisa quando inválido (missão §27).
[[nodiscard]] inline bool spirvLooksValid(const unsigned char* bytes, std::size_t size,
                                          std::string& outError) {
    if (size == 0) {
        outError = "SPIR-V vazio";
        return false;
    }
    if (size % 4 != 0) {
        outError = "SPIR-V com tamanho não múltiplo de 4 (" + std::to_string(size) + ")";
        return false;
    }
    if (size < 20) {
        outError = "SPIR-V menor que o cabeçalho (20 bytes): " + std::to_string(size);
        return false;
    }
    const std::uint32_t magic = static_cast<std::uint32_t>(bytes[0]) |
                               (static_cast<std::uint32_t>(bytes[1]) << 8) |
                               (static_cast<std::uint32_t>(bytes[2]) << 16) |
                               (static_cast<std::uint32_t>(bytes[3]) << 24);
    if (magic != 0x07230203u) {
        outError = "SPIR-V com magic incorreto (esperado 0x07230203)";
        return false;
    }
    return true;
}

/// Tabela de recursos com handles generacionais (contrato ADR-035): índice
/// em 32 bits baixos (+1), geração em 32 bits altos. Slots liberados são
/// reusados com geração incrementada — handles stale são detectados.
template <typename Entry>
class HandleTable {
public:
    HandleTable() = default;

    /// Insere e devolve o handle novo.
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

    /// Acesso por handle; nullptr se nulo/stale/destruído.
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

    /// Remove e devolve a entry (para destruição); false se nulo/stale.
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

    /// Todas as entries vivas (para destruição em cascata).
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

} // namespace eng::rhi::vulkan
