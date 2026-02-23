#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lei::syntax {

enum class TokenKind : uint16_t {
    kEof = 0,
    kError,

    kIdent,
    kIntLit,
    kFloatLit,
    kStringLit,

    kKwImport,
    kKwFrom,
    kKwExport,
    kKwProto,
    kKwPlan,
    kKwLet,
    kKwVar,
    kKwDef,
    kKwAssert,
    kKwIf,
    kKwElse,
    kKwTrue,
    kKwFalse,
    kKwInt,
    kKwFloat,
    kKwString,
    kKwBool,
    kKwReturn,
    kKwFor,
    kKwIn,

    kLParen,
    kRParen,
    kLBrace,
    kRBrace,
    kLBracket,
    kRBracket,
    kComma,
    kColon,
    kSemicolon,
    kDot,

    kAssign,
    kArrow, // ->
    kPlus,
    kMinus,
    kStar,
    kSlash,
    kAndAnd,
    kOrOr,
    kAmp,
    kEqEq,
    kBangEq,
    kBang,
    kColonColon,
};

struct SourceLoc {
    uint32_t line = 1;
    uint32_t column = 1;
};

struct Token {
    TokenKind kind = TokenKind::kError;
    std::string lexeme;
    SourceLoc loc{};
};

constexpr std::string_view token_kind_name(TokenKind k) {
    switch (k) {
        case TokenKind::kEof: return "eof";
        case TokenKind::kError: return "error";
        case TokenKind::kIdent: return "ident";
        case TokenKind::kIntLit: return "int_lit";
        case TokenKind::kFloatLit: return "float_lit";
        case TokenKind::kStringLit: return "string_lit";
        case TokenKind::kKwImport: return "import";
        case TokenKind::kKwFrom: return "from";
        case TokenKind::kKwExport: return "export";
        case TokenKind::kKwProto: return "proto";
        case TokenKind::kKwPlan: return "plan";
        case TokenKind::kKwLet: return "let";
        case TokenKind::kKwVar: return "var";
        case TokenKind::kKwDef: return "def";
        case TokenKind::kKwAssert: return "assert";
        case TokenKind::kKwIf: return "if";
        case TokenKind::kKwElse: return "else";
        case TokenKind::kKwTrue: return "true";
        case TokenKind::kKwFalse: return "false";
        case TokenKind::kKwInt: return "int";
        case TokenKind::kKwFloat: return "float";
        case TokenKind::kKwString: return "string";
        case TokenKind::kKwBool: return "bool";
        case TokenKind::kKwReturn: return "return";
        case TokenKind::kKwFor: return "for";
        case TokenKind::kKwIn: return "in";
        case TokenKind::kLParen: return "(";
        case TokenKind::kRParen: return ")";
        case TokenKind::kLBrace: return "{";
        case TokenKind::kRBrace: return "}";
        case TokenKind::kLBracket: return "[";
        case TokenKind::kRBracket: return "]";
        case TokenKind::kComma: return ",";
        case TokenKind::kColon: return ":";
        case TokenKind::kSemicolon: return ";";
        case TokenKind::kDot: return ".";
        case TokenKind::kAssign: return "=";
        case TokenKind::kArrow: return "->";
        case TokenKind::kPlus: return "+";
        case TokenKind::kMinus: return "-";
        case TokenKind::kStar: return "*";
        case TokenKind::kSlash: return "/";
        case TokenKind::kAndAnd: return "&&";
        case TokenKind::kOrOr: return "||";
        case TokenKind::kAmp: return "&";
        case TokenKind::kEqEq: return "==";
        case TokenKind::kBangEq: return "!=";
        case TokenKind::kBang: return "!";
        case TokenKind::kColonColon: return "::";
    }
    return "unknown";
}

} // namespace lei::syntax
