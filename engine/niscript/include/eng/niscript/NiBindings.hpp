#pragma once

/// eng::ni — bindings do NI-Script.
///
/// TRÊS peças, camadas separadas:
///
/// 1. NiNativeTable — nativos fechados em COMPILE time (segurança §8.1):
///    &BL (biblioteca base pura, visível só com `add &BL`) + nativos de
///    HOST (implementados sobre NiHost). Nomes/arity conhecidos pelo
///    compilador; o runtime CONFERENCE nativeCount (nunca índice trocado).
///
/// 2. NiHost — interface ABSTRATA de serviços do jogo (delta/ações/spawn),
///    implementada pelo CONSUMIDOR (editor no Android, harness nos testes).
///    engine/niscript NÃO conhece input/editor/android.
///
/// 3. NiBindingTable — resolução de CAMINHOS de entidade (`e.position.x`):
///    entradas {alias → get/set} registradas pelo CONSUMIDOR. O adaptador
///    REFLETIDO (niAddReflectionBinding) reusa offsets+typeName do
///    TypeRegistry — MESMO mecanismo do Inspector (auditoria D2: reuso do
///    reflection, zero hard-code de componentes no engine de script).

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "eng/ecs/Ecs.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/niscript/NiValue.hpp"

namespace eng::ni {

class NiExecContext; // NiVm.hpp (frente apenas)

/// Assinatura de nativo: devolve `out` ou preenche `fault`.
/// `argc` é a contagem REAL de argumentos (variadic: [arity, maxArity]).
/// SEM exceções — falha é Fault.
using NiNativeFn = bool (*)(NiExecContext& ctx, const NiValue* args,
                            std::uint16_t argc, NiValue& out, NiFault& fault);

struct NiNativeEntry {
    const char* name = "";
    std::uint16_t arity = 0;      ///< aridade mínima
    std::uint16_t maxArity = 0;   ///< 0 = fixa (== arity); >0 = faixa
    bool moduleBL = false; ///< requer `add &BL`
    NiNativeFn fn = nullptr;
    /// Tipo ESTÁTICO do resultado (Dynamic = desconhecido — checagem em
    /// runtime). Construtores de &BL são tipados (vec2 → Vec2 etc.).
    NiType resultType = NiType::Dynamic;
};

/// Tabela de nativos (fechada em compile time — design §8.1).
class NiNativeTable final {
public:
    /// Biblioteca base &BL (pura — design §7.2). Idempotente.
    void addBaseLibrary();

    /// Nativos de HOST padrão (design §7.3): delta/0, action_down/1,
    /// action_pressed/1, action_released/1, spawn/1, despawn/1, self/0,
    /// find/1 — todos sobre NiHost. Idempotente (por nome).
    void addStandardHost();

    /// Registro extra (extensões do consumidor — nome duplicado é erro).
    [[nodiscard]] bool add(std::string_view name, std::uint16_t arity,
                           NiNativeFn fn, NiType result = NiType::Dynamic);
    /// Registro variadic: argc ∈ [minArity, maxArity] (maxArity > minArity).
    [[nodiscard]] bool add(std::string_view name, std::uint16_t minArity,
                           std::uint16_t maxArity, NiNativeFn fn,
                           NiType result = NiType::Dynamic);

    [[nodiscard]] const NiNativeEntry* find(std::string_view name) const
        noexcept;
    /// Índice da entrada (-1 se ausente) — usado pelo compilador (CALL_N).
    [[nodiscard]] std::ptrdiff_t indexOf(std::string_view name) const noexcept;
    [[nodiscard]] const NiNativeEntry* at(std::size_t index) const noexcept
    {
        return &entries_[index];
    }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    /// NOMES com storage estável (deque: c_str() nunca invalida ao crescer).
    std::deque<std::string> names_;
    std::vector<NiNativeEntry> entries_;
};

/// Serviços do jogo para scripts — implementado pelo CONSUMIDOR.
/// Threads: mesma thread do VM (single-thread — ADR-049).
class NiHost {
public:
    virtual ~NiHost() = default;

    /// Delta do tick corrente (segundos).
    [[nodiscard]] virtual float deltaSeconds() const = 0;
    /// Estado de uma ação de input.
    [[nodiscard]] virtual bool actionDown(std::string_view action) const = 0;
    [[nodiscard]] virtual bool actionPressed(std::string_view action) const = 0;
    [[nodiscard]] virtual bool actionReleased(std::string_view action) const = 0;
    /// Cria um nó nomeado (entidade nula = falha).
    [[nodiscard]] virtual eng::ecs::Entity spawn(std::string_view name) = 0;
    /// Destrói o nó (false = handle obsoleto/nulo — no-op seguro).
    [[nodiscard]] virtual bool despawn(eng::ecs::Entity entity) = 0;
    /// Primeira entidade viva com este Name (nula = ausente).
    [[nodiscard]] virtual eng::ecs::Entity find(
        std::string_view name) const = 0;

    // --- P4.6: movimento de gameplay (padrão Godot/Unity) -------
    // Ambos operam sobre a PRÓPRIA entidade (self do script), em unidades
    // de MUNDO, no eixo XY (z preservado).
    /// move(dx,dy): P4.7.0 B5 — com kinematic_sweep ON (default da cena),
    /// o move de um KINEMATIC com Collider é VARRIDO (TOI+slide — o
    /// script ingênuo COLIDE; parede para e desliza, nunca atravessa).
    /// Demais casos (sem RigidBody, não-kinematic, sem collider, sweep
    /// OFF): translação crua — semântica pré-P4.7. false = entidade sem
    /// transform (no-op seguro).
    [[nodiscard]] virtual bool translate(
        eng::ecs::Entity self, float dx, float dy) = 0;
    /// teleport(x,y): translação CRUA — NUNCA varrida (P4.7.0 B5: a
    /// válvula de escape do autor para spawn/reposicionamento; atravessa
    /// colisores por design MESMO com sweep ON). false = entidade sem
    /// transform (no-op seguro).
    [[nodiscard]] virtual bool teleport(
        eng::ecs::Entity self, float x, float y)
    {
        (void)self;
        (void)x;
        (void)y;
        return false; // default no-op seguro (padrão dos verbos camera.*)
    }
    /// move_and_slide(dx,dy): varredura da esfera do CharacterBody com
    /// deslize (substeps anti-túnel; o mask do próprio corpo decide
    /// contra quem desliza). false = sem CharacterBody/transform — o
    /// chamador vira fault PRECISO (nunca deslize silencioso).
    [[nodiscard]] virtual bool moveAndSlide(
        eng::ecs::Entity self, float dx, float dy,
        eng::math::Vec3& outPosition) = 0;

    // --- P4.7.0: câmera de jogo como API de script --------------
    // Operam sobre a PRIMEIRA câmera ativa da cena (ordem estável — o
    // mesmo "primeiro ativo vence" do CameraTick). Default no-op seguro
    // (hosts sem cena de jogo): false = sem câmera ativa.
    /// camera.zoom(z): pixels por unidade (clamp > 0).
    [[nodiscard]] virtual bool cameraZoom(float /*pixelsPerUnit*/)
    {
        return false;
    }
    /// camera.position(x,y): OFFSETS da câmera em relação à entidade
    /// dela (mesma semântica do Inspector — P2 §11).
    [[nodiscard]] virtual bool cameraPosition(float /*x*/, float /*y*/)
    {
        return false;
    }
    /// camera.follow(name): segue a primeira entidade com este Name
    /// ("" = solta a câmera — follow off).
    [[nodiscard]] virtual bool cameraFollow(std::string_view /*name*/)
    {
        return false;
    }
};

/// Resolução de caminhos de entidade — `e.<alias>.<campo>.<subcampo>…`.
/// `fieldPath` é o SUBCAMINHO dentro do componente (pode ser vazio para o
/// componente inteiro). Falhas preenchem `fault` (precisas — nunca throw).
struct NiComponentBinding {
    std::string alias; ///< "position", "rigidbody", "eng::physics::RigidBody"…
    bool (*get)(void* user, eng::ecs::Entity, std::string_view fieldPath,
                NiValue& out, NiFault& fault) = nullptr;
    bool (*set)(void* user, eng::ecs::Entity, std::string_view fieldPath,
                const NiValue& value, NiFault& fault) = nullptr;
    void* user = nullptr; ///< mundo/cena/estado que o fetch usa
    /// Posse de estado do adapter (niAddReflectionBinding) — a entrada
    /// mantém o adapter vivo; bindings manuais podem deixar nulo.
    std::shared_ptr<void> keepAlive;
};

class NiBindingTable final {
public:
    /// Alias duplicado: o ÚLTIMO vence (o consumidor pode sobrescrever o
    /// açúcar padrão). Ordem de registro preservada (busca linear — tabelas
    /// são pequenas).
    void add(NiComponentBinding binding);

    [[nodiscard]] const NiComponentBinding* find(
        std::string_view alias) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bindings_.empty(); }

    /// Leitura de caminho COMPLETO ("position.x") — resolve o primeiro
    /// segmento como alias e o resto como fieldPath.
    [[nodiscard]] bool get(eng::ecs::Entity, std::string_view fullPath,
                           NiValue& out, NiFault& fault) const;
    /// Escrita de caminho COMPLETO.
    [[nodiscard]] bool set(eng::ecs::Entity, std::string_view fullPath,
                           const NiValue& value, NiFault& fault) const;

private:
    std::vector<NiComponentBinding> bindings_;
};

/// Adaptador REFLETIDO genérico (design §7.3): monta get/set sobre um
/// componente FETCHÁVEL (fetchConst/fetchMutable + user) lendo/escrevendo
/// por OFFSET via TypeInfo — o MESMO mecanismo do Inspector/serialização.
///
/// `basePath` desloca a raiz dos caminhos para DENTRO do componente
/// (ex.: alias "position" → componente Transform, basePath "position" —
/// os caminhos do script "position.x" resolvem relativos ao campo).
/// `valid` (opcional) checa vitalidade do handle para distinguir
/// EntityStale de ComponentMissing.
///
/// Regras de campo (typeName do reflect):
///   f32/f64 → Float (escrita f64→f32 checada: NaN/inf/overflow = Fault);
///   i8..i64/u8..u64 → Int (range checado; u64 > i63 = Fault);
///   bool → Bool; string → String;
///   eng::math::Vec3 → Vec3 (subcampos x/y/z e leitura inteira);
///   eng::math::Vec2 idem; eng::math::Quat → SÓ subcampos (x/y/z/w Float —
///   leitura inteira de quat = Fault "quat não é valor do script";
///   conversão euler↔quat é papel de bindings CUSTOM do consumidor);
///   enum → Int (por valor; NOME de enumerador em v2 — documentado);
///   struct desconhecido → recursão por reflect (subcampos por caminho).
///
/// Retorna false (sem registrar) se o tipo não está no TypeRegistry.
[[nodiscard]] bool niAddReflectionBinding(
    NiBindingTable& table, std::string_view alias, std::string_view typeName,
    const void* (*fetchConst)(void* user, eng::ecs::Entity),
    void* (*fetchMutable)(void* user, eng::ecs::Entity), void* user,
    std::string_view basePath = {},
    bool (*valid)(void* user, eng::ecs::Entity) = nullptr);

} // namespace eng::ni
