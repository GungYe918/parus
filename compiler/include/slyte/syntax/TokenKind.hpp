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

    kPlusPlus, // ++   (prefix/postfix)

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

    constexpr std::string_view token_kind_name(TokenKind k) {
        switch(k) {
            case TokenKind::kEof: return "eof";
            case TokenKind::kError: return "error";
            case TokenKind::kIdent: return "ident";
            case TokenKind::kHole: return "hole";
            case TokenKind::kIntLit: return "int_lit";
            case TokenKind::kFloatLit: return "float_lit";
            case TokenKind::kStringLit: return "string_lit";

            case TokenKind::kKwTrue: return "true";
            case TokenKind::kKwFalse: return "false";
            case TokenKind::kKwNull: return "null";
            case TokenKind::kKwAnd: return "and";
            case TokenKind::kKwOr: return "or";
            case TokenKind::kKwNot: return "not";
            case TokenKind::kKwXor: return "xor";
            case TokenKind::kKwMut: return "mut";
            case TokenKind::kKwRef: return "ref";

            case TokenKind::kLParen: return "(";
            case TokenKind::kRParen: return ")";
            case TokenKind::kLBrace: return "{";
            case TokenKind::kRBrace: return "}";
            case TokenKind::kLBracket: return "[";
            case TokenKind::kRBracket: return "]";
            case TokenKind::kComma: return ",";
            case TokenKind::kColon: return ":";
            case TokenKind::kSemicolon: return ";";
            case TokenKind::kQuestion: return "?";

            case TokenKind::kAssign: return "=";
            case TokenKind::kPlusAssign: return "+=";
            case TokenKind::kMinusAssign: return "-=";
            case TokenKind::kStarAssign: return "*=";
            case TokenKind::kSlashAssign: return "/=";
            case TokenKind::kPercentAssign: return "%=";

            case TokenKind::kPlus: return "+";
            case TokenKind::kMinus: return "-";
            case TokenKind::kStar: return "*";
            case TokenKind::kSlash: return "/";
            case TokenKind::kPercent: return "%";
            case TokenKind::kPlusPlus: return "++";

            case TokenKind::kBang: return "!";
            case TokenKind::kCaret: return "^";
            case TokenKind::kAmpAmp: return "&&";
            case TokenKind::kPipePipe: return "||";

            case TokenKind::kEqEq: return "==";
            case TokenKind::kBangEq: return "!=";
            case TokenKind::kLt: return "<";
            case TokenKind::kLtEq: return "<=";
            case TokenKind::kGt: return ">";
            case TokenKind::kGtEq: return ">=";

            case TokenKind::kLessLess: return "<<";
            case TokenKind::kDotDot: return "..";
            case TokenKind::kDotDotColon: return "..:";

            case TokenKind::kUnknownPunct: return "unknown_punct";
        }

        return "unknown";
    }

} // namespace slyte::syntax
