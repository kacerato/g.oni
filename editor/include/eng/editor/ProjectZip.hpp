#pragma once

/// eng::editor::ProjectZip — exportação/importação de projeto em zip.
///
/// BUG B-A (device round 2): o export/import vivia em Kotlin
/// (java.util.zip) com DUPLA quebra: o export não embrulhava as entradas
/// em uma pasta (import derivava o nome do projeto da ÚLTIMA entrada —
/// "assets"/"scenes"/"project.goni.json" → lixo no workspace + open
/// falhava) e era INTESTÁVEL no Linux. A correção estrutural: o zip do
/// projeto é código C++ aqui, sobre eng::fs::FileSystem (Memory no teste,
/// Rooted no device), testável de ponta a ponta no CI Linux.
///
/// Formato: zip STORE (sem compressão) — zero dependências, determinístico
/// (CRC32 IEEE local, entradas em ordem determinística do FileSystem::list).
/// Leitor aceita STORE e rejeita DEFLATE com erro claro (o zip que o
/// próprio editor escreve é sempre STORE; zips de terceiros comprimidos
/// recebem mensagem honesta — família B-E: "falha de load = erro explícito").

#include <string>
#include <string_view>

#include "eng/core/Result.hpp"
#include "eng/fs/FileSystem.hpp"

namespace eng::editor {

using eng::core::Result;

/// Compacta TODO o conteúdo de `projectDir` (recursivo, arquivos via
/// FileSystem) para o arquivo `outZipPath`, com cada entrada embrulhada
/// em "<wrapperName>/...". `wrapperName` vazio → entradas na raiz do zip
/// (evite: o import fica ambíguo). Sobrescreve o destino se existir.
[[nodiscard]] Result<void> buildProjectZip(eng::fs::FileSystem& fs,
                                           const eng::fs::Path& projectDir,
                                           const eng::fs::Path& outZipPath,
                                           std::string_view wrapperName);

/// Extrai `zipPath` para "<workspaceRoot>/<pasta>" e devolve o NOME DA
/// PASTA criada. A pasta é: o wrapper (1º componente comum) quando o zip
/// tem um; senão `preferredName`. Anti-traversal: entradas com "..",
/// caminhos absolutos ou vazios são recusadas (erro explícito — nunca
/// escreve fora do destino). Destino já existente → erro (sem merge
/// silencioso).
[[nodiscard]] Result<std::string> extractProjectZip(
    eng::fs::FileSystem& fs, const eng::fs::Path& zipPath,
    const eng::fs::Path& workspaceRoot, std::string_view preferredName);

}  // namespace eng::editor
