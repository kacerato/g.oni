#pragma once

/// eng::fs::NativeFileSystem — I/O real via std::filesystem + std::FILE*
/// (FASE 3; ADR-027). Sobrecargas std::error_code em toda parte: o runtime
/// compila sem exceções — std::filesystem em modo lançante está
/// proibido neste módulo.
#include "eng/fs/FileSystem.hpp"

namespace eng::fs {

class NativeFileSystem final : public FileSystem {
public:
    NativeFileSystem() = default;

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
};

} // namespace eng::fs
