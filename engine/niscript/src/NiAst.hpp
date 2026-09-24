#pragma once

/// AST do NI-Script — INTERNO ao módulo (src/). Posse: nós vivem em deques
/// (endereços estáveis), sem destrutores manuais. Sema anexa tipos/escopos
/// por mapa de ponteiro→dado; o Compiler consome AST + anotações.

#include <cstdint>

#include "eng/niscript/NiValue.hpp"
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "eng/niscript/NiProgram.hpp"
#include "eng/niscript/NiScript.hpp"

namespace eng::ni {

// =============================================================================
// Tokens (Lexer → Parser)
// =============================================================================

enum class TokKind : std::uint8_t {
    End, Ident, Int, Float, Str,
    // keywords
    KwF, KwStop, KwUp, KwIf, KwElse, KwRepeat, KwRepair, KwTimeout,
    KwLink, KwTo, KwEmit, KwGive, KwVar, KwAdd, KwAnd, KwOr, KwNot,
    KwTrue, KwFalse,
    // (nomes de TIPO são contextuais — lexam como Ident; ver parseType)
    // símbolos
    Colon, LParen, RParen, Dot, Comma, Amp,
    Assign, Eq, Ne, Lt, Le, Gt, Ge, Plus, Minus, Star, Slash, Percent,
    // Atribuição composta — `x += v` etc. Desugared no Parser
    // para `x = x op v` (reusa 1:1 o caminho NEST_SET/DYN_SET do emitAssign).
    PlusAssign, MinusAssign, StarAssign, SlashAssign,
};

struct Token {
    TokKind kind = TokKind::End;
    std::uint32_t line = 1;
    std::uint32_t col = 1;
    std::string text;   ///< Ident/Str; dígitos canônicos para Int/Float
    std::int64_t i = 0; ///< Int
    double f = 0.0;     ///< Float
};

/// Nomes de TIPO são CONTEXTUAIS: lexam como Ident e são reconhecidos por
/// parseType (e aqui) — permite `vec3(...)` como construtor de &BL.
[[nodiscard]] bool typeKeywordOf(const std::string& text,
                                 NiType& out) noexcept;

/// Lexer — puro, com linha/coluna. Erros como NiDiag (nunca throw).
[[nodiscard]] std::vector<Token> lex(std::string_view source,
                                      std::vector<NiDiag>& diags);

// =============================================================================
// AST
// =============================================================================

struct Expr;
struct Stmt;

enum class UnOp : std::uint8_t { Neg, Not };
enum class BinOp : std::uint8_t {
    Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge, And, Or,
};

struct Expr {
    enum class Kind : std::uint8_t {
        Int, Float, Bool, Str, Ident, Call, Unary, Binary, Member,
    };
    Kind kind = Kind::Int;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    // literals
    std::int64_t i = 0;
    double f = 0.0;
    bool b = false;
    std::string s;                 ///< Str | Ident name | Call name
    // estrutura
    Expr* base = nullptr;          ///< Member base | Unary operando
    Expr* left = nullptr;          ///< Binary
    Expr* right = nullptr;         ///< Binary
    UnOp unOp = UnOp::Neg;
    BinOp binOp = BinOp::Add;
    std::vector<Expr*> args;       ///< Call
};

struct Block {
    std::vector<Stmt*> stmts;
};

struct Stmt {
    enum class Kind : std::uint8_t {
        Var, If, Repeat, Repair, Timeout, Link, Emit, Give, Assign, Expr,
    };
    Kind kind = Kind::Expr;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    // Var
    std::string varName;
    std::optional<NiType> varType; ///< anotação (nullo = inferência/dinâmico)
    Expr* init = nullptr;          ///< pode ser nullptr quando varType existe
    // If/Repeat/Repair/Timeout
    Expr* cond = nullptr;          ///< If | Repeat(count) | Timeout(budget)
    Block* body = nullptr;         ///< If-then | Repeat | Repair | Timeout
    Block* elseBody = nullptr;    ///< If-else
    // Link/Emit/Give/Assign/Expr
    Expr* target = nullptr;       ///< Link target | Give value (opcional)
    std::string eventName;        ///< Emit
    Expr* lvalue = nullptr;       ///< Assign
    Expr* value = nullptr;        ///< Assign RHS | ExprStmt
};

/// Declaração de topo (função/handler/global/add).
struct TopLevel {
    enum class Kind : std::uint8_t { Func, Handler, Global, AddModule };
    Kind kind = Kind::Global;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::string name;             ///< func/event/var/module
    std::vector<std::pair<std::string, std::optional<NiType>>> params; ///< Func
    Block* body = nullptr;        ///< Func/Handler
    std::optional<NiType> varType; ///< Global
    Expr* init = nullptr;         ///< Global
};

/// Parser — puro, com linha/coluna. Erros como NiDiag (nunca throw).
/// POSSE: os nós vivem no `arena` do resultado (unique_ptrs movidos para
/// fora do parser) — os ponteiros crus de TopLevel/Stmts/Exprs apontam
/// para ELE e permanecem válidos enquanto o ParseResult existir.
struct AstArena {
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<std::unique_ptr<Stmt>> stmts;
    std::vector<std::unique_ptr<Expr>> exprs;
};
struct ParseResult {
    AstArena arena;              ///< POSSE dos nós (RAII)
    std::vector<TopLevel> top;
    bool ok = false;
};
[[nodiscard]] ParseResult parse(const std::vector<Token>& tokens,
                                std::vector<NiDiag>& diags);

// =============================================================================
// Sema — símbolos + tipos estáticos (design §3.1)
// =============================================================================

struct SemaResult {
    bool ok = false;
    std::unordered_map<const Expr*, NiType> exprTypes;
    /// Resolução de Ident/Call para o COMPILER (slot/índice + tipo).
    struct IdentRef {
        enum class Kind : std::uint8_t { Local, Global, Func, Native };
        Kind kind = Kind::Local;
        std::uint16_t slot = 0;    ///< Local/Global
        std::uint32_t index = 0;  ///< Func/Native
        NiType type = NiType::Dynamic; ///< tipo declarado do alvo (Local/Global)
    };
    std::unordered_map<const Expr*, IdentRef> identRefs;
    std::unordered_map<const Stmt*, std::uint16_t> varSlots;
    std::unordered_map<const void*, std::uint16_t> localCount; ///< por Func/Handler
    /// name → {slot} globais na ordem de declaração (com tipo).
    std::vector<std::pair<std::string, NiType>> globals;
    /// name → {funcIndex, paramTypes}
    struct FuncSig {
        std::uint32_t funcIndex = 0;
        std::vector<NiType> paramTypes;
    };
    std::unordered_map<std::string, FuncSig> funcs;
    /// handlers na ordem: event → funcIndex (corpo = função aridade 0)
    std::vector<std::pair<std::string, std::uint32_t>> handlers;
    std::vector<std::string> modules; ///< ["BL"] se importado
};

[[nodiscard]] SemaResult analyze(const std::vector<TopLevel>& top,
                                  const NiNativeTable& natives,
                                  std::vector<NiDiag>& diags);

// =============================================================================
// Compiler — AST anotado → bytecode (design §6.4)
// =============================================================================

[[nodiscard]] bool emitBytecode(const std::vector<TopLevel>& top,
                                const SemaResult& sema,
                                const NiNativeTable& natives,
                                NiProgram& program, std::vector<NiDiag>& diags);

} // namespace eng::ni
