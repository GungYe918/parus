#include <lei/parse/Parser.hpp>

#include <cctype>
#include <unordered_map>

namespace lei::parse {

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_continue(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

syntax::TokenKind keyword_or_ident(std::string_view s) {
    using K = syntax::TokenKind;
    static const std::unordered_map<std::string_view, K> kMap = {
        {"import", K::kKwImport},
        {"from", K::kKwFrom},
        {"export", K::kKwExport},
        {"build", K::kKwBuild},
        {"let", K::kKwLet},
        {"const", K::kKwConst},
        {"def", K::kKwDef},
        {"assert", K::kKwAssert},
        {"if", K::kKwIf},
        {"then", K::kKwThen},
        {"else", K::kKwElse},
        {"match", K::kKwMatch},
        {"true", K::kKwTrue},
        {"false", K::kKwFalse},
        {"int", K::kKwInt},
        {"float", K::kKwFloat},
        {"string", K::kKwString},
        {"bool", K::kKwBool},
    };

    auto it = kMap.find(s);
    if (it == kMap.end()) return K::kIdent;
    return it->second;
}

} // namespace

std::vector<syntax::Token> lex(std::string_view source, std::string_view file_path, diag::Bag& diags) {
    using K = syntax::TokenKind;

    std::vector<syntax::Token> toks;
    toks.reserve(source.size() / 2 + 1);

    size_t i = 0;
    uint32_t line = 1;
    uint32_t col = 1;

    auto at = [&](size_t off) -> char {
        const size_t p = i + off;
        if (p >= source.size()) return '\0';
        return source[p];
    };

    auto advance = [&]() {
        if (i >= source.size()) return;
        if (source[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        ++i;
    };

    auto push = [&](K kind, std::string lexeme, uint32_t l, uint32_t c) {
        toks.push_back(syntax::Token{kind, std::move(lexeme), {l, c}});
    };

    while (i < source.size()) {
        char c = at(0);

        // whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // line comment
        if (c == '/' && at(1) == '/') {
            while (i < source.size() && at(0) != '\n') advance();
            continue;
        }

        // block comment
        if (c == '/' && at(1) == '*') {
            advance();
            advance();
            while (i < source.size()) {
                if (at(0) == '*' && at(1) == '/') {
                    advance();
                    advance();
                    break;
                }
                advance();
            }
            continue;
        }

        const uint32_t tok_line = line;
        const uint32_t tok_col = col;

        // punctuation + operators (longest first)
        if (c == '.' && at(1) == '.' && at(2) == '.') {
            push(K::kEllipsis, "...", tok_line, tok_col);
            advance(); advance(); advance();
            continue;
        }
        if (c == ':' && at(1) == ':') {
            push(K::kColonColon, "::", tok_line, tok_col);
            advance(); advance();
            continue;
        }
        if (c == '?' && at(1) == '=') {
            push(K::kDefaultOverlay, "?=", tok_line, tok_col);
            advance(); advance();
            continue;
        }
        if (c == '=' && at(1) == '>') {
            push(K::kFatArrow, "=>", tok_line, tok_col);
            advance(); advance();
            continue;
        }
        if (c == '=' && at(1) == '=') {
            push(K::kEqEq, "==", tok_line, tok_col);
            advance(); advance();
            continue;
        }
        if (c == '!' && at(1) == '=') {
            push(K::kBangEq, "!=", tok_line, tok_col);
            advance(); advance();
            continue;
        }
        if (c == '&' && at(1) == '&') {
            push(K::kAndAnd, "&&", tok_line, tok_col);
            advance(); advance();
            continue;
        }
        if (c == '|' && at(1) == '|') {
            push(K::kOrOr, "||", tok_line, tok_col);
            advance(); advance();
            continue;
        }

        switch (c) {
            case '(': push(K::kLParen, "(", tok_line, tok_col); advance(); continue;
            case ')': push(K::kRParen, ")", tok_line, tok_col); advance(); continue;
            case '{': push(K::kLBrace, "{", tok_line, tok_col); advance(); continue;
            case '}': push(K::kRBrace, "}", tok_line, tok_col); advance(); continue;
            case '[': push(K::kLBracket, "[", tok_line, tok_col); advance(); continue;
            case ']': push(K::kRBracket, "]", tok_line, tok_col); advance(); continue;
            case ',': push(K::kComma, ",", tok_line, tok_col); advance(); continue;
            case ':': push(K::kColon, ":", tok_line, tok_col); advance(); continue;
            case ';': push(K::kSemicolon, ";", tok_line, tok_col); advance(); continue;
            case '.': push(K::kDot, ".", tok_line, tok_col); advance(); continue;
            case '=': push(K::kAssign, "=", tok_line, tok_col); advance(); continue;
            case '+': push(K::kPlus, "+", tok_line, tok_col); advance(); continue;
            case '-': push(K::kMinus, "-", tok_line, tok_col); advance(); continue;
            case '*': push(K::kStar, "*", tok_line, tok_col); advance(); continue;
            case '/': push(K::kSlash, "/", tok_line, tok_col); advance(); continue;
            case '&': push(K::kAmp, "&", tok_line, tok_col); advance(); continue;
            case '!': push(K::kBang, "!", tok_line, tok_col); advance(); continue;
            case '_': push(K::kUnderscore, "_", tok_line, tok_col); advance(); continue;
            default: break;
        }

        // string
        if (c == '"') {
            advance();
            std::string out;
            bool ok = false;
            while (i < source.size()) {
                char ch = at(0);
                if (ch == '\\') {
                    advance();
                    if (i >= source.size()) break;
                    char esc = at(0);
                    switch (esc) {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        default: out.push_back(esc); break;
                    }
                    advance();
                    continue;
                }
                if (ch == '"') {
                    advance();
                    ok = true;
                    break;
                }
                out.push_back(ch);
                advance();
            }
            if (!ok) {
                diags.add(diag::Code::C_INVALID_LITERAL, std::string(file_path), tok_line, tok_col,
                          "unterminated string literal");
            }
            push(K::kStringLit, out, tok_line, tok_col);
            continue;
        }

        // number
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string text;
            bool saw_dot = false;
            while (i < source.size()) {
                char ch = at(0);
                if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '_') {
                    text.push_back(ch);
                    advance();
                    continue;
                }
                if (!saw_dot && ch == '.' && std::isdigit(static_cast<unsigned char>(at(1)))) {
                    saw_dot = true;
                    text.push_back(ch);
                    advance();
                    continue;
                }
                break;
            }
            push(saw_dot ? K::kFloatLit : K::kIntLit, text, tok_line, tok_col);
            continue;
        }

        // ident / keyword
        if (is_ident_start(c)) {
            std::string ident;
            ident.push_back(c);
            advance();
            while (i < source.size() && is_ident_continue(at(0))) {
                ident.push_back(at(0));
                advance();
            }
            push(keyword_or_ident(ident), ident, tok_line, tok_col);
            continue;
        }

        // unknown char
        std::string bad(1, c);
        diags.add(diag::Code::C_UNEXPECTED_TOKEN, std::string(file_path), tok_line, tok_col,
                  "unknown character '" + bad + "'");
        advance();
    }

    toks.push_back(syntax::Token{K::kEof, "", {line, col}});
    return toks;
}

} // namespace lei::parse
