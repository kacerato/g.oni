/// Bindings do NI-Script — nativos (&BL + host), tabela de componentes e
/// adaptador refletido.
///
/// &BL é PURO (funções matemáticas/construtores — sem estado, sem I/O);
/// os nativos de HOST operam sobre NiHost (implementado pelo consumidor);
/// o adaptador refletido lê/escreve componentes por OFFSET com o
/// TypeRegistry — o mesmo mecanismo do Inspector (reuso, zero hard-code).

#include "eng/niscript/NiBindings.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "eng/niscript/NiProgram.hpp"
#include "eng/niscript/NiValue.hpp"
#include "eng/niscript/NiVm.hpp"
#include "eng/reflect/Reflect.hpp"

namespace eng::ni {

// =============================================================================
// NiNativeTable — infra
// =============================================================================

const NiNativeEntry* NiNativeTable::find(std::string_view name) const
    noexcept
{
    for (const NiNativeEntry& e : entries_) {
        if (name == e.name) {
            return &e;
        }
    }
    return nullptr;
}

std::ptrdiff_t NiNativeTable::indexOf(std::string_view name) const noexcept
{
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (name == entries_[i].name) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

bool NiNativeTable::add(std::string_view name, std::uint16_t arity,
                        NiNativeFn fn)
{
    if (find(name) != nullptr) {
        return false;
    }
    names_.push_back(std::string(name));
    NiNativeEntry e;
    e.name = names_.back().c_str(); // deque: estável
    e.arity = arity;
    e.maxArity = 0;
    e.fn = fn;
    entries_.push_back(e);
    return true;
}

bool NiNativeTable::add(std::string_view name, std::uint16_t minArity,
                        std::uint16_t maxArity, NiNativeFn fn)
{
    if (find(name) != nullptr || maxArity <= minArity) {
        return false;
    }
    names_.push_back(std::string(name));
    NiNativeEntry e;
    e.name = names_.back().c_str();
    e.arity = minArity;
    e.maxArity = maxArity;
    e.fn = fn;
    entries_.push_back(e);
    return true;
}

// =============================================================================
// &BL — biblioteca base (pura, determinística — design §7.2)
// =============================================================================

namespace bl {

void typeFault(NiFault& fault, const char* what, const NiValue& got)
{
    fault.kind = NiFault::Kind::Type;
    fault.message = std::string(what) + " (obtido: "
                    + std::string(niTypeName(got.type)) + ")";
}

[[nodiscard]] bool asDouble(const NiValue& v, double& out) noexcept
{
    if (v.type == NiType::Float) {
        out = v.d[0];
        return true;
    }
    if (v.type == NiType::Int) {
        out = static_cast<double>(v.i);
        return true;
    }
    return false;
}

[[nodiscard]] bool num1(NiExecContext& /*ctx*/, const NiValue* args,
                        std::uint16_t /*argc*/, NiValue& out, NiFault& fault,
                        double (*fn)(double), const char* name,
                        bool negativeIsFault)
{
    double x = 0.0;
    if (!asDouble(args[0], x)) {
        typeFault(fault, name, args[0]);
        return false;
    }
    if (negativeIsFault && x < 0.0) {
        fault.kind = NiFault::Kind::Range;
        fault.message = std::string(name) + " de valor negativo";
        return false;
    }
    out = niFloat(fn(x));
    return true;
}

[[nodiscard]] bool fnAbs(NiExecContext& ctx, const NiValue* args,
                        std::uint16_t argc, NiValue& out, NiFault& fault)
{
    return num1(ctx, args, argc, out, fault, std::fabs, "abs", false);
}
[[nodiscard]] bool fnCeil(NiExecContext& ctx, const NiValue* args,
                          std::uint16_t argc, NiValue& out, NiFault& fault)
{
    return num1(ctx, args, argc, out, fault, std::ceil, "ceil", false);
}
[[nodiscard]] bool fnFloor(NiExecContext& ctx, const NiValue* args,
                            std::uint16_t argc, NiValue& out, NiFault& fault)
{
    return num1(ctx, args, argc, out, fault, std::floor, "floor", false);
}
[[nodiscard]] bool fnSqrt(NiExecContext& ctx, const NiValue* args,
                          std::uint16_t argc, NiValue& out, NiFault& fault)
{
    return num1(ctx, args, argc, out, fault, std::sqrt, "sqrt", true);
}
[[nodiscard]] bool fnSin(NiExecContext& ctx, const NiValue* args,
                         std::uint16_t argc, NiValue& out, NiFault& fault)
{
    return num1(ctx, args, argc, out, fault, std::sin, "sin", false);
}
[[nodiscard]] bool fnCos(NiExecContext& ctx, const NiValue* args,
                         std::uint16_t argc, NiValue& out, NiFault& fault)
{
    return num1(ctx, args, argc, out, fault, std::cos, "cos", false);
}

[[nodiscard]] bool fnMin(NiExecContext& /*ctx*/, const NiValue* args,
                         std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    double a = 0.0;
    double b = 0.0;
    const bool ai = args[0].type == NiType::Int;
    const bool bi = args[1].type == NiType::Int;
    if (!asDouble(args[0], a) || !asDouble(args[1], b)) {
        typeFault(fault, "min", args[0].type == NiType::Int
                                   ? args[1]
                                   : args[0]);
        return false;
    }
    if (ai && bi) {
        out = niInt(std::min(args[0].i, args[1].i));
    } else {
        out = niFloat(std::min(a, b));
    }
    return true;
}

[[nodiscard]] bool fnMax(NiExecContext& /*ctx*/, const NiValue* args,
                         std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    double a = 0.0;
    double b = 0.0;
    const bool ai = args[0].type == NiType::Int;
    const bool bi = args[1].type == NiType::Int;
    if (!asDouble(args[0], a) || !asDouble(args[1], b)) {
        typeFault(fault, "max", args[0].type == NiType::Int
                                   ? args[1]
                                   : args[0]);
        return false;
    }
    if (ai && bi) {
        out = niInt(std::max(args[0].i, args[1].i));
    } else {
        out = niFloat(std::max(a, b));
    }
    return true;
}

[[nodiscard]] bool fnClamp(NiExecContext& /*ctx*/, const NiValue* args,
                           std::uint16_t /*argc*/, NiValue& out,
                           NiFault& fault)
{
    double x = 0.0;
    double lo = 0.0;
    double hi = 0.0;
    if (!asDouble(args[0], x) || !asDouble(args[1], lo)
        || !asDouble(args[2], hi)) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "clamp exige numéricos";
        return false;
    }
    if (lo > hi) {
        fault.kind = NiFault::Kind::BadArgument;
        fault.message = "clamp com mínimo maior que o máximo";
        return false;
    }
    out = niFloat(std::clamp(x, lo, hi));
    return true;
}

[[nodiscard]] bool fnStr(NiExecContext& /*ctx*/, const NiValue* args,
                         std::uint16_t /*argc*/, NiValue& out,
                         NiFault& fault)
{
    const NiValue& v = args[0];
    switch (v.type) {
    case NiType::Int: out = niString(std::to_string(v.i)); return true;
    case NiType::Float: out = niString(std::to_string(v.d[0])); return true;
    case NiType::Bool: out = niString(v.i != 0 ? "true" : "false");
        return true;
    case NiType::String: out = v; return true;
    case NiType::Vec2:
        out = niString("(" + std::to_string(v.d[0]) + ", "
                       + std::to_string(v.d[1]) + ")");
        return true;
    case NiType::Vec3:
        out = niString("(" + std::to_string(v.d[0]) + ", "
                       + std::to_string(v.d[1]) + ", "
                       + std::to_string(v.d[2]) + ")");
        return true;
    case NiType::Color:
        out = niString("(" + std::to_string(v.d[0]) + ", "
                       + std::to_string(v.d[1]) + ", "
                       + std::to_string(v.d[2]) + ", "
                       + std::to_string(v.d[3]) + ")");
        return true;
    case NiType::Transform:
        out = niString("((" + std::to_string(v.d[0]) + ", "
                       + std::to_string(v.d[1]) + ", "
                       + std::to_string(v.d[2]) + "), ("
                       + std::to_string(v.d[3]) + ", "
                       + std::to_string(v.d[4]) + ", "
                       + std::to_string(v.d[5]) + "), ("
                       + std::to_string(v.d[6]) + ", "
                       + std::to_string(v.d[7]) + ", "
                       + std::to_string(v.d[8]) + "))");
        return true;
    case NiType::Entity: {
        const eng::ecs::Entity e = niEntityOf(v);
        out = niString("entity(" + std::to_string(e.index) + ":"
                       + std::to_string(e.generation) + ")");
        return true;
    }
    case NiType::Asset:
        out = niString("asset(" + std::to_string(v.i) + ")");
        return true;
    case NiType::Nil:
        fault.kind = NiFault::Kind::NilUse;
        fault.message = "str de nil";
        return false;
    case NiType::CompView:
        fault.kind = NiFault::Kind::Type;
        fault.message = "componente não é imprimível; acesse um campo";
        return false;
    case NiType::Dynamic: break;
    }
    fault.kind = NiFault::Kind::Type;
    fault.message = "str: valor inválido";
    return false;
}

[[nodiscard]] bool fnLen(NiExecContext& /*ctx*/, const NiValue* args,
                         std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    if (args[0].type != NiType::String) {
        typeFault(fault, "len exige string", args[0]);
        return false;
    }
    // comprimento em BYTES UTF-8 (sem segmentação unicode em v1 — docs/08)
    out = niInt(static_cast<std::int64_t>(args[0].s.size()));
    return true;
}

[[nodiscard]] bool fnVec2(NiExecContext& /*ctx*/, const NiValue* args,
                          std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    double x = 0.0;
    double y = 0.0;
    if (!asDouble(args[0], x) || !asDouble(args[1], y)) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "vec2 exige numéricos";
        return false;
    }
    out = niVec2(x, y);
    return true;
}

[[nodiscard]] bool fnVec3(NiExecContext& /*ctx*/, const NiValue* args,
                          std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    if (!asDouble(args[0], x) || !asDouble(args[1], y)
        || !asDouble(args[2], z)) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "vec3 exige numéricos";
        return false;
    }
    out = niVec3(x, y, z);
    return true;
}

[[nodiscard]] bool fnColor(NiExecContext& /*ctx*/, const NiValue* args,
                           std::uint16_t argc, NiValue& out, NiFault& fault)
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (!asDouble(args[0], r) || !asDouble(args[1], g)
        || !asDouble(args[2], b)) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "color exige numéricos";
        return false;
    }
    double a = 1.0;
    if (argc == 4 && !asDouble(args[3], a)) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "color exige numéricos";
        return false;
    }
    out = niColor(r, g, b, a);
    return true;
}

[[nodiscard]] bool fnTransform(NiExecContext& /*ctx*/,
                               const NiValue* args,
                               std::uint16_t /*argc*/, NiValue& out,
                               NiFault& fault)
{
    if (args[0].type != NiType::Vec3 || args[1].type != NiType::Vec3
        || args[2].type != NiType::Vec3) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "transform exige (vec3, vec3, vec3)";
        return false;
    }
    NiTransform t;
    t.position = {args[0].d[0], args[0].d[1], args[0].d[2]};
    t.rotationDegrees = {args[1].d[0], args[1].d[1], args[1].d[2]};
    t.scale = {args[2].d[0], args[2].d[1], args[2].d[2]};
    out = niTransform(t);
    return true;
}

[[nodiscard]] bool fnI(NiExecContext& /*ctx*/, const NiValue* args,
                       std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    if (args[0].type == NiType::Int) {
        out = args[0];
        return true;
    }
    if (args[0].type == NiType::Float) {
        const double v = args[0].d[0];
        if (!std::isfinite(v) || v < -9.2233720368547758e18
            || v >= 9.2233720368547758e18) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "i(): valor fora do range de int";
            return false;
        }
        out = niInt(static_cast<std::int64_t>(v));
        return true;
    }
    typeFault(fault, "i exige numérico", args[0]);
    return false;
}

[[nodiscard]] bool fnF(NiExecContext& /*ctx*/, const NiValue* args,
                       std::uint16_t /*argc*/, NiValue& out, NiFault& fault)
{
    double v = 0.0;
    if (!asDouble(args[0], v)) {
        typeFault(fault, "f exige numérico", args[0]);
        return false;
    }
    out = niFloat(v);
    return true;
}

[[nodiscard]] bool fnComp(NiExecContext& /*ctx*/, const NiValue* args,
                          std::uint16_t /*argc*/, NiValue& out,
                          NiFault& fault)
{
    if (args[0].type != NiType::Entity) {
        typeFault(fault, "comp exige (entity, string)", args[0]);
        return false;
    }
    if (args[1].type != NiType::String) {
        typeFault(fault, "comp exige (entity, string)", args[1]);
        return false;
    }
    out = niCompView(niEntityOf(args[0]), args[1].s);
    return true;
}

} // namespace bl

void NiNativeTable::addBaseLibrary()
{
    if (find("vec2") != nullptr) {
        return; // idempotente
    }
    struct Reg {
        const char* name;
        std::uint16_t arity;
        std::uint16_t maxArity;
        NiNativeFn fn;
        NiType result;
    };
    static const Reg kBL[] = {
        {"abs", 1, 0, bl::fnAbs, NiType::Dynamic},
        {"ceil", 1, 0, bl::fnCeil, NiType::Float},
        {"floor", 1, 0, bl::fnFloor, NiType::Float},
        {"sqrt", 1, 0, bl::fnSqrt, NiType::Float},
        {"sin", 1, 0, bl::fnSin, NiType::Float},
        {"cos", 1, 0, bl::fnCos, NiType::Float},
        {"min", 2, 0, bl::fnMin, NiType::Dynamic},
        {"max", 2, 0, bl::fnMax, NiType::Dynamic},
        {"clamp", 3, 0, bl::fnClamp, NiType::Dynamic},
        {"str", 1, 0, bl::fnStr, NiType::String},
        {"len", 1, 0, bl::fnLen, NiType::Int},
        {"vec2", 2, 0, bl::fnVec2, NiType::Vec2},
        {"vec3", 3, 0, bl::fnVec3, NiType::Vec3},
        {"color", 3, 4, bl::fnColor, NiType::Color},
        {"transform", 3, 0, bl::fnTransform, NiType::Transform},
        {"i", 1, 0, bl::fnI, NiType::Int},
        {"fl", 1, 0, bl::fnF, NiType::Float},
        {"comp", 2, 0, bl::fnComp, NiType::CompView},
    };
    for (const Reg& r : kBL) {
        names_.push_back(r.name);
        NiNativeEntry e;
        e.name = names_.back().c_str();
        e.arity = r.arity;
        e.maxArity = r.maxArity;
        e.moduleBL = true;
        e.fn = r.fn;
        e.resultType = r.result;
        entries_.push_back(e);
    }
}

// =============================================================================
// Nativos de HOST (design §7.3) — sobre NiHost / contexto
// =============================================================================

namespace hostn {

void noHost(NiFault& fault, const char* who)
{
    fault.kind = NiFault::Kind::NativeError;
    fault.message = std::string(who)
                    + ": host não configurado (params.host)";
}

[[nodiscard]] bool fnDelta(NiExecContext& ctx, const NiValue* /*args*/,
                           std::uint16_t /*argc*/, NiValue& out,
                           NiFault& fault)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "delta");
        return false;
    }
    out = niFloat(host->deltaSeconds());
    return true;
}

[[nodiscard]] bool actionFn(NiExecContext& ctx, const NiValue* args,
                            NiValue& out, NiFault& fault, const char* who,
                            bool (NiHost::*fn)(std::string_view) const)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, who);
        return false;
    }
    if (args[0].type != NiType::String) {
        fault.kind = NiFault::Kind::Type;
        fault.message = std::string(who) + " exige string";
        return false;
    }
    out = niBool((host->*fn)(args[0].s));
    return true;
}

[[nodiscard]] bool fnActionDown(NiExecContext& ctx, const NiValue* args,
                                std::uint16_t /*argc*/, NiValue& out,
                                NiFault& fault)
{
    return actionFn(ctx, args, out, fault, "action_down",
                    &NiHost::actionDown);
}
[[nodiscard]] bool fnActionPressed(NiExecContext& ctx, const NiValue* args,
                                   std::uint16_t /*argc*/, NiValue& out,
                                   NiFault& fault)
{
    return actionFn(ctx, args, out, fault, "action_pressed",
                    &NiHost::actionPressed);
}
[[nodiscard]] bool fnActionReleased(NiExecContext& ctx, const NiValue* args,
                                    std::uint16_t /*argc*/, NiValue& out,
                                    NiFault& fault)
{
    return actionFn(ctx, args, out, fault, "action_released",
                    &NiHost::actionReleased);
}

[[nodiscard]] bool fnSpawn(NiExecContext& ctx, const NiValue* args,
                           std::uint16_t /*argc*/, NiValue& out,
                           NiFault& fault)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "spawn");
        return false;
    }
    if (args[0].type != NiType::String) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "spawn exige string";
        return false;
    }
    const eng::ecs::Entity e = host->spawn(args[0].s);
    if (e.index == 0xFFFFFFFFu) {
        fault.kind = NiFault::Kind::NativeError;
        fault.message = "spawn falhou";
        return false;
    }
    out = niEntityValue(e);
    return true;
}

[[nodiscard]] bool fnDespawn(NiExecContext& ctx, const NiValue* args,
                             std::uint16_t /*argc*/, NiValue& out,
                             NiFault& fault)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "despawn");
        return false;
    }
    if (args[0].type != NiType::Entity) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "despawn exige entity";
        return false;
    }
    out = niBool(host->despawn(niEntityOf(args[0])));
    return true;
}

[[nodiscard]] bool fnSelf(NiExecContext& ctx, const NiValue* /*args*/,
                          std::uint16_t /*argc*/, NiValue& out,
                          NiFault& fault)
{
    NiScriptState* instance = ctx.currentInstance();
    if (instance == nullptr) {
        fault.kind = NiFault::Kind::NativeError;
        fault.message = "self fora de execução de script";
        return false;
    }
    out = niEntityValue(instance->self());
    return true;
}

[[nodiscard]] bool fnFind(NiExecContext& ctx, const NiValue* args,
                          std::uint16_t /*argc*/, NiValue& out,
                          NiFault& fault)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "find");
        return false;
    }
    if (args[0].type != NiType::String) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "find exige string";
        return false;
    }
    const eng::ecs::Entity e = host->find(args[0].s);
    if (e.index == 0xFFFFFFFFu) {
        fault.kind = NiFault::Kind::BadArgument;
        fault.message = "entidade '" + args[0].s + "' não encontrada";
        return false;
    }
    out = niEntityValue(e);
    return true;
}

/// Extrai um número (Float OU Int) dos argumentos de
/// move/move_and_slide — scripts autoram com literais dos dois tipos.
[[nodiscard]] bool argNumber(const NiValue& v, float& out,
                             const char* who, NiFault& fault)
{
    if (v.type == NiType::Float) {
        out = static_cast<float>(v.d[0]);
        return true;
    }
    if (v.type == NiType::Int) {
        out = static_cast<float>(v.i);
        return true;
    }
    fault.kind = NiFault::Kind::Type;
    fault.message = std::string(who) + " exige números (dx, dy)";
    return false;
}

/// self + host comuns aos dois verbos de movimento.
[[nodiscard]] bool moveSelf(NiExecContext& ctx, const NiValue* args,
                            float& dx, float& dy, const char* who,
                            eng::ecs::Entity& self, NiFault& fault)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, who);
        return false;
    }
    NiScriptState* instance = ctx.currentInstance();
    if (instance == nullptr) {
        fault.kind = NiFault::Kind::NativeError;
        fault.message = std::string(who)
                        + " fora de execução de script";
        return false;
    }
    self = instance->self();
    if (!argNumber(args[0], dx, who, fault) ||
        !argNumber(args[1], dy, who, fault)) {
        return false;
    }
    return true;
}

/// move(dx,dy): P4.7.0 B5 — com kinematic_sweep ON (default da cena), o
/// move de um KINEMATIC com Collider é VARRIDO (TOI+slide — colide).
/// Demais casos: translação crua (semântica pré-P4.7 — ver NiHost).
[[nodiscard]] bool fnMove(NiExecContext& ctx, const NiValue* args,
                          std::uint16_t /*argc*/, NiValue& out,
                          NiFault& fault)
{
    float dx = 0.f;
    float dy = 0.f;
    eng::ecs::Entity self{0xFFFFFFFFu, 0xFFFFFFFFu};
    if (!moveSelf(ctx, args, dx, dy, "move", self, fault)) {
        return false;
    }
    NiHost* host = ctx.host();
    out = niBool(host->translate(self, dx, dy));
    return true;
}

/// move_and_slide(dx,dy): varredura com deslize (CharacterBody).
[[nodiscard]] bool fnMoveAndSlide(NiExecContext& ctx, const NiValue* args,
                                  std::uint16_t /*argc*/, NiValue& out,
                                  NiFault& fault)
{
    float dx = 0.f;
    float dy = 0.f;
    eng::ecs::Entity self{0xFFFFFFFFu, 0xFFFFFFFFu};
    if (!moveSelf(ctx, args, dx, dy, "move_and_slide", self, fault)) {
        return false;
    }
    NiHost* host = ctx.host();
    eng::math::Vec3 resolved{0.f, 0.f, 0.f};
    if (!host->moveAndSlide(self, dx, dy, resolved)) {
        fault.kind = NiFault::Kind::NativeError;
        fault.message =
            "move_and_slide: a entidade precisa de CharacterBody";
        return false;
    }
    out = niBool(true);
    return true;
}

/// teleport(x,y): translação CRUA — SEM varredura, SEMPRE (P4.7.0 B5:
/// com kinematic_sweep ON, `move` colide; o TELEPORTE é a válvula de
/// escape do autor para spawn/reposicionamento — atravessa por design).
/// Host dedicado (não o translate — que VARRE o kinematic com sweep ON).
[[nodiscard]] bool fnTeleport(NiExecContext& ctx, const NiValue* args,
                              std::uint16_t /*argc*/, NiValue& out,
                              NiFault& fault)
{
    float dx = 0.f;
    float dy = 0.f;
    eng::ecs::Entity self{0xFFFFFFFFu, 0xFFFFFFFFu};
    if (!moveSelf(ctx, args, dx, dy, "teleport", self, fault)) {
        return false;
    }
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "teleport");
        return false;
    }
    out = niBool(host->teleport(self, dx, dy));
    return true;
}

/// camera.zoom(z): zoom da PRIMEIRA câmera ativa (px por unidade).
[[nodiscard]] bool fnCameraZoom(NiExecContext& ctx, const NiValue* args,
                                std::uint16_t /*argc*/, NiValue& out,
                                NiFault& fault)
{
    float zoom = 0.f;
    if (!argNumber(args[0], zoom, "camera.zoom", fault)) {
        return false;
    }
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "camera.zoom");
        return false;
    }
    out = niBool(host->cameraZoom(zoom));
    return true;
}

/// camera.position(x,y): OFFSETS da câmera ativa (semântica Inspector).
[[nodiscard]] bool fnCameraPosition(NiExecContext& ctx, const NiValue* args,
                                    std::uint16_t /*argc*/, NiValue& out,
                                    NiFault& fault)
{
    float x = 0.f;
    float y = 0.f;
    if (!argNumber(args[0], x, "camera.position", fault) ||
        !argNumber(args[1], y, "camera.position", fault)) {
        return false;
    }
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "camera.position");
        return false;
    }
    out = niBool(host->cameraPosition(x, y));
    return true;
}

/// camera.follow(nome): segue a primeira entidade com este Name.
[[nodiscard]] bool fnCameraFollow(NiExecContext& ctx, const NiValue* args,
                                  std::uint16_t /*argc*/, NiValue& out,
                                  NiFault& fault)
{
    NiHost* host = ctx.host();
    if (host == nullptr) {
        noHost(fault, "camera.follow");
        return false;
    }
    if (args[0].type != NiType::String) {
        fault.kind = NiFault::Kind::Type;
        fault.message = "camera.follow exige string (nome da entidade)";
        return false;
    }
    out = niBool(host->cameraFollow(args[0].s));
    return true;
}

} // namespace hostn

void NiNativeTable::addStandardHost()
{
    if (find("delta") != nullptr) {
        return; // idempotente
    }
    struct Reg {
        const char* name;
        std::uint16_t arity;
        NiNativeFn fn;
        NiType result;
    };
    static const Reg kHost[] = {
        {"delta", 0, hostn::fnDelta, NiType::Float},
        {"action_down", 1, hostn::fnActionDown, NiType::Bool},
        {"action_pressed", 1, hostn::fnActionPressed, NiType::Bool},
        {"action_released", 1, hostn::fnActionReleased, NiType::Bool},
        {"spawn", 1, hostn::fnSpawn, NiType::Entity},
        {"despawn", 1, hostn::fnDespawn, NiType::Bool},
        {"self", 0, hostn::fnSelf, NiType::Entity},
        {"find", 1, hostn::fnFind, NiType::Entity},
        // Movimento de gameplay — move cru (teletransporte
        // documentado) e move_and_slide (varredura com deslize).
        {"move", 2, hostn::fnMove, NiType::Bool},
        {"move_and_slide", 2, hostn::fnMoveAndSlide, NiType::Bool},
        // Câmera de jogo autorável por script — mesma
        // câmera do Inspector/CameraTick (primeira ativa vence).
        // Teleporte cru — NUNCA varrido (spawn).
        {"teleport", 2, hostn::fnTeleport, NiType::Bool},
        {"camera.zoom", 1, hostn::fnCameraZoom, NiType::Bool},
        {"camera.position", 2, hostn::fnCameraPosition, NiType::Bool},
        {"camera.follow", 1, hostn::fnCameraFollow, NiType::Bool},
    };
    for (const Reg& r : kHost) {
        names_.push_back(r.name);
        NiNativeEntry e;
        e.name = names_.back().c_str();
        e.arity = r.arity;
        e.maxArity = 0;
        e.moduleBL = false;
        e.fn = r.fn;
        e.resultType = r.result;
        entries_.push_back(e);
    }
}

// =============================================================================
// NiBindingTable
// =============================================================================

void NiBindingTable::add(NiComponentBinding binding)
{
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
        if (it->alias == binding.alias) {
            *it = std::move(binding); // último vence (açúcar pode sobrescrever)
            return;
        }
    }
    bindings_.push_back(std::move(binding));
}

const NiComponentBinding* NiBindingTable::find(
    std::string_view alias) const noexcept
{
    for (const NiComponentBinding& b : bindings_) {
        if (b.alias == alias) {
            return &b;
        }
    }
    return nullptr;
}

bool NiBindingTable::get(eng::ecs::Entity e, std::string_view fullPath,
                         NiValue& out, NiFault& fault) const
{
    const std::size_t dot = fullPath.find('.');
    const std::string_view alias = fullPath.substr(0, dot);
    const std::string_view fieldPath =
        dot == std::string_view::npos ? std::string_view{}
                                       : fullPath.substr(dot + 1);
    const NiComponentBinding* b = find(alias);
    if (b == nullptr) {
        fault.kind = NiFault::Kind::FieldUnknown;
        fault.message = "binding desconhecido: '" + std::string(alias) + "'";
        return false;
    }
    return b->get(b->user, e, fieldPath, out, fault);
}

bool NiBindingTable::set(eng::ecs::Entity e, std::string_view fullPath,
                         const NiValue& value, NiFault& fault) const
{
    const std::size_t dot = fullPath.find('.');
    const std::string_view alias = fullPath.substr(0, dot);
    const std::string_view fieldPath =
        dot == std::string_view::npos ? std::string_view{}
                                       : fullPath.substr(dot + 1);
    const NiComponentBinding* b = find(alias);
    if (b == nullptr) {
        fault.kind = NiFault::Kind::FieldUnknown;
        fault.message = "binding desconhecido: '" + std::string(alias) + "'";
        return false;
    }
    return b->set(b->user, e, fieldPath, value, fault);
}

// =============================================================================
// Adaptador refletido (niAddReflectionBinding)
// =============================================================================

namespace {

/// Estado do adaptador (user pointer da entrada).
struct ReflAdapter {
    const eng::reflect::TypeInfo* info = nullptr;
    std::string basePath; ///< caminho pré-fixado dentro do componente
    const void* (*fetchConst)(void*, eng::ecs::Entity) = nullptr;
    void* (*fetchMutable)(void*, eng::ecs::Entity) = nullptr;
    bool (*valid)(void*, eng::ecs::Entity) = nullptr;
    void* user = nullptr;
};

enum class FieldKind : std::uint8_t {
    F32, F64, I8, I16, I32, I64, U8, U16, U32, U64, Bool, String,
    Vec3, Vec2, Quat, Enum, Struct,
};

FieldKind classify(const eng::reflect::TypeInfo& info)
{
    if (info.kind == eng::reflect::TypeKind::Enum) {
        return FieldKind::Enum;
    }
    const std::string& n = info.name;
    if (n == "f32") { return FieldKind::F32; }
    if (n == "f64") { return FieldKind::F64; }
    if (n == "i8") { return FieldKind::I8; }
    if (n == "i16") { return FieldKind::I16; }
    if (n == "i32") { return FieldKind::I32; }
    if (n == "i64") { return FieldKind::I64; }
    if (n == "u8") { return FieldKind::U8; }
    if (n == "u16") { return FieldKind::U16; }
    if (n == "u32") { return FieldKind::U32; }
    if (n == "u64") { return FieldKind::U64; }
    if (n == "bool") { return FieldKind::Bool; }
    if (n == "string") { return FieldKind::String; }
    if (n == "eng::math::Vec3") { return FieldKind::Vec3; }
    if (n == "eng::math::Vec2") { return FieldKind::Vec2; }
    if (n == "eng::math::Quat") { return FieldKind::Quat; }
    return FieldKind::Struct;
}

/// Localização resolvida: tipo (info) + offset dentro do componente.
struct Loc {
    const eng::reflect::TypeInfo* info = nullptr;
    std::size_t offset = 0;
};

/// Caminha `path` (segmentos separados por '.') dentro do componente.
[[nodiscard]] bool walk(const eng::reflect::TypeInfo* root, std::size_t base,
                        const std::string& path, Loc& out, NiFault& fault)
{
    const eng::reflect::TypeInfo* info = root;
    std::size_t offset = base;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string seg =
            path.substr(start, dot == std::string::npos
                                   ? std::string::npos
                                   : dot - start);
        if (!seg.empty()) {
            const eng::reflect::PropertyInfo* prop = nullptr;
            for (const eng::reflect::PropertyInfo& p : info->properties) {
                if (p.name == seg) {
                    prop = &p;
                    break;
                }
            }
            if (prop == nullptr) {
                fault.kind = NiFault::Kind::FieldUnknown;
                fault.message = "campo '" + seg + "' não existe em "
                                + info->name;
                return false;
            }
            offset += prop->offset;
            const eng::reflect::TypeInfo* next =
                eng::reflect::TypeRegistry::global().find(prop->typeName);
            if (next == nullptr) {
                fault.kind = NiFault::Kind::FieldUnknown;
                fault.message = "tipo '" + prop->typeName
                                + "' não registrado no reflect";
                return false;
            }
            info = next;
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    out.info = info;
    out.offset = offset;
    return true;
}

[[nodiscard]] bool isNullEntity(eng::ecs::Entity e) noexcept
{
    return e.index == 0xFFFFFFFFu && e.generation == 0xFFFFFFFFu;
}

/// Leitura de um valor no endereço (data+offset) pelo tipo.
[[nodiscard]] bool readValue(const void* data, const Loc& loc, NiValue& out,
                            NiFault& fault)
{
    const auto* info = loc.info;
    const auto* bytes = static_cast<const char*>(data) + loc.offset;
    switch (classify(*info)) {
    case FieldKind::F32: {
        float f32 = 0.f;
        std::memcpy(&f32, bytes, sizeof f32);
        out = niFloat(f32);
        return true;
    }
    case FieldKind::F64: {
        double f64 = 0.0;
        std::memcpy(&f64, bytes, sizeof f64);
        out = niFloat(f64);
        return true;
    }
    case FieldKind::I8: {
        std::int8_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::I16: {
        std::int16_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::I32: {
        std::int32_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::I64: {
        std::int64_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::U8: {
        std::uint8_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::U16: {
        std::uint16_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::U32: {
        std::uint32_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        out = niInt(v);
        return true;
    }
    case FieldKind::U64: {
        std::uint64_t v = 0;
        std::memcpy(&v, bytes, sizeof v);
        if (v > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "u64 fora do range de int do script";
            return false;
        }
        out = niInt(static_cast<std::int64_t>(v));
        return true;
    }
    case FieldKind::Bool: {
        bool v = false;
        std::memcpy(&v, bytes, sizeof v);
        out = niBool(v);
        return true;
    }
    case FieldKind::String: {
        const auto* slot = reinterpret_cast<const std::string*>(bytes);
        out = niString(*slot);
        return true;
    }
    case FieldKind::Vec3: {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        std::memcpy(&x, bytes + 0 * sizeof(float), sizeof x);
        std::memcpy(&y, bytes + 1 * sizeof(float), sizeof y);
        std::memcpy(&z, bytes + 2 * sizeof(float), sizeof z);
        out = niVec3(x, y, z);
        return true;
    }
    case FieldKind::Vec2: {
        float x = 0.f;
        float y = 0.f;
        std::memcpy(&x, bytes, sizeof x);
        std::memcpy(&y, bytes + sizeof(float), sizeof y);
        out = niVec2(x, y);
        return true;
    }
    case FieldKind::Quat:
        fault.kind = NiFault::Kind::Type;
        fault.message = "quat não é valor do script; acesse subcampos "
                        "(x/y/z/w) — euler é binding do consumidor";
        return false;
    case FieldKind::Enum: {
        std::int64_t v = 0;
        const std::size_t size = std::min<std::size_t>(info->size, 8);
        std::memcpy(&v, bytes, size);
        out = niInt(v);
        return true;
    }
    case FieldKind::Struct:
        fault.kind = NiFault::Kind::Type;
        fault.message = "struct '" + info->name
                        + "' não é valor do script; acesse subcampos";
        return false;
    }
    return false;
}

/// Escrita com checagens de range/conversão (f64→f32 exato quando possível).
[[nodiscard]] bool writeValue(void* data, const Loc& loc, const NiValue& v,
                             NiFault& fault)
{
    const auto* info = loc.info;
    auto* bytes = static_cast<char*>(data) + loc.offset;
    const auto needFloat = [&]() {
        if (v.type == NiType::Float) {
            return true;
        }
        if (v.type == NiType::Int) {
            return true;
        }
        fault.kind = NiFault::Kind::Type;
        fault.message = "campo numérico exige numérico (obtido: "
                        + std::string(niTypeName(v.type)) + ")";
        return false;
    };
    const auto toF32 = [&](float& outF) {
        const double d = v.type == NiType::Float ? v.d[0]
                                                : static_cast<double>(v.i);
        if (!std::isfinite(d)
            || std::fabs(d) > static_cast<double>(
                                  std::numeric_limits<float>::max())) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "valor fora do range de f32";
            return false;
        }
        outF = static_cast<float>(d);
        return true;
    };
    switch (classify(*info)) {
    case FieldKind::F32: {
        if (!needFloat()) {
            return false;
        }
        float f = 0.f;
        if (!toF32(f)) {
            return false;
        }
        std::memcpy(bytes, &f, sizeof f);
        return true;
    }
    case FieldKind::F64: {
        if (!needFloat()) {
            return false;
        }
        const double d = v.type == NiType::Float
                             ? v.d[0]
                             : static_cast<double>(v.i);
        std::memcpy(bytes, &d, sizeof d);
        return true;
    }
    case FieldKind::I8: {
        if (v.type != NiType::Int) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo inteiro exige int (obtido: "
                            + std::string(niTypeName(v.type)) + ")";
            return false;
        }
        if (v.i < -128 || v.i > 127) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "valor fora do range de i8";
            return false;
        }
        const std::int8_t w = static_cast<std::int8_t>(v.i);
        std::memcpy(bytes, &w, sizeof w);
        return true;
    }
    case FieldKind::I16: {
        if (v.type != NiType::Int) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo inteiro exige int (obtido: "
                            + std::string(niTypeName(v.type)) + ")";
            return false;
        }
        if (v.i < -32768 || v.i > 32767) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "valor fora do range de i16";
            return false;
        }
        const std::int16_t w = static_cast<std::int16_t>(v.i);
        std::memcpy(bytes, &w, sizeof w);
        return true;
    }
    case FieldKind::I32: {
        if (v.type != NiType::Int) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo inteiro exige int (obtido: "
                            + std::string(niTypeName(v.type)) + ")";
            return false;
        }
        if (v.i < std::numeric_limits<std::int32_t>::min()
            || v.i > std::numeric_limits<std::int32_t>::max()) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "valor fora do range de i32";
            return false;
        }
        const std::int32_t w = static_cast<std::int32_t>(v.i);
        std::memcpy(bytes, &w, sizeof w);
        return true;
    }
    case FieldKind::I64: {
        if (v.type != NiType::Int) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo inteiro exige int (obtido: "
                            + std::string(niTypeName(v.type)) + ")";
            return false;
        }
        std::memcpy(bytes, &v.i, sizeof v.i);
        return true;
    }
    case FieldKind::U8: case FieldKind::U16: case FieldKind::U32:
    case FieldKind::U64: {
        if (v.type != NiType::Int) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo inteiro exige int (obtido: "
                            + std::string(niTypeName(v.type)) + ")";
            return false;
        }
        if (v.i < 0) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "valor negativo em campo sem sinal";
            return false;
        }
        const std::uint64_t u = static_cast<std::uint64_t>(v.i);
        const std::size_t size = std::min<std::size_t>(info->size, 8);
        if (size < 8 && (u >> (size * 8)) != 0) {
            fault.kind = NiFault::Kind::Range;
            fault.message = "valor fora do range do campo";
            return false;
        }
        std::memcpy(bytes, &u, size);
        return true;
    }
    case FieldKind::Bool: {
        if (v.type != NiType::Bool) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo bool exige bool";
            return false;
        }
        const bool b = v.i != 0;
        std::memcpy(bytes, &b, sizeof b);
        return true;
    }
    case FieldKind::String: {
        if (v.type != NiType::String) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo string exige string";
            return false;
        }
        auto* slot = reinterpret_cast<std::string*>(bytes);
        *slot = v.s; // atribuição real — SEM memcpy de objeto não-trivial
        return true;
    }
    case FieldKind::Vec3: {
        if (v.type != NiType::Vec3) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo vec3 exige vec3 (obtido: "
                            + std::string(niTypeName(v.type)) + ")";
            return false;
        }
        for (int k = 0; k < 3; ++k) {
            const double d = v.d[k];
            if (!std::isfinite(d)
                || std::fabs(d) > static_cast<double>(
                                      std::numeric_limits<float>::max())) {
                fault.kind = NiFault::Kind::Range;
                fault.message = "componente fora do range de f32";
                return false;
            }
            const float f = static_cast<float>(d);
            std::memcpy(bytes + static_cast<std::size_t>(k) * sizeof(float),
                        &f, sizeof f);
        }
        return true;
    }
    case FieldKind::Vec2: {
        if (v.type != NiType::Vec2) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo vec2 exige vec2";
            return false;
        }
        for (int k = 0; k < 2; ++k) {
            const double d = v.d[k];
            if (!std::isfinite(d)
                || std::fabs(d) > static_cast<double>(
                                      std::numeric_limits<float>::max())) {
                fault.kind = NiFault::Kind::Range;
                fault.message = "componente fora do range de f32";
                return false;
            }
            const float f = static_cast<float>(d);
            std::memcpy(bytes + static_cast<std::size_t>(k) * sizeof(float),
                        &f, sizeof f);
        }
        return true;
    }
    case FieldKind::Quat:
        fault.kind = NiFault::Kind::Type;
        fault.message = "quat aceita apenas subcampos (x/y/z/w)";
        return false;
    case FieldKind::Enum: {
        if (v.type != NiType::Int) {
            fault.kind = NiFault::Kind::Type;
            fault.message = "campo enum exige int (valor)";
            return false;
        }
        const std::size_t size = std::min<std::size_t>(info->size, 8);
        std::memcpy(bytes, &v.i, size);
        return true;
    }
    case FieldKind::Struct:
        fault.kind = NiFault::Kind::Type;
        fault.message = "struct '" + info->name
                        + "' não é gravável do script; use subcampos";
        return false;
    }
    return false;
}

[[nodiscard]] std::string joinPath(const std::string& base,
                                    std::string_view field)
{
    if (base.empty()) {
        return std::string(field);
    }
    if (field.empty()) {
        return base;
    }
    return base + "." + std::string(field);
}

[[nodiscard]] bool reflGet(void* userData, eng::ecs::Entity e,
                           std::string_view fieldPath, NiValue& out,
                           NiFault& fault)
{
    const auto* self = static_cast<const ReflAdapter*>(userData);
    if (isNullEntity(e)) {
        fault.kind = NiFault::Kind::EntityNull;
        fault.message = "leitura em entidade nula";
        return false;
    }
    if (self->valid != nullptr && !self->valid(self->user, e)) {
        fault.kind = NiFault::Kind::EntityStale;
        fault.message = "entidade obsoleta (geração não confere — ADR-024)";
        return false;
    }
    const void* component = self->fetchConst(self->user, e);
    if (component == nullptr) {
        fault.kind = NiFault::Kind::ComponentMissing;
        fault.message = "componente ausente na entidade";
        return false;
    }
    Loc loc;
    if (!walk(self->info, 0, joinPath(self->basePath, fieldPath), loc,
              fault)) {
        return false;
    }
    return readValue(component, loc, out, fault);
}

[[nodiscard]] bool reflSet(void* userData, eng::ecs::Entity e,
                           std::string_view fieldPath, const NiValue& value,
                           NiFault& fault)
{
    const auto* self = static_cast<const ReflAdapter*>(userData);
    if (value.type == NiType::Nil) {
        fault.kind = NiFault::Kind::NilUse;
        fault.message = "nil em escrita de campo";
        return false;
    }
    if (isNullEntity(e)) {
        fault.kind = NiFault::Kind::EntityNull;
        fault.message = "escrita em entidade nula";
        return false;
    }
    if (self->valid != nullptr && !self->valid(self->user, e)) {
        fault.kind = NiFault::Kind::EntityStale;
        fault.message = "entidade obsoleta (geração não confere — ADR-024)";
        return false;
    }
    void* component = self->fetchMutable(self->user, e);
    if (component == nullptr) {
        fault.kind = NiFault::Kind::ComponentMissing;
        fault.message = "componente ausente na entidade";
        return false;
    }
    Loc loc;
    if (!walk(self->info, 0, joinPath(self->basePath, fieldPath), loc,
              fault)) {
        return false;
    }
    return writeValue(component, loc, value, fault);
}

} // namespace

bool niAddReflectionBinding(NiBindingTable& table,
                            std::string_view alias,
                            std::string_view typeName,
                            const void* (*fetchConst)(void*,
                                                     eng::ecs::Entity),
                            void* (*fetchMutable)(void*, eng::ecs::Entity),
                            void* user, std::string_view basePath,
                            bool (*valid)(void*, eng::ecs::Entity))
{
    const eng::reflect::TypeInfo* info =
        eng::reflect::TypeRegistry::global().find(typeName);
    if (info == nullptr) {
        return false;
    }
    auto adapter = std::make_shared<ReflAdapter>();
    adapter->info = info;
    adapter->basePath = std::string(basePath);
    adapter->fetchConst = fetchConst;
    adapter->fetchMutable = fetchMutable;
    adapter->valid = valid;
    adapter->user = user;

    NiComponentBinding binding;
    binding.alias = std::string(alias);
    binding.keepAlive = adapter; // posse: a entrada mantém o adapter vivo
    binding.user = adapter.get();
    binding.get = &reflGet;
    binding.set = &reflSet;
    table.add(std::move(binding));
    return true;
}

} // namespace eng::ni
