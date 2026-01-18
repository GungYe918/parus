// compiler/include/gaupel/lex/Token.hpp
#pragma once
#include <string_view>
#include <gaupel/text/Span.hpp>
#include <gaupel/syntax/TokenKind.hpp>


namespace gaupel {

    struct Token {
        syntax::TokenKind kind = syntax::TokenKind::kError;
        Span span{};
        std::string_view lexeme{};
    };

} // namespace gaupel