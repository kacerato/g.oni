#pragma once

/// eng::fs::FileSystem — abstração de I/O de arquivos (FASE 3; ADR-027).
///
/// Herança simples com métodos virtuais (a missão rejeitou "IFileSystem
/// virtual puro" para substituição em runtime — duas implementações bastam).
///
/// Contratos:
///   - Toda operação rejeita path inválido (vazio/NUL) com InvalidArgument.
///   - readAll* sobre ausente → NotFound; writeAll* exige pai existente
///     (NENHUMA implementação cria diretórios implicitamente — paridade
///     exata entre Native e Memory; use mkdirs antes).
///   - remove segue std::filesystem::remove: apaga arquivo OU diretório
///     VAZIO; devolve false se nada foi removido (não é erro).
///   - rename move arquivo ou árvore inteira.
///   - list devolve entradas em ordem determinística (crescente por path).
///
/// Thread-safety: thread-compatible — instâncias independentes em
/// threads distintas são seguras; uma MESMA instância não é safe para
/// mutação concorrente (sem locks internos).
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/fs/Path.hpp"

namespace eng::fs {

using eng::core::Result;

/// Entrada de listagem (arquivo ou diretório).
struct ListEntry {
    Path path;
    bool isDirectory = false;

    [[nodiscard]] bool operator==(const ListEntry&) const = default;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;

    /// Path existe (arquivo ou diretório)?
    [[nodiscard]] virtual Result<bool> exists(const Path& path) const = 0;

    /// Bytes completos do arquivo.
    [[nodiscard]] virtual Result<std::vector<std::byte>> readAllBytes(
        const Path& path) const = 0;

    /// Texto completo do arquivo (bytes como-estão; sem transcodificação).
    [[nodiscard]] virtual Result<std::string> readAllText(
        const Path& path) const = 0;

    /// Substitui o conteúdo do arquivo (writeAll* não cria pais).
    [[nodiscard]] virtual Result<void> writeAllBytes(
        const Path& path, std::span<const std::byte> bytes) = 0;

    [[nodiscard]] virtual Result<void> writeAllText(
        const Path& path, std::string_view text) = 0;

    /// Apaga arquivo ou diretório VAZIO. false: nada removido (ausente ou
    /// diretório não-vazio — NÃO é erro; message distingue quando útil).
    [[nodiscard]] virtual Result<bool> remove(const Path& path) = 0;

    /// Move/renomeia (sobrescreve destino se existir, como fs::rename).
    [[nodiscard]] virtual Result<void> rename(const Path& from,
                                              const Path& to) = 0;

    /// Cria a árvore de diretórios (Ok se já existir como diretório).
    [[nodiscard]] virtual Result<void> mkdirs(const Path& path) = 0;

    /// Lista os componentes DIRETOS (recursive=false) ou toda a subárvore
    /// (true) de um diretório. Listar arquivo/ausente → NotFound.
    [[nodiscard]] virtual Result<std::vector<ListEntry>> list(
        const Path& dir, bool recursive) const = 0;
};

} // namespace eng::fs
