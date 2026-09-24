#pragma once

/// eng::serial — versionamento de schema e migrations (FASE 3; ADR-031).
///
/// Infraestrutura APENAS: nenhuma migration ativa nesta fase (nenhum schema
/// antigo existe — ADR-031). Cada tipo serializado declara sua schemaVersion
/// no payloadVersion do envelope; arquivos com versão MAIOR que a suportada
/// são erro claro; MENOR → roda a cadeia de migrations registradas.
///
/// Thread-safety: registro em inicialização single-threaded;
/// depois de construído, o registry é read-only e seguro para leitura
/// concorrente (sem locks — contrato documentado, igual ao TypeRegistry
/// exceto pela ausência de escrita pós-init).
#include <cstdint>
#include <memory>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::serial {

using SchemaVersion = std::uint32_t;

/// Um passo de migração: transforma dados de `from()` para `to()`.
class Migration {
public:
    virtual ~Migration() = default;

    [[nodiscard]] virtual SchemaVersion from() const noexcept = 0;
    [[nodiscard]] virtual SchemaVersion to() const noexcept = 0;
    /// Transforma os dados (JsonValue → JsonValue). Erro → falha clara.
    [[nodiscard]] virtual eng::core::Result<JsonValue> migrate(
        JsonValue data) const = 0;
};

class MigrationRegistry final {
public:
    /// Registra um passo. Erros: from >= to (InvalidArgument) ou já existe
    /// passo saindo de from() (AlreadyExists).
    eng::core::Result<void> add(std::unique_ptr<Migration> step);

    /// Aplica a cadeia `current → target` (exigem current <= target;
    /// current > target → InvalidArgument — downgrade não existe).
    /// Sem passo para alguma versão intermediária → ParseError claro.
    [[nodiscard]] eng::core::Result<JsonValue> migrateTo(
        JsonValue data, SchemaVersion current, SchemaVersion target) const;

    /// Maior versão alcançável conhecida (0 se vazio).
    [[nodiscard]] SchemaVersion latestKnown() const noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return steps_.size(); }

private:
    /// Ordenado por from(); invariantes: from < to; from único.
    std::vector<std::unique_ptr<Migration>> steps_;
};

} // namespace eng::serial
