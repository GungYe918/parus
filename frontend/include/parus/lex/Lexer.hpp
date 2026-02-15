// frontend/include/parus/lex/Lexer.hpp
#pragma once
#include <parus/lex/Token.hpp>
#include <parus/diag/Diagnostic.hpp>

#include <string_view>
#include <vector>


namespace parus {

    class Lexer {
    public:
        Lexer(std::string_view source, std::uint32_t file_id)
            : Lexer(source, file_id, nullptr) {}

        Lexer(std::string_view source, std::uint32_t file_id, diag::Bag* diags);

        std::vector<Token> lex_all();

    private:
        char peek(size_t k = 0) const;
        bool eof() const;
        char bump();

        void skip_ws_and_comments();

        Token lex_number();
        Token lex_ident_or_kw();
        Token lex_string();     // supports "..." only (F/R triple strings later)
        Token lex_char();       // supports 'C', '글', '\n', '\'', '\\' (minimal)

        Token lex_punct_or_unknown();

        void emit_eof(std::vector<Token>& out);

        bool validate_utf8_all(uint32_t& bad_off) const;
        void report_invalid_utf8(uint32_t bad_off);

        std::string_view source_;
        uint32_t file_id_ = 0;
        size_t pos_ = 0;

        diag::Bag* diags_ = nullptr;
    };

} // namespace parus