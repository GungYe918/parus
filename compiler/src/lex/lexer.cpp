// compiler/src/lex/lexer.cpp
#include <gaupel/lex/Lexer.hpp>
#include <gaupel/syntax/Punct.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <cctype>
#include <algorithm>
#include <sstream> 


namespace gaupel {

    static bool utf8_validate_strict(std::string_view s, uint32_t& bad_off) {
        auto is_cont = [&](unsigned char b) -> bool {
            return (b & 0xC0) == 0x80;
        };

        size_t i = 0;
        while (i < s.size()) {
            unsigned char b0 = static_cast<unsigned char>(s[i]);

            // ASCII
            if (b0 < 0x80) {
                i += 1;
                continue;
            }

            // continuation byte cannot start a sequence
            if (b0 >= 0x80 && b0 <= 0xBF) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }

            // 2-byte: C2..DF
            if (b0 >= 0xC2 && b0 <= 0xDF) {
                if (i + 1 >= s.size()) { bad_off = (uint32_t)i; return false; }
                unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
                if (!is_cont(b1)) { bad_off = (uint32_t)i; return false; }

                // overlong already prevented by C2.. rule
                i += 2;
                continue;
            }

            // 3-byte: E0..EF
            if (b0 >= 0xE0 && b0 <= 0xEF) {
                if (i + 2 >= s.size()) { bad_off = (uint32_t)i; return false; }
                unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
                unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
                if (!is_cont(b1) || !is_cont(b2)) { bad_off = (uint32_t)i; return false; }

                // reject overlong: E0 A0..BF
                if (b0 == 0xE0 && b1 < 0xA0) { bad_off = (uint32_t)i; return false; }

                // reject surrogate: ED 80..9F (U+D800..U+DFFF)
                if (b0 == 0xED && b1 >= 0xA0) { bad_off = (uint32_t)i; return false; }

                i += 3;
                continue;
            }

            // 4-byte: F0..F4
            if (b0 >= 0xF0 && b0 <= 0xF4) {
                if (i + 3 >= s.size()) { bad_off = (uint32_t)i; return false; }
                unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
                unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
                unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
                if (!is_cont(b1) || !is_cont(b2) || !is_cont(b3)) { bad_off = (uint32_t)i; return false; }

                // reject overlong: F0 90..BF
                if (b0 == 0xF0 && b1 < 0x90) { bad_off = (uint32_t)i; return false; }

                // reject > U+10FFFF: F4 80..8F only
                if (b0 == 0xF4 && b1 > 0x8F) { bad_off = (uint32_t)i; return false; }

                i += 4;
                continue;
            }

            // invalid leading byte (C0,C1,F5..FF etc)
            bad_off = static_cast<uint32_t>(i);
            return false;
        }

        return true;
    }

    static bool is_ident_start(char c) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) return true; // UTF-8 lead/cont bytes: accept as identifier (v0 policy)
        return std::isalpha(u) || c == '_';
    }

    static bool is_ident_cont(char c) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) return true; // UTF-8 bytes: accept as identifier (v0 policy)
        return std::isalnum(u) || c == '_';
    }

    Lexer::Lexer(std::string_view source, uint32_t file_id, diag::Bag* diags) 
        : source_(source), file_id_(file_id), diags_(diags) {}

    bool Lexer::validate_utf8_all(uint32_t& bad_off) const {
        return utf8_validate_strict(source_, bad_off);
    }

    static std::string byte_hex2(unsigned char b) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string s;
        s.push_back(kHex[(b >> 4) & 0xF]);
        s.push_back(kHex[b & 0xF]);
        return s;
    }
    
    void Lexer::report_invalid_utf8(uint32_t bad_off) {
        if (!diags_) return;

        const uint32_t hi = std::min<uint32_t>(bad_off + 1, (uint32_t)source_.size());
        Span sp{file_id_, bad_off, hi};

        diag::Diagnostic d(diag::Severity::kFatal, diag::Code::kInvalidUtf8, sp);

        // args = offset + offending byte hex
        d.add_arg_int((int)bad_off);

        unsigned char b = 0;
        if (bad_off < source_.size()) {
            b = static_cast<unsigned char>(source_[bad_off]);
        }
        d.add_arg(byte_hex2(b));

        diags_->add(std::move(d));
    }

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
        bool saw_float_suffix = false;
    
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

        // suffix:
        // - float: f, lf, f32/f64/f128
        // - int:   i8/i16/i32/i64/i128, u8/u16/u32/u64/u128, isize/usize
        // - unknown suffix is consumed as one token and validated later in tyck
        if (!eof() && std::isalpha(static_cast<unsigned char>(peek()))) {
            if (peek() == 'l' && peek(1) == 'f') {
                saw_float_suffix = true;
                bump(); // l
                bump(); // f
                while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) bump();
            } else if (peek() == 'f') {
                saw_float_suffix = true;
                bump(); // f
                while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) bump();
            } else {
                while (!eof()) {
                    const unsigned char u = static_cast<unsigned char>(peek());
                    if (std::isalnum(u) || peek() == '_') {
                        bump();
                        continue;
                    }
                    break;
                }
            }
        }

        size_t end = pos_;
        Token t;
        t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
        t.lexeme = source_.substr(start, end - start);
        t.kind = (saw_dot || saw_float_suffix) ? syntax::TokenKind::kFloatLit : syntax::TokenKind::kIntLit;
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

    Token Lexer::lex_char() {
        // assumes current char is '\''
        const size_t start = pos_;
        bump(); // consume opening '

        if (eof()) {
            // unterminated
            Token t;
            t.kind = syntax::TokenKind::kError;
            t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(pos_)};
            t.lexeme = source_.substr(start, pos_ - start);
            return t;
        }

        // minimal escape support
        if (peek() == '\\') {
            bump(); // '\'
            if (!eof()) bump(); // escaped char (n, t, r, \, ')
        } else {
            // consume exactly one UTF-8 codepoint (minimal, permissive):
            // - if ASCII: 1 byte
            // - else: 2~4 bytes based on leading bits
            unsigned char c0 = static_cast<unsigned char>(peek());
            size_t need = 1;
            if ((c0 & 0x80u) == 0x00u) need = 1;
            else if ((c0 & 0xE0u) == 0xC0u) need = 2;
            else if ((c0 & 0xF0u) == 0xE0u) need = 3;
            else if ((c0 & 0xF8u) == 0xF0u) need = 4;
            for (size_t i = 0; i < need && !eof(); ++i) bump();
        }

        // expect closing '
        if (peek() == '\'') {
            bump();
            Token t;
            t.kind = syntax::TokenKind::kCharLit;
            t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(pos_)};
            t.lexeme = source_.substr(start, pos_ - start);
            return t;
        }

        // unterminated or too long
        // recover: consume until closing ' or newline/eof (best-effort)
        while (!eof() && peek() != '\'' && peek() != '\n') bump();
        if (peek() == '\'') bump();

        Token t;
        t.kind = syntax::TokenKind::kError;
        t.span = Span{file_id_, static_cast<uint32_t>(start), static_cast<uint32_t>(pos_)};
        t.lexeme = source_.substr(start, pos_ - start);
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
        if (t.lexeme == "static") { t.kind = syntax::TokenKind::kKwStatic; return t; }

        // stmt keywords
        if (t.lexeme == "let")      { t.kind = syntax::TokenKind::kKwLet;      return t; }
        if (t.lexeme == "set")      { t.kind = syntax::TokenKind::kKwSet;      return t; }
        if (t.lexeme == "if")       { t.kind = syntax::TokenKind::kKwIf;       return t; }
        if (t.lexeme == "elif")     { t.kind = syntax::TokenKind::kKwElif;     return t; }
        if (t.lexeme == "else")     { t.kind = syntax::TokenKind::kKwElse;     return t; }
        if (t.lexeme == "while")    { t.kind = syntax::TokenKind::kKwWhile;    return t; }
        if (t.lexeme == "do")       { t.kind = syntax::TokenKind::kKwDo;       return t; }
        if (t.lexeme == "loop")     { t.kind = syntax::TokenKind::kKwLoop;     return t; }
        if (t.lexeme == "in")       { t.kind = syntax::TokenKind::kKwIn;       return t; }
        if (t.lexeme == "return")   { t.kind = syntax::TokenKind::kKwReturn;   return t; }
        if (t.lexeme == "break")    { t.kind = syntax::TokenKind::kKwBreak;    return t; }
        if (t.lexeme == "continue") { t.kind = syntax::TokenKind::kKwContinue; return t; }

        if (t.lexeme == "switch")   { t.kind = syntax::TokenKind::kKwSwitch;    return t; }
        if (t.lexeme == "case")     { t.kind = syntax::TokenKind::kKwCase;      return t; }
        if (t.lexeme == "default")  { t.kind = syntax::TokenKind::kKwDefault;   return t; }

        if (t.lexeme == "use")      { t.kind = syntax::TokenKind::kKwUse;       return t; }
        if (t.lexeme == "module")   { t.kind = syntax::TokenKind::kKwModule;    return t; }        
        if (t.lexeme == "as")       { t.kind = syntax::TokenKind::kKwAs;        return t; }

        if (t.lexeme == "commit")   { t.kind = syntax::TokenKind::kKwCommit;   return t; }
        if (t.lexeme == "recast")   { t.kind = syntax::TokenKind::kKwRecast;   return t; }

        if (t.lexeme == "fn")       { t.kind = syntax::TokenKind::kKwFn;       return t; }
        if (t.lexeme == "field")    { t.kind = syntax::TokenKind::kKwField;    return t; }
        if (t.lexeme == "acts")     { t.kind = syntax::TokenKind::kKwActs;     return t; }
        if (t.lexeme == "export")   { t.kind = syntax::TokenKind::kKwExport;   return t; }
        // - 설계 변경: pure/comptime 등 @ + attr에서 attr은 이제 kIdent로 취급
        // if (t.lexeme == "pure")     { t.kind = syntax::TokenKind::kKwPure;     return t; }
        // if (t.lexeme == "comptime") { t.kind = syntax::TokenKind::kKwComptime; return t; }

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

        // strict utf8 gate
        uint32_t bad_off = 0;
        if (!validate_utf8_all(bad_off)) {
            report_invalid_utf8(bad_off);
            emit_eof(out);
            return out;
        }
        
        while (!eof()) {
            skip_ws_and_comments();
            if (eof()) break;

            char c = peek();
            unsigned char u = static_cast<unsigned char>(c);

            if (std::isdigit(u)) {
                out.push_back(lex_number());
                continue;
            }

            if (c == '"') {
                out.push_back(lex_string());
                continue;
            }

            if (peek() == '\'') {
                out.push_back(lex_char());
                continue;
            }
            
            // ident / keyword (ASCII or UTF-8 bytes)
            if (is_ident_start(c)) {
                out.push_back(lex_ident_or_kw());
                continue;
            }

            out.push_back(lex_punct_or_unknown());
        }

        emit_eof(out);
        return out;
    }

} // namespace gaupel
