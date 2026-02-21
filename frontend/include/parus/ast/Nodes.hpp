// frontend/include/parus/ast/Nodes.hpp
#pragma once
#include <parus/text/Span.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/lex/Token.hpp>
#include <parus/ty/Type.hpp>

#include <cstdint>
#include <deque>
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

    using TypeNodeId = uint32_t;
    inline constexpr TypeNodeId k_invalid_type_node = 0xFFFF'FFFFu;

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
        kFieldInit, // TypePath{ name: expr, ... }
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
        kMacroCall,

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
        kManual,      // manual[perm,...] { ... }
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

    inline constexpr uint8_t kManualPermGet = 1u << 0;
    inline constexpr uint8_t kManualPermSet = 1u << 1;
    inline constexpr uint8_t kManualPermAbi = 1u << 2;

    // --------------------
    // Call Args
    // --------------------
    enum class ArgKind : uint8_t {
        kPositional,
        kLabeled,
    };

    struct Arg {
        ArgKind kind = ArgKind::kPositional;

        bool has_label = false;
        bool is_hole = false;     // label: _ (hole payload)
        std::string_view label{};
        ExprId expr = k_invalid_expr;

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
    enum class SelfReceiverKind : uint8_t {
        kNone = 0,
        kRead,   // self      -> &Self
        kMut,    // self mut  -> &mut Self
        kMove,   // self move -> Self
    };

    struct Param {
        std::string_view name{};
        TypeId type = k_invalid_type;
        TypeNodeId type_node = k_invalid_type_node;

        bool is_mut = false;
        bool is_self = false; // receiver marker
        SelfReceiverKind self_kind = SelfReceiverKind::kNone;

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

    struct FieldMember {
        TypeId type = k_invalid_type;
        TypeNodeId type_node = k_invalid_type_node;
        std::string_view name{};
        Span span{};
    };

    struct FieldInitEntry {
        std::string_view name{};
        ExprId expr = k_invalid_expr;
        Span span{};
    };

    struct FStringPart {
        bool is_expr = false;
        std::string_view text{};
        ExprId expr = k_invalid_expr;
        Span span{};
    };

    enum class CastKind : uint8_t {
        kAs,        // expr as T
        kAsOptional,// expr as? T
        kAsForce,   // expr as! T
    };

    enum class TypeNodeKind : uint8_t {
        kError,
        kNamedPath,
        kOptional,
        kArray,
        kBorrow,
        kEscape,
        kPtr,
        kFn,
        kMacroCall,
    };

    struct TypeNode {
        TypeNodeKind kind = TypeNodeKind::kError;
        Span span{};

        // for named path
        uint32_t path_begin = 0;
        uint32_t path_count = 0;

        // common child for optional/array/borrow/escape/ptr
        TypeNodeId elem = k_invalid_type_node;

        // array
        bool array_has_size = false;
        uint32_t array_size = 0;

        // borrow/ptr
        bool is_mut = false;

        // fn type
        TypeNodeId fn_ret = k_invalid_type_node;
        uint32_t fn_param_begin = 0; // slice in AstArena::type_node_children_
        uint32_t fn_param_count = 0;

        // macro call
        uint32_t macro_path_begin = 0; // slice in AstArena::path_segs_
        uint32_t macro_path_count = 0;
        uint32_t macro_arg_begin = 0;  // slice in AstArena::macro_tokens_
        uint32_t macro_arg_count = 0;

        // optional cache after TypeResolve
        TypeId resolved_type = k_invalid_type;
    };

    enum class MacroMatchKind : uint8_t {
        kExpr,
        kStmt,
        kItem,
        kType,
        kToken,
    };

    enum class MacroOutKind : uint8_t {
        kExpr,
        kStmt,
        kItem,
        kType,
    };

    enum class MacroFragKind : uint8_t {
        kExpr,
        kStmt,
        kItem,
        kType,
        kPath,
        kIdent,
        kBlock,
        kTt,
    };

    struct MacroTypedCapture {
        std::string_view name{};
        MacroFragKind frag = MacroFragKind::kExpr;
        bool variadic = false;
        Span span{};
    };

    struct MacroArm {
        uint32_t capture_begin = 0; // slice in AstArena::macro_captures_
        uint32_t capture_count = 0;
        MacroOutKind out_kind = MacroOutKind::kExpr;
        uint32_t template_token_begin = 0; // slice in AstArena::macro_tokens_
        uint32_t template_token_count = 0;
        bool token_pattern = false; // reserved for Phase2
        Span span{};
    };

    struct MacroGroup {
        MacroMatchKind match_kind = MacroMatchKind::kExpr;
        uint32_t arm_begin = 0; // slice in AstArena::macro_arms_
        uint32_t arm_count = 0;
        bool phase2_token_group = false; // true when `with token` used
        Span span{};
    };

    struct MacroDecl {
        std::string_view name{};
        uint32_t group_begin = 0; // slice in AstArena::macro_groups_
        uint32_t group_count = 0;
        uint32_t scope_depth = 0;
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

        // unary payload
        // - kUnary && op==kAmp 인 경우 "&mut x"를 표현하기 위해 사용
        bool unary_is_mut = false;

        // literals / identifiers
        std::string_view text{};

        // string literal metadata
        bool string_is_raw = false;     // R"""..."""
        bool string_is_format = false;  // F"""..."""
        uint32_t string_part_begin = 0; // slice into AstArena::fstring_parts_
        uint32_t string_part_count = 0;
        // optional folded/normalized literal text for lowering (quoted literal form)
        std::string_view string_folded_text{};

        // call args storage (Arg 배열 slice)
        uint32_t arg_begin = 0;
        uint32_t arg_count = 0;

        // field init entries storage (FieldInitEntry 배열 slice)
        uint32_t field_init_begin = 0;
        uint32_t field_init_count = 0;

        // Loop expr
        bool loop_has_header = false;      // loop (v in xs) { ... }
        std::string_view loop_var{};       // v
        ExprId loop_iter = k_invalid_expr; // xs (또는 range expr)
        StmtId loop_body = k_invalid_stmt; // '{ ... }' block stmt id

        // cast payload
        TypeId cast_type = k_invalid_type;
        TypeNodeId cast_type_node = k_invalid_type_node;
        CastKind cast_kind = CastKind::kAs;

        // macro call payload
        uint32_t macro_path_begin = 0; // slice in AstArena::path_segs_
        uint32_t macro_path_count = 0;
        uint32_t macro_token_begin = 0; // slice in AstArena::macro_tokens_
        uint32_t macro_token_count = 0;

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

    /// @brief 선언의 외부 링크 ABI 종류(v0: 미지정/ C ABI)
    enum class LinkAbi : uint8_t {
        kNone = 0,
        kC,
    };

    /// @brief field 레이아웃 지정자(v0: 미지정/ C 레이아웃)
    enum class FieldLayout : uint8_t {
        kNone = 0,
        kC,
    };

    // use stmt
    enum class UseKind : uint8_t {
        kError,
        kImport,       // import foo [as alias];
        kTypeAlias,    // use NewT = u32;
        kPathAlias,    // use A::B = name;
        kNestAlias,    // use nest a::b [as alias];
        kTextSubst,    // use PI 3.14f;
        kActsEnable,   // use T with acts(NameOrDefault);
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
        bool is_extern = false;
        LinkAbi link_abi = LinkAbi::kNone;
        std::string_view name{};
        TypeId type = k_invalid_type;
        TypeNodeId type_node = k_invalid_type_node;
        ExprId init = k_invalid_expr;

        // ---- def decl ----
        uint32_t attr_begin = 0;
        uint32_t attr_count = 0;

        bool is_export = false;
        
        FnMode fn_mode = FnMode::kNone;

        TypeId fn_ret = k_invalid_type;
        TypeNodeId fn_ret_type_node = k_invalid_type_node;

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

        // def/operator
        bool fn_is_operator = false; // true when declared as `operator(...)`
        syntax::TokenKind fn_operator_token = syntax::TokenKind::kError;
        bool fn_operator_is_postfix = false; // used for ++pre/++post disambiguation

        // ---- switch ----
        uint32_t case_begin = 0;
        uint32_t case_count = 0;
        bool has_default = false;

        // ---- field decl ----
        FieldLayout field_layout = FieldLayout::kNone;
        uint32_t field_align = 0; // 0 means unspecified
        uint32_t field_member_begin = 0;
        uint32_t field_member_count = 0;

        // ---- acts decl ----
        bool acts_is_for = false;          // true: `acts for T` or `acts Name for T`
        bool acts_has_set_name = false;    // true: `acts Name for T`
        TypeId acts_target_type = k_invalid_type;
        TypeNodeId acts_target_type_node = k_invalid_type_node;

        // ---- use ----
        UseKind use_kind = UseKind::kError;

        // 공통: "use" 뒤 첫 ident (alias name / subst name / type alias name 등)
        std::string_view use_name{};

        // --- TypeAlias: name = TypeId (Stmt.type 사용) ---
        // --- TextSubst: name + expr (Stmt.expr 사용) ---

        // PathAlias: path segments slice + rhs ident
        uint32_t use_path_begin = 0;
        uint32_t use_path_count = 0;
        std::string_view use_rhs_ident{}; // "= Ident" 의 Ident

        // ---- var binding acts sugar ----
        // let/set ... = Expr with acts(NameOrDefault);
        bool var_has_acts_binding = false;
        bool var_acts_is_default = false;
        TypeId var_acts_target_type = k_invalid_type; // typed let에서만 파싱 시점 확정
        TypeNodeId var_acts_target_type_node = k_invalid_type_node;
        uint32_t var_acts_set_path_begin = 0;
        uint32_t var_acts_set_path_count = 0;
        std::string_view var_acts_set_name{};

        // ---- nest decl ----
        uint32_t nest_path_begin = 0;
        uint32_t nest_path_count = 0;
        bool nest_is_file_directive = false; // nest foo;

        // ---- manual stmt ----
        // bit0: get, bit1: set, bit2: abi
        uint8_t manual_perm_mask = 0;
    };

    // --------------------
    // Arena
    // --------------------
    class AstArena {
    public:
        ExprId add_expr(const Expr& e) { exprs_.push_back(e); return static_cast<ExprId>(exprs_.size() - 1); }
        StmtId add_stmt(const Stmt& s) { stmts_.push_back(s); return static_cast<StmtId>(stmts_.size() - 1); }
        TypeNodeId add_type_node(const TypeNode& t) {
            type_nodes_.push_back(t);
            return static_cast<TypeNodeId>(type_nodes_.size() - 1);
        }
        uint32_t add_type_node_child(TypeNodeId id) {
            type_node_children_.push_back(id);
            return static_cast<uint32_t>(type_node_children_.size() - 1);
        }

        uint32_t add_arg(const Arg& a) { args_.push_back(a); return static_cast<uint32_t>(args_.size() - 1); }

        void add_fn_attr(const Attr& a) { fn_attrs_.push_back(a); }
        uint32_t add_param(const Param& p) {  params_.push_back(p); return static_cast<uint32_t>(params_.size() - 1);  }

        uint32_t add_switch_case(const SwitchCase& c) {  switch_cases_.push_back(c); return (uint32_t)switch_cases_.size() - 1;  }

        uint32_t add_field_member(const FieldMember& f) {
            field_members_.push_back(f);
            return (uint32_t)field_members_.size() - 1;
        }

        uint32_t add_field_init_entry(const FieldInitEntry& f) {
            field_init_entries_.push_back(f);
            return (uint32_t)field_init_entries_.size() - 1;
        }

        uint32_t add_fstring_part(const FStringPart& p) {
            fstring_parts_.push_back(p);
            return (uint32_t)fstring_parts_.size() - 1;
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
        uint32_t add_macro_token(const Token& t) {
            macro_tokens_.push_back(t);
            return static_cast<uint32_t>(macro_tokens_.size() - 1);
        }
        uint32_t add_macro_capture(const MacroTypedCapture& c) {
            macro_captures_.push_back(c);
            return static_cast<uint32_t>(macro_captures_.size() - 1);
        }
        uint32_t add_macro_arm(const MacroArm& a) {
            macro_arms_.push_back(a);
            return static_cast<uint32_t>(macro_arms_.size() - 1);
        }
        uint32_t add_macro_group(const MacroGroup& g) {
            macro_groups_.push_back(g);
            return static_cast<uint32_t>(macro_groups_.size() - 1);
        }
        uint32_t add_macro_decl(const MacroDecl& d) {
            macro_decls_.push_back(d);
            return static_cast<uint32_t>(macro_decls_.size() - 1);
        }

        // accessors
        const Expr& expr(ExprId id) const { return exprs_[id]; }
        Expr& expr_mut(ExprId id) { return exprs_[id]; }
        const std::vector<Expr>& exprs() const { return exprs_; }
        std::vector<Expr>& exprs_mut() { return exprs_; }

        const Stmt& stmt(StmtId id) const { return stmts_[id]; }
        Stmt& stmt_mut(StmtId id) { return stmts_[id]; }
        const std::vector<Stmt>& stmts() const { return stmts_; }
        std::vector<Stmt>& stmts_mut() { return stmts_; }

        const TypeNode& type_node(TypeNodeId id) const { return type_nodes_[id]; }
        TypeNode& type_node_mut(TypeNodeId id) { return type_nodes_[id]; }
        const std::vector<TypeNode>& type_nodes() const { return type_nodes_; }
        std::vector<TypeNode>& type_nodes_mut() { return type_nodes_; }
        const std::vector<TypeNodeId>& type_node_children() const { return type_node_children_; }
        std::vector<TypeNodeId>& type_node_children_mut() { return type_node_children_; }

        const std::vector<Arg>& args() const { return args_; }
        std::vector<Arg>& args_mut() { return args_; }

        const std::vector<Attr>& fn_attrs() const { return fn_attrs_; }
        std::vector<Attr>& fn_attrs_mut() { return fn_attrs_; }

        const std::vector<Param>& params() const { return params_; }
        std::vector<Param>& params_mut() { return params_; }

        const std::vector<SwitchCase>& switch_cases() const {  return switch_cases_;  }
        std::vector<SwitchCase>& switch_cases_mut() {  return switch_cases_;  }  

        const std::vector<FieldMember>& field_members() const { return field_members_; }
        std::vector<FieldMember>& field_members_mut() { return field_members_; }

        const std::vector<FieldInitEntry>& field_init_entries() const { return field_init_entries_; }
        std::vector<FieldInitEntry>& field_init_entries_mut() { return field_init_entries_; }

        const std::vector<FStringPart>& fstring_parts() const { return fstring_parts_; }
        std::vector<FStringPart>& fstring_parts_mut() { return fstring_parts_; }

        const std::vector<std::string_view>& path_segs() const { return path_segs_; }
        std::vector<std::string_view>& path_segs_mut() { return path_segs_; }

        const std::vector<StmtId>& stmt_children() const { return stmt_children_; }
        std::vector<StmtId>& stmt_children_mut() { return stmt_children_; }
        const std::vector<Token>& macro_tokens() const { return macro_tokens_; }
        std::vector<Token>& macro_tokens_mut() { return macro_tokens_; }
        const std::vector<MacroTypedCapture>& macro_captures() const { return macro_captures_; }
        std::vector<MacroTypedCapture>& macro_captures_mut() { return macro_captures_; }
        const std::vector<MacroArm>& macro_arms() const { return macro_arms_; }
        std::vector<MacroArm>& macro_arms_mut() { return macro_arms_; }
        const std::vector<MacroGroup>& macro_groups() const { return macro_groups_; }
        std::vector<MacroGroup>& macro_groups_mut() { return macro_groups_; }
        const std::vector<MacroDecl>& macro_decls() const { return macro_decls_; }
        std::vector<MacroDecl>& macro_decls_mut() { return macro_decls_; }

    private:
        std::vector<Expr> exprs_;
        std::vector<Stmt> stmts_;
        std::vector<TypeNode> type_nodes_;
        std::vector<TypeNodeId> type_node_children_;
        std::vector<Arg>  args_;

        std::vector<Attr> fn_attrs_;
        std::vector<Param> params_;

        std::vector<SwitchCase> switch_cases_;
        std::vector<FieldMember> field_members_;
        std::vector<FieldInitEntry> field_init_entries_;
        std::vector<FStringPart> fstring_parts_;
        std::deque<std::string> owned_strings_;
        std::vector<std::string_view> path_segs_;

        std::vector<StmtId> stmt_children_;
        std::vector<Token> macro_tokens_;
        std::vector<MacroTypedCapture> macro_captures_;
        std::vector<MacroArm> macro_arms_;
        std::vector<MacroGroup> macro_groups_;
        std::vector<MacroDecl> macro_decls_;
    };

} // namespace parus::ast
