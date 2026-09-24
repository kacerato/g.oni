#pragma once

/// eng::fs::Path — wrapper fino sobre std::filesystem::path (FASE 3, missão
/// §2.11; ADR-027).
///
/// Decisões:
///   - ENVOLVE std::filesystem::path; não substitui, não reparsing próprio.
///   - Forma canônica de texto = GENÉRICA ('/' como separador, sem aspectos
///     de host) — é o que se persiste e se compara.
///   - Path NÃO se auto-normaliza; `normalized()` devolve a forma lexical
///     normalizada (a.b/c/.. → a). I/O usa o path como dado.
///   - `valid()` = não-vazio e sem NUL embutido; operações de FileSystem
///     rejeitam paths inválidos com InvalidArgument (checagem centralizada).
///   - `isWithin(root)` = anti-traversal: ambos lexically_normal, e este
///     path é root ou está DENTRO dele por componentes (prefixo por
///     componente, não por bytes — "/data-base" não está dentro de "/data").
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace eng::fs {

class Path {
public:
    Path() = default;

    /// Constrói da forma genérica ("/a/b"). Não normaliza.
    explicit Path(std::string_view generic);

    /// Tag para o ctor de path nativo (desambigua const char*/string das
    /// duas conversões possíveis — string_view e filesystem::path).
    struct FromNative {};

    /// Envolve um path nativo pronto.
    explicit Path(FromNative, std::filesystem::path native);

    /// Factory de path nativo (preferida fora da classe).
    [[nodiscard]] static Path fromNative(std::filesystem::path native);

    /// Factory explícita para o path vazio/raiz relativa ".".
    [[nodiscard]] static Path current() { return Path(std::string_view(".")); }

    // --- composição --------------------------------------------------------

    /// Concatena com separador ("/a" / "b" → "/a/b"). Não normaliza.
    [[nodiscard]] Path operator/(const Path& tail) const;
    Path& operator/=(const Path& tail);

    /// Apelido nomeado de operator/ (preferido em código não idiomático).
    [[nodiscard]] Path joined(const Path& tail) const { return *this / tail; }

    // --- decomposição -------------------------------------------------------

    /// Diretório pai ("/a/b.txt" → "/a"; "b" → "."; "/a" → "/").
    [[nodiscard]] Path parent() const;

    /// Último componente ("/a/b.txt" → "b.txt"; "/a/" → "").
    [[nodiscard]] Path filename() const;

    /// filename sem a extensão ("b.txt" → "b").
    [[nodiscard]] Path stem() const;

    /// Extensão COM ponto ("b.txt" → ".txt"; sem extensão → "").
    [[nodiscard]] Path extension() const;

    // --- forma/consulta -----------------------------------------------------

    /// Forma lexicalmente normalizada (colapsa ".", "..", "//" onde
    /// possível; ".." no topo é preservado — denota escape relativo).
    [[nodiscard]] Path normalized() const;

    /// Absoluto no léxico (começa com '/')? Não consulta o filesystem.
    [[nodiscard]] bool isAbsolute() const noexcept;

    /// Vazio ("")?
    [[nodiscard]] bool isEmpty() const noexcept;

    /// Não-vazio e sem NUL embutido (checado pelas operações de I/O).
    [[nodiscard]] bool valid() const noexcept;

    /// Este path é `root` ou está estritamente dentro dele? Ambos são
    /// normalizados antes da comparação por componentes.
    [[nodiscard]] bool isWithin(const Path& root) const;

    /// Forma genérica canônica como std::string (o que se persiste).
    [[nodiscard]] std::string str() const;

    /// O path nativo envolvido (para interop com std::filesystem).
    [[nodiscard]] const std::filesystem::path& native() const noexcept;

    // --- comparação ---------------------------------------------------------

    [[nodiscard]] friend bool operator==(const Path& a, const Path& b) noexcept
    {
        return a.native_ == b.native_;
    }
    [[nodiscard]] friend bool operator!=(const Path& a, const Path& b) noexcept
    {
        return !(a == b);
    }

private:
    std::filesystem::path native_{};
};

} // namespace eng::fs

/// Hash de Path — habilita uso em unordered containers.
template<>
struct std::hash<eng::fs::Path> {
    [[nodiscard]] std::size_t operator()(const eng::fs::Path& p) const noexcept
    {
        return std::hash<std::filesystem::path>{}(p.native());
    }
};
