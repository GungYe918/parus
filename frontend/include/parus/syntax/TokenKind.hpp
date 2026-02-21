// frontend/include/parus/syntax/TokenKind.hpp
#pragma once
#include <string_view>
#include <cstdint>
#include <type_traits>

namespace parus::syntax {

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
        kCharLit,     // 'C', '\n', '한', ...

        // keywords (subset for expressions / decls)
        kKwTrue,
        kKwFalse,
        kKwNull,

        kKwAnd,
        kKwOr,
        kKwNot,
        kKwXor,

        kKwMut,
        kKwStatic,
        // NOTE(스펙 변경): ref 토큰은 삭제됨.

        // stmt keywords
        kKwLet,
        kKwSet,
        kKwIf,
        kKwElif,
        kKwElse,
        kKwWhile,
        kKwDo,
        kKwReturn,
        kKwBreak,
        kKwContinue,
        kKwManual,

        // ---- keywords (decl / control) ----
        kKwExport,
        kKwExtern,
        kKwLayout,
        kKwAlign,
        kKwFn,
        kKwField,
        kKwActs,
        kKwClass,
        kKwSwitch,
        kKwCase,
        kKwDefault,
        kKwLoop,
        kKwIn,
        kKwCommit,
        kKwRecast,
        kKwPub,
        kKwSub,
        kKwPure,
        kKwComptime,

        kKwUse,
        kKwImport,
        kKwModule,
        kKwAs,
        kKwNest,

        // punct / delimiters
        kAt,       // @
        kDollar,   // $
        kArrow,    // ->
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
        kQuestionQuestion,        // ??     (null-coalescing)
        kQuestionQuestionAssign,  // ??=    (null-coalescing assign)
        kDot,       // .

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

        kAmp,      // &
        kAmpAmp,   // &&
        kPipePipe, // ||

        kEqEq,     // ==
        kBangEq,   // !=
        kLt,       // <
        kLtEq,     // <=
        kGt,       // >
        kGtEq,     // >=

        // deprecated. separated into shift/pipe
        // kLessLess, // <<   (pipe operator in Parus)

        kShiftLeft,   // <<  (bit shift)
        kShiftRight,  // >>  (bit shift)

        kPipeFwd,     // |>  (pipe forward)
        kPipeRev,     // <|  (pipe reverse)

        kDotDot,       // ..
        kDotDotColon,  // ..:
        kColonColon, // :: - path separator

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
            case TokenKind::kCharLit: return "char_lit";

            case TokenKind::kKwTrue: return "true";
            case TokenKind::kKwFalse: return "false";
            case TokenKind::kKwNull: return "null";
            case TokenKind::kKwAnd: return "and";
            case TokenKind::kKwOr: return "or";
            case TokenKind::kKwNot: return "not";
            case TokenKind::kKwXor: return "xor";
            case TokenKind::kKwMut: return "mut";
            case TokenKind::kKwStatic: return "static";

            case TokenKind::kKwFn: return "def";
            case TokenKind::kKwExport: return "export";
            case TokenKind::kKwExtern: return "extern";
            case TokenKind::kKwLayout: return "layout";
            case TokenKind::kKwAlign: return "align";
            case TokenKind::kKwField: return "field";
            case TokenKind::kKwActs: return "acts";
            case TokenKind::kKwLet: return "let";
            case TokenKind::kKwSet: return "set";
            case TokenKind::kKwIf:  return "if";
            case TokenKind::kKwElif: return "elif";
            case TokenKind::kKwElse: return "else";
            case TokenKind::kKwWhile: return "while";
            case TokenKind::kKwDo: return "do";
            case TokenKind::kKwLoop: return "loop";
            case TokenKind::kKwIn: return "in";
            case TokenKind::kKwReturn: return "return";
            case TokenKind::kKwBreak: return "break";
            case TokenKind::kKwContinue: return "continue";
            case TokenKind::kKwManual: return "manual";

            case TokenKind::kKwSwitch: return "switch";
            case TokenKind::kKwCase: return "case";
            case TokenKind::kKwDefault: return "default";

            case TokenKind::kKwPure: return "pure";
            case TokenKind::kKwComptime: return "comptime";

            case TokenKind::kKwUse: return "use";
            case TokenKind::kKwImport: return "import";
            case TokenKind::kKwModule: return "module";
            case TokenKind::kKwAs: return "as";
            case TokenKind::kKwNest: return "nest";

            case TokenKind::kKwPub: return "pub";
            case TokenKind::kKwSub: return "sub";
            case TokenKind::kKwCommit: return "commit";
            case TokenKind::kKwRecast: return "recast";

            case TokenKind::kKwClass: return "class";

            case TokenKind::kAt: return "@";
            case TokenKind::kDollar: return "$";
            case TokenKind::kArrow: return "->";
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
            case TokenKind::kQuestionQuestion: return "??";
            case TokenKind::kQuestionQuestionAssign: return "?" "?="; // "??=" but avoids trigraph
            case TokenKind::kDot: return ".";

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
            case TokenKind::kAmp:    return "&";
            case TokenKind::kPipePipe: return "||";

            case TokenKind::kEqEq: return "==";
            case TokenKind::kBangEq: return "!=";
            case TokenKind::kLt: return "<";
            case TokenKind::kLtEq: return "<=";
            case TokenKind::kGt: return ">";
            case TokenKind::kGtEq: return ">=";

            case TokenKind::kShiftLeft:  return "<<";
            case TokenKind::kShiftRight: return ">>";
            case TokenKind::kPipeFwd:    return "|>";
            case TokenKind::kPipeRev:    return "<|";
            case TokenKind::kDotDot: return "..";
            case TokenKind::kDotDotColon: return "..:";
            case TokenKind::kColonColon: return "::";

            case TokenKind::kUnknownPunct: return "unknown_punct";
        }

        return "unknown";
    }

} // namespace parus::syntax
