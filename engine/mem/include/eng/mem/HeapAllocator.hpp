#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "eng/mem/Allocator.hpp"

namespace eng::mem {

/// Alocador de backing sobre malloc/posix_memalign com rastreio de
/// alocações vivas (permite `owns` e relatório de vazamentos no destrutor).
///
/// Estatísticas são O(1); `owns`/`deallocate` são O(1) amortizado via mapa
/// de ponteiros vivos — o custo existe de propósito nesta fase (rastreio em
/// debug) e será reduzido em hot paths nas fases seguintes.
class HeapAllocator final : public Allocator {
public:
    /// `name` deve apontar para armazenamento durável (string literal).
    explicit HeapAllocator(const char* name = "HeapAllocator") noexcept;

    ~HeapAllocator() override;

    HeapAllocator(const HeapAllocator&) = delete;
    HeapAllocator& operator=(const HeapAllocator&) = delete;

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept override;
    void deallocate(void* ptr) noexcept override;
    [[nodiscard]] void* reallocate(void* ptr, std::size_t newSize,
                                   std::size_t alignment) noexcept override;
    [[nodiscard]] bool owns(const void* ptr) const noexcept override;
    [[nodiscard]] const char* name() const noexcept override;

    // --- estatísticas --------------------------------------------------------

    struct Stats {
        std::size_t activeAllocations;  ///< alocações vivas agora
        std::size_t activeBytes;         ///< bytes vivos agora
        std::size_t totalAllocations;    ///< alocações desde a construção
        std::size_t peakAllocations;     ///< pico de alocações simultâneas
    };

    [[nodiscard]] Stats stats() const noexcept;

    /// Verdadeiro quando o destrutor encontraria vazamentos.
    [[nodiscard]] bool hasLeaks() const noexcept;

private:
    struct AllocationInfo {
        std::size_t size;
    };

    const char* name_;
    std::unordered_map<void*, AllocationInfo> live_;
    std::size_t activeBytes_{0};
    std::size_t totalAllocations_{0};
    std::size_t peakAllocations_{0};
};

} // namespace eng::mem
