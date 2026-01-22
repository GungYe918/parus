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

    using TypeId = uint32_t;
    inline constexpr TypeId k_invalid_type = 0xFFFF'FFFFu;


    enum class ExprKind : uint8_t {
        kError,
        kIntLit,
        kFloatLit,
        kStringLit,
        kCharLit,
        kBoolLit,
        kNullLit,
        kIdent,
        kHole,     // "_" expression (only meaningful inside call args)
        kUnary,
        kPostfixUnary,
        kBinary,
        kAssign,
        kTernary,
        kCall,
        kIndex,
    };

    enum class TypeKind : uint8_t {
        kError,
        kNamed,
    };

    enum class StmtKind : uint8_t {
        kError,
        kEmpty,       // ';'
        kExprStmt,    // expr ';'
        kBlock,       // '{' ... '}'
        
        kVar,         // let/set variable declaration 
        kIf,          // if cond block else block?
        kWhile,       // while cond block
        kReturn,      // return expr? ';'
        kBreak,       // break ';'
        kContinue,    // continue ';'
        kFnDecl,
    };

    struct Arg {
        bool has_label = false;
        std::string_view label{};
        bool is_hole = false;     // label: _  (hole payload)
        ExprId expr = k_invalid_expr;
        Span span{};
    };

    struct Param {
        std::string_view name{};
        TypeId type = k_invalid_type;
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

    struct Type {
        TypeKind kind{};
        Span span{};
        std::string_view text{};    // for kNamed: raw token text view (e.g. "u32")
    };

    struct Stmt {
        StmtKind kind{};
        Span span{};

        // generic slots (interpret by kind)
        ExprId expr = k_invalid_expr;     // for ExprStmt

        // extra stmt links
        StmtId a = k_invalid_stmt;      // If: then block, While: body block
        StmtId b = k_invalid_stmt;      // If: else block (or invalid)


        uint32_t stmt_begin = 0;          // for Block (stmt list)
        uint32_t stmt_count = 0;          // for Block

        // Var payload (let/set)
        bool is_set = false;            // false=let, true=set
        bool is_mut = false;
        std::string_view name{};        // identifier

        TypeId type = k_invalid_type;   // let requires; set forbids in v0
        ExprId init = k_invalid_expr;   // initializer expression (set requires in v0)

        bool is_export = false;
        bool is_pure = false;
        bool is_comptime = false;
        bool is_throwing = false;

        uint32_t param_begin = 0;
        uint32_t param_count = 0;
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

        TypeId add_type(const Type& t) {
            types_.push_back(t);
            return static_cast<TypeId>(types_.size() - 1);
        }

        uint32_t add_arg(const Arg& a) {
            args_.push_back(a);
            return static_cast<uint32_t>(args_.size() - 1);
        }

        uint32_t add_param(const Param& p) {
            params_.push_back(p);
            return static_cast<uint32_t>(params_.size() - 1);
        }

        uint32_t add_stmt_child(StmtId id) {
            stmt_children_.push_back(id);
            return static_cast<uint32_t>(stmt_children_.size() - 1);
        }


        const Expr& expr(ExprId id) const       {  return exprs_[id];  }
        Expr& expr_mut(ExprId id)               {  return exprs_[id];  }
        const std::vector<Expr>& exprs() const  {  return exprs_;  }
        
        const Type& type_node(TypeId id) const  {  return types_[id];   }
        std::vector<Type>& types_mut()          {  return types_;       }
        const std::vector<Type>& types() const  {  return types_;       }

        const std::vector<Arg>& args()   const  {  return args_;   }

        const Stmt& stmt(StmtId id) const       {  return stmts_[id];  }
        Stmt& stmt_mut(StmtId id)               {  return stmts_[id];  }
        const std::vector<StmtId>& stmt_children() const {  return stmt_children_;  }
        const std::vector<Stmt>& stmts() const  {  return stmts_;  }

        const std::vector<Param>& params() const { return params_; }
    
    private:
        std::vector<Expr>   exprs_;
        std::vector<Stmt>   stmts_;
        std::vector<Arg>    args_;
        std::vector<Type>   types_;
        std::vector<Param>  params_;

        std::vector<StmtId> stmt_children_;
    };

} // namespace gaupel::ast