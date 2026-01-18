// compiler/include/slyte/lex/Lexer.hpp
#pragma once
#include <slyte/lex/Token.hpp>

#include <string_view>
#include <vector>


namespace slyte {

    class Lexer {
    public:
        Lexer(std::string_view source, std::uint32_t file_id);

        std::vector<Token> lex_all();

    private:
        char peek(size_t k = 0) const;
        bool eof() const;
        char bump();

        void skip_ws_and_comments();

        Token lex_number();
        Token lex_ident_or_kw();
        Token lex_string(); // supports "..." only (F/R triple strings later)

        Token lex_punct_or_unknown();

        void emit_eof(std::vector<Token>& out);

        std::string_view source_;
        uint32_t file_id_ = 0;
        size_t pos_ = 0;
    };

} // namespace slyte