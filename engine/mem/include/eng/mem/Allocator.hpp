#pragma once

#include <cstddef>

namespace eng::mem {

/// Interface de alocadores. Contrato:
/// - Todas as operações são `noexcept`; falha de alocação devolve nullptr
///   (o chamador decide como reportar via eng::core::Result).
/// - `alignment` deve ser potência de dois; comportamento com alignment
///   inválido é devolver nullptr (nunca UB).
/// - `deallocate(nullptr)` é permitido e não faz nada.
/// - Alocadores não são copiáveis; o ciclo de vida é gerenciado pelo dono.
class Allocator {
public:
    virtual ~Allocator();

    Allocator() = default;
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    /// Reserva `size` bytes alinhados; nullptr quando esgotado/inválido.
    [[nodiscard]] virtual void* allocate(std::size_t size, std::size_t alignment) noexcept = 0;

    /// Libera uma alocação anterior (nullptr é aceito).
    virtual void deallocate(void* ptr) noexcept = 0;

    /// Redimensiona preservando min(tamanhoAntigo, newSize) bytes.
    /// ptr nulo equivale a allocate; newSize zero equivale a deallocate.
    [[nodiscard]] virtual void* reallocate(void* ptr, std::size_t newSize,
                                           std::size_t alignment) noexcept = 0;

    /// Diz se `ptr` foi devolvido por este alocador e segue vivo.
    [[nodiscard]] virtual bool owns(const void* ptr) const noexcept = 0;

    /// Nome estável para diagnóstico (nunca nulo).
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace eng::mem
