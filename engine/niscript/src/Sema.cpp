/// Sema do NI-Script — símbolos + tipos estáticos.
///
/// Duas passadas: (1) coleta de topo (globais/funcs/handlers/módulos +
/// colisões); (2) corpos com escopos léxicos, inferência e TODAS as
/// checagens estáticas do design. Resultado alimenta o Compiler.
///
/// Regras centrais (design §3.1): tipo de `var` é FIXO na declaração;
/// Dynamic (leituras de campo de entidade / resultados não tipáveis) se
/// propaga e é checado em RUNTIME pelas instruções — o estático falha só
/// quando AMBOS os operandos são conhecidos e incompatíveis.

#include "NiAst.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng::ni {
namespace {

bool isNumeric(NiType t) noexcept
{
    return t == NiType::Int || t == NiType::Float;
}

bool isVec(NiType t) noexcept
{
    return t == NiType::Vec2 || t == NiType::Vec3;
}

/// Nome de campo válido de vec2/vec3/color/transform (design §3.1) e o
/// tipo resultante; Dynamic = não é campo estrutural (entity/dynamic —
/// resolvido em runtime).
[[nodiscard]] bool structField(NiType base, const std::string& field,
                               NiType& out)
{
    switch (base) {
    case NiType::Vec2:
        if (field == "x" || field == "y") {
            out = NiType::Float;
            return true;
        }
        return false;
    case NiType::Vec3:
        if (field == "x" || field == "y" || field == "z") {
            out = NiType::Float;
            return true;
        }
        return false;
    case NiType::Color:
        if (field == "r" || field == "g" || field == "b"
            || field == "a") {
            out = NiType::Float;
            return true;
        }
        return false;
    case NiType::Transform:
        if (field == "position" || field == "rotation" || field == "scale") {
            out = NiType::Vec3;
            return true;
        }
        return false;
    default: return false;
    }
}

class Analyzer final {
public:
    Analyzer(const NiNativeTable& natives, std::vector<NiDiag>& diags)
        : natives_(natives), diags_(diags)
    {
    }

    [[nodiscard]] SemaResult run(const std::vector<TopLevel>& top)
    {
        collectTop(top);
        if (ok_) {
            walkTop(top);
        }
        result_.ok = ok_ && diags_.empty();
        return std::move(result_);
    }

private:
    const NiNativeTable& natives_;
    std::vector<NiDiag>& diags_;
    SemaResult result_;
    bool ok_ = true;

    struct Scope {
        std::unordered_map<std::string, std::pair<std::uint16_t, NiType>>
            names; ///< name → {slot, tipo}
    };
    std::vector<Scope> scopes_;
    bool inFunction_ = false; ///< `give` permitido
    std::uint16_t nextSlot_ = 0;
    std::uint16_t maxSlots_ = 0;
    const void* currentFuncKey_ = nullptr;

    // mapa globals por nome (slot = índice no vector de result_)
    std::unordered_map<std::string, std::uint16_t> globalSlots_;
    std::unordered_map<std::string, SemaResult::FuncSig> funcSigs_;
    bool importedBL_ = false;

    void error(std::uint32_t line, std::uint32_t col, std::string message)
    {
        ok_ = false;
        NiDiag d;
        d.line = line;
        d.col = col;
        d.message = std::move(message);
        diags_.push_back(std::move(d));
    }

    void errorAt(const Expr* e, std::string message)
    {
        error(e->line, e->col, std::move(message));
    }

    void errorAt(const Stmt* s, std::string message)
    {
        error(s->line, s->col, std::move(message));
    }

    void errorAt(const TopLevel& t, std::string message)
    {
        error(t.line, t.col, std::move(message));
    }

    // --- passada 1: coleta de topo -------------------------------------------

    void collectTop(const std::vector<TopLevel>& top)
    {
        std::uint32_t funcIndex = 0;
        for (const TopLevel& item : top) {
            switch (item.kind) {
            case TopLevel::Kind::AddModule:
                if (item.name != "BL") {
                    errorAt(item, "módulo desconhecido: &" + item.name
                                  + " (disponível: BL)");
                    break;
                }
                if (!importedBL_) {
                    importedBL_ = true;
                    result_.modules.push_back(item.name);
                }
                break;
            case TopLevel::Kind::Global: {
                if (item.init == nullptr && !item.varType.has_value()) {
                    errorAt(item, "var global '" + item.name
                                  + "' exige tipo ou inicializador");
                    break;
                }
                if (globalSlots_.count(item.name) != 0
                    || funcSigs_.count(item.name) != 0) {
                    errorAt(item, "redeclaração de '" + item.name + "'");
                    break;
                }
                const NiType type =
                    item.varType.value_or(NiType::Dynamic);
                globalSlots_[item.name] =
                    static_cast<std::uint16_t>(result_.globals.size());
                result_.globals.emplace_back(item.name, type);
                break;
            }
            case TopLevel::Kind::Func: {
                if (funcSigs_.count(item.name) != 0
                    || globalSlots_.count(item.name) != 0) {
                    errorAt(item, "redeclaração de '" + item.name + "'");
                    break;
                }
                SemaResult::FuncSig sig;
                sig.funcIndex = funcIndex++;
                for (const auto& [pname, ptype] : item.params) {
                    (void)pname;
                    sig.paramTypes.push_back(
                        ptype.value_or(NiType::Dynamic));
                }
                funcSigs_[item.name] = sig;
                result_.funcs[item.name] = sig;
                break;
            }
            case TopLevel::Kind::Handler: {
                for (const auto& [ev, idx] : result_.handlers) {
                    (void)idx;
                    if (ev == item.name) {
                        errorAt(item, "handler duplicado para o evento '"
                                          + item.name + "'");
                        break;
                    }
                }
                result_.handlers.emplace_back(
                    item.name, static_cast<std::uint32_t>(funcIndex++));
                break;
            }
            }
        }
    }

    // --- passada 2: corpos ------------------------------------------------------

    void walkTop(const std::vector<TopLevel>& top)
    {
        // globais visíveis como escopo base (inits em ordem de declaração —
        // um init vê os anteriores; design §4)
        for (const TopLevel& item : top) {
            switch (item.kind) {
            case TopLevel::Kind::Global:
                if (item.init != nullptr) {
                    const NiType type = walkExpr(item.init);
                    const NiType declared =
                        item.varType.value_or(type);
                    checkAssignCompatible(declared, type, item.init,
                                         "inicializador do global '"
                                             + item.name + "'");
                }
                break;
            case TopLevel::Kind::Func:
            case TopLevel::Kind::Handler: {
                // corpo = função própria
                scopes_.clear();
                nextSlot_ = 0;
                maxSlots_ = 0;
                inFunction_ = item.kind == TopLevel::Kind::Func;
                currentFuncKey_ = &item;
                pushScope();
                if (item.kind == TopLevel::Kind::Func) {
                    for (std::size_t p = 0; p < item.params.size(); ++p) {
                        const auto& [pname, ptype] = item.params[p];
                        declareLocal(pname, ptype.value_or(NiType::Dynamic),
                                     item.line, item.col);
                    }
                }
                if (item.body != nullptr) {
                    walkBlock(item.body);
                }
                popScope();
                result_.localCount[currentFuncKey_] = maxSlots_;
                break;
            }
            case TopLevel::Kind::AddModule: break;
            }
        }
    }

    void pushScope() { scopes_.push_back({}); }
    void popScope() { scopes_.pop_back(); }

    void declareLocal(const std::string& name, NiType type, std::uint32_t line,
                      std::uint32_t col)
    {
        if (scopes_.empty()) {
            error(line, col, "interno: escopo ausente");
            return;
        }
        Scope& scope = scopes_.back();
        if (scope.names.count(name) != 0) {
            error(line, col, "redeclaração de '" + name + "' no mesmo bloco");
            return;
        }
        if (nextSlot_ >= 0xFFFF) {
            error(line, col, "muitas variáveis locais (limite 65535)");
            return;
        }
        scope.names[name] = {nextSlot_++, type};
        if (nextSlot_ > maxSlots_) {
            maxSlots_ = nextSlot_;
        }
    }

    /// Resolve um nome: escopos (dentro→fora) → globais.
    /// Devolve {slot, tipo, isLocal}.
    struct NameRef {
        std::uint16_t slot = 0;
        NiType type = NiType::Dynamic;
        bool local = false;
        bool found = false;
    };
    [[nodiscard]] NameRef resolveName(const std::string& name) const
    {
        NameRef ref;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->names.find(name);
            if (found != it->names.end()) {
                ref.slot = found->second.first;
                ref.type = found->second.second;
                ref.local = true;
                ref.found = true;
                return ref;
            }
        }
        const auto g = globalSlots_.find(name);
        if (g != globalSlots_.end()) {
            ref.slot = g->second;
            ref.type = result_.globals[g->second].second;
            ref.found = true;
            return ref;
        }
        return ref;
    }

    void walkBlock(const Block* block)
    {
        pushScope();
        for (const Stmt* s : block->stmts) {
            walkStmt(s);
        }
        popScope();
    }

    void walkStmt(const Stmt* s)
    {
        switch (s->kind) {
        case Stmt::Kind::Var: {
            if (s->init == nullptr && !s->varType.has_value()) {
                errorAt(s, "var '" + s->varName
                               + "' exige tipo ou inicializador");
                return;
            }
            NiType type = NiType::Dynamic;
            if (s->init != nullptr) {
                type = walkExpr(s->init);
            }
            if (s->varType.has_value()) {
                checkAssignCompatible(s->varType.value(), type, s->init,
                                      "inicializador de '" + s->varName + "'");
                type = s->varType.value();
            }
            declareLocal(s->varName, type, s->line, s->col);
            result_.varSlots[s] = nextSlot_ - 1;
            return;
        }
        case Stmt::Kind::If: {
            const NiType t = walkExpr(s->cond);
            if (t != NiType::Bool && t != NiType::Dynamic) {
                errorAt(s, "condição de if deve ser bool (obtido: "
                               + std::string(niTypeName(t)) + ")");
            }
            walkBlock(s->body);
            if (s->elseBody != nullptr) {
                walkBlock(s->elseBody);
            }
            return;
        }
        case Stmt::Kind::Repeat: {
            const NiType t = walkExpr(s->cond);
            if (t != NiType::Int && t != NiType::Dynamic) {
                errorAt(s, "contagem de repeat deve ser int (obtido: "
                               + std::string(niTypeName(t)) + ")");
            }
            if (s->cond->kind == Expr::Kind::Int
                && (s->cond->i < 0 || s->cond->i > kRepeatMax)) {
                errorAt(s, "contagem de repeat fora de [0, "
                               + std::to_string(kRepeatMax)
                               + "] (design §5.2)");
            }
            walkBlock(s->body);
            return;
        }
        case Stmt::Kind::Repair: {
            walkBlock(s->body);
            return;
        }
        case Stmt::Kind::Timeout: {
            const NiType t = walkExpr(s->cond);
            if (t != NiType::Int && t != NiType::Dynamic) {
                errorAt(s, "orçamento de timeout deve ser int (obtido: "
                               + std::string(niTypeName(t)) + ")");
            }
            if (s->cond->kind == Expr::Kind::Int && s->cond->i < 1) {
                errorAt(s, "orçamento de timeout deve ser >= 1 (design §5.4)");
            }
            walkBlock(s->body);
            return;
        }
        case Stmt::Kind::Link: {
            const NiType t = walkExpr(s->target); // tipo checado abaixo
            if (t != NiType::Entity && t != NiType::Dynamic) {
                errorAt(s, "link to exige entity (obtido: "
                               + std::string(niTypeName(t)) + ")");
            }
            return;
        }
        case Stmt::Kind::Emit: return;
        case Stmt::Kind::Give: {
            if (!inFunction_) {
                errorAt(s, "'give' só é permitido dentro de 'f'");
                return;
            }
            if (s->target != nullptr) {
                (void)walkExpr(s->target); // anotação apenas
            }
            return;
        }
        case Stmt::Kind::Assign: {
            walkAssign(s);
            return;
        }
        case Stmt::Kind::Expr: {
            (void)walkExpr(s->value); // anotação + checagens de efeito
            return;
        }
        }
    }

    void checkAssignCompatible(NiType declared, NiType rhs, const Expr* at,
                               const std::string& what)
    {
        if (rhs == NiType::Dynamic || declared == NiType::Dynamic) {
            return; // runtime decide (CHECK_TYPE/uso)
        }
        if (declared != rhs) {
            errorAt(at, what + ": tipo " + std::string(niTypeName(rhs))
                             + " não é compatível com "
                             + std::string(niTypeName(declared)));
        }
    }

    void walkAssign(const Stmt* s)
    {
        // RHS primeiro (avaliação completa antes da escrita — design §5.3.3)
        const NiType rhs = walkExpr(s->value);

        // lvalue: cadeia de campos a partir de Ident
        std::vector<const Expr*> chain;
        const Expr* root = s->lvalue;
        while (root->kind == Expr::Kind::Member) {
            chain.push_back(root);
            root = root->base;
        }
        if (root->kind != Expr::Kind::Ident) {
            errorAt(s->lvalue, "alvo de atribuição inválido");
            return;
        }
        std::reverse(chain.begin(), chain.end());
        // campos em ordem (chain[i]->s)

        if (chain.empty()) {
            // atribuição simples
            const NameRef ref = resolveName(root->s);
            if (!ref.found) {
                errorAt(root, "variável desconhecida: " + root->s);
                return;
            }
            SemaResult::IdentRef ir;
            ir.kind = ref.local ? SemaResult::IdentRef::Kind::Local
                                : SemaResult::IdentRef::Kind::Global;
            ir.slot = ref.slot;
            ir.type = ref.type;
            result_.identRefs[root] = ir;
            if (ref.type != NiType::Dynamic && rhs != NiType::Dynamic
                && ref.type != rhs) {
                errorAt(s->value,
                        "atribuição a '" + root->s + "': tipo "
                            + std::string(niTypeName(rhs))
                            + " não é compatível com "
                            + std::string(niTypeName(ref.type)));
            }
            return;
        }

        const NameRef ref = resolveName(root->s);
        if (!ref.found) {
            errorAt(root, "variável desconhecida: " + root->s);
            return;
        }
        SemaResult::IdentRef ir;
        ir.kind = ref.local ? SemaResult::IdentRef::Kind::Local
                            : SemaResult::IdentRef::Kind::Global;
        ir.slot = ref.slot;
        ir.type = ref.type;
        result_.identRefs[root] = ir;

        NiType base = ref.type;
        // caminho: cadeia de campos sobre `base`
        if (base == NiType::Entity || base == NiType::Dynamic
            || base == NiType::CompView) {
            // caminho dinâmico — resolução em runtime; escrita aninhada em
            // raiz DINÂMICA é restrita (docs/ni-script/02 §escrita)
            if (base == NiType::Dynamic && chain.size() > 1) {
                errorAt(s->lvalue,
                        "atribuição aninhada em variável dinâmica: anote a "
                        "variável (ex.: var p: vec3 = e.position)");
                return;
            }
            return;
        }

        // cadeia estrutural estática: valida campo a campo
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const std::string& field = chain[i]->s;
            NiType fieldType = NiType::Dynamic;
            if (!structField(base, field, fieldType)) {
                errorAt(chain[i],
                        "campo desconhecido '" + field + "' em "
                            + std::string(niTypeName(base)));
                return;
            }
            if (i + 1 == chain.size()) {
                // último campo: compatibilidade com RHS
                if (fieldType != NiType::Dynamic && rhs != NiType::Dynamic
                    && fieldType != rhs) {
                    errorAt(s->value,
                            "atribuição a '" + field + "': tipo "
                                + std::string(niTypeName(rhs))
                                + " não é compatível com "
                                + std::string(niTypeName(fieldType)));
                }
            }
            base = fieldType;
        }
    }

    // --- expressões ------------------------------------------------------------

    [[nodiscard]] NiType walkExpr(const Expr* e)
    {
        const NiType t = walkExprInner(e);
        result_.exprTypes[e] = t;
        return t;
    }

    [[nodiscard]] NiType walkExprInner(const Expr* e)
    {
        switch (e->kind) {
        case Expr::Kind::Int: return NiType::Int;
        case Expr::Kind::Float: return NiType::Float;
        case Expr::Kind::Bool: return NiType::Bool;
        case Expr::Kind::Str: return NiType::String;
        case Expr::Kind::Ident: {
            const NameRef ref = resolveName(e->s);
            if (!ref.found) {
                if (funcSigs_.count(e->s) != 0) {
                    errorAt(e, "função '" + e->s
                                   + "' não é um valor (chame com '()')");
                    return NiType::Dynamic;
                }
                errorAt(e, "nome desconhecido: " + e->s);
                return NiType::Dynamic;
            }
            SemaResult::IdentRef ir;
            ir.kind = ref.local ? SemaResult::IdentRef::Kind::Local
                                : SemaResult::IdentRef::Kind::Global;
            ir.slot = ref.slot;
            ir.type = ref.type;
            result_.identRefs[e] = ir;
            return ref.type;
        }
        case Expr::Kind::Call: return walkCall(e);
        case Expr::Kind::Unary: {
            const NiType a = walkExpr(e->base);
            if (e->unOp == UnOp::Neg) {
                if (isNumeric(a) || a == NiType::Dynamic) {
                    return a;
                }
                errorAt(e, "unário '-' exige numérico (obtido: "
                               + std::string(niTypeName(a)) + ")");
                return NiType::Dynamic;
            }
            // Not
            if (a == NiType::Bool || a == NiType::Dynamic) {
                return NiType::Bool;
            }
            errorAt(e, "'not' exige bool (obtido: "
                           + std::string(niTypeName(a)) + ")");
            return NiType::Dynamic;
        }
        case Expr::Kind::Binary: return walkBinary(e);
        case Expr::Kind::Member: {
            const NiType base = walkExpr(e->base);
            if (base == NiType::Entity || base == NiType::Dynamic
                || base == NiType::CompView) {
                return NiType::Dynamic; // resolução runtime (bindings)
            }
            NiType out = NiType::Dynamic;
            if (!structField(base, e->s, out)) {
                errorAt(e, "campo desconhecido '" + e->s + "' em "
                               + std::string(niTypeName(base)));
            }
            return out;
        }
        }
        return NiType::Dynamic;
    }

    [[nodiscard]] NiType walkCall(const Expr* e)
    {
        // resolução: escopos → funcs → nativos (design: funcs sombreiam)
        const NameRef local = resolveName(e->s);
        if (local.found) {
            errorAt(e, "'" + e->s + "' não é chamável (variável)");
            for (const Expr* arg : e->args) {
                (void)walkExpr(arg); // anotação mesmo em erro
            }
            return NiType::Dynamic;
        }
        const auto func = funcSigs_.find(e->s);
        if (func != funcSigs_.end()) {
            if (func->second.paramTypes.size() != e->args.size()) {
                errorAt(e, "função '" + e->s + "' espera "
                               + std::to_string(func->second.paramTypes.size())
                               + " argumento(s), recebidos "
                               + std::to_string(e->args.size()));
            }
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                const NiType at = walkExpr(e->args[i]);
                if (i < func->second.paramTypes.size()) {
                    checkAssignCompatible(func->second.paramTypes[i], at,
                                          e->args[i],
                                          "argumento " + std::to_string(i + 1)
                                              + " de '" + e->s + "'");
                }
            }
            SemaResult::IdentRef ir;
            ir.kind = SemaResult::IdentRef::Kind::Func;
            ir.index = func->second.funcIndex;
            result_.identRefs[e] = ir;
            return NiType::Dynamic; // retorno não declarado — runtime
        }
        const NiNativeEntry* native = natives_.find(e->s);
        if (native != nullptr) {
            if (native->moduleBL && !importedBL_) {
                errorAt(e, "nativo '" + e->s
                               + "' requer 'add &BL' no topo do script");
            }
            const std::uint16_t maxArity =
                native->maxArity == 0 ? native->arity : native->maxArity;
            const std::size_t argc = e->args.size();
            if (argc < native->arity || argc > maxArity) {
                errorAt(e, "nativo '" + e->s + "' espera "
                               + std::to_string(native->arity)
                               + (maxArity == native->arity
                                      ? std::string(" argumento(s)")
                                      : ".." + std::to_string(maxArity)
                                            + " argumento(s)")
                               + ", recebidos " + std::to_string(argc));
            }
            for (const Expr* arg : e->args) {
                (void)walkExpr(arg); // anotação + checagens do argumento
            }
            SemaResult::IdentRef ir;
            ir.kind = SemaResult::IdentRef::Kind::Native;
            ir.index = static_cast<std::uint32_t>(natives_.indexOf(e->s));
            result_.identRefs[e] = ir;
            return native->resultType;
        }
        errorAt(e, "nome desconhecido: " + e->s);
        for (const Expr* arg : e->args) {
            (void)walkExpr(arg); // anotação mesmo em erro
        }
        return NiType::Dynamic;
    }

    [[nodiscard]] NiType walkBinary(const Expr* e)
    {
        const NiType l = walkExpr(e->left);
        const NiType r = walkExpr(e->right);
        const bool dyn = l == NiType::Dynamic || r == NiType::Dynamic;

        switch (e->binOp) {
        case BinOp::Eq: case BinOp::Ne: return NiType::Bool;
        case BinOp::And: case BinOp::Or: {
            if (!dyn) {
                if (l != NiType::Bool || r != NiType::Bool) {
                    errorAt(e, "and/or exige bool (obtido: "
                                   + std::string(niTypeName(l)) + " e "
                                   + std::string(niTypeName(r)) + ")");
                }
            }
            return NiType::Bool;
        }
        case BinOp::Lt: case BinOp::Le: case BinOp::Gt: case BinOp::Ge: {
            if (!dyn && (!isNumeric(l) || !isNumeric(r))) {
                errorAt(e, "comparação exige numéricos (obtido: "
                               + std::string(niTypeName(l)) + " e "
                               + std::string(niTypeName(r)) + ")");
            }
            return NiType::Bool;
        }
        case BinOp::Add: {
            if (dyn) {
                return NiType::Dynamic;
            }
            if (l == NiType::String && r == NiType::String) {
                return NiType::String;
            }
            if (isNumeric(l) && isNumeric(r)) {
                return (l == NiType::Float || r == NiType::Float)
                           ? NiType::Float
                           : NiType::Int;
            }
            if (isVec(l) && l == r) {
                return l;
            }
            errorAt(e, "'+' inválido entre "
                           + std::string(niTypeName(l)) + " e "
                           + std::string(niTypeName(r)));
            return NiType::Dynamic;
        }
        case BinOp::Sub: {
            if (dyn) {
                return NiType::Dynamic;
            }
            if (isNumeric(l) && isNumeric(r)) {
                return (l == NiType::Float || r == NiType::Float)
                           ? NiType::Float
                           : NiType::Int;
            }
            if (isVec(l) && l == r) {
                return l;
            }
            errorAt(e, "'-' inválido entre "
                           + std::string(niTypeName(l)) + " e "
                           + std::string(niTypeName(r)));
            return NiType::Dynamic;
        }
        case BinOp::Mul: {
            if (dyn) {
                return NiType::Dynamic;
            }
            if (isNumeric(l) && isNumeric(r)) {
                return (l == NiType::Float || r == NiType::Float)
                           ? NiType::Float
                           : NiType::Int;
            }
            if (isVec(l) && isNumeric(r)) {
                return l;
            }
            if (isNumeric(l) && isVec(r)) {
                return r;
            }
            errorAt(e, "'*' inválido entre "
                           + std::string(niTypeName(l)) + " e "
                           + std::string(niTypeName(r)));
            return NiType::Dynamic;
        }
        case BinOp::Div: {
            if (dyn) {
                return NiType::Dynamic;
            }
            if (isNumeric(l) && isNumeric(r)) {
                return (l == NiType::Float || r == NiType::Float)
                           ? NiType::Float
                           : NiType::Int;
            }
            errorAt(e, "'/' exige numéricos (obtido: "
                           + std::string(niTypeName(l)) + " e "
                           + std::string(niTypeName(r)) + ")");
            return NiType::Dynamic;
        }
        case BinOp::Mod: {
            if (dyn) {
                return NiType::Dynamic;
            }
            if (l == NiType::Int && r == NiType::Int) {
                return NiType::Int;
            }
            errorAt(e, "'%' exige int (obtido: "
                           + std::string(niTypeName(l)) + " e "
                           + std::string(niTypeName(r)) + ")");
            return NiType::Dynamic;
        }
        }
        return NiType::Dynamic;
    }
};

} // namespace

SemaResult analyze(const std::vector<TopLevel>& top,
                    const NiNativeTable& natives, std::vector<NiDiag>& diags)
{
    Analyzer analyzer(natives, diags);
    return analyzer.run(top);
}

} // namespace eng::ni
