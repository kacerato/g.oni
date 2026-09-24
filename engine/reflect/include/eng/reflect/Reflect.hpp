#pragma once

/// eng::reflect — metadados de tipos para editor, serialização, scripting e
/// inspector.
///
/// A reflexão nativa de C++ ainda não existe; este módulo fornece
/// registro EXPLÍCITO via macros (ENG_REFLECT*). O desenho completo —
/// incluindo o que a reflexão deliberadamente NÃO faz — está em
/// docs/adr/ADR-021-reflection-strategy.md.
///
/// O que este módulo NÃO faz (por decisão, ver ADR-021):
///   - introspecção de membros privados (offsetof exige layout público);
///   - herança polimórfica automática (sem RTTI — ADR-005);
///   - invocação de métodos por metadado (chamadas seguras virão na fase de
///     scripting, via ponteiros-de-função registrados explicitamente);
///   - reflexão de std::vector<T> (adiado — §B.1 marca como opcional).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace eng::reflect {

/// Identificador opaco e DETERMINÍSTICO de tipo: FNV-1a 64 do nome canônico.
/// Determinístico entre execuções e unidades de tradução (não depende de ordem
/// de registro). Probabilidade de colisão por par: 2^-64 — ver ADR-021.
using TypeId = std::uint64_t;

/// FNV-1a 64. Avaliável em tempo de compilação e em runtime.
[[nodiscard]] constexpr TypeId typeIdOf(std::string_view canonicalName) noexcept
{
    TypeId hash = 14695981039346656037ull;
    for (const char c : canonicalName) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Categoria do tipo registrado.
enum class TypeKind : std::uint8_t {
    Primitive, ///< tipos embutidos (i32, f32, bool, string...)
    Struct,    ///< tipos com propriedades (offset + tipo por campo)
    Enum,      ///< enumerações com valores listados
};

/// Descritor de propriedade usado no registro (visões não donas — o registry
/// copia os dados).
struct PropertyDesc {
    std::string_view name;
    std::size_t offset = 0;
    std::string_view typeName; ///< nome canônico do tipo do campo
    /// Dica de UI/edição (evolução P0-6, ADR-052): livre, por convenção
    /// "texture" | "color:<grupo>:<canal>". Vazia = sem dica.
    std::string_view hint;
};

/// Descritor de enumerador (par nome → valor).
struct EnumeratorDesc {
    std::string_view name;
    std::int64_t value = 0;
};

/// Metadados de uma propriedade (cópia pertencente ao registry).
struct PropertyInfo {
    std::string name;
    std::size_t offset = 0;
    std::string typeName;
    TypeId typeId = 0; ///< id resolvido se o tipo do campo já estava registrado (senão 0; consultável por typeName)
    std::string hint;   ///< dica de edição (ADR-052; "" quando ausente)
};

/// Metadados de um enumerador (cópia pertencente ao registry).
struct EnumeratorInfo {
    std::string name;
    std::int64_t value = 0;
};

/// Metadados completos de um tipo registrado (cópia pertencente ao registry).
struct TypeInfo {
    std::string name;
    TypeId id = 0;
    TypeKind kind = TypeKind::Primitive;
    std::size_t size = 0;
    std::size_t alignment = 0;
    std::vector<PropertyInfo> properties;
    std::vector<EnumeratorInfo> enumerators;
    TypeId underlyingTypeId = 0; ///< enums: tipo integral subjacente
};

/// Registro de tipos do processo.
///
/// Propriedades de concorrência: escrita (registro) sob lock
/// exclusivo; leitura (find/count) sob lock compartilhado — leituras
/// concorrentes são seguras e testadas. Registro esperado em startup
/// (inicialização estática das macros ENG_REFLECT*), leitura a qualquer
/// momento.
///
/// Lookup de tipo não registrado devolve nullptr — nunca aborta.
class TypeRegistry {
public:
    /// Instância global do processo (justificativa: ADR-021 — singleton
    /// justificado por registro declarativo em inicialização estática).
    /// Construção thread-safe (magic static); registra os tipos embutidos
    /// (bool, i8..u64, f32, f64, u64, string) na primeira inicialização.
    [[nodiscard]] static TypeRegistry& global() noexcept;

    /// Registra um tipo. Cópia PROFUNDA de todos os descritores — o registro
    /// sobrevive ao escopo de quem registrou. Idempotente por nome: re-registro
    /// devolve o id existente SEM alterar metadados (primeiro registro vence —
    /// segura para macros em headers incluídos em múltiplas TUs).
    /// Colisão de FNV com nome distinto: o segundo registro é descartado e o
    /// id existente devolvido (degradação segura, ver ADR-021).
    TypeId registerType(TypeKind kind,
                        std::string_view name,
                        std::size_t size,
                        std::size_t alignment,
                        std::span<const PropertyDesc> properties = {},
                        std::span<const EnumeratorDesc> enumerators = {},
                        std::string_view underlyingTypeName = {});

    /// Consulta por nome canônico. Ausente → nullptr.
    [[nodiscard]] const TypeInfo* find(std::string_view name) const;

    /// Consulta por id opaco. Ausente → nullptr.
    [[nodiscard]] const TypeInfo* find(TypeId id) const;

    /// Quantidade de tipos registrados.
    [[nodiscard]] std::size_t count() const;

    /// Remove TUDO (inclusive embutidos). Invalida ponteiros TypeInfo*.
    /// Uso: testes. global() NÃO re-registra os embutidos após clear().
    void clear();

private:
    TypeRegistry() = default;

    mutable std::shared_mutex mutex_;
    std::deque<TypeInfo> types_;                            ///< deque: endereços estáveis em push_back
    std::unordered_map<std::string_view, std::size_t> byName_; ///< chaves apontam para strings estáveis em types_
    std::unordered_map<TypeId, std::size_t> byId_;
};

/// Traço extensível: nome canônico de um tipo C++ para ENG_REFLECT_FIELD
/// (modo automático). Tipos não mapeados → erro de compilação (deliberado);
/// use ENG_REFLECT_FIELD_AS ou especialize PrimitiveName em código de
/// integração.
template<typename T>
struct PrimitiveName; ///< sem definição padrão

#define ENG_REFLECT_DETAIL_PRIMITIVE(Type, Name)                             \
    template<> struct PrimitiveName<Type> {                                  \
        static constexpr std::string_view value = Name;                      \
    }

ENG_REFLECT_DETAIL_PRIMITIVE(bool, "bool");
ENG_REFLECT_DETAIL_PRIMITIVE(char, "i8");
ENG_REFLECT_DETAIL_PRIMITIVE(signed char, "i8");
ENG_REFLECT_DETAIL_PRIMITIVE(unsigned char, "u8");
ENG_REFLECT_DETAIL_PRIMITIVE(short, "i16");
ENG_REFLECT_DETAIL_PRIMITIVE(unsigned short, "u16");
ENG_REFLECT_DETAIL_PRIMITIVE(int, "i32");
ENG_REFLECT_DETAIL_PRIMITIVE(unsigned int, "u32");
ENG_REFLECT_DETAIL_PRIMITIVE(long, "i64");
ENG_REFLECT_DETAIL_PRIMITIVE(unsigned long, "u64");
ENG_REFLECT_DETAIL_PRIMITIVE(long long, "i64");
ENG_REFLECT_DETAIL_PRIMITIVE(unsigned long long, "u64");
ENG_REFLECT_DETAIL_PRIMITIVE(float, "f32");
ENG_REFLECT_DETAIL_PRIMITIVE(double, "f64");
ENG_REFLECT_DETAIL_PRIMITIVE(std::string, "string");

/// Nome canônico de T (modo automático dos macros de campo).
template<typename T>
[[nodiscard]] constexpr std::string_view typeNameOf() noexcept
{
    return PrimitiveName<T>::value;
}

namespace detail {

/// Detecta se T possui especialização de PrimitiveName (nome canônico).
template<typename T, typename = void>
struct HasCanonicalName : std::false_type {};

template<typename T>
struct HasCanonicalName<T, std::void_t<decltype(PrimitiveName<T>::value)>>
    : std::true_type {};

/// Nome de registro do tipo: especialização de PrimitiveName quando existir
/// (nome canônico), senão o nome literal do macro (#Type). Garante que o
/// nome registrado de um tipo e o typeName reportado nos campos COINCIDEM —
/// requisito para resolução de typeId entre registros.
template<typename T>
[[nodiscard]] constexpr std::string_view canonicalNameOf(std::string_view macroName) noexcept
{
    if constexpr (HasCanonicalName<T>::value) {
        return PrimitiveName<T>::value;
    } else {
        return macroName;
    }
}

/// Concatenação dupla: expande __LINE__ antes de colar (a colagem direta
/// `x##__LINE__` não expande o operando — GCC trata `__LINE__` como token
/// literal, colidindo identificadores entre invocações).
#define ENG_REFLECT_DETAIL_CONCAT_IMPL(a, b) a##b
#define ENG_REFLECT_DETAIL_CONCAT(a, b) ENG_REFLECT_DETAIL_CONCAT_IMPL(a, b)
#define ENG_REFLECT_DETAIL_UNIQUE(prefix) ENG_REFLECT_DETAIL_CONCAT(prefix, __LINE__)

/// Acumulador RAII usado pelos macros ENG_REFLECT_* — submete o tipo ao
/// registry global no destrutor. Não usar diretamente.
class Registrar {
public:
    Registrar(TypeKind kind, std::string_view name, std::size_t size,
              std::size_t alignment, std::string_view underlyingTypeName = {})
        : kind_(kind), name_(name), size_(size), alignment_(alignment),
          underlying_(underlyingTypeName) {}

    Registrar(const Registrar&) = delete;
    Registrar& operator=(const Registrar&) = delete;

    Registrar& property(std::string_view name, std::size_t offset,
                        std::string_view typeName)
    {
        properties_.push_back(PropertyDesc{name, offset, typeName, {}});
        return *this;
    }

    Registrar& property(std::string_view name, std::size_t offset,
                        std::string_view typeName, std::string_view hint)
    {
        properties_.push_back(PropertyDesc{name, offset, typeName, hint});
        return *this;
    }

    Registrar& enumerator(std::string_view name, std::int64_t value)
    {
        enumerators_.push_back(EnumeratorDesc{name, value});
        return *this;
    }

    ~Registrar() { submit(); }

private:
    void submit();

    TypeKind kind_;
    std::string_view name_;
    std::size_t size_;
    std::size_t alignment_;
    std::string_view underlying_;
    std::vector<PropertyDesc> properties_;   ///< string_views: literais dos macros (storage estático)
    std::vector<EnumeratorDesc> enumerators_;
};

/// Registro simples usado por ENG_REFLECT (sem propriedades). Sempre true —
/// permite inicialização estática bool.
[[nodiscard]] inline bool registerSimple(TypeKind kind, std::string_view name,
                                         std::size_t size, std::size_t alignment)
{
    (void)TypeRegistry::global().registerType(kind, name, size, alignment);
    return true;
}

} // namespace detail

// =============================================================================
// Macros de registro — usar em escopo de NAMESPACE, FORA da classe.
// Cada bloco BEGIN/END expande para UMA declaração
// `static const bool <id> = [] { ... }();` — o lambda garante que BEGIN e
// END compartilhem o mesmo identificador sem depender de colagem de tokens
// entre linhas. <id> deriva de __LINE__ (dupla expansão): um registro por
// linha por TU. static ⇒ linkage interno, re-registro entre TUs é
// idempotente (primeiro vence — ADR-021).
//
//   ENG_REFLECT(OpacoType)                          // tipo sem propriedades
//
//   ENG_REFLECT_BEGIN(Estrutura)
//       ENG_REFLECT_FIELD(campo)                    // tipo via PrimitiveName
//       ENG_REFLECT_FIELD_AS(campo, "eng::math::Vec3")
//   ENG_REFLECT_END()
//
//   ENG_REFLECT_ENUM_BEGIN(Cor)
//       ENG_REFLECT_ENUM_VALUE(Red)
//   ENG_REFLECT_ENUM_END()
//
// Requisitos: layout padrão (offsetof) e campos públicos.
// =============================================================================

/// Registra um tipo sem propriedades (opaco/primitivo). Nome de registro:
/// canônico quando PrimitiveName<T> existe, senão #Type.
#define ENG_REFLECT(Type)                                                    \
    [[maybe_unused]] static const bool                                       \
        ENG_REFLECT_DETAIL_UNIQUE(eng_reflect_registered_) =                 \
        ::eng::reflect::detail::registerSimple(                              \
            ::eng::reflect::TypeKind::Primitive,                             \
            ::eng::reflect::detail::canonicalNameOf<Type>(#Type),            \
            sizeof(Type), alignof(Type));

#define ENG_REFLECT_BEGIN(Type)                                              \
    [[maybe_unused]] static const bool                                       \
        ENG_REFLECT_DETAIL_UNIQUE(eng_reflect_registered_) = [] {            \
        using eng_refl_t_ = Type;                                            \
        ::eng::reflect::detail::Registrar eng_refl_reg_(                    \
            ::eng::reflect::TypeKind::Struct,                                \
            ::eng::reflect::detail::canonicalNameOf<eng_refl_t_>(#Type),     \
            sizeof(eng_refl_t_), alignof(eng_refl_t_));

/// Registra uma propriedade. Instrução completa: dispensa `;` do usuário
/// (semicolons extras do usuário são statements vazios — também válidos).
#define ENG_REFLECT_FIELD(member)                                            \
        eng_refl_reg_.property(#member, offsetof(eng_refl_t_, member),       \
            ::eng::reflect::typeNameOf<decltype(eng_refl_t_::member)>());

#define ENG_REFLECT_FIELD_AS(member, TypeName)                               \
        eng_refl_reg_.property(#member, offsetof(eng_refl_t_, member), TypeName);

/// Campo com DICA de edição (evolução P0-6, ADR-052): "texture",
/// "color:<grupo>:<canal(r|g|b|a)>"... O hint trafega no PropertyInfo e é
/// consumido por Inspector/UI — nunca altera serialização nem layout.
#define ENG_REFLECT_FIELD_HINT(member, Hint)                                 \
        eng_refl_reg_.property(#member, offsetof(eng_refl_t_, member),       \
            ::eng::reflect::typeNameOf<decltype(eng_refl_t_::member)>(),     \
            Hint);

#define ENG_REFLECT_FIELD_AS_HINT(member, TypeName, Hint)                    \
        eng_refl_reg_.property(#member, offsetof(eng_refl_t_, member),      \
            TypeName, Hint);

#define ENG_REFLECT_ENUM_BEGIN(Type)                                         \
    [[maybe_unused]] static const bool                                       \
        ENG_REFLECT_DETAIL_UNIQUE(eng_reflect_registered_) = [] {            \
        using eng_refl_t_ = Type;                                            \
        ::eng::reflect::detail::Registrar eng_refl_reg_(                    \
            ::eng::reflect::TypeKind::Enum,                                  \
            ::eng::reflect::detail::canonicalNameOf<eng_refl_t_>(#Type),     \
            sizeof(eng_refl_t_), alignof(eng_refl_t_),                       \
            ::eng::reflect::typeNameOf<std::underlying_type_t<eng_refl_t_>>());

#define ENG_REFLECT_ENUM_VALUE(name)                                         \
        eng_refl_reg_.enumerator(#name,                                      \
            static_cast<std::int64_t>(eng_refl_t_::name));

#define ENG_REFLECT_END()                                                    \
        (void)eng_refl_reg_;                                                 \
        return true;                                                         \
    }();

#define ENG_REFLECT_ENUM_END ENG_REFLECT_END

} // namespace eng::reflect
