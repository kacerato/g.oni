/// Lexer do NI-Script — tokens com linha/coluna.
///
/// Puro: sem I/O, sem estado global. Erros são NiDiag (nunca throw —
/// ADR-004). Comentários: `#` até o fim da linha. Strings: "..." com
/// escapes mínimos (\" \\ \n \t); NOVA LINHA CRUA dentro de "..." = erro.

#include "eng/niscript/NiValue.hpp"
#include "NiAst.hpp"

#include <cctype>
#include <cerrno>
#include <charconv> // from_chars<int64> — ok em libc++/NDK (só double falta)
#include <cmath>
#include <cstdlib>  // strtod — double (libc++/NDK sem from_chars<double>)
#include <optional>
#include <string>

namespace eng::ni {
namespace {

bool isIdentStart(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

struct Keyword {
    const char* text;
    TokKind kind;
};

const Keyword kKeywords[] = {
    {"f", TokKind::KwF},         {"stop", TokKind::KwStop},
    {"up", TokKind::KwUp},       {"if", TokKind::KwIf},
    {"else", TokKind::KwElse},   {"repeat", TokKind::KwRepeat},
    {"repair", TokKind::KwRepair}, {"timeout", TokKind::KwTimeout},
    {"link", TokKind::KwLink},   {"to", TokKind::KwTo},
    {"emit", TokKind::KwEmit},   {"give", TokKind::KwGive},
    {"var", TokKind::KwVar},     {"add", TokKind::KwAdd},
    {"and", TokKind::KwAnd},     {"or", TokKind::KwOr},
    {"not", TokKind::KwNot},     {"true", TokKind::KwTrue},
    {"false", TokKind::KwFalse},
};

/// (nomes de tipo são CONTEXTUAIS — a definição exportada de
/// typeKeywordOf vive fora deste namespace anônimo, mais abaixo)

TokKind keywordOf(const std::string& text) noexcept
{
    for (const Keyword& k : kKeywords) {
        if (text == k.text) {
            return k.kind;
        }
    }
    return TokKind::Ident;
}

} // namespace

bool typeKeywordOf(const std::string& text, NiType& out) noexcept
{
    if (text == "int") { out = NiType::Int; return true; }
    if (text == "float") { out = NiType::Float; return true; }
    if (text == "bool") { out = NiType::Bool; return true; }
    if (text == "string") { out = NiType::String; return true; }
    if (text == "vec2") { out = NiType::Vec2; return true; }
    if (text == "vec3") { out = NiType::Vec3; return true; }
    if (text == "color") { out = NiType::Color; return true; }
    if (text == "entity") { out = NiType::Entity; return true; }
    if (text == "asset") { out = NiType::Asset; return true; }
    if (text == "transform") { out = NiType::Transform; return true; }
    return false;
}

std::vector<Token> lex(std::string_view source, std::vector<NiDiag>& diags)
{
    std::vector<Token> tokens;
    std::uint32_t line = 1;
    std::uint32_t col = 1;
    std::size_t i = 0;
    const std::size_t n = source.size();

    auto push = [&](TokKind kind, std::uint32_t l, std::uint32_t c,
                    std::string text) {
        Token t;
        t.kind = kind;
        t.line = l;
        t.col = c;
        t.text = std::move(text);
        tokens.push_back(std::move(t));
    };

    auto error = [&](std::uint32_t l, std::uint32_t c, std::string msg) {
        NiDiag d;
        d.line = l;
        d.col = c;
        d.message = std::move(msg);
        diags.push_back(std::move(d));
    };

    while (i < n) {
        const char c = source[i];
        // espaços (colunas contam em BYTES — documentado em docs/ni-script/08)
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            ++col;
            continue;
        }
        if (c == '\n') {
            ++i;
            ++line;
            col = 1;
            continue;
        }
        // comentário
        if (c == '#') {
            while (i < n && source[i] != '\n') {
                ++i;
                ++col;
            }
            continue;
        }
        const std::uint32_t tokLine = line;
        const std::uint32_t tokCol = col;

        // string
        if (c == '"') {
            std::string value;
            ++i; // abre
            ++col;
            bool closed = false;
            while (i < n) {
                const char sc = source[i];
                if (sc == '"') {
                    ++i;
                    ++col;
                    closed = true;
                    break;
                }
                if (sc == '\n') {
                    error(tokLine, tokCol,
                          "string não terminada (quebra de linha crua)");
                    break;
                }
                if (sc == '\\') {
                    ++i;
                    ++col;
                    if (i >= n) {
                        error(tokLine, tokCol, "escape no fim da fonte");
                        break;
                    }
                    const char esc = source[i];
                    ++i;
                    ++col;
                    switch (esc) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    default:
                        error(tokLine, tokCol,
                              "escape desconhecido '\\" + std::string(1, esc)
                                  + "'");
                        break;
                    }
                    continue;
                }
                value.push_back(sc);
                ++i;
                ++col;
            }
            if (!closed) {
                if (diags.empty()
                    || diags.back().line != tokLine
                    || diags.back().col != tokCol) {
                    // ainda sem diagnóstico específico deste token
                    if (i >= n) {
                        error(tokLine, tokCol,
                              "string não terminada (fim da fonte)");
                    }
                }
            }
            Token t;
            t.kind = TokKind::Str;
            t.line = tokLine;
            t.col = tokCol;
            t.text = std::move(value);
            tokens.push_back(std::move(t));
            continue;
        }

        // número: [0-9]+ ('.' [0-9]+)? — SEM exceções:
        // from_chars nunca lança; overflow é diagnóstico com linha/coluna.
        if (isDigit(c)) {
            std::string digits;
            bool isFloat = false;
            while (i < n && isDigit(source[i])) {
                digits.push_back(source[i]);
                ++i;
                ++col;
            }
            if (i + 1 < n && source[i] == '.' && isDigit(source[i + 1])) {
                isFloat = true;
                digits.push_back('.');
                ++i;
                ++col;
                while (i < n && isDigit(source[i])) {
                    digits.push_back(source[i]);
                    ++i;
                    ++col;
                }
            }
            Token t;
            t.line = tokLine;
            t.col = tokCol;
            t.text = digits;
            if (isFloat) {
                // strtod (libc++/NDK não tem from_chars<double> — GCC tem;
                // clang não). O motor NUNCA chama setlocale — o ponto
                // decimal é sempre '.' do locale "C" (determinístico).
                t.kind = TokKind::Float;
                errno = 0;
                char* end = nullptr;
                const double value = std::strtod(digits.c_str(), &end);
                if (end != nullptr && *end == '\0' && errno != ERANGE) {
                    t.f = value;
                } else {
                    error(tokLine, tokCol,
                          "literal float inválido/fora de range: " + digits);
                    t.f = 0.0;
                }
            } else {
                t.kind = TokKind::Int;
                std::int64_t value = 0;
                const auto [ptr, ec] = std::from_chars(
                    digits.data(), digits.data() + digits.size(), value);
                if (ec == std::errc{} && ptr == digits.data() + digits.size()) {
                    t.i = value;
                } else {
                    error(tokLine, tokCol,
                          "literal inteiro fora do range i64: " + digits);
                    t.i = 0;
                }
            }
            tokens.push_back(std::move(t));
            continue;
        }

        // identificador/keyword
        if (isIdentStart(c)) {
            std::string text;
            while (i < n && isIdentStart(source[i])) {
                text.push_back(source[i]);
                ++i;
                ++col;
            }
            // ORDEM importa: keywordOf ANTES do move (bug clássico de
            // ordem de avaliação de argumentos — GCC avalia à direita).
            const TokKind keyword = keywordOf(text);
            push(keyword, tokLine, tokCol, std::move(text));
            continue;
        }

        // símbolos (2 chars primeiro)
        auto two = [&](char second, TokKind kind) {
            if (i + 1 < n && source[i + 1] == second) {
                const std::string s{c, second};
                push(kind, tokLine, tokCol, s);
                i += 2;
                col += 2;
                return true;
            }
            return false;
        };
        if (c == '=' && two('=', TokKind::Eq)) { continue; }
        if (c == '!' && two('=', TokKind::Ne)) { continue; }
        if (c == '<' && two('=', TokKind::Le)) { continue; }
        if (c == '>' && two('=', TokKind::Ge)) { continue; }
        // Atribuição composta — += -= *= /= (o script canônico
        // do editor usa `position.x -= dt`; sem isto o PLAY falha em
        // silêncio para o autor — erro só no log).
        if (c == '+' && two('=', TokKind::PlusAssign)) { continue; }
        if (c == '-' && two('=', TokKind::MinusAssign)) { continue; }
        if (c == '*' && two('=', TokKind::StarAssign)) { continue; }
        if (c == '/' && two('=', TokKind::SlashAssign)) { continue; }

        struct OneChar {
            char c;
            TokKind kind;
        };
        static constexpr OneChar kOne[] = {
            {':', TokKind::Colon}, {'(', TokKind::LParen},
            {')', TokKind::RParen}, {'.', TokKind::Dot},
            {',', TokKind::Comma}, {'&', TokKind::Amp},
            {'=', TokKind::Assign}, {'<', TokKind::Lt},
            {'>', TokKind::Gt}, {'+', TokKind::Plus},
            {'-', TokKind::Minus}, {'*', TokKind::Star},
            {'/', TokKind::Slash}, {'%', TokKind::Percent},
        };
        bool matched = false;
        for (const OneChar& one : kOne) {
            if (c == one.c) {
                const std::string s{c};
                push(one.kind, tokLine, tokCol, s);
                ++i;
                ++col;
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }

        error(tokLine, tokCol,
              "caractere inesperado '" + std::string(1, c) + "'");
        ++i;
        ++col;
    }

    Token end;
    end.kind = TokKind::End;
    end.line = line;
    end.col = col;
    tokens.push_back(std::move(end));
    return tokens;
}

} // namespace eng::ni
