// compiler/include/slyte/lex/Token.hpp
#pragma once
#include <string_view>
#include <slyte/text/Span.hpp>
#include <slyte/syntax/TokenKind.hpp>


namespace slyte {

    struct Token {
        syntax::TokenKind kind = syntax::TokenKind::kError;
        Span span{};
        std::string_view lexeme{};
    };

} // namespace slyte