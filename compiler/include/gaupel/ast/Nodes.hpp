// compiler/include/gaupel/ast/Nodes.hpp
#pragma once
#include <gaupel/text/Span.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <cstdint>
#include <string_view>
#include <vector>


namespace gaupel::ast {

    using ExprId = uint32_t;
    inline constexpr ExprId k_invalid_expr = 0xFFFF'FFFFu;
    using StmtId = uint32_t;
    inline constexpr StmtId k_invalid_stmt = 0xFFFF'FFFFu;

    enum class ExprKind : uint8_t {
        kIntLit,
        kFloatLit,
        kStringLit,
        kBoolLit,
        kNullLit,
        kIdent,
        kHole,     // "_" expression (only meaningful inside call args)
        kUnary,
        kPostfixUnary,
        kBinary,
        kTernary,
        kCall,
        kIndex,
    };

    enum class StmtKind : uint8_t {
        kEmpty,       // ';'
        kExprStmt,    // expr ';'
        kBlock,       // '{' ... '}'
        // v0 확장 후보:
        // kLet, kIf, kWhile, kReturn, kBreak, kContinue ...
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

    struct Stmt {
        StmtKind kind{};
        Span span{};

        // generic slots (interpret by kind)
        ExprId expr = k_invalid_expr;     // for ExprStmt
        uint32_t stmt_begin = 0;          // for Block (stmt list)
        uint32_t stmt_count = 0;          // for Block
    };

    class AstArena {
    public:
        ExprId add_expr(const Expr& e) {
            exprs_.push_back(e);
            return static_cast<ExprId>(exprs_.size() - 1);
        }

        StmtId add_stmt(const Stmt& s) {
            stmts_.push_back(s);
            return static_cast<StmtId>(stmts_.size() -1);
        }

        uint32_t add_arg(const Arg& a) {
            args_.push_back(a);
            return static_cast<uint32_t>(args_.size() - 1);
        }

        uint32_t add_stmt_child(StmtId id) {
            stmt_children_.push_back(id);
            return static_cast<uint32_t>(stmt_children_.size() - 1);
        }


        const Expr& expr(ExprId id) const   {  return exprs_[id];  }
        Expr& expr_mut(ExprId id)           {  return exprs_[id];  }

        const std::vector<Expr>& exprs() const {  return exprs_;  }
        const std::vector<Arg>& args()   const {  return args_;   }

        const Stmt& stmt(StmtId id) const   {  return stmts_[id];  }
        Stmt& stmt_mut(StmtId id)           {  return stmts_[id];  }

        const std::vector<StmtId>& stmt_children() const { return stmt_children_; }
        const std::vector<Stmt>& stmts() const {  return stmts_;  }
    
    private:
        std::vector<Expr> exprs_;
        std::vector<Stmt> stmts_;
        std::vector<Arg> args_;

        std::vector<StmtId> stmt_children_;
    };

} // namespace gaupel::ast