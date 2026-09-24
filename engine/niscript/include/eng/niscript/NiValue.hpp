#pragma once

/// eng::ni — modelo de valores do NI-Script.
///
/// NI-Script é a linguagem de script PRÓPRIA do G.oni: compila
/// UMA vez para bytecode; o VM executa bytecode (sem JIT). Este header
/// define o modelo de VALORES — o que atravessa a fronteira VM ↔ C++.
///
/// Regras de valor (design phase11_audit/design.md §3):
///   - Não existe literal `nil` na linguagem: todo `var` tem zero-value ou
///     inicializador. O estado Nil existe apenas INTERNAMENTE (locais
///     não inicializadas, retorno implícito) e CONSUMÍ-Lo em operação
///     tipada é Fault reparável — nunca UB.
///   - `entity` é HANDLE geracional (índice+geração), NUNCA ponteiro: valor
///     obsoleto é no-op seguro ou Fault (ADR-024 no ECS subjacente).
///   - `asset` é o AssetId (u64) — opaco para o script.
///   - CompView é INTERNO (cola do binding): entidade + prefixo de caminho
///     acumulado por DYN_GET; resolução acontece no uso tipado. Não é um
///     tipo declarável do script.
///
/// Floats do script: f64 IEEE-754 (builds SEM fast-math — determinismo);
/// conversões para f32/i32 nas fronteiras dos bindings são checadas.

#include <cstdint>
#include <string>
#include <string_view>

#include "eng/ecs/Ecs.hpp"

namespace eng::ni {

/// Tipo ESTÁTICO (checagem em compile time) e de VALOR (runtime).
/// `Dynamic` só existe como tipo ESTÁTICO (expressões cujo tipo o compilador
/// não conhece: leituras de campo de entidade); valores runtime nunca são
/// Dynamic. `Nil` como tipo estático = "função sem give".
enum class NiType : std::uint8_t {
    Nil = 0,
    Int,
    Float,
    Bool,
    String,
    Vec2,
    Vec3,
    Color,
    Entity,
    Asset,
    Transform,
    CompView,
    Dynamic,
};

/// Nome canônico do tipo (para diagnósticos e ADR-049 docs).
[[nodiscard]] std::string_view niTypeName(NiType type) noexcept;

/// vec2/vec3/color/transform do SCRIPT (f64 — conversão na fronteira).
struct NiVec2 {
    double x = 0.0, y = 0.0;
    [[nodiscard]] friend bool operator==(const NiVec2&,
                                          const NiVec2&) = default;
};
struct NiVec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    [[nodiscard]] friend bool operator==(const NiVec3&,
                                          const NiVec3&) = default;
};
struct NiColor {
    double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
    [[nodiscard]] friend bool operator==(const NiColor&,
                                          const NiColor&) = default;
};
/// TRS de script — MESMA convenção do editor: rotação em GRAUS Euler XYZ
/// (a conversão para Quat vive na fronteira do binding).
struct NiTransform {
    NiVec3 position{};
    NiVec3 rotationDegrees{};
    NiVec3 scale{1.0, 1.0, 1.0};
    [[nodiscard]] friend bool operator==(const NiTransform&,
                                          const NiTransform&) = default;
};

/// Valor do VM. Layout por tipo (`d` é o storage numérico vetorial):
///   Int/Bool  → i;  Float → d[0];  String → s;
///   Vec2 → d[0..1];  Vec3 → d[0..2];  Color → d[0..3];
///   Transform → d[0..8] (pos, rotGraus, scale);
///   Entity → i = (geração<<32)|índice (kNoEntityValue = nula);
///   Asset → i (u64);
///   CompView → i = entidade empacotada, s = prefixo do caminho.
struct NiValue {
    NiType type = NiType::Nil;
    std::int64_t i = 0;
    double d[9] = {};
    std::string s;

    [[nodiscard]] friend bool operator==(const NiValue& a, const NiValue& b) {
        if (a.type != b.type) {
            return false;
        }
        switch (a.type) {
        case NiType::Nil: return true;
        case NiType::Int: case NiType::Bool: case NiType::Entity:
        case NiType::Asset:
            return a.i == b.i;
        case NiType::Float: return a.d[0] == b.d[0];
        case NiType::Vec2: return a.d[0] == b.d[0] && a.d[1] == b.d[1];
        case NiType::Vec3: return a.d[0] == b.d[0] && a.d[1] == b.d[1]
                                  && a.d[2] == b.d[2];
        case NiType::Color: return a.d[0] == b.d[0] && a.d[1] == b.d[1]
                                   && a.d[2] == b.d[2] && a.d[3] == b.d[3];
        case NiType::Transform:
            for (int k = 0; k < 9; ++k) {
                if (a.d[k] != b.d[k]) { return false; }
            }
            return true;
        case NiType::String: case NiType::CompView: return a.s == b.s;
        case NiType::Dynamic: break;
        }
        return false;
    }
    [[nodiscard]] friend bool operator!=(const NiValue& a, const NiValue& b) {
        return !(a == b);
    }
};

/// Entidade nula (handle "nenhuma") — mesmo valor-sentinela do editor.
inline constexpr std::uint64_t kNoEntityPacked =
    (static_cast<std::uint64_t>(0xFFFFFFFFu) << 32) | 0xFFFFFFFFu;

[[nodiscard]] inline NiValue niEntityValue(eng::ecs::Entity e) noexcept
{
    NiValue v;
    v.type = NiType::Entity;
    v.i = (static_cast<std::uint64_t>(e.generation) << 32) | e.index;
    return v;
}
[[nodiscard]] inline eng::ecs::Entity niEntityOf(const NiValue& v) noexcept
{
    return eng::ecs::Entity{
        static_cast<std::uint32_t>(v.i & 0xFFFFFFFFu),
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(v.i) >> 32)};
}
[[nodiscard]] inline bool niIsNullEntity(const NiValue& v) noexcept
{
    return v.type == NiType::Entity
           && static_cast<std::uint64_t>(v.i) == kNoEntityPacked;
}

/// Construtores de valor.
[[nodiscard]] NiValue niInt(std::int64_t v) noexcept;
[[nodiscard]] NiValue niFloat(double v) noexcept;
[[nodiscard]] NiValue niBool(bool v) noexcept;
[[nodiscard]] NiValue niString(std::string v) noexcept;
[[nodiscard]] NiValue niVec2(double x, double y) noexcept;
[[nodiscard]] NiValue niVec3(double x, double y, double z) noexcept;
[[nodiscard]] NiValue niColor(double r, double g, double b,
                              double a = 1.0) noexcept;
[[nodiscard]] NiValue niTransform(const NiTransform& t) noexcept;
[[nodiscard]] NiValue niAsset(std::uint64_t id) noexcept;
[[nodiscard]] NiValue niCompView(eng::ecs::Entity e, std::string prefix) noexcept;

/// Zero-value de um tipo declarado (`var x: T`).
[[nodiscard]] NiValue niZero(NiType type);

/// Falta de runtime (design §5.3): carregada com linha/coluna do bytecode.
struct NiFault {
    enum class Kind : std::uint8_t {
        Type,          ///< operação com tipo incompatível (runtime)
        NilUse,        ///< nil consumido em operação tipada
        DivByZero,     ///< '/' ou '%' com divisor zero
        Range,         ///< conversão/valor fora de range (i(), f()…)
        Timeout,       ///< orçamento de instruções esgotado
        RepeatLimit,   ///< contagem de repeat fora de [0, 65536]
        EmitDepth,     ///< emissão aninhada além de 32
        Stack,         ///< estouro de pilha de valores/frames
        EntityStale,   ///< entidade obsoleta em operação que exige viva
        EntityNull,    ///< entidade nula em operação que exige viva
        ComponentMissing, ///< componente ausente na entidade
        FieldUnknown,  ///< caminho não resolvido pela tabela de bindings
        BadArgument,   ///< argumento inválido para nativo (spawn/find/…)
        LinkLimit,     ///< mais de 64 links por instância
        NativeError,   ///< nativo de host falhou
        BadWrite,      ///< alvo de escrita inválido
    };
    Kind kind = Kind::Type;
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t col = 0;

    /// Nome curto do tipo de falha (determinístico, para logs/testes).
    [[nodiscard]] static std::string_view kindName(Kind kind) noexcept;
};

/// Limites do VM (design §5/§6 — todos determinísticos).
inline constexpr std::uint64_t kDefaultBudget = 1'000'000; ///< instruções/evento
inline constexpr std::int64_t kRepeatMax = 65'536;         ///< repeat N máximo
inline constexpr std::size_t kMaxLinks = 64;               ///< por instância
inline constexpr std::uint32_t kMaxEmitDepth = 32;         ///< emit aninhado
inline constexpr std::size_t kMaxFrames = 256;              ///< profundidade call
inline constexpr std::size_t kMaxValueStack = 65'536;       ///< slots

} // namespace eng::ni
