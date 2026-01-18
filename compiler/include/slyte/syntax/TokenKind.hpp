// compiler/include/slyte/syntax/TokenKind.hpp
#pragma once
#include <cstdint>


namespace slyte::syntax {

    enum class TokenKind : uint16_t {
    // special
    kEof = 0,
    kError,

    // identifiers / literals
    kIdent,
    kHole,        // "_" (single underscore)
    kIntLit,
    kFloatLit,
    kStringLit,

    // keywords (subset for expressions / decls)
    kKwTrue,
    kKwFalse,
    kKwNull,

    kKwAnd,
    kKwOr,
    kKwNot,
    kKwXor,

    kKwMut,
    kKwRef,

    // punct / delimiters
    kLParen,   // (
    kRParen,   // )
    kLBrace,   // {
    kRBrace,   // }
    kLBracket, // [
    kRBracket, // ]

    kComma,     // ,
    kColon,     // :
    kSemicolon, // ;
    kQuestion,  // ?

    // operators
    kAssign,      // =
    kPlusAssign,  // +=
    kMinusAssign, // -=
    kStarAssign,  // *=
    kSlashAssign, // /=
    kPercentAssign, // %=

    kPlus,     // +
    kMinus,    // -
    kStar,     // *
    kSlash,    // /
    kPercent,  // %

    kBang,     // !
    kCaret,    // ^

    kAmpAmp,   // &&
    kPipePipe, // ||

    kEqEq,     // ==
    kBangEq,   // !=
    kLt,       // <
    kLtEq,     // <=
    kGt,       // >
    kGtEq,     // >=

    kLessLess, // <<   (pipe operator in Slyte)

    kDotDot,       // ..
    kDotDotColon,  // ..:

    // for convenience in parser recovery
    kUnknownPunct,
    };

} // namespace slyte::syntax
