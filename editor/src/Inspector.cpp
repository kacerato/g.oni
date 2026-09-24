#include "eng/editor/Inspector.hpp"

/// Inspector — campos por offset+typeName via reflect (FASE 8, §8.4).
///
/// Zero conhecimento de componentes específicos: Transform/Name são lidos
/// como QUALQUER struct refletida. Valores trafegam como string (boundary
/// neutra — JNI recebe texto; §4 da auditoria).

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "eng/log/Macros.hpp"
#include "eng/reflect/Reflect.hpp"
#include "eng/scene/Name.hpp"
#include "eng/scene/SceneSerializer.hpp"

namespace eng::editor {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;
using ComponentEntry = eng::scene::detail::ComponentEntry;

ENG_LOG_CATEGORY("editor");

[[nodiscard]] Error inspectorError(StatusCode code, std::string message)
{
    return Error{code, "Inspector: " + std::move(message)};
}

/// Ordem FIXA de categorias do painel Add/Inspector (P4.7.0 Bloco 1).
/// Categorias desconhecidas/vazias caem no fim ("Outros").
[[nodiscard]] std::size_t categoryOrder(std::string_view category)
{
    static constexpr std::string_view kOrder[] = {
        "Transform", "Render", "Física", "Lógica",
        "Áudio", "Câmera", "FX",
    };
    for (std::size_t i = 0; i < std::size(kOrder); ++i) {
        if (kOrder[i] == category) {
            return i;
        }
    }
    return std::size(kOrder);
}

const ComponentEntry* entryOf(std::string_view component)
{
    const auto& entries = eng::scene::detail::componentEntries();
    const auto it = entries.find(std::string(component));
    return it == entries.end() ? nullptr : &it->second;
}

// =============================================================================
// Formatação/parse por typeName (primitivas + enums; structs recursam)
// =============================================================================

[[nodiscard]] std::string formatFloat(float v) noexcept
{
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : "0";
}

[[nodiscard]] std::string formatDouble(double v) noexcept
{
    char buf[48];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : "0";
}

[[nodiscard]] std::string formatIntegral(const void* member,
                                         const eng::reflect::TypeInfo& type)
{
    if (type.name == "i8" || type.name == "i16" || type.name == "i32" ||
        type.name == "i64") {
        std::int64_t v = 0;
        if (type.name == "i8") {
            v = *static_cast<const std::int8_t*>(member);
        } else if (type.name == "i16") {
            v = *static_cast<const std::int16_t*>(member);
        } else if (type.name == "i32") {
            v = *static_cast<const std::int32_t*>(member);
        } else {
            v = *static_cast<const std::int64_t*>(member);
        }
        return std::to_string(v);
    }
    std::uint64_t v = 0;
    if (type.name == "u8") {
        v = *static_cast<const std::uint8_t*>(member);
    } else if (type.name == "u16") {
        v = *static_cast<const std::uint16_t*>(member);
    } else if (type.name == "u32") {
        v = *static_cast<const std::uint32_t*>(member);
    } else {
        v = *static_cast<const std::uint64_t*>(member);
    }
    return std::to_string(v);
}

/// Escreve um inteiro de 64 bits no tamanho exato do tipo (endianness
/// correta por acesso tipado — nada de memcpy de bytes baixos).
void writeIntegral(void* member, std::size_t size, std::int64_t v) noexcept
{
    switch (size) {
    case 1: *static_cast<std::int8_t*>(member) = static_cast<std::int8_t>(v); break;
    case 2: *static_cast<std::int16_t*>(member) = static_cast<std::int16_t>(v); break;
    case 4: *static_cast<std::int32_t*>(member) = static_cast<std::int32_t>(v); break;
    default: *static_cast<std::int64_t*>(member) = v; break;
    }
}

/// Lê o campo primitivo em `member` segundo `type` → string.
[[nodiscard]] Result<std::string> formatValue(
    const void* member, const eng::reflect::TypeInfo& type)
{
    if (type.kind == eng::reflect::TypeKind::Enum) {
        // Enum por NOME (estável — ADR-033).
        std::int64_t raw = 0;
        if (type.underlyingTypeId != 0) {
            const auto* underlying =
                eng::reflect::TypeRegistry::global().find(type.underlyingTypeId);
            if (underlying == nullptr) {
                return makeUnexpected(inspectorError(
                    StatusCode::NotFound,
                    "enum '" + type.name + "' sem tipo subjacente registrado"));
            }
            const std::string rawText = formatIntegral(member, *underlying);
            raw = std::strtoll(rawText.c_str(), nullptr, 10);
        }
        for (const auto& enumerator : type.enumerators) {
            if (enumerator.value == raw) {
                return enumerator.name;
            }
        }
        return makeUnexpected(inspectorError(
            StatusCode::NotFound, "enum '" + type.name + "' valor " +
                                      std::to_string(raw) +
                                      " sem enumerador"));
    }
    if (type.kind == eng::reflect::TypeKind::Struct) {
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "tipo '" + type.name + "' é struct — use o caminho do subcampo"));
    }
    if (type.name == "bool") {
        return std::string(*static_cast<const bool*>(member) ? "true" : "false");
    }
    if (type.name == "f32") {
        return formatFloat(*static_cast<const float*>(member));
    }
    if (type.name == "f64") {
        return formatDouble(*static_cast<const double*>(member));
    }
    if (type.name == "string") {
        return *static_cast<const std::string*>(member);
    }
    return formatIntegral(member, type);
}

/// Escreve `value` em `member` segundo `type`. Erro preciso; sem escrita
/// parcial (valida ANTES).
[[nodiscard]] Result<void> parseValue(void* member,
                                     const eng::reflect::TypeInfo& type,
                                     std::string_view value)
{
    if (type.kind == eng::reflect::TypeKind::Enum) {
        for (const auto& enumerator : type.enumerators) {
            if (enumerator.name == value) {
                writeIntegral(member, type.size, enumerator.value);
                return {};
            }
        }
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "valor '" + std::string(value) + "' não é enumerador de '" +
                type.name + "'"));
    }
    if (type.kind == eng::reflect::TypeKind::Struct) {
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "tipo '" + type.name + "' é struct — use o caminho do subcampo"));
    }
    if (type.name == "bool") {
        if (value == "true") {
            *static_cast<bool*>(member) = true;
            return {};
        }
        if (value == "false") {
            *static_cast<bool*>(member) = false;
            return {};
        }
        return makeUnexpected(inspectorError(StatusCode::InvalidArgument,
                                             "bool espera true/false"));
    }
    if (type.name == "f32" || type.name == "f64") {
        std::string text(value);
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
            !std::isfinite(parsed)) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "'" + text + "' não é número finito válido"));
        }
        if (type.name == "f32") {
            *static_cast<float*>(member) = static_cast<float>(parsed);
        } else {
            *static_cast<double*>(member) = parsed;
        }
        return {};
    }
    if (type.name == "string") {
        *static_cast<std::string*>(member) = std::string(value);
        return {};
    }
    // Inteiros — validação completa.
    std::string text(value);
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
        return makeUnexpected(inspectorError(StatusCode::InvalidArgument,
                                             "'" + text + "' não é inteiro"));
    }
    const long long min =
        (type.name == "i8")   ? std::numeric_limits<std::int8_t>::min()
        : (type.name == "i16") ? std::numeric_limits<std::int16_t>::min()
        : (type.name == "i32") ? std::numeric_limits<std::int32_t>::min()
                              : std::numeric_limits<std::int64_t>::min();
    const long long max =
        (type.name == "i8")   ? std::numeric_limits<std::int8_t>::max()
        : (type.name == "i16") ? std::numeric_limits<std::int16_t>::max()
        : (type.name == "i32") ? std::numeric_limits<std::int32_t>::max()
                              : std::numeric_limits<std::int64_t>::max();
    if (parsed < min || parsed > max) {
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "'" + text + "' fora da faixa de '" + type.name + "'"));
    }
    writeIntegral(member, type.size, parsed);
    return {};
}

/// Resolve o ENDEREÇO de um campo folha por caminho "a.b.c" dentro do
/// componente `base` (struct registrada). Template sobre a constância do
/// ponteiro (leitura via get, escrita via getMutable).
struct ResolvedField {
    void* member = nullptr;
    const eng::reflect::TypeInfo* type = nullptr;
};

template <typename Ptr>
[[nodiscard]] eng::core::Result<ResolvedField> resolveFieldImpl(
    Ptr base, const eng::reflect::TypeInfo& type, std::string_view path)
{
    using CharPtr =
        std::conditional_t<std::is_const_v<std::remove_pointer_t<Ptr>>,
                           const char*, char*>;
    CharPtr cursor = static_cast<CharPtr>(base);
    const eng::reflect::TypeInfo* current = &type;

    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t dot = path.find('.', begin);
        const std::string_view part =
            path.substr(begin, dot == std::string_view::npos
                                   ? std::string_view::npos
                                   : dot - begin);
        if (part.empty()) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "caminho '" + std::string(path) + "' tem componente vazio"));
        }
        const eng::reflect::PropertyInfo* found = nullptr;
        for (const auto& property : current->properties) {
            if (property.name == part) {
                found = &property;
                break;
            }
        }
        if (found == nullptr) {
            return makeUnexpected(inspectorError(
                StatusCode::NotFound,
                "campo '" + std::string(part) + "' não existe em '" +
                    current->name + "'"));
        }
        const eng::reflect::TypeInfo* next =
            eng::reflect::TypeRegistry::global().find(found->typeName);
        if (next == nullptr) {
            return makeUnexpected(inspectorError(
                StatusCode::NotFound, "tipo '" + found->typeName +
                                          "' não registrado no reflect"));
        }
        cursor = static_cast<CharPtr>(cursor) + found->offset;
        current = next;
        if (dot == std::string_view::npos) {
            break;
        }
        begin = dot + 1;
    }
    return ResolvedField{const_cast<void*>(static_cast<const void*>(cursor)),
                         current};
}

// =============================================================================
// Kind semântico + grupos de cor (evolução P0-6, ADR-052)
// =============================================================================

/// Kind de UI para um campo folha — deriva do TIPO + HINT (nunca do nome).
[[nodiscard]] std::string kindOf(const eng::reflect::PropertyInfo& property,
                                const eng::reflect::TypeInfo& type)
{
    if (type.kind == eng::reflect::TypeKind::Enum) {
        return "enum";
    }
    if (type.name == "bool") {
        return "bool";
    }
    if (type.name == "f32" || type.name == "f64") {
        return "number";
    }
    if (type.name == "i8" || type.name == "i16" || type.name == "i32" ||
        type.name == "i64" || type.name == "u8" || type.name == "u16" ||
        type.name == "u32" || type.name == "u64") {
        return "int";
    }
    if (type.name == "string") {
        // Hint de string = referência a asset do projeto (texture, audio,
        // material, script) ou forma de edição (code): a UI escolhe o
        // seletor pelo kind.
        if (!property.hint.empty() && !property.hint.starts_with("color:")) {
            return property.hint;
        }
        return "text";
    }
    return "text";
}

/// Hint "color:<grupo>:<canal>" → {grupo, canal r/g/b/a} (nullo se não é cor).
struct ColorHint {
    std::string_view group;
    char channel = 0; // 'r' | 'g' | 'b' | 'a'
};

[[nodiscard]] std::optional<ColorHint> parseColorHint(
    std::string_view hint)
{
    constexpr std::string_view kPrefix = "color:";
    if (hint.size() <= kPrefix.size() ||
        hint.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    const std::string_view rest = hint.substr(kPrefix.size());
    const std::size_t colon = rest.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        colon + 1 >= rest.size()) {
        return std::nullopt;
    }
    const std::string_view group = rest.substr(0, colon);
    const std::string_view channel = rest.substr(colon + 1);
    if (channel.size() != 1) {
        return std::nullopt;
    }
    const char c = channel[0];
    if (c != 'r' && c != 'g' && c != 'b' && c != 'a') {
        return std::nullopt;
    }
    return ColorHint{group, c};
}

/// Um dígito hex → valor 0..15 (nullo quando inválido).
[[nodiscard]] std::optional<int> hexDigit(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return std::nullopt;
}

/// Par hex "2F" → 0..255.
[[nodiscard]] std::optional<int> hexByte(char hi, char lo) noexcept
{
    const auto h = hexDigit(hi);
    const auto l = hexDigit(lo);
    if (!h.has_value() || !l.has_value()) {
        return std::nullopt;
    }
    return *h * 16 + *l;
}

/// Float [0..1] → byte 0..255 (clamp + round-half-away-from-zero).
[[nodiscard]] int floatToChannel(float v) noexcept
{
    if (!(v > 0.f)) {
        return 0; // NaN e negativos → 0
    }
    if (v > 1.f) {
        return 255;
    }
    return static_cast<int>(v * 255.f + 0.5f);
}

/// Canais → "#RRGGBB" (3) ou "#RRGGBBAA" (4).
[[nodiscard]] std::string formatColorHex(const int channels[4], int count)
{
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", channels[0],
                  channels[1], channels[2]);
    std::string out(buf);
    if (count == 4) {
        char abuf[3];
        std::snprintf(abuf, sizeof(abuf), "%02X", channels[3]);
        out += abuf;
    }
    return out;
}

/// "#RRGGBB[AA]" → 4 floats [0..1] (alpha 1 quando ausente). Erro preciso.
[[nodiscard]] Result<std::array<float, 4>> parseColorHex(
    std::string_view value)
{
    const std::string text(value);
    if (text.size() != 7 && text.size() != 9) {
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "cor espera '#RRGGBB' ou '#RRGGBBAA' (recebido '" + text +
                "')"));
    }
    if (text[0] != '#') {
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "cor deve começar com '#' (recebido '" + text + "')"));
    }
    const int pairs = text.size() == 9 ? 4 : 3;
    std::array<float, 4> out{0.f, 0.f, 0.f, 1.f};
    for (int i = 0; i < pairs; ++i) {
        const auto byte = hexByte(text[1 + i * 2], text[2 + i * 2]);
        if (!byte.has_value()) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "cor tem dígito hex inválido em '" + text + "'"));
        }
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(*byte) / 255.f;
    }
    return out;
}

/// Divide um path de grupo de cor em partes não-vazias.
[[nodiscard]] std::vector<std::string_view> splitCommaPath(
    std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t comma = path.find(',', begin);
        const std::string_view part =
            path.substr(begin, comma == std::string_view::npos
                                    ? std::string_view::npos
                                    : comma - begin);
        parts.push_back(part);
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return parts;
}

}  // namespace

// =============================================================================
// API pública
// =============================================================================

std::vector<std::string> Inspector::catalog()
{
    std::vector<std::string> names;
    for (const auto& [name, entry] : eng::scene::detail::componentEntries()) {
        (void)entry;
        names.push_back(name);
    }
    return names;
}

std::vector<Inspector::CatalogEntry> Inspector::catalogEntries()
{
    std::vector<CatalogEntry> entries;
    for (const auto& [name, entry] : eng::scene::detail::componentEntries()) {
        entries.push_back(CatalogEntry{name, entry.contract.category,
                                       entry.contract.scriptAlias});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const CatalogEntry& a, const CatalogEntry& b) {
                         const std::size_t oa = categoryOrder(a.category);
                         const std::size_t ob = categoryOrder(b.category);
                         if (oa != ob) {
                             return oa < ob;
                         }
                         return a.name < b.name;
                     });
    return entries;
}

const eng::scene::detail::ComponentContract* Inspector::contractOf(
    std::string_view component)
{
    const ComponentEntry* entry = entryOf(component);
    return entry == nullptr ? nullptr : &entry->contract;
}

namespace {

/// P4.7.0 Bloco 1: onValidate APÓS escrita (Inspector::setField).
/// Contrato violado → ROLLBACK pelo valor anterior (capturado ANTES da
/// escrita — round-trip string neutro por tipo) e erro preciso. O
/// rollback usa Inspector::setField com `allowRollback=false` (sem
/// recursão). Sem onValidate registrado → no-op.
[[nodiscard]] Result<void> validateAfterWrite(
    eng::scene::Scene& scene, eng::ecs::Entity entity,
    const ComponentEntry& entry, std::string_view component,
    std::string_view fieldPath, const std::optional<std::string>& previous,
    bool allowRollback)
{
    if (entry.onValidate == nullptr) {
        return {};
    }
    auto validated = entry.onValidate(scene, entity, entry);
    if (!validated.isError()) {
        return {};
    }
    if (allowRollback && previous.has_value()) {
        // Restaura o estado anterior (era válido por construção — falha
        // de validação no restore é ignorada: o erro ORIGINAL é o que
        // importa para o autor).
        (void)Inspector::setField(scene, entity, component, fieldPath,
                                  *previous);
    }
    return makeUnexpected(inspectorError(
        StatusCode::InvalidArgument,
        "valor rejeitado pelo contrato do componente: "
        + validated.error().message));
}

} // namespace

std::vector<std::string> Inspector::componentsOf(const eng::scene::Scene& scene,
                                                 eng::ecs::Entity entity)
{
    std::vector<std::string> present;
    const auto& world = scene.world();
    if (!scene.isNode(entity)) {
        return present;
    }
    for (const auto& [name, entry] : eng::scene::detail::componentEntries()) {
        if (entry.has(world, entity)) {
            present.push_back(name);
        }
    }
    return present;
}

Result<std::vector<Inspector::Field>> Inspector::fieldsOf(
    const eng::scene::Scene& scene, eng::ecs::Entity entity,
    std::string_view component)
{
    const ComponentEntry* entry = entryOf(component);
    if (entry == nullptr) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "componente '" + std::string(component) + "' fora do catálogo"));
    }
    if (!scene.isNode(entity) || !entry->has(scene.world(), entity)) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "entidade não possui o componente '" + std::string(component) +
                "'"));
    }
    const void* base = entry->get(scene.world(), entity);
    if (base == nullptr || entry->info == nullptr) {
        return makeUnexpected(inspectorError(StatusCode::Internal,
                                             "componente sumiu entre has/get"));
    }

    std::vector<Field> fields;
    const auto flatten = [&](auto&& self, std::string_view prefix,
                             const void* obj,
                             const eng::reflect::TypeInfo& type) -> void {
        // Grupo de cor em formação (evolução P0-6): canais CONSECUTIVOS com
        // hint color:<grupo>:<canal> colapsam em UM campo sintético.
        struct ColorPart {
            const eng::reflect::PropertyInfo* prop;
            const eng::reflect::TypeInfo* type;
            char channel;
            std::string path;
        };
        std::vector<ColorPart> colorRun;
        std::string colorGroup;

        const auto flushColorRun = [&]() {
            if (colorRun.empty()) {
                return;
            }
            // Validação: grupo completo precisa de r+g+b (+a opcional),
            // cada canal exatamente uma vez, todos f32. Grupo incompleto
            // degrada para campos individuais (honesto, sem quebrar UI).
            int rCount = 0, gCount = 0, bCount = 0, aCount = 0;
            bool usable = true;
            for (const auto& part : colorRun) {
                if (part.type == nullptr || part.type->name != "f32") {
                    usable = false;
                    break;
                }
                switch (part.channel) {
                case 'r': ++rCount; break;
                case 'g': ++gCount; break;
                case 'b': ++bCount; break;
                case 'a': ++aCount; break;
                default: usable = false; break;
                }
            }
            const bool complete = rCount == 1 && gCount == 1 && bCount == 1 &&
                                  (aCount == 0 || aCount == 1);
            if (!usable || !complete) {
                ENG_WARN("Inspector: grupo de cor '{}' incompleto em '{}' — "
                         "canais viram campos numéricos individuais",
                         colorGroup, type.name);
                for (const auto& part : colorRun) {
                    auto value = formatValue(
                        static_cast<const char*>(obj) + part.prop->offset,
                        *part.type);
                    if (!value.isError()) {
                        fields.push_back(Field{part.path, part.type->name,
                                               std::move(value.value()),
                                               "number", ""});
                    }
                }
                colorRun.clear();
                return;
            }
            // Campo sintético: path = canais por vírgula NA ORDEM r,g,b,a.
            ColorPart ordered[4] = {};
            for (const auto& part : colorRun) {
                const int slot = part.channel == 'r'   ? 0
                                 : part.channel == 'g' ? 1
                                 : part.channel == 'b' ? 2
                                                       : 3;
                ordered[slot] = part;
            }
            const int count = aCount == 1 ? 4 : 3;
            std::string path;
            int channels[4] = {0, 0, 0, 255};
            for (int i = 0; i < count; ++i) {
                if (i > 0) {
                    path += ',';
                }
                path += ordered[i].path;
                channels[i] = floatToChannel(
                    *static_cast<const float*>(static_cast<const void*>(
                        static_cast<const char*>(obj) +
                        ordered[i].prop->offset)));
            }
            fields.push_back(Field{std::move(path), "color",
                                   formatColorHex(channels, count), "color",
                                   ""});
            colorRun.clear();
        };

        const auto emitProperty = [&](const eng::reflect::PropertyInfo& property,
                                     const std::string& path,
                                     const void* member,
                                     const eng::reflect::TypeInfo& fieldType) {
            auto value = formatValue(member, fieldType);
            if (value.isError()) {
                ENG_WARN("Inspector: campo '{}' ilegível ({})", path,
                         value.error().message);
                return;
            }
            std::string options;
            if (fieldType.kind == eng::reflect::TypeKind::Enum) {
                for (std::size_t i = 0; i < fieldType.enumerators.size();
                     ++i) {
                    if (i > 0) {
                        options += '|';
                    }
                    options += fieldType.enumerators[i].name;
                }
            }
            fields.push_back(Field{path, property.typeName,
                                   std::move(value.value()),
                                   kindOf(property, fieldType),
                                   std::move(options)});
        };

        for (const auto& property : type.properties) {
            const eng::reflect::TypeInfo* fieldType =
                eng::reflect::TypeRegistry::global().find(property.typeName);
            if (fieldType == nullptr) {
                ENG_WARN("Inspector: tipo '{}' do campo '{}' não registrado",
                         property.typeName, property.name);
                continue;
            }
            const void* member = static_cast<const char*>(obj) + property.offset;
            const std::string path =
                prefix.empty()
                    ? property.name
                    : std::string(prefix) + "." + property.name;

            // Canal de cor? Agrupa com o run corrente (mesmo grupo) ou abre
            // novo run (grupo distinto / primeiro canal).
            if (const auto hint = parseColorHint(property.hint)) {
                if (!colorRun.empty() && hint->group != colorGroup) {
                    flushColorRun();
                }
                if (colorRun.empty()) {
                    colorGroup = std::string(hint->group);
                }
                colorRun.push_back(
                    ColorPart{&property, fieldType, hint->channel, path});
                continue;
            }
            flushColorRun(); // hint comum encerra o run de cor anterior

            if (fieldType->kind == eng::reflect::TypeKind::Struct) {
                // Struct conhecida → recursão (Vec3/Quat/...).
                self(self, path, member, *fieldType);
                continue;
            }
            emitProperty(property, path, member, *fieldType);
        }
        flushColorRun(); // run no fim da struct
    };
    flatten(flatten, "", base, *entry->info);
    return fields;
}

Result<std::string> Inspector::getField(const eng::scene::Scene& scene,
                                         eng::ecs::Entity entity,
                                         std::string_view component,
                                         std::string_view fieldPath)
{
    const ComponentEntry* entry = entryOf(component);
    if (entry == nullptr) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "componente '" + std::string(component) + "' fora do catálogo"));
    }
    if (!scene.isNode(entity) || !entry->has(scene.world(), entity)) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "entidade não possui o componente '" + std::string(component) +
                "'"));
    }
    // Leitura por caminho — ponteiro CONST (get do catálogo; sem mutação).
    const void* base = entry->get(scene.world(), entity);
    if (base == nullptr || entry->info == nullptr) {
        return makeUnexpected(inspectorError(StatusCode::Internal,
                                             "componente sumiu entre has/get"));
    }
    // Grupo de cor (P0-6): path comma-junto lido como hex único.
    if (fieldPath.find(',') != std::string_view::npos) {
        const auto parts = splitCommaPath(fieldPath);
        if (parts.size() != 3 && parts.size() != 4) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "grupo de cor precisa de 3 ou 4 canais (recebido " +
                    std::to_string(parts.size()) + ")"));
        }
        int channels[4] = {0, 0, 0, 255};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].empty()) {
                return makeUnexpected(inspectorError(
                    StatusCode::InvalidArgument,
                    "caminho '" + std::string(fieldPath) +
                        "' tem canal vazio"));
            }
            auto resolved = resolveFieldImpl(base, *entry->info, parts[i]);
            if (resolved.isError()) {
                return makeUnexpected(resolved.error());
            }
            if (resolved.value().type->name != "f32") {
                return makeUnexpected(inspectorError(
                    StatusCode::InvalidArgument,
                    "canal '" + std::string(parts[i]) +
                        "' não é f32 — grupos de cor exigem floats"));
            }
            channels[i] = floatToChannel(
                *static_cast<const float*>(resolved.value().member));
        }
        return formatColorHex(channels, static_cast<int>(parts.size()));
    }
    auto resolved = resolveFieldImpl(base, *entry->info, fieldPath);
    if (resolved.isError()) {
        return makeUnexpected(resolved.error());
    }
    return formatValue(resolved.value().member, *resolved.value().type);
}

Result<void> Inspector::setField(eng::scene::Scene& scene,
                                  eng::ecs::Entity entity,
                                  std::string_view component,
                                  std::string_view fieldPath,
                                  std::string_view value)
{
    const ComponentEntry* entry = entryOf(component);
    if (entry == nullptr) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "componente '" + std::string(component) + "' fora do catálogo"));
    }
    if (!scene.isNode(entity) || !entry->has(scene.world(), entity)) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "entidade não possui o componente '" + std::string(component) +
                "'"));
    }
    void* base = entry->getMutable(scene.world(), entity);
    if (base == nullptr || entry->info == nullptr) {
        return makeUnexpected(inspectorError(StatusCode::Internal,
                                             "componente sumiu entre has/get"));
    }
    // Valor ANTES da escrita — rollback do onValidate (P4.7.0 B1).
    // Falha de leitura (campo novo sem representação) = sem rollback.
    std::optional<std::string> previousValue;
    if (entry->onValidate != nullptr) {
        auto previous = getField(scene, entity, component, fieldPath);
        if (!previous.isError()) {
            previousValue = std::move(previous.value());
        }
    }
    // Grupo de cor (P0-6): valida TUDO antes de escrever qualquer canal.
    if (fieldPath.find(',') != std::string_view::npos) {
        const auto parts = splitCommaPath(fieldPath);
        if (parts.size() != 3 && parts.size() != 4) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "grupo de cor precisa de 3 ou 4 canais (recebido " +
                    std::to_string(parts.size()) + ")"));
        }
        auto parsed = parseColorHex(value);
        if (parsed.isError()) {
            return makeUnexpected(parsed.error());
        }
        // Resolução completa ANTES da primeira escrita (sem escrita parcial).
        struct ResolvedChannel {
            float* member;
        };
        std::array<ResolvedChannel, 4> targets{};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].empty()) {
                return makeUnexpected(inspectorError(
                    StatusCode::InvalidArgument,
                    "caminho '" + std::string(fieldPath) +
                        "' tem canal vazio"));
            }
            auto resolved = resolveFieldImpl(base, *entry->info, parts[i]);
            if (resolved.isError()) {
                return makeUnexpected(resolved.error());
            }
            if (resolved.value().type->name != "f32") {
                return makeUnexpected(inspectorError(
                    StatusCode::InvalidArgument,
                    "canal '" + std::string(parts[i]) +
                        "' não é f32 — grupos de cor exigem floats"));
            }
            targets[i] = ResolvedChannel{
                static_cast<float*>(resolved.value().member)};
        }
        for (std::size_t i = 0; i < parts.size(); ++i) {
            *targets[i].member = parsed.value()[i];
        }
        return validateAfterWrite(scene, entity, *entry, component,
                                  fieldPath, previousValue,
                                  /*allowRollback=*/true);
    }
    auto resolved = resolveFieldImpl(base, *entry->info, fieldPath);
    if (resolved.isError()) {
        return makeUnexpected(resolved.error());
    }
    auto written = parseValue(resolved.value().member,
                              *resolved.value().type, value);
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    return validateAfterWrite(scene, entity, *entry, component, fieldPath,
                              previousValue, /*allowRollback=*/true);
}

bool Inspector::isRemovable(std::string_view component)
{
    // Transform é a geometria do nó (Scene emplanta); Name é o rótulo
    // mínimo do editor. Hierarchy/SceneIdentity/WorldMatrix são internos
    // (nem aparecem no catálogo — isInternalComponentName no serializer).
    return component != "eng::math::Transform" && component != "eng::scene::Name";
}

Result<void> Inspector::addComponent(eng::scene::Scene& scene,
                                      eng::ecs::Entity entity,
                                      std::string_view component,
                                      void* attachUser)
{
    const ComponentEntry* entry = entryOf(component);
    if (entry == nullptr) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "componente '" + std::string(component) + "' fora do catálogo"));
    }
    if (!scene.isNode(entity)) {
        return makeUnexpected(inspectorError(StatusCode::InvalidArgument,
                                              "entidade obsoleta"));
    }
    if (entry->has(scene.world(), entity)) {
        return makeUnexpected(inspectorError(
            StatusCode::AlreadyExists,
            "entidade já possui '" + std::string(component) + "'"));
    }

    // --- P4.7.0 Bloco 1: ComponentContract -----------------------------
    const eng::scene::detail::ComponentContract& contract = entry->contract;
    for (const std::string& needed : contract.required) {
        const ComponentEntry* req = entryOf(needed);
        if (req == nullptr) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "contrato de '" + std::string(component) +
                "' exige tipo desconhecido '" + needed + "'"));
        }
        if (!req->has(scene.world(), entity)) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "'" + std::string(component) + "' exige '" + needed +
                "' — adicione '" + needed + "' antes"));
        }
    }
    for (const std::string& conflicting : contract.conflicts) {
        const ComponentEntry* conf = entryOf(conflicting);
        if (conf != nullptr && conf->has(scene.world(), entity)) {
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "'" + std::string(component) + "' conflita com '" +
                conflicting + "' na mesma entidade — remova '" +
                conflicting + "' primeiro"));
        }
    }
    if (contract.single && entry->count != nullptr
        && entry->count(scene.world()) != 0) {
        return makeUnexpected(inspectorError(
            StatusCode::AlreadyExists,
            "'" + std::string(component) +
            "' é único na cena (single) — só uma entidade pode tê-lo"));
    }

    auto added = entry->emplaceDefault(*entry, scene.world(), entity);
    if (added.isError()) {
        return added;
    }

    // Hook de anexo (registro NATIVO de efeitos colaterais — luz casa
    // com camada, física informa runtime, etc. — P4.7.0 Bloco 1).
    if (entry->onAttach != nullptr) {
        entry->onAttach(scene, entity, *entry, attachUser);
    }
    // Validação pós-anexo (estado default deve ser consistente).
    if (entry->onValidate != nullptr) {
        auto validated = entry->onValidate(scene, entity, *entry);
        if (validated.isError()) {
            // Contrato default violado: rollback do anexo (o componente
            // default-construído nunca deve nascer inválido).
            (void)entry->removeFrom(scene.world(), entity);
            if (entry->onDetach != nullptr) {
                entry->onDetach(scene, entity, *entry, attachUser);
            }
            return makeUnexpected(inspectorError(
                StatusCode::InvalidArgument,
                "'" + std::string(component) + "' default inválido: "
                + validated.error().message));
        }
    }
    return {};
}

Result<void> Inspector::removeComponent(eng::scene::Scene& scene,
                                         eng::ecs::Entity entity,
                                         std::string_view component,
                                         void* detachUser)
{
    const ComponentEntry* entry = entryOf(component);
    if (entry == nullptr) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "componente '" + std::string(component) + "' fora do catálogo"));
    }
    if (!isRemovable(component)) {
        return makeUnexpected(inspectorError(
            StatusCode::InvalidArgument,
            "componente '" + std::string(component) + "' é protegido"));
    }
    if (!scene.isNode(entity) || !entry->has(scene.world(), entity)) {
        return makeUnexpected(inspectorError(
            StatusCode::NotFound,
            "entidade não possui '" + std::string(component) + "'"));
    }

    // --- P4.7.0 Bloco 1: dependência reversa ----------------------------
    // Outro componente PRESENTE na entidade exige o removido? Recusa com
    // o nome do dependente (autor decide a ordem — nada some de surpresa).
    for (const auto& [name, other] : eng::scene::detail::componentEntries()) {
        if (name == component || !other.has(scene.world(), entity)) {
            continue;
        }
        for (const std::string& needed : other.contract.required) {
            if (needed == component) {
                return makeUnexpected(inspectorError(
                    StatusCode::InvalidArgument,
                    "não é possível remover '" + std::string(component) +
                    "': '" + name + "' exige este componente — remova '" +
                    name + "' primeiro"));
            }
        }
    }

    (void)entry->removeFrom(scene.world(), entity);
    if (entry->onDetach != nullptr) {
        entry->onDetach(scene, entity, *entry, detachUser);
    }
    return {};
}

}  // namespace eng::editor
