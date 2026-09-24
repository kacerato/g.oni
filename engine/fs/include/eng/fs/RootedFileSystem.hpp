#pragma once

/// eng::fs::RootedFileSystem — mapeamento workspace: a fronteira entre a
/// localização FÍSICA (absoluta) e os paths RELATIVOS do editor
/// (RECOVERY P0; ADR-027).
///
/// O PROBLEMA QUE ESTA CLASSE RESOLVE (reproduzido em dispositivo Android):
///
///   EditorActivity (Kotlin) passa filesDir/projects — ABSOLUTO
///        ↓ JNI (nativeEditorCreate)
///   EditorDocument recebia o root ABSOLUTO → ProjectPaths computava
///   assetsRoot() ABSOLUTO → validações anti-absoluto do editor
///   (AssetBrowser::import / EditorDocument::scriptWrite) rejeitavam:
///        "AssetBrowser: destino absoluto é proibido"
///        "EditorDocument: caminho absoluto proibido"
///
/// A correção NÃO é enfraquecer a validação (o editor continua exigindo
/// paths relativos — regra dura da missão §2.6/§8.1), e sim completar a
/// arquitetura com a camada de CONVERSÃO que faltava:
///
///   Android/SAF URI absoluto          (aquisição — papel da plataforma, §D7)
///        ↓ staging copiado p/ DENTRO do workspace (Kotlin)
///   RootedFileSystem                 (ESTA CLASSE — workspace mapping)
///        ↓ (root_ / path).normalized()
///   FileSystem base (Native/Memory)  (I/O real — absoluto ou CWD)
///
/// Contratos:
///   - Entrada é SEMPRE relativa ao root: path ABSOLUTO → InvalidArgument
///     (violação de fronteira — é bug do chamador, não se corrige aqui);
///     ".." que escaparia do root → InvalidArgument (anti-traversal).
///   - O join é NORMALIZADO (paridade com MemoryFileSystem::keyOf):
///     "./a" e "a" mapeiam para o MESMO lugar.
///   - list() devolve paths RE-RELATIVIZADOS ao root (forma normalizada):
///     o chamador nunca vê — nem persiste — o root absoluto.
///   - writeAll* NÃO cria pais (paridade exata com Native/Memory — as
///     implementações não divergem no que criam implicitamente: nada).
///   - Thread-safety: thread-compatible, como o resto de eng::fs.
#include "eng/fs/FileSystem.hpp"

namespace eng::fs {

class RootedFileSystem final : public FileSystem {
public:
    /// `base` é EMPRESTADA (o dono é o host — mesma regra do documento do
    /// editor). `root` é o local físico do workspace: absoluto no Android
    /// (filesDir/projects), relativo ou "." no Linux/testes. O root é
    /// armazenado na forma normalizada.
    RootedFileSystem(FileSystem& base, const Path& root);

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

    /// Root físico (normalizado) — diagnóstico/testes. NÃO use para I/O:
    /// a razão de existir desta classe é o root NUNCA vazar para o editor.
    [[nodiscard]] const Path& root() const noexcept { return root_; }

private:
    /// Valida (relativo, sem escape) e mapeia: (root_ / path).normalized().
    [[nodiscard]] Result<Path> mapIn(const Path& path) const;

    FileSystem* base_;  ///< I/O real (emprestada)
    Path root_{};       ///< local físico do workspace (normalizado)
};

} // namespace eng::fs
