// compiler/include/gaupel/ast/Nodes.hpp
#pragma once
#include <gaupel/text/Span.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <cstdint>
#include <string_view>
#include <vector>


namespace gaupel::ast {

    // --------------------
    // Node Ids
    // --------------------
    using ExprId = uint32_t;
    inline constexpr ExprId k_invalid_expr = 0xFFFF'FFFFu;

    using StmtId = uint32_t;
    inline constexpr StmtId k_invalid_stmt = 0xFFFF'FFFFu;

    using TypeId = uint32_t;
    inline constexpr TypeId k_invalid_type = 0xFFFF'FFFFu;


    // --------------------
    // Expr
    // --------------------
    enum class ExprKind : uint8_t {
        kError,

        // literals / primary
        kIntLit,
        kFloatLit,
        kStringLit,
        kCharLit,
        kBoolLit,
        kNullLit,
        kIdent,
        kHole,     // "_" expression (특히 pipe-hole / call hole 용)

        // operators
        kUnary,
        kPostfixUnary,
        kBinary,
        kAssign,
        kTernary,

        // postfix
        kCall,
        kIndex,

        // loop
        kLoop,
    };

    // --------------------
    // Type
    // --------------------
    enum class TypeKind : uint8_t {
        kError,
        kNamed,     // v0: Ident 기반 NamedType만 지원
        kArray,     // T[]
        kOptional,  // T?
    };

    // --------------------
    // Stmt
    // --------------------
    enum class StmtKind : uint8_t {
        kError,
        kEmpty,       // ';'
        kExprStmt,    // expr ';'
        kBlock,       // '{' ... '}'

        // stmt/decl 혼용이 가능한 언어지만, 의미적으로는 "decl 성격"인 stmt도 존재
        kVar,         // let/set
        kIf,
        kWhile,
        kReturn,
        kBreak,
        kContinue,

        // decl-like
        kFnDecl,
    };

    // --------------------
    // Call Args
    // --------------------
    enum class ArgKind : uint8_t {
        kPositional,
        kLabeled,
        kNamedGroup, // call-site "{ ... }" 그룹 자체
    };

    struct Arg {
        ArgKind kind = ArgKind::kPositional;

        bool has_label = false;
        bool is_hole = false;     // label: _ (hole payload)
        std::string_view label{};
        ExprId expr = k_invalid_expr;

        // for NamedGroup
        uint32_t child_begin = 0;
        uint32_t child_count = 0;

        Span span{};
    };

    // --------------------
    // Attributes
    // --------------------
    struct Attr {
        std::string_view name; // "@pure" -> "pure"
        Span span;
    };

    // --------------------
    // Function Params
    // --------------------
    struct Param {
        std::string_view name{};
        TypeId type = k_invalid_type;

        // default 값: "= Expr"
        bool has_default = false;
        ExprId default_expr = k_invalid_expr;

        // 함수 선언의 named-group({}) 내부 파라미터인지 여부
        bool is_named_group = false;

        Span span{};
    };

    // --------------------
    // Expr/Type/Stmt nodes
    // --------------------
    struct Expr {
        ExprKind kind{};
        Span span{};

        // generic slots (kind에 따라 해석)
        syntax::TokenKind op = syntax::TokenKind::kError;
        ExprId a = k_invalid_expr;
        ExprId b = k_invalid_expr;
        ExprId c = k_invalid_expr;

        // literals / identifiers
        std::string_view text{};

        // call args storage (Arg 배열 slice)
        uint32_t arg_begin = 0;
        uint32_t arg_count = 0;

        // Loop expr
        bool loop_has_header = false;      // loop (v in xs) { ... }
        std::string_view loop_var{};       // v
        ExprId loop_iter = k_invalid_expr; // xs (또는 range expr)
        StmtId loop_body = k_invalid_stmt; // '{ ... }' block stmt id
    };

    struct Type {
        TypeKind kind{};
        Span span{};
        std::string_view text{}; // for kNamed

        // NOTE: suffix types share this child slot:
        // - kArray: elem = element type
        // - kOptional: elem = inner type
        TypeId elem = k_invalid_type;
    };

    // --------------------
    // Function Decl Mode
    // --------------------
    enum class FnMode : uint8_t {
        kNone = 0,
        kPub,
        kSub,
    };

    struct Stmt {
        StmtKind kind{};
        Span span{};

        // ---- stmt 공통 ----
        ExprId expr = k_invalid_expr; // ExprStmt 등

        // control-flow link
        StmtId a = k_invalid_stmt;    // If: then block / While: body block / FnDecl: body block
        StmtId b = k_invalid_stmt;    // If: else block

        // block children slice (stmt_children_에 대한 범위)
        uint32_t stmt_begin = 0;
        uint32_t stmt_count = 0;

        // ---- var ----
        bool is_set = false;          // false=let, true=set
        bool is_mut = false;
        std::string_view name{};
        TypeId type = k_invalid_type;
        ExprId init = k_invalid_expr;

        // ---- fn decl ----
        uint32_t attr_begin = 0;
        uint32_t attr_count = 0;

        bool is_export = false;
        
        FnMode fn_mode = FnMode::kNone;

        bool is_pure = false;         // qualifier 키워드형
        bool is_comptime = false;     // qualifier 키워드형

        // NOTE: "commit/recast" 같은 decl-qualifier를 확장 대비로 저장
        bool is_commit = false;
        bool is_recast = false;

        bool is_throwing = false;     // name?

        uint32_t param_begin = 0;
        uint32_t param_count = 0;

        // [param_begin, param_begin+positional_param_count) : positional
        // 나머지: named-group
        uint32_t positional_param_count = 0;
        bool has_named_group = false;
    };

    // --------------------
    // Arena
    // --------------------
    class AstArena {
    public:
        ExprId add_expr(const Expr& e) { exprs_.push_back(e); return static_cast<ExprId>(exprs_.size() - 1); }
        StmtId add_stmt(const Stmt& s) { stmts_.push_back(s); return static_cast<StmtId>(stmts_.size() - 1); }
        TypeId add_type(const Type& t) { types_.push_back(t); return static_cast<TypeId>(types_.size() - 1); }

        uint32_t add_arg(const Arg& a) { args_.push_back(a); return static_cast<uint32_t>(args_.size() - 1); }
        uint32_t add_named_group_arg(const Arg& a) { named_group_args_.push_back(a); return static_cast<uint32_t>(named_group_args_.size() - 1); }

        void add_fn_attr(const Attr& a) { fn_attrs_.push_back(a); }
        uint32_t add_param(const Param& p) { params_.push_back(p); return static_cast<uint32_t>(params_.size() - 1); }

        uint32_t add_stmt_child(StmtId id) { stmt_children_.push_back(id); return static_cast<uint32_t>(stmt_children_.size() - 1); }

        // accessors
        const Expr& expr(ExprId id) const { return exprs_[id]; }
        Expr& expr_mut(ExprId id) { return exprs_[id]; }
        const std::vector<Expr>& exprs() const { return exprs_; }

        const Type& type_node(TypeId id) const { return types_[id]; }
        std::vector<Type>& types_mut() { return types_; }
        const std::vector<Type>& types() const { return types_; }

        const Stmt& stmt(StmtId id) const { return stmts_[id]; }
        Stmt& stmt_mut(StmtId id) { return stmts_[id]; }
        const std::vector<Stmt>& stmts() const { return stmts_; }

        const std::vector<Arg>& args() const { return args_; }
        std::vector<Arg>& args_mut() { return args_; }

        const std::vector<Arg>& named_group_args() const { return named_group_args_; }
        std::vector<Arg>& named_group_args_mut() { return named_group_args_; }

        const std::vector<Attr>& fn_attrs() const { return fn_attrs_; }
        std::vector<Attr>& fn_attrs_mut() { return fn_attrs_; }

        const std::vector<Param>& params() const { return params_; }
        std::vector<Param>& params_mut() { return params_; }

        const std::vector<StmtId>& stmt_children() const { return stmt_children_; }
        std::vector<StmtId>& stmt_children_mut() { return stmt_children_; }

    private:
        std::vector<Expr> exprs_;
        std::vector<Stmt> stmts_;
        std::vector<Arg>  args_;
        std::vector<Arg>  named_group_args_;

        std::vector<Attr> fn_attrs_;
        std::vector<Type> types_;
        std::vector<Param> params_;

        std::vector<StmtId> stmt_children_;
    };

} // namespace gaupel::ast