// frontend/include/parus/lex/Token.hpp
#pragma once
#include <string_view>
#include <parus/text/Span.hpp>
#include <parus/syntax/TokenKind.hpp>


namespace parus {

    struct Token {
        syntax::TokenKind kind = syntax::TokenKind::kError;
        Span span{};
        std::string_view lexeme{};
    };

} // namespace parus