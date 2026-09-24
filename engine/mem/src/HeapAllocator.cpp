#include "eng/mem/HeapAllocator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eng::mem {

namespace {

constexpr std::size_t kMinAlignment = sizeof(void*);

[[nodiscard]] bool isPowerOfTwo(std::size_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

/// Ajusta o alinhamento mínimo exigido pelo posix_memalign.
[[nodiscard]] std::size_t normalizeAlignment(std::size_t alignment) noexcept {
    if (alignment < kMinAlignment) {
        return kMinAlignment;
    }
    return alignment;
}

} // namespace

HeapAllocator::HeapAllocator(const char* name) noexcept : name_(name) {}

HeapAllocator::~HeapAllocator() {
    if (hasLeaks()) {
        // Relatório de vazamento no shutdown. eng::mem não depende de
        // eng::log nesta fase (justificativa em docs/architecture/00-overview.md).
        std::fprintf(stderr,
                     "[eng][mem][HeapAllocator] %zu alocacoes vazaram (%zu bytes) — '%s'\n",
                     live_.size(), activeBytes_, name_);
    }
}

void* HeapAllocator::allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0 || !isPowerOfTwo(alignment)) {
        return nullptr;
    }

    void* ptr = nullptr;
    const int rc = ::posix_memalign(&ptr, normalizeAlignment(alignment), size);
    if (rc != 0 || ptr == nullptr) {
        return nullptr; // EINVAL/ENOMEM — sem exceções, sem UB
    }

    live_.emplace(ptr, AllocationInfo{size});
    activeBytes_ += size;
    ++totalAllocations_;
    if (live_.size() > peakAllocations_) {
        peakAllocations_ = live_.size();
    }
    return ptr;
}

void HeapAllocator::deallocate(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    const auto it = live_.find(ptr);
    if (it == live_.end()) {
        return; // ponteiro estranho: ignorar silenciosamente (sem UB)
    }
    activeBytes_ -= it->second.size;
    live_.erase(it);
    ::free(ptr);
}

void* HeapAllocator::reallocate(void* ptr, std::size_t newSize,
                                std::size_t alignment) noexcept {
    if (ptr == nullptr) {
        return allocate(newSize, alignment);
    }
    if (newSize == 0) {
        deallocate(ptr);
        return nullptr;
    }

    const auto it = live_.find(ptr);
    if (it == live_.end()) {
        return nullptr; // ponteiro não gerenciado por este alocador
    }

    void* grown = allocate(newSize, alignment);
    if (grown == nullptr) {
        return nullptr;
    }
    const std::size_t bytesToCopy = it->second.size < newSize ? it->second.size : newSize;
    std::memcpy(grown, ptr, bytesToCopy);
    deallocate(ptr);
    return grown;
}

bool HeapAllocator::owns(const void* ptr) const noexcept {
    return ptr != nullptr && live_.find(const_cast<void*>(ptr)) != live_.end();
}

const char* HeapAllocator::name() const noexcept {
    return name_;
}

HeapAllocator::Stats HeapAllocator::stats() const noexcept {
    return Stats{
        live_.size(),
        activeBytes_,
        totalAllocations_,
        peakAllocations_,
    };
}

bool HeapAllocator::hasLeaks() const noexcept {
    return !live_.empty();
}

} // namespace eng::mem
