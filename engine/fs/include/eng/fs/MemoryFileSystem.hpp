#pragma once

/// eng::fs::MemoryFileSystem — filesystem em memória para testes (FASE 3,
/// missão §2.5; ADR-027).
///
/// - Semântica PARITÁRIA com NativeFileSystem (mesmos contratos, incluindo
///   "writeAll não cria pais" e "remove só apaga vazio") — testes rodam
///   contra as duas sem mudar de comportamento.
/// - Armazenamento: std::map ordenado por path genérico normalizado —
///   listagem determinística (crescente) sem ordenar por rodada.
/// - Thread-safety: NÃO é thread-safe para acesso concorrente à
///   mesma instância (sem locks; uso tipicamente single-threaded em testes).
#include <map>
#include <set>
#include <string>
#include <vector>

#include "eng/fs/FileSystem.hpp"

namespace eng::fs {

class MemoryFileSystem final : public FileSystem {
public:
    MemoryFileSystem() = default;

    [[nodiscard]] Result<bool> exists(const Path& path) const override;
    [[nodiscard]] Result<std::vector<std::byte>> readAllBytes(
        const Path& path) const override;
    [[nodiscard]] Result<std::string> readAllText(
        const Path& path) const override;
    [[nodiscard]] Result<void> writeAllBytes(
        const Path& path, std::span<const std::byte> bytes) override;
    [[nodiscard]] Result<void> writeAllText(
        const Path& path, std::string_view text) override;
    [[nodiscard]] Result<bool> remove(const Path& path) override;
    [[nodiscard]] Result<void> rename(const Path& from,
                                      const Path& to) override;
    [[nodiscard]] Result<void> mkdirs(const Path& path) override;
    [[nodiscard]] Result<std::vector<ListEntry>> list(
        const Path& dir, bool recursive) const override;

    // --- extras de diagnóstico (testes) ------------------------------------

    /// Quantidade de arquivos armazenados.
    [[nodiscard]] std::size_t fileCount() const noexcept { return files_.size(); }

    /// Quantidade de diretórios explícitos.
    [[nodiscard]] std::size_t dirCount() const noexcept { return dirs_.size(); }

private:
    /// Chave canônica: path genérico normalizado.
    [[nodiscard]] static std::string keyOf(const Path& path);

    /// Pai existe como diretório?
    [[nodiscard]] bool parentDirExists(const std::string& key) const;

    std::map<std::string, std::vector<std::byte>> files_;
    std::set<std::string> dirs_;
};

} // namespace eng::fs
