// compiler/include/slyte/ast/Nodes.hpp
#pragma once
#include <slyte/text/Span.hpp>
#include <slyte/syntax/TokenKind.hpp>

#include <cstdint>
#include <string_view>
#include <vector>


namespace slyte::ast {

    using ExprId = uint32_t;
    inline constexpr ExprId k_invalid_expr = 0xFFFF'FFFFu;

    enum class ExprKind : uint8_t {
        kIntLit,
        kFloatLit,
        kStringLit,
        kBoolLit,
        kNullLit,
        kIdent,
        kHole,     // "_" expression (only meaningful inside call args)
        kUnary,
        kBinary,
        kTernary,
        kCall,
        kIndex,
    };

    struct Arg {
        bool has_label = false;
        std::string_view label{};
        bool is_hole = false;     // label: _  (hole payload)
        ExprId expr = k_invalid_expr;
        Span span{};
    };

    struct Expr {
        ExprKind kind{};
        Span span{};

        // generic slots (interpret by kind)
        syntax::TokenKind op = syntax::TokenKind::kError;
        ExprId a = k_invalid_expr;
        ExprId b = k_invalid_expr;
        ExprId c = k_invalid_expr;

        // literals / identifiers
        std::string_view text{};

        // call/index arg storage
        uint32_t arg_begin = 0;
        uint32_t arg_count = 0;
    };      

    class AstArena {
    public:
        ExprId add_expr(const Expr& e) {
            exprs_.push_back(e);
            return static_cast<ExprId>(exprs_.size() - 1);
        }

        uint32_t add_arg(const Arg& a) {
            args_.push_back(a);
            return static_cast<uint32_t>(args_.size() - 1);
        }

        const Expr& expr(ExprId id) const   {  return exprs_[id];  }
        Expr& expr_mut(ExprId id)           {  return exprs_[id];  }

        const std::vector<Expr>& exprs() const {  return exprs_;  }
        const std::vector<Arg>& args()   const {  return args_;   }
    
    private:
        std::vector<Expr> exprs_;
        std::vector<Arg> args_;
    };

} // namespace slyte::ast