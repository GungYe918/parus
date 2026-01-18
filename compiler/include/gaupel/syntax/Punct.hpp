// compiler/include/gaupel/syntax/Punct.hpp
#pragma once
#include <gaupel/syntax/TokenKind.hpp>

#include <array>
#include <string_view>


namespace gaupel::syntax {

    // Maximal munch: longer punctuations first.
    struct PunctEntry {
    std::string_view text;
    TokenKind kind;
    };

    
    inline constexpr std::array<PunctEntry, 35> k_punct_table = {{
        {"..:", TokenKind::kDotDotColon},
        {"..",  TokenKind::kDotDot},

        {"<<",  TokenKind::kLessLess},

        {"&&",  TokenKind::kAmpAmp},
        {"||",  TokenKind::kPipePipe},

        {"==",  TokenKind::kEqEq},
        {"!=",  TokenKind::kBangEq},
        {"<=",  TokenKind::kLtEq},
        {">=",  TokenKind::kGtEq},

        {"++", TokenKind::kPlusPlus},
        {"+=",  TokenKind::kPlusAssign},
        {"-=",  TokenKind::kMinusAssign},
        {"*=",  TokenKind::kStarAssign},
        {"/=",  TokenKind::kSlashAssign},
        {"%=",  TokenKind::kPercentAssign},

        {"(",   TokenKind::kLParen},
        {")",   TokenKind::kRParen},
        {"{",   TokenKind::kLBrace},
        {"}",   TokenKind::kRBrace},
        {"[",   TokenKind::kLBracket},
        {"]",   TokenKind::kRBracket},

        {",",   TokenKind::kComma},
        {":",   TokenKind::kColon},
        {";",   TokenKind::kSemicolon},
        {"?",   TokenKind::kQuestion},

        {"=",   TokenKind::kAssign},
        {"+",   TokenKind::kPlus},
        {"-",   TokenKind::kMinus},
        {"*",   TokenKind::kStar},
        {"/",   TokenKind::kSlash},
        {"%",   TokenKind::kPercent},

        {"!",   TokenKind::kBang},
        {"^",   TokenKind::kCaret},

        {"<",   TokenKind::kLt},
        {">",   TokenKind::kGt},
    }};

} // namespace gaupel::syntax