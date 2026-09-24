#pragma once

/// eng::fs::File — handle de arquivo RAII.
///
/// - Envolve std::FILE* (C API): sem exceções, sem flags de stream C++.
/// - Modos: Read (rb) ou Write (wb, trunca). Append não entra na FASE 3.
/// - Move-only (o handle é único). Abertura via FileSystem (Native).
/// - Erros de I/O pontuais retornam Result com core::Error; o destrutor
///   apenas fecha (fechar nunca falha de forma acionável aqui).
#include <cstdio>
#include <cstdint>
#include <span>

#include "eng/core/Result.hpp"
#include "eng/fs/Path.hpp"

namespace eng::fs {

using eng::core::Result;

enum class FileOpenMode : std::uint8_t {
    Read,  ///< "rb" — arquivo precisa existir.
    Write, ///< "wb" — cria/trunca.
};

class File final {
public:
    File() = default;
    ~File();

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    /// Abre um arquivo nativo. Falhas: NotFound, InvalidArgument (path),
    /// IOError (permissão etc.).
    [[nodiscard]] static Result<File> open(const Path& path, FileOpenMode mode);

    [[nodiscard]] bool isOpen() const noexcept { return handle_ != nullptr; }

    /// Lê até out.size() bytes na posição corrente. Retorno: bytes lidos
    /// (0 = fim de arquivo). Erro de I/O → IOError.
    [[nodiscard]] Result<std::size_t> read(std::span<std::byte> out);

    /// Escreve in inteiro (falha se não couber tudo) → bytes escritos.
    [[nodiscard]] Result<std::size_t> write(std::span<const std::byte> in);

    /// Tamanho em bytes do arquivo ABERTO (via fflush+fseek/ftell).
    [[nodiscard]] Result<std::uint64_t> size();

    /// Fecha explicitamente (idempotente; o destrutor também fecha).
    void close() noexcept;

private:
    explicit File(std::FILE* handle) noexcept;

    std::FILE* handle_ = nullptr;
};

} // namespace eng::fs
