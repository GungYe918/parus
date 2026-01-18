// compiler/src/lex/lexer.cpp
#include <slyte/lex/Lexer.hpp>
#include <slyte/syntax/Punct.hpp>
#include <slyte/syntax/TokenKind.hpp>

#include <cctype>
#include <algorithm>


namespace slyte {

    static bool is_ident_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    static bool is_ident_cont(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    Lexer::Lexer(std::string_view source, uint32_t file_id) 
        : source_(source), file_id_(file_id) {}

    char Lexer::peek(size_t k) const {
        size_t i = pos_ + k;
        if (i >= source_.size()) return '\0';
        return source_[i];
    }

    bool Lexer::eof() const {
        return pos_ >= source_.size();
    }

    char Lexer::bump() {
        if (eof()) return '\0';
        return source_[pos_++];
    }

    void Lexer::skip_ws_and_comments() {
        while (1) {
            // white space
            while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) bump();

            // line comment //
            if (peek() == '/' && peek(1) == '/') {
                bump(); bump();
                while (!eof() && peek() != '\n') bump();
                continue;
            }

            // block comment /* ... */
            if (peek() == '/' && peek(1) == '*') {
                bump(); bump();
                while (!eof()) {
                    if (peek() == '*' && peek(1) == '/') { bump(); bump(); break; }
                    bump();
                }

                continue;
            }

            break;
        }
    }

    Token Lexer::lex_number() {
        size_t start = pos_;
        bool saw_dot = false;
    
        // digits (allow '_')
        auto scan_digits = [&] {
            bool any = false;
            while (!eof()) {
                char c = peek();
                if (std::isdigit(static_cast<unsigned char>(c))) { bump(); any = true; continue; }
                if (c == '_') { bump(); continue; }
                break;
            }

            return any;
        };

        scan_digits();

        // float: digits '.' digits
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            saw_dot = true;
            bump(); // .
            scan_digits();
        }

        // suffix (simplified): accept letters/numbers for now
        while (!eof() && std::isalpha(static_cast<unsigned char>(peek()))) bump();

        size_t end = pos_;
        Token t;
        t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
        t.lexeme = source_.substr(start, end - start);
        t.kind = saw_dot ? syntax::TokenKind::kFloatLit : syntax::TokenKind::kIntLit;
        return t;
    }

    Token Lexer::lex_string() {
        size_t start = pos_;
        bump(); // opening "

        while (!eof()) {
            char c = bump();
            if (c == '\\') {
            if (!eof()) bump(); // escape next
            continue;
            }
            if (c == '"') break;
        }

        size_t end = pos_;
        Token t;
        t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
        t.lexeme = source_.substr(start, end - start);
        t.kind = syntax::TokenKind::kStringLit;
        return t;
    }

    Token Lexer::lex_ident_or_kw() {
        size_t start = pos_;
        bump(); // first char
        while (!eof() && is_ident_cont(peek())) bump();
        size_t end = pos_;

        Token t;
        t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
        t.lexeme = source_.substr(start, end - start);

        // special "_" -> hole
        if (t.lexeme == "_") {
            t.kind = syntax::TokenKind::kHole;
            return t;
        }

        // keywords for expressions
        if (t.lexeme == "true")  { t.kind = syntax::TokenKind::kKwTrue;  return t; }
        if (t.lexeme == "false") { t.kind = syntax::TokenKind::kKwFalse; return t; }
        if (t.lexeme == "null")  { t.kind = syntax::TokenKind::kKwNull;  return t; }

        if (t.lexeme == "and") { t.kind = syntax::TokenKind::kKwAnd; return t; }
        if (t.lexeme == "or")  { t.kind = syntax::TokenKind::kKwOr;  return t; }
        if (t.lexeme == "not") { t.kind = syntax::TokenKind::kKwNot; return t; }
        if (t.lexeme == "xor") { t.kind = syntax::TokenKind::kKwXor; return t; }

        if (t.lexeme == "mut") { t.kind = syntax::TokenKind::kKwMut; return t; }
        if (t.lexeme == "ref") { t.kind = syntax::TokenKind::kKwRef; return t; }

        t.kind = syntax::TokenKind::kIdent;
        return t;
    }

    Token Lexer::lex_punct_or_unknown() {
        size_t start = pos_;

        // maximal munch using k_punct_table
        for (const auto& e : syntax::k_punct_table) {
            const auto s = e.text;
            bool ok = true;
            for (size_t i = 0; i < s.size(); ++i) {
                if (peek(i) != s[i]) { ok = false; break; }
            }
            if (!ok) continue;

            for (size_t i = 0; i < s.size(); ++i) bump();
            size_t end = pos_;

            Token t;
            t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
            t.lexeme = source_.substr(start, end - start);
            t.kind = e.kind;
            return t;
        }

        // unknown single char punct
        bump();
        size_t end = pos_;
        Token t;
        t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
        t.lexeme = source_.substr(start, end - start);
        t.kind = syntax::TokenKind::kUnknownPunct;
        return t;
    }

    void Lexer::emit_eof(std::vector<Token>& out) {
        Token t;
        t.kind = syntax::TokenKind::kEof;
        t.span = Span{file_id_, static_cast<uint32_t>(source_.size()), static_cast<uint32_t>(source_.size())};
        t.lexeme = std::string_view{};
        out.push_back(t);
    }

    std::vector<Token> Lexer::lex_all() {
        std::vector<Token> out;
        out.reserve(source_.size() / 4);
        
        while (!eof()) {
            skip_ws_and_comments();
            if (eof()) break;

            char c = peek();
            if (std::isdigit(static_cast<unsigned char>(c))) {
                out.push_back(lex_number());
                continue;
            }

            if (c == '"') {
                out.push_back(lex_string());
                continue;
            }
            
            if (is_ident_start(c)) {
                out.push_back(lex_ident_or_kw());
                continue;
            }

            out.push_back(lex_punct_or_unknown());
        }

        emit_eof(out);
        return out;
    }

} // namespace slyte