// frontend/include/parus/ast/Nodes.hpp
#pragma once
#include <parus/text/Span.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/ty/Type.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


namespace parus::ast {

    // --------------------
    // Node Ids
    // --------------------
    using ExprId = uint32_t;
    inline constexpr ExprId k_invalid_expr = 0xFFFF'FFFFu;

    using StmtId = uint32_t;
    inline constexpr StmtId k_invalid_stmt = 0xFFFF'FFFFu;

    // NOTE:
    // - TypeId/Type struct/TypeKind are owned by parus::ty.
    // - AST only stores ty::TypeId as a reference.
    using TypeId = parus::ty::TypeId;
    inline constexpr TypeId k_invalid_type = parus::ty::kInvalidType;


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
        kArrayLit,
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
        
        kIfExpr,
        kBlockExpr,

        kCast,
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
        kDoScope,     // do { ... }
        kDoWhile,     // do { ... } while (cond);
        kReturn,
        kBreak,
        kContinue,

        // switch
        kSwitch,

        // decl-like
        kFnDecl,
        kFieldDecl,
        kActsDecl,

        // use
        kUse,
        kNestDecl,   // nest foo; / nest foo { ... }
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

        bool is_mut = false;
        bool is_self = false; // acts-for receiver marker: `self name: T`

        // default 값: "= Expr"
        bool has_default = false;
        ExprId default_expr = k_invalid_expr;

        // 함수 선언의 named-group({}) 내부 파라미터인지 여부
        bool is_named_group = false;

        Span span{};
    };

    enum class CasePatKind : uint8_t {
        kError,
        kInt,
        kChar,
        kString,
        kBool,
        kNull,
        kIdent,
    };

    struct SwitchCase {
        bool is_default = false;

        CasePatKind pat_kind = CasePatKind::kError;
        std::string_view pat_text{}; // literal/ident 원문 저장

        StmtId body = k_invalid_stmt; // 항상 block
        Span span{};
    };

    struct FfiField {
        TypeId type = k_invalid_type;
        std::string_view name{};
        Span span{};
    };

    struct FieldMember {
        TypeId type = k_invalid_type;
        std::string_view name{};
        Span span{};
    };

    enum class CastKind : uint8_t {
        kAs,        // expr as T
        kAsOptional,// expr as? T
        kAsForce,   // expr as! T
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

        // unary payload
        // - kUnary && op==kAmp 인 경우 "&mut x"를 표현하기 위해 사용
        bool unary_is_mut = false;

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

        // cast payload
        TypeId cast_type = k_invalid_type;
        CastKind cast_kind = CastKind::kAs;

        // -----------------------------------------
        // target/expected type (from tyck)
        //
        // - "이 expr이 놓인 자리에서 요구되는 타입"
        // - 실제 expr의 타입(tyck.expr_types)와는 다를 수 있다.
        //   예) return slot, assignment RHS slot, call arg slot, cast operand slot 등
        // - v0에선 optional 정규화/캐스팅 규칙/진단 메시지 강화에 특히 유용.
        // -----------------------------------------
        TypeId target_type = k_invalid_type;
    };

    // --------------------
    // Function Decl Mode
    // --------------------
    enum class FnMode : uint8_t {
        kNone = 0,
        kPub,
        kSub,
    };

    // use stmt
    enum class UseKind : uint8_t {
        kError,
        kImport,       // import foo [as alias];
        kTypeAlias,    // use NewT = u32;
        kPathAlias,    // use A::B = name;
        kTextSubst,    // use PI 3.14f;
        kFFIFunc,      // use func::ffi<sig> name;
        kFFIStruct,    // use struct::ffi Name { ... }
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
        bool is_static = false;
        std::string_view name{};
        TypeId type = k_invalid_type;
        ExprId init = k_invalid_expr;

        // ---- fn decl ----
        uint32_t attr_begin = 0;
        uint32_t attr_count = 0;

        bool is_export = false;
        
        FnMode fn_mode = FnMode::kNone;

        TypeId fn_ret = k_invalid_type;

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

        // fn/operator
        bool fn_is_operator = false; // true when declared as `operator(...)`
        syntax::TokenKind fn_operator_token = syntax::TokenKind::kError;
        bool fn_operator_is_postfix = false; // used for ++pre/++post disambiguation

        // ---- switch ----
        uint32_t case_begin = 0;
        uint32_t case_count = 0;
        bool has_default = false;

        // ---- field decl ----
        uint32_t field_member_begin = 0;
        uint32_t field_member_count = 0;

        // ---- acts decl ----
        bool acts_is_for = false;          // true: `acts for T` or `acts Name for T`
        bool acts_has_set_name = false;    // true: `acts Name for T`
        TypeId acts_target_type = k_invalid_type;

        // ---- use ----
        UseKind use_kind = UseKind::kError;

        // 공통: "use" 뒤 첫 ident (alias name / subst name / type alias name / ffi symbol name 등)
        std::string_view use_name{};

        // --- TypeAlias: name = TypeId (Stmt.type 사용) ---
        // --- TextSubst: name + expr (Stmt.expr 사용) ---
        // --- FFIFunc:  use_name + type(fn signature TypeId in Stmt.type) ---

        // PathAlias: path segments slice + rhs ident
        uint32_t use_path_begin = 0;
        uint32_t use_path_count = 0;
        std::string_view use_rhs_ident{}; // "= Ident" 의 Ident

        // ---- nest decl ----
        uint32_t nest_path_begin = 0;
        uint32_t nest_path_count = 0;
        bool nest_is_file_directive = false; // nest foo;

        // FFIStruct: struct name(use_name) + fields slice
        uint32_t use_field_begin = 0;
        uint32_t use_field_count = 0;
    };

    // --------------------
    // Arena
    // --------------------
    class AstArena {
    public:
        ExprId add_expr(const Expr& e) { exprs_.push_back(e); return static_cast<ExprId>(exprs_.size() - 1); }
        StmtId add_stmt(const Stmt& s) { stmts_.push_back(s); return static_cast<StmtId>(stmts_.size() - 1); }

        uint32_t add_arg(const Arg& a) { args_.push_back(a); return static_cast<uint32_t>(args_.size() - 1); }
        uint32_t add_named_group_arg(const Arg& a) { named_group_args_.push_back(a); return static_cast<uint32_t>(named_group_args_.size() - 1); }

        void add_fn_attr(const Attr& a) { fn_attrs_.push_back(a); }
        uint32_t add_param(const Param& p) {  params_.push_back(p); return static_cast<uint32_t>(params_.size() - 1);  }

        uint32_t add_switch_case(const SwitchCase& c) {  switch_cases_.push_back(c); return (uint32_t)switch_cases_.size() - 1;  }

        uint32_t add_ffi_field(const FfiField& f) {
            ffi_fields_.push_back(f);
            return (uint32_t)ffi_fields_.size() - 1;
        }

        uint32_t add_field_member(const FieldMember& f) {
            field_members_.push_back(f);
            return (uint32_t)field_members_.size() - 1;
        }

        std::string_view add_owned_string(std::string s) {
            owned_strings_.push_back(std::move(s));
            return owned_strings_.back();
        }

        uint32_t add_path_seg(std::string_view s) {
            path_segs_.push_back(s);
            return (uint32_t)path_segs_.size() - 1;
        }

        uint32_t add_stmt_child(StmtId id) {  stmt_children_.push_back(id); return static_cast<uint32_t>(stmt_children_.size() - 1);  }

        // accessors
        const Expr& expr(ExprId id) const { return exprs_[id]; }
        Expr& expr_mut(ExprId id) { return exprs_[id]; }
        const std::vector<Expr>& exprs() const { return exprs_; }

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

        const std::vector<SwitchCase>& switch_cases() const {  return switch_cases_;  }
        std::vector<SwitchCase>& switch_cases_mut() {  return switch_cases_;  }  

        const std::vector<FfiField>& ffi_fields() const { return ffi_fields_; }
        std::vector<FfiField>& ffi_fields_mut() { return ffi_fields_; }

        const std::vector<FieldMember>& field_members() const { return field_members_; }
        std::vector<FieldMember>& field_members_mut() { return field_members_; }

        const std::vector<std::string_view>& path_segs() const { return path_segs_; }
        std::vector<std::string_view>& path_segs_mut() { return path_segs_; }

        const std::vector<StmtId>& stmt_children() const { return stmt_children_; }
        std::vector<StmtId>& stmt_children_mut() { return stmt_children_; }

    private:
        std::vector<Expr> exprs_;
        std::vector<Stmt> stmts_;
        std::vector<Arg>  args_;
        std::vector<Arg>  named_group_args_;

        std::vector<Attr> fn_attrs_;
        std::vector<Param> params_;

        std::vector<SwitchCase> switch_cases_;
        std::vector<FfiField> ffi_fields_;
        std::vector<FieldMember> field_members_;
        std::vector<std::string> owned_strings_;
        std::vector<std::string_view> path_segs_;

        std::vector<StmtId> stmt_children_;
    };

} // namespace parus::ast
