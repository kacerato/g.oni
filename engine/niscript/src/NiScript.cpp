/// Pipeline de compilação do NI-Script.
///
///   .nis → Lexer → Parser → Sema → Compiler → NiProgram (bytecode)
///
/// Puro e determinístico: mesmo input + mesma tabela de nativos produzem
/// o MESMO bytecode (testado). Diagnósticos com linha/coluna em toda
/// etapa; erros NUNCA são exceções.

#include "eng/niscript/NiScript.hpp"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "NiAst.hpp"

namespace eng::ni {

eng::core::Result<std::shared_ptr<const NiProgram>> compile(
    std::string_view source, const CompileOptions& options,
    std::vector<NiDiag>* diagnostics)
{
    using eng::core::Error;
    using eng::core::StatusCode;

    if (options.natives == nullptr) {
        if (diagnostics != nullptr) {
            diagnostics->push_back({1, 1,
                "CompileOptions sem tabela de nativos"});
        }
        return eng::core::makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "ni::compile: CompileOptions exige tabela de nativos"});
    }

    std::vector<NiDiag> diags;

    // 1. Lexer
    const std::vector<Token> tokens = lex(source, diags);
    if (!diags.empty()) {
        if (diagnostics != nullptr) {
            *diagnostics = diags;
        }
        return eng::core::makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "ni::compile: erros de lexing ("
                + std::to_string(diags.size()) + ")"});
    }
    // 2. Parser
    ParseResult parsed = parse(tokens, diags);
    if (!parsed.ok || !diags.empty()) {
        if (diagnostics != nullptr) {
            *diagnostics = diags;
        }
        return eng::core::makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "ni::compile: erros de parsing ("
                + std::to_string(diags.size()) + ")"});
    }

    // 3. Sema
    SemaResult sema = analyze(parsed.top, *options.natives, diags);
    if (!sema.ok || !diags.empty()) {
        if (diagnostics != nullptr) {
            *diagnostics = diags;
        }
        return eng::core::makeUnexpected(Error{
            StatusCode::InvalidArgument,
            "ni::compile: erros de análise ("
                + std::to_string(diags.size()) + ")"});
    }

    // 4. Compiler
    auto program = std::make_shared<NiProgram>();
    std::vector<NiDiag> emitDiags;
    if (!emitBytecode(parsed.top, sema, *options.natives, *program,
                      emitDiags)) {
        if (diagnostics != nullptr) {
            *diagnostics = emitDiags.empty() ? diags : emitDiags;
        }
        return eng::core::makeUnexpected(Error{
            StatusCode::Internal,
            "ni::compile: falha do emitter de bytecode"});
    }

    return eng::core::Result<std::shared_ptr<const NiProgram>>(
        std::move(program));
}

} // namespace eng::ni
