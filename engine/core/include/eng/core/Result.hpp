#pragma once

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>

#include "eng/core/Error.hpp"

namespace eng::core {

/// Wrapper de erro para construção explícita de Result em estado de falha.
template <typename E>
struct Unexpected {
    E error;
};

template <typename E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> makeUnexpected(E&& error) {
    return Unexpected<std::decay_t<E>>{std::forward<E>(error)};
}

/// Result<T, E> — canal de erros do runtime. Sem exceções:
/// funções que podem falhar retornam Result em vez de lançar.
///
/// - `value()`/`error()` só podem ser chamados no estado correspondente
///   (verificado por assert em builds de debug; NDEBUG remove a checagem).
/// - Armazenamento por união bruta alinhada: sizeof(Result<T,E>) é
///   max(sizeof(T), sizeof(E)) + 1 byte de tag, sem exigir que T seja
///   default-construtível.
/// - E precisa ser move-construtível.
template <typename T, typename E = Error>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, void>, "Use a especialização Result<void, E>");
    static_assert(std::is_move_constructible_v<E>, "E precisa ser move-construtível");
    static_assert(std::is_move_constructible_v<T>, "T precisa ser move-construtível");

public:
    using ValueType = T;
    using ErrorType = E;

    Result(const T& value) : hasValue_(true) {
        ::new (storage()) T(value);
    }

    Result(T&& value) : hasValue_(true) {
        ::new (storage()) T(std::move(value));
    }

    template <typename EArg>
    Result(Unexpected<EArg> unexpected) : hasValue_(false) {
        ::new (storage()) E(std::move(unexpected.error));
    }

    Result(const Result& other) : hasValue_(other.hasValue_) {
        if (hasValue_) {
            ::new (storage()) T(other.value());
        } else {
            ::new (storage()) E(other.error());
        }
    }

    Result(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<T>&& std::is_nothrow_move_constructible_v<E>)
        : hasValue_(other.hasValue_) {
        if (hasValue_) {
            ::new (storage()) T(std::move(other).value());
        } else {
            ::new (storage()) E(std::move(other).error());
        }
    }

    Result& operator=(const Result& other) {
        if (this == &other) {
            return *this;
        }
        destroyActive();
        hasValue_ = other.hasValue_;
        if (hasValue_) {
            ::new (storage()) T(other.value());
        } else {
            ::new (storage()) E(other.error());
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept(
        std::is_nothrow_move_assignable_v<T>&& std::is_nothrow_move_assignable_v<E>) {
        if (this == &other) {
            return *this;
        }
        destroyActive();
        hasValue_ = other.hasValue_;
        if (hasValue_) {
            ::new (storage()) T(std::move(other).value());
        } else {
            ::new (storage()) E(std::move(other).error());
        }
        return *this;
    }

    ~Result() {
        destroyActive();
    }

    // --- estado -------------------------------------------------------------

    [[nodiscard]] bool ok() const noexcept { return hasValue_; }
    [[nodiscard]] bool isError() const noexcept { return !hasValue_; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue_; }

    // --- valor / erro ---------------------------------------------------------

    [[nodiscard]] const T& value() const& noexcept {
        assert(hasValue_ && "Result::value() em estado de erro");
        return *valuePtr();
    }

    [[nodiscard]] T& value() & noexcept {
        assert(hasValue_ && "Result::value() em estado de erro");
        return *valuePtr();
    }

    [[nodiscard]] T&& value() && noexcept {
        assert(hasValue_ && "Result::value() em estado de erro");
        return std::move(*valuePtr());
    }

    [[nodiscard]] const E& error() const& noexcept {
        assert(!hasValue_ && "Result::error() em estado de sucesso");
        return *errorPtr();
    }

    [[nodiscard]] E& error() & noexcept {
        assert(!hasValue_ && "Result::error() em estado de sucesso");
        return *errorPtr();
    }

    [[nodiscard]] E&& error() && noexcept {
        assert(!hasValue_ && "Result::error() em estado de sucesso");
        return std::move(*errorPtr());
    }

    /// Valor presente, ou `fallback` (por cópia) quando em erro.
    [[nodiscard]] T valueOr(T fallback) const {
        if (hasValue_) {
            return *valuePtr();
        }
        return fallback;
    }

private:
    void destroyActive() noexcept {
        if (hasValue_) {
            valuePtr()->~T();
        } else {
            errorPtr()->~E();
        }
    }

    [[nodiscard]] void* storage() noexcept { return storage_.bytes; }

    [[nodiscard]] T* valuePtr() noexcept {
        return std::launder(reinterpret_cast<T*>(storage_.bytes));
    }

    [[nodiscard]] const T* valuePtr() const noexcept {
        return std::launder(reinterpret_cast<const T*>(storage_.bytes));
    }

    [[nodiscard]] E* errorPtr() noexcept {
        return std::launder(reinterpret_cast<E*>(storage_.bytes));
    }

    [[nodiscard]] const E* errorPtr() const noexcept {
        return std::launder(reinterpret_cast<const E*>(storage_.bytes));
    }

    struct Storage {
        alignas(alignof(T)) alignas(alignof(E)) unsigned char
            bytes[sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E)];
    };

    Storage storage_{};
    bool hasValue_{false};
};

/// Especialização de status puro: Result<void, E> representa operações que
/// só podem falhar sem produzir valor. E precisa ser default-construtível.
template <typename E>
class [[nodiscard]] Result<void, E> {
    static_assert(std::is_default_constructible_v<E>,
                  "E precisa ser default-construtível em Result<void, E>");
    static_assert(std::is_move_assignable_v<E>,
                  "E precisa ser move-atribuível em Result<void, E>");

public:
    using ValueType = void;
    using ErrorType = E;

    Result() noexcept : hasValue_(true) {}

    template <typename EArg>
    Result(Unexpected<EArg> unexpected) : hasValue_(false) {
        error_ = E(std::move(unexpected.error));
    }

    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;
    ~Result() = default;

    [[nodiscard]] bool ok() const noexcept { return hasValue_; }
    [[nodiscard]] bool isError() const noexcept { return !hasValue_; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue_; }

    [[nodiscard]] const E& error() const& noexcept {
        assert(!hasValue_ && "Result::error() em estado de sucesso");
        return error_;
    }

    [[nodiscard]] E& error() & noexcept {
        assert(!hasValue_ && "Result::error() em estado de sucesso");
        return error_;
    }

    [[nodiscard]] E&& error() && noexcept {
        assert(!hasValue_ && "Result::error() em estado de sucesso");
        return std::move(error_);
    }

private:
    bool hasValue_{true};
    E error_{};
};

/// Guia de dedução: Result(makeUnexpected(e)) deduz Result<void, E>.
template <typename E>
Result(Unexpected<E>) -> Result<void, E>;

} // namespace eng::core
