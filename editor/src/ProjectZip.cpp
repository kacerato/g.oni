#include "eng/editor/ProjectZip.hpp"

/// ProjectZip — implementação (P4.2, bug B-A). Ver header para o
/// porquê (export/import do projeto testável no Linux; wrapper único;
/// anti-traversal; erro explícito em vez de projeto vazio silencioso).
///
/// Formato gravado/lido: zip STORE-ONLY (method 0). Estruturas:
///   - Local file header  "PK\x03\x04" (30 bytes + nome)
///   - Central directory  "PK\x01\x02" (46 bytes + nome)
///   - End of central dir "PK\x05\x06" (22 bytes)
/// CRC32 IEEE (polinômio 0xEDB88320) calculado localmente — tabela
/// constexpr; sem zlib (mobile: binário menor, zero licenças extras).

#include <array>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "eng/log/Macros.hpp"

namespace eng::editor {

ENG_LOG_CATEGORY("project-zip");

namespace {

using eng::core::StatusCode;

[[nodiscard]] eng::core::Error zipError(StatusCode code,
                                        std::string message)
{
    // Error agregado de 2 campos (ADR-004): {code, message}. O prefixo
    // "zip: " identifica a origem na mensagem exibida ao usuário.
    return eng::core::Error{code, "zip: " + std::move(message)};
}

// --- CRC32 (IEEE, refletido — o padrão do formato zip) ----------------------

struct Crc32Table {
    std::array<std::uint32_t, 256> v{};
    constexpr Crc32Table()
    {
        constexpr std::uint32_t kPoly = 0xEDB88320u;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) != 0u ? (kPoly ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

constexpr Crc32Table kCrc32Table{};

[[nodiscard]] std::uint32_t crc32Of(std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte b : bytes) {
        const std::uint32_t index =
            (crc ^ static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b))) &
            0xFFu;
        crc = kCrc32Table.v[index] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// --- escrita little-endian (formato zip é LE) --------------------------------

void putU16(std::vector<std::byte>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
}

void putU32(std::vector<std::byte>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}

[[nodiscard]] std::uint16_t readU16(const std::vector<std::byte>& in,
                                    std::size_t offset) noexcept
{
    if (offset + 2 > in.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(in[offset]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[offset + 1]))
         << 8));
}

[[nodiscard]] std::uint32_t readU32(const std::vector<std::byte>& in,
                                    std::size_t offset) noexcept
{
    if (offset + 4 > in.size()) {
        return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(in[offset + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

constexpr std::uint32_t kSigLocal = 0x04034B50u;
constexpr std::uint32_t kSigCentral = 0x02014B50u;
constexpr std::uint32_t kSigEocd = 0x06054B50u;

[[nodiscard]] bool hasSignature(const std::vector<std::byte>& in,
                                std::size_t offset, std::uint32_t signature)
{
    return offset + 4 <= in.size() && readU32(in, offset) == signature;
}

// --- validação de nome de entrada (anti-traversal) ----------------------------

/// Nome de entrada SEGURO: relativo, sem "..", sem '\\' (zip do Windows
/// usa '/' — barra invertida fora é suspeita), sem leading '/'.
[[nodiscard]] bool isSafeEntryName(std::string_view name) noexcept
{
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return false;
    }
    if (name.find('\\') != std::string_view::npos) {
        return false;
    }
    if (name.find('\0') != std::string_view::npos) {
        return false;
    }
    // Componentes: "" e "." tolerados? "." só em forma estranha — recusa.
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::string_view component =
            name.substr(start, slash == std::string_view::npos
                                   ? std::string_view::npos
                                   : slash - start);
        if (component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

[[nodiscard]] std::vector<std::string_view> splitComponents(
    std::string_view name)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::string_view component =
            name.substr(start, slash == std::string_view::npos
                                   ? std::string_view::npos
                                   : slash - start);
        if (!component.empty()) {
            parts.push_back(component);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

}  // namespace

// =============================================================================
// Escrita
// =============================================================================

Result<void> buildProjectZip(eng::fs::FileSystem& fs,
                             const eng::fs::Path& projectDir,
                             const eng::fs::Path& outZipPath,
                             std::string_view wrapperName)
{
    if (!projectDir.valid() || !outZipPath.valid()) {
        return makeUnexpected(zipError(StatusCode::InvalidArgument,
                                       "caminho inválido"));
    }
    auto listed = fs.list(projectDir, true);
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }

    // Prefixo de projeto (as entradas do list vêm COM o prefixo do dir —
    // "MeuJogo/scenes/main.json"); o rel é o que vai DENTRO do wrapper.
    // NORMALIZADO: projectDir pode chegar como "./TestGame" (workspace
    // raiz "."), mas as chaves do list são lexically normalizadas — sem
    // o normalize o strip falha e o wrapper é aplicado DUAS vezes
    // ("TestGame/TestGame/..." — pego pelo round-trip do teste B-A).
    const std::string dirKey = projectDir.normalized().str();
    const bool rootLike = dirKey == "." || dirKey == "/" || dirKey.empty();
    const std::string prefix = rootLike ? std::string{} : dirKey + "/";

    struct FileEntry {
        std::string name;   // "<wrapper>/rel" (nome final no zip)
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::size_t localOffset = 0;
    };

    std::vector<std::byte> zip;
    std::vector<FileEntry> entries;
    entries.reserve(listed.value().size());

    for (const auto& entry : listed.value()) {
        if (entry.isDirectory) {
            continue;  // dirs implícitos: zip recria pelos parents dos arquivos
        }
        std::string rel = entry.path.str();
        if (!prefix.empty() && rel.rfind(prefix, 0) == 0) {
            rel = rel.substr(prefix.size());
        }
        if (rel.empty()) {
            continue;
        }
        if (!isSafeEntryName(rel)) {
            return makeUnexpected(zipError(
                StatusCode::InvalidArgument,
                "caminho de projeto inseguro para o zip: " + rel));
        }
        std::string name;
        name.reserve(wrapperName.size() + 1 + rel.size());
        if (!wrapperName.empty()) {
            name.append(wrapperName).append(1, '/');
        }
        name.append(rel);

        auto bytes = fs.readAllBytes(entry.path);
        if (bytes.isError()) {
            return makeUnexpected(bytes.error());
        }
        FileEntry file;
        file.name = std::move(name);
        file.size = static_cast<std::uint32_t>(bytes.value().size());
        file.crc = crc32Of(bytes.value());
        file.localOffset = zip.size();

        // Local file header + dados (STORE).
        putU32(zip, kSigLocal);
        putU16(zip, 20);        // version needed
        putU16(zip, 0);         // flags
        putU16(zip, 0);         // method = STORE
        putU16(zip, 0);         // mod time
        putU16(zip, 0);         // mod date
        putU32(zip, file.crc);
        putU32(zip, file.size); // compressed
        putU32(zip, file.size); // uncompressed
        putU16(zip, static_cast<std::uint16_t>(file.name.size()));
        putU16(zip, 0);         // extra len
        for (const char c : file.name) {
            zip.push_back(static_cast<std::byte>(c));
        }
        zip.insert(zip.end(), bytes.value().begin(), bytes.value().end());
        entries.push_back(std::move(file));
    }

    const std::size_t centralOffset = zip.size();
    for (const auto& file : entries) {
        putU32(zip, kSigCentral);
        putU16(zip, 20);        // version made by
        putU16(zip, 20);        // version needed
        putU16(zip, 0);         // flags
        putU16(zip, 0);         // method
        putU16(zip, 0);         // time
        putU16(zip, 0);         // date
        putU32(zip, file.crc);
        putU32(zip, file.size);
        putU32(zip, file.size);
        putU16(zip, static_cast<std::uint16_t>(file.name.size()));
        putU16(zip, 0);         // extra
        putU16(zip, 0);         // comment
        putU16(zip, 0);         // disk start
        putU16(zip, 0);         // internal attrs
        putU32(zip, 0);         // external attrs
        putU32(zip, static_cast<std::uint32_t>(file.localOffset));
        for (const char c : file.name) {
            zip.push_back(static_cast<std::byte>(c));
        }
    }
    const std::size_t centralSize = zip.size() - centralOffset;

    // EOCD.
    putU32(zip, kSigEocd);
    putU16(zip, 0);
    putU16(zip, 0);
    putU16(zip, static_cast<std::uint16_t>(entries.size()));
    putU16(zip, static_cast<std::uint16_t>(entries.size()));
    putU32(zip, static_cast<std::uint32_t>(centralSize));
    putU32(zip, static_cast<std::uint32_t>(centralOffset));
    putU16(zip, 0);

    // Destino: pai precisa existir (contrato do FileSystem) — o zip de
    // export vive na RAIZ do workspace (caminho dado pelo chamador).
    auto written = fs.writeAllBytes(
        outZipPath, std::span{zip.data(), zip.size()});
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    ENG_INFO("project-zip: export '{}' ({} arquivos, {} bytes) → '{}'",
             wrapperName.empty() ? "-" : wrapperName, entries.size(),
             zip.size(), outZipPath.str());
    return {};
}

// =============================================================================
// Leitura
// =============================================================================

Result<std::string> extractProjectZip(eng::fs::FileSystem& fs,
                                      const eng::fs::Path& zipPath,
                                      const eng::fs::Path& workspaceRoot,
                                      std::string_view preferredName)
{
    if (!zipPath.valid() || !workspaceRoot.valid() || preferredName.empty()) {
        return makeUnexpected(zipError(StatusCode::InvalidArgument,
                                       "argumentos inválidos"));
    }
    auto zipBytes = fs.readAllBytes(zipPath);
    if (zipBytes.isError()) {
        return makeUnexpected(zipBytes.error());
    }
    const std::vector<std::byte>& in = zipBytes.value();

    // EOCD (varre do fim — comentário de até 64 KiB é legal no formato).
    std::size_t eocd = in.size();
    const std::size_t kMinEocd = 22;
    if (in.size() < kMinEocd) {
        return makeUnexpected(
            zipError(StatusCode::ParseError, "zip truncado (sem EOCD)"));
    }
    for (std::size_t back = 0;
         back <= in.size() - kMinEocd && back <= 65535u + kMinEocd; ++back) {
        const std::size_t at = in.size() - kMinEocd - back;
        if (hasSignature(in, at, kSigEocd)) {
            eocd = at;
            break;
        }
    }
    if (eocd >= in.size() || !hasSignature(in, eocd, kSigEocd)) {
        return makeUnexpected(zipError(StatusCode::ParseError,
                                       "não é um zip (EOCD ausente)"));
    }
    const std::uint16_t entryCount = readU16(in, eocd + 10);
    const std::uint32_t centralOffset = readU32(in, eocd + 16);

    struct RawEntry {
        std::string name;
        std::uint16_t method = 0;
        std::uint32_t size = 0;
        std::uint32_t crc = 0;
        std::uint32_t localOffset = 0;
        bool isDirectory = false;
    };
    std::vector<RawEntry> raw;
    raw.reserve(entryCount);
    std::size_t cursor = centralOffset;
    for (std::uint16_t i = 0; i < entryCount; ++i) {
        if (!hasSignature(in, cursor, kSigCentral)) {
            return makeUnexpected(zipError(StatusCode::ParseError,
                                           "central directory corrompido"));
        }
        RawEntry entry;
        entry.method = readU16(in, cursor + 10);
        entry.crc = readU32(in, cursor + 16);
        entry.size = readU32(in, cursor + 24);  // uncompressed == compressed (STORE)
        const std::uint16_t nameLen = readU16(in, cursor + 28);
        entry.localOffset = readU32(in, cursor + 42);
        if (cursor + 46 + nameLen > in.size()) {
            return makeUnexpected(zipError(StatusCode::ParseError,
                                           "nome de entrada truncado"));
        }
        entry.name.reserve(nameLen);
        for (std::size_t c = 0; c < nameLen; ++c) {
            entry.name.push_back(
                static_cast<char>(std::to_integer<std::uint8_t>(
                    in[cursor + 46 + c])));
        }
        entry.isDirectory = (!entry.name.empty() && entry.name.back() == '/');
        raw.push_back(std::move(entry));
        cursor += 46u + nameLen + readU16(in, cursor + 30) /*extra*/ +
                  readU16(in, cursor + 32) /*comment*/;
    }

    // Wrapper: o primeiro componente do caminho de project.goni.json —
    // robusto (não depende da ORDEM das entradas, só da ESTRUTURA).
    std::string wrapper;
    bool sawProjectFile = false;
    for (const auto& entry : raw) {
        if (entry.isDirectory) {
            continue;
        }
        const auto parts = splitComponents(entry.name);
        if (parts.size() >= 2 && parts.back() == "project.goni.json") {
            wrapper = std::string{parts.front()};
            sawProjectFile = true;
            break;
        }
        if (parts.size() == 1 && parts.front() == "project.goni.json") {
            sawProjectFile = true;  // zip SEM wrapper — preferredName decide
        }
    }
    if (!sawProjectFile) {
        return makeUnexpected(zipError(
            StatusCode::ParseError,
            "zip sem project.goni.json — não é um projeto G.ONI"));
    }

    // Nome final da pasta: wrapper vence; senão preferredName (sugestão
    // da UI — nome do arquivo do zip). Sanitizado: sem '/' e não-oculto.
    std::string folder = wrapper;
    if (folder.empty()) {
        folder = std::string{preferredName};
    }
    if (folder.empty() || folder.find('/') != std::string::npos ||
        folder.front() == '.') {
        return makeUnexpected(zipError(StatusCode::InvalidArgument,
                                       "nome de projeto inválido: '" + folder +
                                           "'"));
    }

    // Pasta ocupada (reimportar o mesmo jogo): vira "Nome 2", "Nome 3"…
    // em vez de sobrescrever ou falhar.
    eng::fs::Path target = workspaceRoot / eng::fs::Path{folder};
    for (int suffix = 2;; ++suffix) {
        auto exists = fs.exists(target);
        if (exists.isError()) {
            return makeUnexpected(exists.error());
        }
        if (!exists.value()) {
            break;
        }
        if (suffix > 999) {
            return makeUnexpected(zipError(
                StatusCode::AlreadyExists,
                "projeto '" + folder + "' já existe no workspace"));
        }
        target = workspaceRoot /
                 eng::fs::Path{folder + " " + std::to_string(suffix)};
    }
    folder = target.filename().str();
    auto made = fs.mkdirs(target);
    if (made.isError()) {
        return makeUnexpected(made.error());
    }

    // Fase de escrita: TODA entrada validada ANTES de qualquer byte —
    // falha de validação não deixa projeto pela metade (sem estado vazio
    // que "abre" e engana — regra B-A).
    struct PlannedFile {
        eng::fs::Path destination;
        std::string name;
        std::uint16_t method;
        std::uint32_t size;
        std::uint32_t crc;
        std::uint32_t localOffset;
    };
    std::vector<PlannedFile> planned;
    planned.reserve(raw.size());
    const std::string wrapperPrefix = wrapper.empty() ? std::string{} : wrapper + "/";
    for (const auto& entry : raw) {
        if (!isSafeEntryName(entry.name)) {
            return makeUnexpected(zipError(
                StatusCode::ParseError, "entrada insegura no zip: " + entry.name));
        }
        std::string rel = entry.name;
        if (!wrapper.empty()) {
            if (rel.rfind(wrapperPrefix, 0) != 0) {
                continue;  // entrada fora do wrapper (lixo de terceiros) — ignora
            }
            rel = rel.substr(wrapperPrefix.size());
        }
        if (rel.empty()) {
            continue;
        }
        if (entry.isDirectory) {
            continue;  // criada implicitamente pelo writeAllBytes dos pais
        }
        planned.push_back(PlannedFile{target / eng::fs::Path{rel}, rel,
                                      entry.method, entry.size, entry.crc,
                                      entry.localOffset});
    }

    std::size_t writtenFiles = 0;
    for (const auto& file : planned) {
        if (file.method != 0) {
            return makeUnexpected(zipError(
                StatusCode::NotSupported,
                "entrada comprimida (DEFLATE) não suportada: " + file.name +
                    " — re-exporte pelo editor (zip store)"));
        }
        // Local header: pular nome+extra DO PRÓPRIO header local (podem
        // diferir do central — flags de data descriptor).
        const std::size_t localAt = file.localOffset;
        if (!hasSignature(in, localAt, kSigLocal)) {
            return makeUnexpected(zipError(StatusCode::ParseError,
                                           "local header corrompido: " +
                                               file.name));
        }
        const std::uint16_t localNameLen = readU16(in, localAt + 26);
        const std::uint16_t localExtraLen = readU16(in, localAt + 28);
        const std::size_t dataAt = localAt + 30u + localNameLen + localExtraLen;
        if (dataAt + file.size > in.size()) {
            return makeUnexpected(zipError(StatusCode::ParseError,
                                           "dados truncados: " + file.name));
        }
        std::vector<std::byte> content(in.begin() + static_cast<std::ptrdiff_t>(dataAt),
                                       in.begin() + static_cast<std::ptrdiff_t>(dataAt + file.size));
        if (crc32Of(content) != file.crc) {
            return makeUnexpected(zipError(StatusCode::ParseError,
                                           "CRC inválido: " + file.name));
        }
        auto parentMade = fs.mkdirs(file.destination.parent());
        if (parentMade.isError()) {
            return makeUnexpected(parentMade.error());
        }
        auto w = fs.writeAllBytes(
            file.destination, std::span{content.data(), content.size()});
        if (w.isError()) {
            return makeUnexpected(w.error());
        }
        ++writtenFiles;
    }

    ENG_INFO("project-zip: import '{}' → pasta '{}' ({} arquivos)",
             wrapper.empty() ? "(sem wrapper)" : wrapper, folder, writtenFiles);
    return folder;
}

}  // namespace eng::editor
