/// Parser do NI-Script — recursivo descendente.
///
/// Blocos terminam por `stop` (casamento ESTRUTURAL, sem semântica de
/// indentação — design §2). Fronteira de statement: tokens de início de
/// statement (`var/if/repeat/...`) não podem continuar expressão, então a
/// gramática é não-ambígua com parse guloso de expressões.
///
/// Puro; erros como NiDiag com linha/coluna; nunca throw.
/// Recuperação: erro estrutural REPORTA e o parser segue do próximo token
/// (diagnósticos múltiplos por passada) — com avanço garantido.

#include "NiAst.hpp"

#include <cstdint>
#include <string>

namespace eng::ni {
namespace {

class Parser final {
public:
    Parser(const std::vector<Token>& tokens, std::vector<NiDiag>& diags)
        : tokens_(tokens), diags_(diags)
    {
    }

    [[nodiscard]] ParseResult run()
    {
        ParseResult result;
        while (!check(TokKind::End)) {
            if (!parseTop(result.top)) {
                break;
            }
        }
        result.ok = diags_.empty();
        // POSSE sai do parser: os ponteiros crus do AST continuam válidos
        result.arena.blocks = std::move(blocks_);
        result.arena.stmts = std::move(stmts_);
        result.arena.exprs = std::move(exprs_);
        return result;
    }

private:
    const std::vector<Token>& tokens_;
    std::vector<NiDiag>& diags_;
    std::size_t pos_ = 0;
    std::size_t lastProgress_ = 0;

    // --- navegação -----------------------------------------------------------

    [[nodiscard]] const Token& peek() const noexcept { return tokens_[pos_]; }

    [[nodiscard]] bool check(TokKind kind) const noexcept
    {
        return peek().kind == kind;
    }

    const Token& advance() noexcept
    {
        lastProgress_ = pos_;
        const Token& t = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) {
            ++pos_;
        }
        return t;
    }

    [[nodiscard]] bool accept(TokKind kind) noexcept
    {
        if (check(kind)) {
            advance();
            return true;
        }
        return false;
    }

    void error(std::string message)
    {
        NiDiag d;
        d.line = peek().line;
        d.col = peek().col;
        d.message = std::move(message);
        diags_.push_back(std::move(d));
        // avanço garantido: nunca ficar preso no mesmo token
        if (pos_ == lastProgress_) {
            advance();
        }
    }

    [[nodiscard]] bool expect(TokKind kind, const char* message)
    {
        if (accept(kind)) {
            return true;
        }
        error(message);
        return false;
    }

    [[nodiscard]] bool expectIdent(std::string& out, const char* message)
    {
        if (check(TokKind::Ident)) {
            out = advance().text;
            return true;
        }
        error(message);
        return false;
    }

    // --- topo ----------------------------------------------------------------

    [[nodiscard]] bool parseTop(std::vector<TopLevel>& top)
    {
        const Token& t = peek();
        switch (t.kind) {
        case TokKind::KwAdd: {
            advance();
            if (!expect(TokKind::Amp, "esperado '&' após 'add'")) {
                return false;
            }
            TopLevel item;
            item.kind = TopLevel::Kind::AddModule;
            item.line = t.line;
            item.col = t.col;
            if (!expectIdent(item.name, "esperado nome do módulo")) {
                return false;
            }
            top.push_back(std::move(item));
            return true;
        }
        case TokKind::KwF: {
            advance();
            TopLevel item;
            item.kind = TopLevel::Kind::Func;
            item.line = t.line;
            item.col = t.col;
            if (!expectIdent(item.name, "esperado nome da função")) {
                return false;
            }
            if (!expect(TokKind::LParen, "esperado '(' após nome")) {
                return false;
            }
            if (!accept(TokKind::RParen)) {
                for (;;) {
                    std::string pname;
                    if (!expectIdent(pname, "esperado nome do parâmetro")) {
                        return false;
                    }
                    std::optional<NiType> ptype;
                    if (accept(TokKind::Colon)) {
                        NiType type = NiType::Dynamic;
                        if (!parseType(type)) {
                            return false;
                        }
                        ptype = type;
                    }
                    item.params.emplace_back(pname, ptype);
                    if (accept(TokKind::Comma)) {
                        continue;
                    }
                    break;
                }
                if (!expect(TokKind::RParen, "esperado ')' após parâmetros")) {
                    return false;
                }
            }
            if (!expect(TokKind::Colon, "esperado ':' antes do corpo")) {
                return false;
            }
            item.body = parseBlock();
            top.push_back(std::move(item));
            return true;
        }
        case TokKind::KwUp: {
            advance();
            TopLevel item;
            item.kind = TopLevel::Kind::Handler;
            item.line = t.line;
            item.col = t.col;
            if (!expectIdent(item.name, "esperado nome do evento")) {
                return false;
            }
            if (!expect(TokKind::Colon, "esperado ':' antes do corpo")) {
                return false;
            }
            item.body = parseBlock();
            top.push_back(std::move(item));
            return true;
        }
        case TokKind::KwVar: {
            TopLevel item;
            item.kind = TopLevel::Kind::Global;
            if (!parseVarDecl(item)) {
                return false;
            }
            top.push_back(std::move(item));
            return true;
        }
        default:
            error("esperado 'add', 'f', 'up' ou 'var' no topo do script");
            return false;
        }
    }

    /// varDecl := "var" IDENT [":" type] ["=" expr]
    /// (a exigência de tipo OU inicializador é do SEMA — aqui só sintaxe).
    [[nodiscard]] bool parseVarDecl(TopLevel& item)
    {
        const Token& kw = peek();
        if (!expect(TokKind::KwVar, "interno: 'var'")) {
            return false;
        }
        item.line = kw.line;
        item.col = kw.col;
        if (!expectIdent(item.name, "esperado nome da variável")) {
            return false;
        }
        if (accept(TokKind::Colon)) {
            NiType type = NiType::Dynamic;
            if (!parseType(type)) {
                return false;
            }
            item.varType = type;
        }
        if (accept(TokKind::Assign)) {
            Expr* init = parseExpr();
            if (init == nullptr) {
                return false;
            }
            item.init = init;
        }
        return true;
    }

    [[nodiscard]] bool parseType(NiType& out)
    {
        const Token& t = peek();
        if (t.kind != TokKind::Ident) {
            error("esperado nome de tipo (int/float/bool/string/vec2/vec3/"
                  "color/entity/asset/transform)");
            return false;
        }
        if (!eng::ni::typeKeywordOf(t.text, out)) {
            error("tipo desconhecido: '" + t.text + "'");
            return false;
        }
        advance();
        return true;
    }

    // --- statements ----------------------------------------------------------

    [[nodiscard]] Block* parseBlock()
    {
        blocks_.push_back(std::make_unique<Block>());
        Block* block = blocks_.back().get();
        for (;;) {
            if (check(TokKind::End)) {
                error("esperado 'stop' para fechar o bloco (fim da fonte)");
                return block;
            }
            if (check(TokKind::KwStop)) {
                advance();
                return block;
            }
            if (check(TokKind::KwElse)) {
                error("'else' sem 'if' correspondente");
                return block;
            }
            if (!parseStmt(block)) {
                return block;
            }
        }
    }

    [[nodiscard]] bool parseStmt(Block* block)
    {
        const Token& t = peek();
        switch (t.kind) {
        case TokKind::KwVar: {
            Stmt* s = newStmt(Stmt::Kind::Var, t);
            // reuso da estrutura: nome/tipo/init
            TopLevel tmp;
            if (!parseVarDecl(tmp)) {
                return false;
            }
            s->varName = std::move(tmp.name);
            s->varType = tmp.varType;
            s->init = tmp.init;
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwIf: {
            advance();
            Stmt* s = newStmt(Stmt::Kind::If, t);
            s->cond = parseExpr();
            if (s->cond == nullptr) {
                return false;
            }
            if (!expect(TokKind::Colon, "esperado ':' após condição")) {
                return false;
            }
            s->body = parseBlock();
            if (accept(TokKind::KwElse)) {
                if (!expect(TokKind::Colon, "esperado ':' após 'else'")) {
                    return false;
                }
                s->elseBody = parseBlock();
            }
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwRepeat: {
            advance();
            Stmt* s = newStmt(Stmt::Kind::Repeat, t);
            s->cond = parseExpr();
            if (s->cond == nullptr) {
                return false;
            }
            if (!expect(TokKind::Colon, "esperado ':' após contagem")) {
                return false;
            }
            s->body = parseBlock();
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwRepair: {
            advance();
            if (!expect(TokKind::Colon, "esperado ':' após 'repair'")) {
                return false;
            }
            Stmt* s = newStmt(Stmt::Kind::Repair, t);
            s->body = parseBlock();
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwTimeout: {
            advance();
            Stmt* s = newStmt(Stmt::Kind::Timeout, t);
            s->cond = parseExpr();
            if (s->cond == nullptr) {
                return false;
            }
            if (!expect(TokKind::Colon, "esperado ':' após orçamento")) {
                return false;
            }
            s->body = parseBlock();
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwLink: {
            advance();
            if (!expect(TokKind::KwTo, "esperado 'to' após 'link'")) {
                return false;
            }
            Stmt* s = newStmt(Stmt::Kind::Link, t);
            s->target = parseExpr();
            if (s->target == nullptr) {
                return false;
            }
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwEmit: {
            advance();
            Stmt* s = newStmt(Stmt::Kind::Emit, t);
            if (!expectIdent(s->eventName, "esperado nome do evento")) {
                return false;
            }
            block->stmts.push_back(s);
            return true;
        }
        case TokKind::KwGive: {
            advance();
            Stmt* s = newStmt(Stmt::Kind::Give, t);
            if (canStartExpression()) {
                s->target = parseExpr();
                if (s->target == nullptr) {
                    return false;
                }
            }
            block->stmts.push_back(s);
            return true;
        }
        default: break;
        }

        // atribuição ou expressão
        Expr* e = parseExpr();
        if (e == nullptr) {
            return false;
        }
        // `=` e os compostos += -= *= /= (desugar no parse).
        const bool isAssign = check(TokKind::Assign) ||
                              check(TokKind::PlusAssign) ||
                              check(TokKind::MinusAssign) ||
                              check(TokKind::StarAssign) ||
                              check(TokKind::SlashAssign);
        if (isAssign) {
            if (e->kind != Expr::Kind::Ident
                && e->kind != Expr::Kind::Member) {
                error("alvo de atribuição inválido (esperado variável ou "
                      "campo)");
                return false;
            }
            const TokKind op = peek().kind;
            advance();
            Stmt* s = newStmt(Stmt::Kind::Assign, t);
            s->lvalue = e;
            if (op == TokKind::Assign) {
                s->value = parseExpr();
            } else {
                // Desugar `x op= v` → `x = x op v`. O nó do alvo é REUSADO
                // como operando esquerdo (arena do parser — endereços
                // estáveis, sem dupla posse); o Compiler lê o lvalue duas
                // vezes (load + store), exatamente a semântica esperada.
                Expr* bin = newExpr(Expr::Kind::Binary, t);
                bin->binOp = op == TokKind::PlusAssign ? BinOp::Add
                           : op == TokKind::MinusAssign ? BinOp::Sub
                           : op == TokKind::StarAssign ? BinOp::Mul
                                                       : BinOp::Div;
                bin->left = e;
                bin->right = parseExpr();
                s->value = bin;
            }
            if (s->value == nullptr) {
                return false;
            }
            block->stmts.push_back(s);
            return true;
        }
        Stmt* s = newStmt(Stmt::Kind::Expr, t);
        s->value = e;
        block->stmts.push_back(s);
        return true;
    }

    [[nodiscard]] bool canStartExpression() const noexcept
    {
        switch (peek().kind) {
        case TokKind::Int: case TokKind::Float: case TokKind::Str:
        case TokKind::Ident: case TokKind::KwTrue: case TokKind::KwFalse:
        case TokKind::LParen: case TokKind::Minus: case TokKind::KwNot:
            return true;
        default: return false;
        }
    }

    // --- expressões ------------------------------------------------------------

    [[nodiscard]] Expr* newExpr(Expr::Kind kind, const Token& at)
    {
        exprs_.push_back(std::make_unique<Expr>());
        Expr* e = exprs_.back().get();
        e->kind = kind;
        e->line = at.line;
        e->col = at.col;
        return e;
    }

    [[nodiscard]] Stmt* newStmt(Stmt::Kind kind, const Token& at)
    {
        stmts_.push_back(std::make_unique<Stmt>());
        Stmt* s = stmts_.back().get();
        s->kind = kind;
        s->line = at.line;
        s->col = at.col;
        return s;
    }

    [[nodiscard]] Expr* parseExpr() { return parseOr(); }

    [[nodiscard]] Expr* parseOr()
    {
        Expr* left = parseAnd();
        if (left == nullptr) {
            return nullptr;
        }
        while (check(TokKind::KwOr)) {
            const Token& at = advance();
            Expr* right = parseAnd();
            if (right == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Binary, at);
            e->binOp = BinOp::Or;
            e->left = left;
            e->right = right;
            left = e;
        }
        return left;
    }

    [[nodiscard]] Expr* parseAnd()
    {
        Expr* left = parseNot();
        if (left == nullptr) {
            return nullptr;
        }
        while (check(TokKind::KwAnd)) {
            const Token& at = advance();
            Expr* right = parseNot();
            if (right == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Binary, at);
            e->binOp = BinOp::And;
            e->left = left;
            e->right = right;
            left = e;
        }
        return left;
    }

    [[nodiscard]] Expr* parseNot()
    {
        if (check(TokKind::KwNot)) {
            const Token& at = advance();
            Expr* inner = parseNot();
            if (inner == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Unary, at);
            e->unOp = UnOp::Not;
            e->base = inner;
            return e;
        }
        return parseCmp();
    }

    [[nodiscard]] Expr* parseCmp()
    {
        Expr* left = parseAdd();
        if (left == nullptr) {
            return nullptr;
        }
        BinOp op;
        switch (peek().kind) {
        case TokKind::Eq: op = BinOp::Eq; break;
        case TokKind::Ne: op = BinOp::Ne; break;
        case TokKind::Lt: op = BinOp::Lt; break;
        case TokKind::Le: op = BinOp::Le; break;
        case TokKind::Gt: op = BinOp::Gt; break;
        case TokKind::Ge: op = BinOp::Ge; break;
        default: return left;
        }
        const Token& at = advance();
        Expr* right = parseAdd();
        if (right == nullptr) {
            return nullptr;
        }
        Expr* e = newExpr(Expr::Kind::Binary, at);
        e->binOp = op;
        e->left = left;
        e->right = right;
        return e;
    }

    [[nodiscard]] Expr* parseAdd()
    {
        Expr* left = parseMul();
        if (left == nullptr) {
            return nullptr;
        }
        for (;;) {
            BinOp op;
            if (check(TokKind::Plus)) {
                op = BinOp::Add;
            } else if (check(TokKind::Minus)) {
                op = BinOp::Sub;
            } else {
                return left;
            }
            const Token& at = advance();
            Expr* right = parseMul();
            if (right == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Binary, at);
            e->binOp = op;
            e->left = left;
            e->right = right;
            left = e;
        }
    }

    [[nodiscard]] Expr* parseMul()
    {
        Expr* left = parseUnary();
        if (left == nullptr) {
            return nullptr;
        }
        for (;;) {
            BinOp op;
            if (check(TokKind::Star)) {
                op = BinOp::Mul;
            } else if (check(TokKind::Slash)) {
                op = BinOp::Div;
            } else if (check(TokKind::Percent)) {
                op = BinOp::Mod;
            } else {
                return left;
            }
            const Token& at = advance();
            Expr* right = parseUnary();
            if (right == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Binary, at);
            e->binOp = op;
            e->left = left;
            e->right = right;
            left = e;
        }
    }

    [[nodiscard]] Expr* parseUnary()
    {
        if (check(TokKind::Minus)) {
            const Token& at = advance();
            Expr* inner = parseUnary();
            if (inner == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Unary, at);
            e->unOp = UnOp::Neg;
            e->base = inner;
            return e;
        }
        if (check(TokKind::KwNot)) {
            const Token& at = advance();
            Expr* inner = parseUnary();
            if (inner == nullptr) {
                return nullptr;
            }
            Expr* e = newExpr(Expr::Kind::Unary, at);
            e->unOp = UnOp::Not;
            e->base = inner;
            return e;
        }
        return parsePostfix();
    }

    [[nodiscard]] Expr* parsePostfix()
    {
        Expr* e = parsePrimary();
        if (e == nullptr) {
            return nullptr;
        }
        for (;;) {
            if (!check(TokKind::Dot)) {
                return e;
            }
            const Token& at = advance();
            if (!check(TokKind::Ident)) {
                error("esperado nome de campo após '.'");
                return nullptr;
            }
            const Token& field = advance();
            Expr* m = newExpr(Expr::Kind::Member, at);
            m->s = field.text;
            m->base = e;
            e = m;
        }
    }

    [[nodiscard]] Expr* parsePrimary()
    {
        const Token& t = peek();
        switch (t.kind) {
        case TokKind::Int: {
            advance();
            Expr* e = newExpr(Expr::Kind::Int, t);
            e->i = t.i;
            return e;
        }
        case TokKind::Float: {
            advance();
            Expr* e = newExpr(Expr::Kind::Float, t);
            e->f = t.f;
            return e;
        }
        case TokKind::Str: {
            advance();
            Expr* e = newExpr(Expr::Kind::Str, t);
            e->s = t.text;
            return e;
        }
        case TokKind::KwTrue:
        case TokKind::KwFalse: {
            advance();
            Expr* e = newExpr(Expr::Kind::Bool, t);
            e->b = t.kind == TokKind::KwTrue;
            return e;
        }
        case TokKind::Ident: {
            advance();
            // P4.7.0 B4: chamadas com NOME PONTUADO ("camera.zoom",
            // "camera.position", "camera.follow" — verbos da câmera).
            // Backtracking barato: snapshot do cursor; cadeia
            // IDENT ('.' IDENT)* seguida de '(' = chamada pontuada;
            // caso contrário o cursor é RESTAURADO e o caminho normal
            // (ident + postfix de campo/atribuição) fica intocado.
            {
                const std::size_t saved = pos_;
                std::string dotted = t.text;
                bool dottedCall = false;
                while (check(TokKind::Dot)) {
                    advance(); // '.'
                    if (peek().kind != TokKind::Ident) {
                        break;
                    }
                    dotted.push_back('.');
                    dotted += advance().text;
                    if (check(TokKind::LParen)) {
                        dottedCall = true;
                        break;
                    }
                    if (!check(TokKind::Dot)) {
                        break;
                    }
                }
                if (dottedCall) {
                    advance(); // '('
                    Expr* e = newExpr(Expr::Kind::Call, t);
                    e->s = dotted;
                    if (!check(TokKind::RParen)) {
                        for (;;) {
                            Expr* arg = parseExpr();
                            if (arg == nullptr) {
                                return nullptr;
                            }
                            e->args.push_back(arg);
                            if (accept(TokKind::Comma)) {
                                continue;
                            }
                            break;
                        }
                    }
                    if (!expect(TokKind::RParen,
                                "esperado ')' após argumentos")) {
                        return nullptr;
                    }
                    return e;
                }
                pos_ = saved;          // sem chamada: volta ao ident base
                lastProgress_ = saved; // ...e o progresso volta com ele
            }
            if (check(TokKind::LParen)) {
                advance();
                Expr* e = newExpr(Expr::Kind::Call, t);
                e->s = t.text;
                if (!check(TokKind::RParen)) {
                    for (;;) {
                        Expr* arg = parseExpr();
                        if (arg == nullptr) {
                            return nullptr;
                        }
                        e->args.push_back(arg);
                        if (accept(TokKind::Comma)) {
                            continue;
                        }
                        break;
                    }
                }
                if (!expect(TokKind::RParen, "esperado ')' após argumentos")) {
                    return nullptr;
                }
                return e;
            }
            Expr* e = newExpr(Expr::Kind::Ident, t);
            e->s = t.text;
            return e;
        }
        case TokKind::LParen: {
            advance();
            Expr* inner = parseExpr();
            if (inner == nullptr) {
                return nullptr;
            }
            if (!expect(TokKind::RParen, "esperado ')'")) {
                return nullptr;
            }
            return inner;
        }
        default:
            error("esperado expressão");
            return nullptr;
        }
    }

    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<std::unique_ptr<Stmt>> stmts_;
    std::vector<std::unique_ptr<Expr>> exprs_;
};

} // namespace

ParseResult parse(const std::vector<Token>& tokens, std::vector<NiDiag>& diags)
{
    Parser parser(tokens, diags);
    return parser.run();
}

} // namespace eng::ni
