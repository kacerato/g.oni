#pragma once

/// EditorProtocol — a única porta de entrada da UI para o editor.
///
/// A UI (Kotlin) não conhece o documento: ela lê um SNAPSHOT do estado e
/// envia OPERAÇÕES em JSON. Toda mutação passa por `call`, então a camada
/// JNI fica fina e todo o comportamento é testável no Linux.
///
///   snapshot(sinceKey) -> "" se nada mudou desde `sinceKey`,
///                         senão {"key":…, "project":…, "hierarchy":[…], …}
///   call({"op":"entity.create","template":"sprite"})
///                      -> {"ok":true,"result":…} | {"ok":false,"error":"…"}
///
/// Entidades cruzam como inteiros (EditorDocument::packEntity; 0 = nenhuma).
/// A lista de operações está em docs/editor-protocol.md.

#include <cstdint>
#include <string>
#include <string_view>

namespace eng::editor {

class EditorDocument;
class EditorHost;

class EditorProtocol final {
public:
    /// `host` é opcional: sem ele, operações de GPU/áudio do host (cache de
    /// texturas, backend, métricas) viram no-op — é o modo dos testes.
    explicit EditorProtocol(EditorDocument& doc, EditorHost* host = nullptr);

    [[nodiscard]] std::string snapshot(std::uint64_t sinceKey);
    [[nodiscard]] std::string call(std::string_view requestJson);

private:
    EditorDocument& doc_;
    EditorHost* host_;
    std::uint64_t calls_ = 0;
};

} // namespace eng::editor
