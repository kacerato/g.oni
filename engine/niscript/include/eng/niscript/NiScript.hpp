#pragma once

/// eng::ni — pipeline de compilação do NI-Script.
///
///   .nis → Lexer → Parser → Sema → Compiler → NiProgram (bytecode)
///
/// A compilação é PURA: sem I/O, sem relógio, sem estado global — mesmo
/// input + mesma tabela de nativos produzem o MESMO bytecode (testado).
/// Diagnósticos com LINHA/COLUNA em toda etapa (missão: ferramentas).
///
/// Segurança (design §8): o compilador só emite índices validados —
/// nomes desconhecidos (variável/função/nativo/módulo) são ERRO de
/// compilação; bytecode nunca referencia índice fora das tabelas.

#include <memory>
#include <string_view>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/niscript/NiBindings.hpp"
#include "eng/niscript/NiProgram.hpp"
#include "eng/niscript/NiValue.hpp"

namespace eng::ni {

/// Diagnóstico de compilação (erro — v1 não emite warnings).
struct NiDiag {
    std::uint32_t line = 0;   ///< 1-based
    std::uint32_t col = 0;    ///< 1-based
    std::string message;
};

/// Opções de compilação.
struct CompileOptions {
    /// Tabela de nativos visível ao script (BL + host — design §7).
    /// Obrigatória (mesmo vazia — scripts sem nativos).
    const NiNativeTable* natives = nullptr;
};

/// Compila fonte `.nis` → programa. Erros: Result com o PRIMEIRO erro +
/// `diagnostics` (se não nulo) recebe TODOS os erros encontrados (a etapa
/// continua coletando quando é seguro — parser/sema não-fatais).
[[nodiscard]] eng::core::Result<std::shared_ptr<const NiProgram>> compile(
    std::string_view source, const CompileOptions& options,
    std::vector<NiDiag>* diagnostics = nullptr);

} // namespace eng::ni
