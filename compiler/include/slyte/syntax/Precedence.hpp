// compiler/include/syntax/Precedence.hpp
#pragma once
#include <slyte/syntax/TokenKind.hpp>

#include <optional>


namespace slyte::syntax {

    // Higher number = tighter binding.
    enum class Assoc : uint8_t {
        kLeft, kRight
    };

    struct InfixInfo {
        int prec;
        Assoc assoc;
    };

    struct PrefixInfo {
        int prec;
    };

    inline constexpr int k_prec_postfix = 100; // call(), index[]


    // Infix precedence roughly matches the spec table (C-like) with Slyte additions.
    constexpr std::optional<InfixInfo> infix_info(TokenKind k) {
        switch(k) {
            // assignment (right assoc)
            case TokenKind::kAssign:
            case TokenKind::kPlusAssign:
            case TokenKind::kMinusAssign:
            case TokenKind::kStarAssign:
            case TokenKind::kSlashAssign:
            case TokenKind::kPercentAssign: 
                return InfixInfo{10, Assoc::kRight};

            // ternary handled specially (not here)

            // pipe operator '<<' (left assoc, between assignment and logical)
            case TokenKind::kLessLess:
                return InfixInfo{20, Assoc::kLeft};

            // logical or
            case TokenKind::kPipePipe:
            case TokenKind::kKwOr:
                return InfixInfo{30, Assoc::kLeft};

            // logical xor
            case TokenKind::kCaret:
            case TokenKind::kKwXor:
                return InfixInfo{40, Assoc::kLeft};

            // logical and
            case TokenKind::kAmpAmp:
            case TokenKind::kKwAnd:
                return InfixInfo{50, Assoc::kLeft};

            // equality
            case TokenKind::kEqEq:
            case TokenKind::kBangEq:
                return InfixInfo{60, Assoc::kLeft};

            // relational
            case TokenKind::kLt:
            case TokenKind::kLtEq:
            case TokenKind::kGt:
            case TokenKind::kGtEq:
                return InfixInfo{70, Assoc::kLeft};

            // additive
            case TokenKind::kPlus:
            case TokenKind::kMinus:
                return InfixInfo{80, Assoc::kLeft};

            // multiplicative
            case TokenKind::kStar:
            case TokenKind::kSlash:
            case TokenKind::kPercent:
                return InfixInfo{90, Assoc::kLeft};

            default:
                return std::nullopt;
        }
    }

    constexpr std::optional<PrefixInfo> prefix_info(TokenKind k) {
        switch(k) {
            case TokenKind::kBang:
            case TokenKind::kKwNot:
            case TokenKind::kPlus:
            case TokenKind::kMinus:
                return PrefixInfo{95};
            default:
                return std::nullopt;
        }
    }

} // namespace slyte::syntax