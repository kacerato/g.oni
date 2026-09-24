#pragma once

/// eng::ni — bytecode do NI-Script.
///
/// Compilação UMA VEZ para bytecode (`.nis → compile() → NiProgram`); o VM
/// executa bytecode.
///
/// Instruções: `struct NiInstr { op; a; b; }` — operandos imediatos u32;
/// desvios (JMP/JMPF) são RELATIVOS ao pc (int32 em `a`); ENDERECOS
/// absolutos aparecem em ENTER_REPAIR (continuação) apenas.
///
/// Cada função carrega sourceMap (`lines`/`cols`, paralelo ao código) para
/// Faults com localização precisa linha/coluna (missão: diagnóstico).
///
/// Tabela de opcodes — semântica completa em docs/ni-script/05-bytecode.md:
///   CONST a        push consts[a]
///   ZERO a          push zero-value de NiType(a) (var sem inicializador;
///                   entity → handle nulo; nil para give implícito)
///   LOAD_L a / STORE_L a        local a do frame corrente
///   LOAD_G a / STORE_G a        global a da INSTÂNCIA corrente
///   CHECK_TYPE a  pop v; resolve CompView; tipo != NiType(a) → Fault; push
///   ADD SUB MUL DIV MOD NEG     numéricos (resolve CompView; checa runtime)
///   NOT           bool
///   EQ NE         igualdade estrutural (SEM resolve de CompView)
///   LT LE GT GE   numéricos
///   JMP a / JMPF a              pc += int32(a); JMPF pop (bool)
///   CALL_F a      chama funcs[a] (nparams do próprio func; frame novo)
///   CALL_N a      chama natives[a] (arity da entrada)
///   GIVE          pop valor → retorno (trunca até base do frame)
///   POP           descarta
///   VEC_GET a     pop struct; push componente (a = NiField)
///   NEST_SET a    pop valor; pop struct; escreve cadeia consts[a]; push
///   DYN_GET a     pop base; push membro consts[a] (acumula CompView ou
///                 lê componente de vec/color/transform — ver docs/05)
///   DYN_SET a     pop valor; pop base; escrita (CompView/Entity → binding
///                 write-through, push base COMO ESTÁ; vec/transform valor
///                 → push cópia modificada)
///   TO_ENTITY     pop v; Entity → push; CompView → push entidade EXTRAÍDA
///                 (normaliza o slot raiz em atribuições `e.campo.x = …`)
///   SELF          push entidade da instância dona do frame
///   LINK_TO       pop entidade → tabela de links da instância
///   EMIT a        evento consts[a]: handler local + propagação BFS
///   ENTER_REPAIR a / EXIT_REPAIR   região guardada
///   ENTER_TIMEOUT / EXIT_TIMEOUT   orçamento
///   REPEAT_INIT a / REPEAT_STEP a  iteração controlada

#include <cstdint>
#include <string>
#include <vector>

#include "eng/niscript/NiValue.hpp"

namespace eng::ni {

enum class OpCode : std::uint8_t {
    CONST = 0,
    ZERO,
    LOAD_L, STORE_L,
    LOAD_G, STORE_G,
    CHECK_TYPE,
    ADD, SUB, MUL, DIV, MOD, NEG,
    NOT,
    EQ, NE, LT, LE, GT, GE,
    JMP, JMPF,
    CALL_F, CALL_N,
    GIVE, POP,
    VEC_GET, NEST_SET,
    DYN_GET, DYN_SET, TO_ENTITY,
    SELF, LINK_TO, EMIT,
    ENTER_REPAIR, EXIT_REPAIR,
    ENTER_TIMEOUT, EXIT_TIMEOUT,
    REPEAT_INIT, REPEAT_STEP,
};

/// Campos de acesso estático (vec2/vec3/color/transform). Numéricos de 0..3
/// (x=r, y=g, z=b, w=a); struct de transform a partir de 8.
enum class NiField : std::uint32_t {
    X = 0, Y = 1, Z = 2, W = 3,
    Position = 8, Rotation = 9, Scale = 10,
};

/// Constante do pool (Int/Bool → i; Float → d[0]; String → s;
/// FieldChain → chain — cadeia de NiField do NEST_SET).
struct NiConst {
    enum class Kind : std::uint8_t { Int, Float, Bool, String, FieldChain };
    Kind kind = Kind::Int;
    std::int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<std::uint32_t> chain; ///< FieldChain: NiField em ordem
};

struct NiInstr {
    OpCode op = OpCode::CONST;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

/// Uma função do programa (handlers são funções de aridade 0).
struct NiFunc {
    std::string name;
    std::uint16_t nparams = 0;
    std::uint16_t nlocals = 0; ///< total de slots (parâmetros + locais)
    std::vector<NiInstr> code;
    std::vector<std::uint32_t> lines; ///< sourceMap paralelo ao código
    std::vector<std::uint32_t> cols;
    std::vector<NiType> paramTypes;  ///< Dynamic = sem anotação
};

/// Handler de evento (`up NOME`).
struct NiHandler {
    std::string name;
    std::uint32_t funcIndex = 0;
};

/// Global de instância (`var` no topo do script).
struct NiGlobal {
    std::string name;
    NiType type = NiType::Dynamic;
    NiValue zero; ///< zero-value copiado ao instanciar
};

/// Programa compilado. Imutável após a compilação; compartilhado entre
/// instâncias (shared_ptr) — estado por instância vive em NiScriptState.
struct NiProgram {
    std::vector<NiConst> consts;
    std::vector<NiFunc> funcs;
    std::vector<NiHandler> handlers;
    std::vector<NiGlobal> globals;
    std::vector<std::string> modules; ///< ex.: "BL"
    /// Tamanho da tabela de nativos usada na compilação — o runtime CONFERE
    /// (divergência tabela/bytecode = erro de configuração do host, nunca
    /// executa com índice trocado).
    std::size_t nativeCount = 0;

    /// Handler do evento (nullptr se o script não o define).
    [[nodiscard]] const NiHandler* findHandler(
        std::string_view event) const noexcept;
};

} // namespace eng::ni
