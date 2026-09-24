/// VM do NI-Script — execução de bytecode.
///
/// Modelo: stack machine com ORÇAMENTO GLOBAL de instruções (§6.2 —
/// determinístico, sem threads/alarme); regiões repair/timeout
/// numa pilha de REGIÕES por execução; Faults nunca viram exceções
/// — desmontam até a região `repair` mais interna ou abortam o
/// EVENTO (isolamento — o script não morre, design §5.3.5).
///
/// Frames carregam o PROGRAMA dono (emit cruza instâncias — §4), a
/// INSTÂNCIA dona (globais/self/links) e a base da pilha de valores.

#include "eng/niscript/NiVm.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eng/niscript/NiProgram.hpp"

namespace eng::ni {

const NiHandler* NiProgram::findHandler(std::string_view event) const noexcept
{
    for (const NiHandler& h : handlers) {
        if (h.name == event) {
            return &h;
        }
    }
    return nullptr;
}

// =============================================================================
// NiScriptState / NiInstanceSet
// =============================================================================

NiScriptState::NiScriptState(std::shared_ptr<const NiProgram> program,
                             eng::ecs::Entity self)
    : program_(std::move(program)), self_(self)
{
    globals_.reserve(program_->globals.size());
    for (const NiGlobal& g : program_->globals) {
        globals_.push_back(g.zero);
    }
}

const NiValue* NiScriptState::global(std::string_view name) const noexcept
{
    const auto& globals = program_->globals;
    for (std::size_t i = 0; i < globals.size(); ++i) {
        if (globals[i].name == name) {
            return &globals_[i];
        }
    }
    return nullptr;
}

NiScriptState& NiInstanceSet::create(
    std::shared_ptr<const NiProgram> program, ecs::Entity self)
{
    instances_.push_back(
        std::make_unique<NiScriptState>(std::move(program), self));
    return *instances_.back();
}

std::vector<NiScriptState*> NiInstanceSet::instancesOf(
    ecs::Entity entity) const
{
    std::vector<NiScriptState*> found;
    for (const std::unique_ptr<NiScriptState>& instance : instances_) {
        if (instance->self() == entity) {
            found.push_back(instance.get());
        }
    }
    return found;
}

std::vector<std::optional<NiFault>> NiInstanceSet::broadcast(
    const char* event, NiVm& vm, const NiExecContext::Params& params)
{
    std::vector<std::optional<NiFault>> faults;
    for (const std::unique_ptr<NiScriptState>& instance : instances_) {
        faults.push_back(vm.run(*instance, event, params));
    }
    return faults;
}

// =============================================================================
// Operações internas da execução (amigas de NiExecContext)
// =============================================================================

[[nodiscard]] bool niFieldIdLocal(const std::string& name,
                           std::uint32_t& id) noexcept
{
    if (name == "x" || name == "r") { id = static_cast<std::uint32_t>(NiField::X); return true; }
    if (name == "y" || name == "g") { id = static_cast<std::uint32_t>(NiField::Y); return true; }
    if (name == "z" || name == "b") { id = static_cast<std::uint32_t>(NiField::Z); return true; }
    if (name == "w" || name == "a") { id = static_cast<std::uint32_t>(NiField::W); return true; }
    if (name == "position") { id = static_cast<std::uint32_t>(NiField::Position); return true; }
    if (name == "rotation") { id = static_cast<std::uint32_t>(NiField::Rotation); return true; }
    if (name == "scale") { id = static_cast<std::uint32_t>(NiField::Scale); return true; }
    return false;
}

[[nodiscard]] NiValue constToValue(const NiConst& c)
{
    switch (c.kind) {
    case NiConst::Kind::Int: return niInt(c.i);
    case NiConst::Kind::Float: return niFloat(c.d);
    case NiConst::Kind::Bool: return niBool(c.i != 0);
    case NiConst::Kind::String: return niString(c.s);
    case NiConst::Kind::FieldChain: return NiValue{}; // nil interno
    }
    return NiValue{};
}

[[nodiscard]] bool isNum(const NiValue& v) noexcept
{
    return v.type == NiType::Int || v.type == NiType::Float;
}

struct NiExecOps final {
    static void fail(NiExecContext& x, NiFault::Kind kind, std::string message)
    {
        NiFault f;
        f.kind = kind;
        f.message = std::move(message);
        if (!x.frames_.empty()) {
            const auto& frame = x.frames_.back();
            if (frame.pc > 0 && frame.pc <= frame.func->lines.size()) {
                f.line = frame.func->lines[frame.pc - 1];
                f.col = frame.func->cols[frame.pc - 1];
            }
        }
        x.fault_ = std::move(f);
    }

    static void failAt(NiExecContext& x, const NiFault& f)
    {
        NiFault copy = f;
        if (!x.frames_.empty() && copy.line == 0) {
            const auto& frame = x.frames_.back();
            if (frame.pc > 0 && frame.pc <= frame.func->lines.size()) {
                copy.line = frame.func->lines[frame.pc - 1];
                copy.col = frame.func->cols[frame.pc - 1];
            }
        }
        x.fault_ = std::move(copy);
    }

    /// Desmonta até a região repair mais interna. false = abortar evento.
    [[nodiscard]] static bool unwind(NiExecContext& x)
    {
        while (!x.regions_.empty()) {
            const NiExecContext::Region region = x.regions_.back();
            x.regions_.pop_back();
            if (region.kind == NiExecContext::Region::Kind::Repair) {
                x.stack_.resize(region.stackDepth);
                x.frames_.resize(region.frameDepth);
                if (x.frames_.empty()) {
                    return false;
                }
                x.frames_.back().pc = region.contPc;
                x.deadline_ = region.savedDeadline;
                return true;
            }
        }
        return false;
    }

    /// Resolve CompView → valor (tabela de bindings). Outros: como estão.
    [[nodiscard]] static bool resolveValue(NiExecContext& x,
                                            const NiValue& in, NiValue& out)
    {
        if (in.type != NiType::CompView) {
            out = in;
            return true;
        }
        if (x.params_.bindings == nullptr || x.params_.bindings->empty()) {
            fail(x, NiFault::Kind::FieldUnknown,
                 "acesso a campo de entidade sem tabela de bindings");
            return false;
        }
        NiFault readFault;
        if (!x.params_.bindings->get(niEntityOf(in), in.s, out, readFault)) {
            failAt(x, readFault);
            return false;
        }
        return true;
    }

    [[nodiscard]] static bool popResolved(NiExecContext& x, NiValue& out)
    {
        if (x.stack_.empty()) {
            fail(x, NiFault::Kind::Stack, "pilha de valores vazia (interno)");
            return false;
        }
        const NiValue top = std::move(x.stack_.back());
        x.stack_.pop_back();
        return resolveValue(x, top, out);
    }

    [[nodiscard]] static bool popAny(NiExecContext& x, NiValue& out)
    {
        if (x.stack_.empty()) {
            fail(x, NiFault::Kind::Stack, "pilha de valores vazia (interno)");
            return false;
        }
        out = std::move(x.stack_.back());
        x.stack_.pop_back();
        return true;
    }

    static void push(NiExecContext& x, NiValue v)
    {
        x.stack_.push_back(std::move(v));
    }

    /// Empilha frame de chamada (args já na pilha; checa params tipados).
    /// `fromEmit`: frame criado por propagação/handler local de emit — conta
    /// na profundidade de emissão.
    [[nodiscard]] static bool doCall(NiExecContext& x,
                                     const NiProgram* program,
                                     const NiFunc* func,
                                     NiScriptState* owner,
                                     bool fromEmit = false)
    {
        if (fromEmit) {
            if (x.emitDepth_ >= kMaxEmitDepth) {
                fail(x, NiFault::Kind::EmitDepth,
                     "emissão aninhada além de "
                         + std::to_string(kMaxEmitDepth));
                return false;
            }
            ++x.emitDepth_;
        }
        if (x.frames_.size() >= kMaxFrames) {
            fail(x, NiFault::Kind::Stack,
                 "profundidade de chamadas excedida (limite "
                     + std::to_string(kMaxFrames) + ")");
            return false;
        }
        const std::uint32_t nparams = func->nparams;
        if (x.stack_.size() < nparams) {
            fail(x, NiFault::Kind::Stack,
                 "argumentos insuficientes (interno)");
            return false;
        }
        const std::uint32_t base =
            static_cast<std::uint32_t>(x.stack_.size()) - nparams;
        for (std::uint32_t i = 0; i < nparams; ++i) {
            if (i < func->paramTypes.size()
                && func->paramTypes[i] != NiType::Dynamic) {
                NiValue resolved;
                if (!resolveValue(x, x.stack_[base + i], resolved)) {
                    return false;
                }
                if (resolved.type == NiType::Nil) {
                    fail(x, NiFault::Kind::NilUse,
                         "argumento " + std::to_string(i + 1) + " de '"
                             + func->name + "': nil não é "
                             + std::string(niTypeName(func->paramTypes[i])));
                    return false;
                }
                if (resolved.type != func->paramTypes[i]) {
                    fail(x, NiFault::Kind::Type,
                         "argumento " + std::to_string(i + 1) + " de '"
                             + func->name + "': esperado "
                             + std::string(niTypeName(func->paramTypes[i]))
                             + ", obtido "
                             + std::string(niTypeName(resolved.type)));
                    return false;
                }
                x.stack_[base + i] = std::move(resolved);
            }
        }
        if (static_cast<std::size_t>(base) + func->nlocals
            > kMaxValueStack) {
            fail(x, NiFault::Kind::Stack,
                 "pilha de valores excedida (limite "
                     + std::to_string(kMaxValueStack) + ")");
            return false;
        }
        NiExecContext::Frame frame;
        frame.program = program;
        frame.func = func;
        frame.owner = owner;
        frame.pc = 0;
        frame.base = base;
        frame.regionBase = x.regions_.size();
        frame.fromEmit = fromEmit;
        x.frames_.push_back(frame);
        x.stack_.resize(static_cast<std::size_t>(base) + func->nlocals);
        return true;
    }

    /// Retorno (GIVE): valor → caller; região do frame é desfeita. Frames
    /// criados por `emit` decretam a profundidade de emissão.
    static void doReturn(NiExecContext& x, NiValue value)
    {
        const NiExecContext::Frame frame = x.frames_.back();
        x.frames_.pop_back();
        x.regions_.resize(frame.regionBase);
        x.stack_.resize(frame.base);
        if (frame.fromEmit) {
            --x.emitDepth_;
        }
        if (x.frames_.empty()) {
            return; // fim do evento — valor do handler raiz descartado
        }
        push(x, std::move(value));
    }

    /// emit: handler local + BFS por links; chamadas empilhadas em
    /// ordem INVERSA (frames LIFO ⇒ execução na ordem coletada).
    [[nodiscard]] static bool doEmit(NiExecContext& x, const std::string& event)
    {
        struct Pending {
            const NiProgram* program;
            const NiFunc* func;
            NiScriptState* owner;
        };
        std::vector<Pending> pending;

        // COPIA os dados da origem ANTES de qualquer push (realocação)
        const NiProgram* originProgram = nullptr;
        NiScriptState* emitter = nullptr;
        if (!x.frames_.empty()) {
            originProgram = x.frames_.back().program;
            emitter = x.frames_.back().owner;
        }
        if (originProgram != nullptr) {
            if (const NiHandler* local =
                    originProgram->findHandler(event)) {
                pending.push_back(
                    {originProgram,
                     &originProgram->funcs[local->funcIndex], emitter});
            }
        }

        if (x.params_.set != nullptr && emitter != nullptr) {
            std::vector<ecs::Entity> queue(emitter->links().begin(),
                                           emitter->links().end());
            std::unordered_set<ecs::Entity> visited{emitter->self()};
            while (!queue.empty()) {
                const ecs::Entity e = queue.front();
                queue.erase(queue.begin());
                if (visited.count(e) != 0) {
                    continue;
                }
                visited.insert(e);
                for (NiScriptState* instance :
                     x.params_.set->instancesOf(e)) {
                    const NiProgram& p = instance->program();
                    if (const NiHandler* h = p.findHandler(event)) {
                        pending.push_back(
                            {&p, &p.funcs[h->funcIndex], instance});
                    }
                    queue.insert(queue.end(), instance->links().begin(),
                                 instance->links().end());
                }
            }
        }

        bool ok = true;
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            if (!doCall(x, it->program, it->func, it->owner, true)) {
                ok = false;
                break;
            }
        }
        return ok;
    }

    // --- campos de struct (vec/color/transform) ---------------------------

    [[nodiscard]] static bool vecGet(const NiValue& base, std::uint32_t field,
                                     NiValue& out, NiExecContext& x)
    {
        switch (base.type) {
        case NiType::Vec2:
        case NiType::Vec3:
        case NiType::Color: {
            const std::uint32_t max =
                base.type == NiType::Vec2 ? 2
                : base.type == NiType::Vec3 ? 3
                                            : 4;
            if (field >= max) {
                fail(x, NiFault::Kind::Type,
                     "campo inválido para "
                         + std::string(niTypeName(base.type)));
                return false;
            }
            out = niFloat(base.d[field]);
            return true;
        }
        case NiType::Transform: {
            if (field < static_cast<std::uint32_t>(NiField::Position)
                || field > static_cast<std::uint32_t>(NiField::Scale)) {
                fail(x, NiFault::Kind::Type, "campo inválido para transform");
                return false;
            }
            const std::size_t k = static_cast<std::size_t>(field)
                                  - static_cast<std::size_t>(NiField::Position);
            out = niVec3(base.d[3 * k], base.d[3 * k + 1], base.d[3 * k + 2]);
            return true;
        }
        default:
            fail(x, NiFault::Kind::Type,
                 "tipo " + std::string(niTypeName(base.type))
                     + " não tem campos estáticos");
            return false;
        }
    }

    [[nodiscard]] static bool vecSet(NiValue& base, std::uint32_t field,
                                     const NiValue& value, NiExecContext& x)
    {
        if (value.type == NiType::Nil) {
            fail(x, NiFault::Kind::NilUse, "nil usado em escrita de campo");
            return false;
        }
        switch (base.type) {
        case NiType::Vec2: case NiType::Vec3: case NiType::Color: {
            const std::uint32_t max =
                base.type == NiType::Vec2 ? 2
                : base.type == NiType::Vec3 ? 3
                                            : 4;
            if (field >= max) {
                fail(x, NiFault::Kind::Type,
                     "campo inválido para "
                         + std::string(niTypeName(base.type)));
                return false;
            }
            if (value.type != NiType::Float && value.type != NiType::Int) {
                fail(x, NiFault::Kind::Type,
                     "componente exige numérico (obtido: "
                         + std::string(niTypeName(value.type)) + ")");
                return false;
            }
            base.d[field] = value.type == NiType::Float
                                ? value.d[0]
                                : static_cast<double>(value.i);
            return true;
        }
        case NiType::Transform: {
            if (field < static_cast<std::uint32_t>(NiField::Position)
                || field > static_cast<std::uint32_t>(NiField::Scale)) {
                fail(x, NiFault::Kind::Type, "campo inválido para transform");
                return false;
            }
            if (value.type != NiType::Vec3) {
                fail(x, NiFault::Kind::Type,
                     "campo de transform exige vec3 (obtido: "
                         + std::string(niTypeName(value.type)) + ")");
                return false;
            }
            const std::size_t k = static_cast<std::size_t>(field)
                                  - static_cast<std::size_t>(NiField::Position);
            base.d[3 * k] = value.d[0];
            base.d[3 * k + 1] = value.d[1];
            base.d[3 * k + 2] = value.d[2];
            return true;
        }
        default:
            fail(x, NiFault::Kind::Type,
                 "tipo " + std::string(niTypeName(base.type))
                     + " não tem campos escrevíveis");
            return false;
        }
    }

    /// NEST_SET: escreve a cadeia em CÓPIAS (value semantics) e devolve a
    /// struct modificada no topo da pilha.
    [[nodiscard]] static bool nestSet(NiExecContext& x, NiValue base,
                                      const std::vector<std::uint32_t>& chain,
                                      const NiValue& value)
    {
        if (chain.empty()) {
            fail(x, NiFault::Kind::BadWrite,
                 "cadeia de campos vazia (interno)");
            return false;
        }
        if (chain.size() == 1) {
            if (!vecSet(base, chain[0], value, x)) {
                return false;
            }
            push(x, std::move(base));
            return true;
        }
        NiValue sub;
        if (!vecGet(base, chain[0], sub, x)) {
            return false;
        }
        std::vector<std::uint32_t> rest(chain.begin() + 1, chain.end());
        if (!nestSet(x, std::move(sub), rest, value)) {
            return false;
        }
        NiValue modified = std::move(x.stack_.back());
        x.stack_.pop_back();
        if (!vecSet(base, chain[0], modified, x)) {
            return false;
        }
        push(x, std::move(base));
        return true;
    }

    /// DYN_GET: base + campo → compview acumulado OU componente de valor.
    [[nodiscard]] static bool dynGet(NiExecContext& x, NiValue base,
                                     const std::string& field)
    {
        switch (base.type) {
        case NiType::Entity:
            push(x, niCompView(niEntityOf(base), field));
            return true;
        case NiType::CompView:
            base.s += "." + field;
            push(x, std::move(base));
            return true;
        case NiType::Vec2: case NiType::Vec3: case NiType::Color:
        case NiType::Transform: {
            std::uint32_t id = 0;
            if (!niFieldIdLocal(field, id)) {
                fail(x, NiFault::Kind::Type,
                     "campo desconhecido '" + field + "' em "
                         + std::string(niTypeName(base.type)));
                return false;
            }
            NiValue out;
            if (!vecGet(base, id, out, x)) {
                return false;
            }
            push(x, std::move(out));
            return true;
        }
        case NiType::Nil:
            fail(x, NiFault::Kind::NilUse, "nil não tem campos");
            return false;
        default:
            fail(x, NiFault::Kind::Type,
                 "tipo " + std::string(niTypeName(base.type))
                     + " não suporta acesso a campo");
            return false;
        }
    }

    /// DYN_SET: pop valor, pop base → escrita. SEMPRE empilha a base
    /// resultante (entity para write-through — o STORE raiz é no-op
    /// seguro; valor modificado para structs — o STORE raiz persiste).
    [[nodiscard]] static bool dynSet(NiExecContext& x, const NiValue& value,
                                     NiValue base, const std::string& field)
    {
        if (value.type == NiType::Nil) {
            fail(x, NiFault::Kind::NilUse, "nil em escrita de campo");
            return false;
        }
        switch (base.type) {
        case NiType::Entity: {
            if (x.params_.bindings == nullptr || x.params_.bindings->empty()) {
                fail(x, NiFault::Kind::FieldUnknown,
                     "escrita em entidade sem tabela de bindings");
                return false;
            }
            if (niIsNullEntity(base)) {
                fail(x, NiFault::Kind::EntityNull,
                     "escrita em entidade nula");
                return false;
            }
            NiFault writeFault;
            if (!x.params_.bindings->set(niEntityOf(base), field, value,
                                         writeFault)) {
                failAt(x, writeFault);
                return false;
            }
            push(x, std::move(base)); // base COMO ESTÁ (entity)
            return true;
        }
        case NiType::CompView: {
            if (x.params_.bindings == nullptr || x.params_.bindings->empty()) {
                fail(x, NiFault::Kind::FieldUnknown,
                     "escrita em componente sem tabela de bindings");
                return false;
            }
            const std::string path = base.s + "." + field;
            NiFault writeFault;
            if (!x.params_.bindings->set(niEntityOf(base), path, value,
                                         writeFault)) {
                failAt(x, writeFault);
                return false;
            }
            push(x, std::move(base)); // compview PERMANECE compview
            return true;
        }
        default: {
            std::uint32_t id = 0;
            if (!niFieldIdLocal(field, id)) {
                fail(x, NiFault::Kind::Type,
                     "campo desconhecido '" + field + "' em "
                         + std::string(niTypeName(base.type)));
                return false;
            }
            if (!vecSet(base, id, value, x)) {
                return false;
            }
            push(x, std::move(base));
            return true;
        }
        }
    }

    // --- aritmética ---------------------------------------------------------

    [[nodiscard]] static bool numericBinary(NiExecContext& x, OpCode op)
    {
        NiValue r;
        NiValue l;
        if (!popResolved(x, r) || !popResolved(x, l)) {
            return false;
        }
        if (l.type == NiType::Nil || r.type == NiType::Nil) {
            fail(x, NiFault::Kind::NilUse,
                 "nil usado em operação numérica");
            return false;
        }
        if (!isNum(l) || !isNum(r)) {
            fail(x, NiFault::Kind::Type,
                 std::string("operandos numéricos esperados (obtidos: ")
                     + std::string(niTypeName(l.type)) + " e "
                     + std::string(niTypeName(r.type)) + ")");
            return false;
        }
        if (op == OpCode::DIV) {
            const bool zero = r.type == NiType::Int ? r.i == 0
                                                    : r.d[0] == 0.0;
            if (zero) {
                fail(x, NiFault::Kind::DivByZero, "divisão por zero");
                return false;
            }
        }
        if (op == OpCode::MOD && (l.type != NiType::Int
                                  || r.type != NiType::Int)) {
            fail(x, NiFault::Kind::Type, "'%' exige int");
            return false;
        }
        if (l.type == NiType::Int && r.type == NiType::Int) {
            std::int64_t result = 0;
            switch (op) {
            case OpCode::ADD: result = l.i + r.i; break;
            case OpCode::SUB: result = l.i - r.i; break;
            case OpCode::MUL: result = l.i * r.i; break;
            case OpCode::DIV: result = l.i / r.i; break; // trunc p/ zero
            case OpCode::MOD: result = l.i % r.i; break;
            default: break;
            }
            push(x, niInt(result));
            return true;
        }
        const double a = l.type == NiType::Float
                             ? l.d[0]
                             : static_cast<double>(l.i);
        const double b = r.type == NiType::Float
                             ? r.d[0]
                             : static_cast<double>(r.i);
        double result = 0.0;
        switch (op) {
        case OpCode::ADD: result = a + b; break;
        case OpCode::SUB: result = a - b; break;
        case OpCode::MUL: result = a * b; break;
        case OpCode::DIV: result = a / b; break;
        default: break;
        }
        push(x, niFloat(result));
        return true;
    }

    /// + - * com vetores e concatenação de strings (design §3.1:
    /// v+w, v-w, v*escalar, escalar*v, "a"+"b"; '/' NÃO é definida para
    /// vetores — cai na checagem numérica).
    [[nodiscard]] static bool vecBinary(NiExecContext& x, OpCode op)
    {
        NiValue r;
        NiValue l;
        if (!popResolved(x, r) || !popResolved(x, l)) {
            return false;
        }
        if (op == OpCode::ADD && l.type == NiType::String
            && r.type == NiType::String) {
            std::string concat = l.s + r.s;
            push(x, niString(std::move(concat)));
            return true;
        }
        const bool vecL = l.type == NiType::Vec2 || l.type == NiType::Vec3;
        const bool vecR = r.type == NiType::Vec2 || r.type == NiType::Vec3;
        if (vecL && vecR) {
            if (l.type != r.type) {
                fail(x, NiFault::Kind::Type,
                     "vetores de dimensões distintas ("
                         + std::string(niTypeName(l.type)) + " e "
                         + std::string(niTypeName(r.type)) + ")");
                return false;
            }
            NiValue out = l;
            const std::size_t n = l.type == NiType::Vec2 ? 2 : 3;
            for (std::size_t k = 0; k < n; ++k) {
                out.d[k] = op == OpCode::ADD ? l.d[k] + r.d[k]
                                             : l.d[k] - r.d[k];
            }
            push(x, std::move(out));
            return true;
        }
        if (vecL && isNum(r) && op != OpCode::DIV) {
            const double s = r.type == NiType::Float
                                 ? r.d[0]
                                 : static_cast<double>(r.i);
            NiValue out = l;
            const std::size_t n = l.type == NiType::Vec2 ? 2 : 3;
            for (std::size_t k = 0; k < n; ++k) {
                out.d[k] = op == OpCode::MUL ? l.d[k] * s : l.d[k] / s;
            }
            push(x, std::move(out));
            return true;
        }
        if (isNum(l) && vecR && op == OpCode::MUL) {
            const double s = l.type == NiType::Float
                                 ? l.d[0]
                                 : static_cast<double>(l.i);
            NiValue out = r;
            const std::size_t n = r.type == NiType::Vec2 ? 2 : 3;
            for (std::size_t k = 0; k < n; ++k) {
                out.d[k] = r.d[k] * s;
            }
            push(x, std::move(out));
            return true;
        }
        // não é vetor → escalar (reempilha na ordem original)
        push(x, std::move(l));
        push(x, std::move(r));
        return numericBinary(x, op);
    }
};

// =============================================================================
// NiVm::run — laço de dispatch
// =============================================================================

std::optional<NiFault> NiVm::run(NiScriptState& state,
                                 std::string_view event,
                                 const NiExecContext::Params& paramsIn)
{
    const NiProgram& program = state.program();
    if (paramsIn.natives == nullptr) {
        NiFault f;
        f.kind = NiFault::Kind::NativeError;
        f.message = "NiVm::run sem tabela de nativos (configuração do host)";
        return f;
    }
    if (paramsIn.natives->size() != program.nativeCount) {
        NiFault f;
        f.kind = NiFault::Kind::NativeError;
        f.message =
            "tabela de nativos divergente do bytecode (compilado com "
            + std::to_string(program.nativeCount) + ", runtime com "
            + std::to_string(paramsIn.natives->size()) + ")";
        return f;
    }
    const NiHandler* handler = program.findHandler(event);
    if (handler == nullptr) {
        return std::nullopt; // sem handler — nada a fazer
    }

    NiExecContext x;
    x.params_ = paramsIn;
    x.deadline_ = paramsIn.budget;
    if (!NiExecOps::doCall(x, &program, &program.funcs[handler->funcIndex],
                           &state)) {
        state.lastFault_ = x.fault_;
        return x.fault_;
    }

    for (;;) {
        if (x.frames_.empty()) {
            return std::nullopt; // evento concluído sem fault
        }
        if (x.frames_.back().pc >= x.frames_.back().func->code.size()) {
            // fim implícito sem GIVE (defensivo — o compiler emite GIVE)
            NiExecOps::doReturn(x, NiValue{});
            continue;
        }
        const std::uint32_t pc = x.frames_.back().pc;
        const NiInstr in = x.frames_.back().func->code[pc];
        x.frames_.back().pc = pc + 1;
        const NiProgram* frameProgram = x.frames_.back().program;

        // orçamento global — ANTES da execução de cada instrução
        ++x.count_;
        if (x.count_ > x.deadline_) {
            NiExecOps::fail(x, NiFault::Kind::Timeout,
                            "orçamento de instruções esgotado ("
                                + std::to_string(paramsIn.budget) + ")");
            if (!NiExecOps::unwind(x)) {
                state.lastFault_ = x.fault_;
                return x.fault_;
            }
            // capturado por repair: registrado e segue
            state.lastFault_ = x.fault_;
            x.fault_.reset();
            continue;
        }
        // hook de trace — visão const, nunca muta
        if (traceStride_ != 0 && traceHook_ && (x.count_ % traceStride_) == 0) {
            NiExecContext::TraceInfo info;
            info.pc = pc;
            const auto& fn = *x.frames_.back().func;
            info.line = pc < fn.lines.size() ? fn.lines[pc] : 0;
            info.col = pc < fn.cols.size() ? fn.cols[pc] : 0;
            info.op = in.op;
            info.stackDepth = x.stack_.size();
            info.frameDepth = x.frames_.size();
            traceHook_(info);
        }

        bool ok = true;
        switch (in.op) {
        case OpCode::CONST:
            NiExecOps::push(x, constToValue(frameProgram->consts[in.a]));
            break;
        case OpCode::ZERO:
            NiExecOps::push(x, niZero(static_cast<NiType>(in.a)));
            break;
        case OpCode::LOAD_L: {
            const auto& frame = x.frames_.back();
            const std::size_t slot =
                static_cast<std::size_t>(frame.base) + in.a;
            if (slot >= x.stack_.size()) {
                NiExecOps::fail(x, NiFault::Kind::Stack,
                                "local fora da pilha (interno)");
                ok = false;
                break;
            }
            NiExecOps::push(x, x.stack_[slot]);
            break;
        }
        case OpCode::STORE_L: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            const auto& frame = x.frames_.back();
            const std::size_t slot =
                static_cast<std::size_t>(frame.base) + in.a;
            if (slot >= x.stack_.size()) {
                NiExecOps::fail(x, NiFault::Kind::Stack,
                                "local fora da pilha (interno)");
                ok = false;
                break;
            }
            x.stack_[slot] = std::move(v);
            break;
        }
        case OpCode::LOAD_G: {
            NiScriptState* owner = x.frames_.back().owner;
            if (in.a >= owner->globals_.size()) {
                NiExecOps::fail(x, NiFault::Kind::Stack,
                                "global fora do range (interno)");
                ok = false;
                break;
            }
            NiExecOps::push(x, owner->globals_[in.a]);
            break;
        }
        case OpCode::STORE_G: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            NiScriptState* owner = x.frames_.back().owner;
            if (in.a >= owner->globals_.size()) {
                NiExecOps::fail(x, NiFault::Kind::Stack,
                                "global fora do range (interno)");
                ok = false;
                break;
            }
            owner->globals_[in.a] = std::move(v);
            break;
        }
        case OpCode::CHECK_TYPE: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            const NiType expected = static_cast<NiType>(in.a);
            NiValue resolved;
            if (!NiExecOps::resolveValue(x, v, resolved)) {
                ok = false;
                break;
            }
            if (resolved.type == NiType::Nil) {
                NiExecOps::fail(x, NiFault::Kind::NilUse,
                                "valor nil atribuído a variável tipada");
                ok = false;
                break;
            }
            if (resolved.type != expected) {
                NiExecOps::fail(
                    x, NiFault::Kind::Type,
                    "tipo " + std::string(niTypeName(resolved.type))
                        + " não é compatível com "
                        + std::string(niTypeName(expected)));
                ok = false;
                break;
            }
            NiExecOps::push(x, std::move(resolved));
            break;
        }
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
            ok = NiExecOps::vecBinary(x, in.op);
            break;
        case OpCode::DIV:
        case OpCode::MOD:
            ok = NiExecOps::numericBinary(x, in.op);
            break;
        case OpCode::NEG: {
            NiValue v;
            if (!NiExecOps::popResolved(x, v)) {
                ok = false;
                break;
            }
            if (v.type == NiType::Nil) {
                NiExecOps::fail(x, NiFault::Kind::NilUse, "nil em negação");
                ok = false;
                break;
            }
            if (v.type == NiType::Float) {
                NiExecOps::push(x, niFloat(-v.d[0]));
            } else if (v.type == NiType::Int) {
                NiExecOps::push(x, niInt(-v.i));
            } else if (v.type == NiType::Vec2 || v.type == NiType::Vec3) {
                const std::size_t n = v.type == NiType::Vec2 ? 2 : 3;
                for (std::size_t k = 0; k < n; ++k) {
                    v.d[k] = -v.d[k];
                }
                NiExecOps::push(x, std::move(v));
            } else {
                NiExecOps::fail(
                    x, NiFault::Kind::Type,
                    "'-' exige numérico/vetor (obtido: "
                        + std::string(niTypeName(v.type)) + ")");
                ok = false;
            }
            break;
        }
        case OpCode::NOT: {
            NiValue v;
            if (!NiExecOps::popResolved(x, v)) {
                ok = false;
                break;
            }
            if (v.type == NiType::Nil) {
                NiExecOps::fail(x, NiFault::Kind::NilUse, "nil em 'not'");
                ok = false;
                break;
            }
            if (v.type != NiType::Bool) {
                NiExecOps::fail(x, NiFault::Kind::Type,
                                "'not' exige bool");
                ok = false;
                break;
            }
            NiExecOps::push(x, niBool(v.i == 0));
            break;
        }
        case OpCode::EQ:
        case OpCode::NE: {
            NiValue r;
            NiValue l;
            if (!NiExecOps::popAny(x, r) || !NiExecOps::popAny(x, l)) {
                ok = false;
                break;
            }
            const bool equal = l == r;
            NiExecOps::push(x, niBool(in.op == OpCode::EQ ? equal : !equal));
            break;
        }
        case OpCode::LT: case OpCode::LE: case OpCode::GT: case OpCode::GE: {
            NiValue r;
            NiValue l;
            if (!NiExecOps::popResolved(x, r)
                || !NiExecOps::popResolved(x, l)) {
                ok = false;
                break;
            }
            if (!isNum(l) || !isNum(r)) {
                NiExecOps::fail(x, NiFault::Kind::Type,
                                "comparação exige numéricos");
                ok = false;
                break;
            }
            const double a = l.type == NiType::Float
                                 ? l.d[0]
                                 : static_cast<double>(l.i);
            const double b = r.type == NiType::Float
                                 ? r.d[0]
                                 : static_cast<double>(r.i);
            bool result = false;
            switch (in.op) {
            case OpCode::LT: result = a < b; break;
            case OpCode::LE: result = a <= b; break;
            case OpCode::GT: result = a > b; break;
            case OpCode::GE: result = a >= b; break;
            default: break;
            }
            NiExecOps::push(x, niBool(result));
            break;
        }
        case OpCode::JMP:
            x.frames_.back().pc =
                pc + 1 + static_cast<std::int32_t>(in.a);
            break;
        case OpCode::JMPF: {
            NiValue v;
            if (!NiExecOps::popResolved(x, v)) {
                ok = false;
                break;
            }
            if (v.type == NiType::Nil) {
                NiExecOps::fail(x, NiFault::Kind::NilUse,
                                "nil em condição");
                ok = false;
                break;
            }
            if (v.type != NiType::Bool) {
                NiExecOps::fail(x, NiFault::Kind::Type,
                                "condição deve ser bool");
                ok = false;
                break;
            }
            if (v.i == 0) {
                x.frames_.back().pc =
                    pc + 1 + static_cast<std::int32_t>(in.a);
            }
            break;
        }
        case OpCode::CALL_F: {
            const NiFunc* callee = &frameProgram->funcs[in.a];
            ok = NiExecOps::doCall(x, frameProgram, callee,
                                   x.frames_.back().owner);
            break;
        }
        case OpCode::CALL_N: {
            const NiNativeEntry& entry = *paramsIn.natives->at(in.a);
            const std::uint16_t argc = static_cast<std::uint16_t>(in.b);
            if (x.stack_.size() < argc) {
                NiExecOps::fail(x, NiFault::Kind::Stack,
                                "argumentos insuficientes para nativo (interno)");
                ok = false;
                break;
            }
            NiValue out;
            NiFault nativeFault;
            if (!entry.fn(x, x.stack_.data() + x.stack_.size() - argc, argc,
                          out, nativeFault)) {
                NiExecOps::failAt(x, nativeFault);
                ok = false;
                break;
            }
            for (std::uint16_t k = 0; k < argc; ++k) {
                x.stack_.pop_back();
            }
            NiExecOps::push(x, std::move(out));
            break;
        }
        case OpCode::GIVE: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            NiExecOps::doReturn(x, std::move(v));
            break;
        }
        case OpCode::POP: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            break;
        }
        case OpCode::VEC_GET: {
            NiValue base;
            if (!NiExecOps::popAny(x, base)) {
                ok = false;
                break;
            }
            NiValue out;
            if (!NiExecOps::vecGet(base, in.a, out, x)) {
                ok = false;
                break;
            }
            NiExecOps::push(x, std::move(out));
            break;
        }
        case OpCode::NEST_SET: {
            NiValue value;
            NiValue base;
            if (!NiExecOps::popAny(x, value)
                || !NiExecOps::popAny(x, base)) {
                ok = false;
                break;
            }
            const std::vector<std::uint32_t>& chain =
                frameProgram->consts[in.a].chain;
            if (!NiExecOps::nestSet(x, std::move(base), chain, value)) {
                ok = false;
                break;
            }
            break;
        }
        case OpCode::DYN_GET: {
            NiValue base;
            if (!NiExecOps::popAny(x, base)) {
                ok = false;
                break;
            }
            ok = NiExecOps::dynGet(x, std::move(base),
                                   frameProgram->consts[in.a].s);
            break;
        }
        case OpCode::DYN_SET: {
            NiValue value;
            NiValue base;
            if (!NiExecOps::popAny(x, value)
                || !NiExecOps::popAny(x, base)) {
                ok = false;
                break;
            }
            ok = NiExecOps::dynSet(x, value, std::move(base),
                                   frameProgram->consts[in.a].s);
            break;
        }
        case OpCode::TO_ENTITY: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            if (v.type == NiType::Entity) {
                NiExecOps::push(x, std::move(v));
            } else if (v.type == NiType::CompView) {
                NiExecOps::push(x, niEntityValue(niEntityOf(v)));
            } else {
                NiExecOps::fail(x, NiFault::Kind::Type,
                                "esperado entity/compview para extrair "
                                "entidade");
                ok = false;
            }
            break;
        }
        case OpCode::SELF:
            NiExecOps::push(x, niEntityValue(x.frames_.back().owner->self()));
            break;
        case OpCode::LINK_TO: {
            NiValue v;
            if (!NiExecOps::popAny(x, v)) {
                ok = false;
                break;
            }
            if (v.type != NiType::Entity) {
                NiExecOps::fail(x, NiFault::Kind::Type,
                                "link to exige entity (obtido: "
                                    + std::string(niTypeName(v.type)) + ")");
                ok = false;
                break;
            }
            if (niIsNullEntity(v)) {
                NiExecOps::fail(x, NiFault::Kind::EntityNull,
                                "link to com entidade nula");
                ok = false;
                break;
            }
            const ecs::Entity target = niEntityOf(v);
            if (target == x.frames_.back().owner->self()) {
                NiExecOps::fail(x, NiFault::Kind::BadArgument,
                                "link to para a própria entidade");
                ok = false;
                break;
            }
            std::vector<ecs::Entity>& links =
                x.frames_.back().owner->links_;
            if (std::find(links.begin(), links.end(), target)
                == links.end()) {
                if (links.size() >= kMaxLinks) {
                    NiExecOps::fail(
                        x, NiFault::Kind::LinkLimit,
                        "limite de links por instância ("
                            + std::to_string(kMaxLinks) + ")");
                    ok = false;
                    break;
                }
                links.push_back(target);
            }
            break;
        }
        case OpCode::EMIT:
            ok = NiExecOps::doEmit(x, frameProgram->consts[in.a].s);
            break;
        case OpCode::ENTER_REPAIR: {
            NiExecContext::Region region;
            region.kind = NiExecContext::Region::Kind::Repair;
            region.stackDepth = x.stack_.size();
            region.frameDepth = x.frames_.size();
            region.contPc = in.a; // absoluto
            region.savedDeadline = x.deadline_;
            x.regions_.push_back(region);
            break;
        }
        case OpCode::EXIT_REPAIR: {
            if (!x.regions_.empty()
                && x.regions_.back().kind
                       == NiExecContext::Region::Kind::Repair) {
                const NiExecContext::Region region = x.regions_.back();
                x.regions_.pop_back();
                x.deadline_ = region.savedDeadline;
            }
            break;
        }
        case OpCode::ENTER_TIMEOUT: {
            NiValue v;
            if (!NiExecOps::popResolved(x, v)) {
                ok = false;
                break;
            }
            if (v.type != NiType::Int || v.i < 1) {
                NiExecOps::fail(x, NiFault::Kind::Timeout,
                                "orçamento de timeout deve ser int >= 1");
                ok = false;
                break;
            }
            NiExecContext::Region region;
            region.kind = NiExecContext::Region::Kind::Timeout;
            region.stackDepth = x.stack_.size();
            region.frameDepth = x.frames_.size();
            region.savedDeadline = x.deadline_;
            x.regions_.push_back(region);
            const std::uint64_t newDeadline =
                x.count_ + static_cast<std::uint64_t>(v.i);
            if (newDeadline < x.deadline_) { // o mais APERTADO vigora
                x.deadline_ = newDeadline;
            }
            break;
        }
        case OpCode::EXIT_TIMEOUT: {
            if (!x.regions_.empty()
                && x.regions_.back().kind
                       == NiExecContext::Region::Kind::Timeout) {
                const NiExecContext::Region region = x.regions_.back();
                x.regions_.pop_back();
                x.deadline_ = region.savedDeadline;
            }
            break;
        }
        case OpCode::REPEAT_INIT: {
            NiValue v;
            if (!NiExecOps::popResolved(x, v)) {
                ok = false;
                break;
            }
            if (v.type != NiType::Int) {
                NiExecOps::fail(
                    x, NiFault::Kind::Type,
                    "contagem de repeat deve ser int (obtido: "
                        + std::string(niTypeName(v.type)) + ")");
                ok = false;
                break;
            }
            if (v.i < 0 || v.i > kRepeatMax) {
                NiExecOps::fail(x, NiFault::Kind::RepeatLimit,
                                "contagem de repeat fora de [0, "
                                    + std::to_string(kRepeatMax) + "]");
                ok = false;
                break;
            }
            if (v.i == 0) {
                x.frames_.back().pc =
                    pc + 1 + static_cast<std::int32_t>(in.a);
            } else {
                NiExecOps::push(x, std::move(v)); // contador na pilha
            }
            break;
        }
        case OpCode::REPEAT_STEP: {
            if (x.stack_.empty() || x.stack_.back().type != NiType::Int) {
                NiExecOps::fail(x, NiFault::Kind::Stack,
                                "contador de repeat perdido (interno)");
                ok = false;
                break;
            }
            NiValue& counter = x.stack_.back();
            counter.i -= 1;
            if (counter.i > 0) {
                x.frames_.back().pc =
                    pc + 1 + static_cast<std::int32_t>(in.a);
            } else {
                x.stack_.pop_back();
            }
            break;
        }
        }

        if (!ok) {
            if (!NiExecOps::unwind(x)) {
                state.lastFault_ = x.fault_;
                return x.fault_;
            }
            // fault CAPTURADO por repair: registrado na
            // instância — consultável do C++, não introspectável do script.
            state.lastFault_ = x.fault_;
            x.fault_.reset();
        }
    }
}

} // namespace eng::ni
