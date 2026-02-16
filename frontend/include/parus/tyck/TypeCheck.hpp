// frontend/include/parus/tyck/TypeCheck.hpp
#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/sema/SymbolTable.hpp>
#include <parus/text/Span.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/num/BigInt.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map> 
#include <unordered_set>


namespace parus::tyck {

    // tyck의 에러는 우선 compile-safe한 독립 포맷으로 저장
    // (diag::Bag 연동은 프로젝트 내 Diagnostic API가 확정되면 쉽게 브릿지 가능)
    struct TyError {
        Span span{};
        std::string message{};
    };

    struct TyckResult {
        bool ok = true;
        std::vector<ty::TypeId> expr_types; // ast.exprs() index에 대응
        std::vector<ast::StmtId> expr_overload_target; // expr index -> selected decl (call/operator), invalid if builtin path
        std::vector<TyError> errors;
    };

    class TypeChecker {
    public:
        TypeChecker(ast::AstArena& ast, ty::TypePool& types)
            : ast_(ast), types_(types) {}

        TypeChecker(ast::AstArena& ast, ty::TypePool& types, diag::Bag& bag)
            : ast_(ast), types_(types), diag_bag_(&bag) {}

        void bind_diag(diag::Bag& bag) { diag_bag_ = &bag; }

        // program(StmtId) 하나를 타입체크
        TyckResult check_program(ast::StmtId program_stmt);

    private:

        // --------------------
        // Slot (value/discard)
        // --------------------
        enum class Slot : uint8_t {
            kValue,    // value is required
            kDiscard,  // value can be discarded
        };

        // --------------------
        // Loop context stack (break type collection)
        // --------------------
        struct LoopCtx {
            bool has_any_break = false;        // break; or break expr;
            bool has_value_break = false;      // break expr; existed
            bool has_null_break = false;       // break; existed
            bool may_natural_end = false;      // iter-loop natural end -> null
            ty::TypeId joined_value = ty::kInvalidType; // join of break expr types
        };

        std::vector<LoopCtx> loop_stack_;
        uint32_t stmt_loop_depth_ = 0; // while/loop-stmt style depth (non-value loops)

        // --------------------
        // core
        // --------------------
        void first_pass_collect_top_level_(ast::StmtId program_stmt);
        void second_pass_check_program_(ast::StmtId program_stmt);

        // stmt
        void check_stmt_(ast::StmtId sid);
        void check_stmt_block_(const ast::Stmt& s);
        void check_stmt_var_(ast::StmtId sid);
        void check_stmt_if_(const ast::Stmt& s);
        void check_stmt_while_(const ast::Stmt& s);
        void check_stmt_do_scope_(const ast::Stmt& s);
        void check_stmt_do_while_(const ast::Stmt& s);
        void check_stmt_manual_(const ast::Stmt& s);
        void check_stmt_return_(const ast::Stmt& s);
        void check_stmt_switch_(const ast::Stmt& s);
        void check_stmt_fn_decl_(const ast::Stmt& s);
        void check_stmt_field_decl_(ast::StmtId sid);
        void check_stmt_acts_decl_(const ast::Stmt& s);

        // expr
        ty::TypeId check_expr_(ast::ExprId eid);
        ty::TypeId check_expr_(ast::ExprId eid, Slot slot);
        ty::TypeId check_expr_unary_(const ast::Expr& e);
        ty::TypeId check_expr_postfix_unary_(const ast::Expr& e);
        ty::TypeId check_expr_binary_(const ast::Expr& e);
        ty::TypeId check_expr_assign_(const ast::Expr& e);
        ty::TypeId check_expr_ternary_(const ast::Expr& e);
        ty::TypeId check_expr_call_(const ast::Expr& e);
        ty::TypeId check_expr_index_(const ast::Expr& e);
        ty::TypeId check_expr_array_lit_(const ast::Expr& e);
        ty::TypeId check_expr_if_(const ast::Expr& e);
        ty::TypeId check_expr_block_(const ast::Expr& e);
        ty::TypeId check_expr_loop_(const ast::Expr& e);
        ty::TypeId check_expr_cast_(const ast::Expr& e);

        ty::TypeId check_expr_if_(const ast::Expr& e, Slot slot);    // overload
        ty::TypeId check_expr_block_(const ast::Expr& e, Slot slot); // overload
        ty::TypeId check_expr_loop_(const ast::Expr& e, Slot slot);  // overload


        // --------------------
        // helpers
        // --------------------

        void err_(Span sp, std::string msg);

        // 코드/args 기반 진단 헬퍼
        void diag_(diag::Code code, Span sp);
        void diag_(diag::Code code, Span sp, std::string_view a0);
        void diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1);
        void diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2);

        std::optional<uint32_t> root_place_symbol_(ast::ExprId place) const;
        bool is_mutable_symbol_(uint32_t sym_id) const;

        bool is_place_expr_(ast::ExprId eid) const;
        bool is_range_expr_(ast::ExprId eid) const;
        bool is_index_int_type_(ty::TypeId t) const;

        bool is_optional_(ty::TypeId t) const;
        ty::TypeId optional_elem_(ty::TypeId opt) const;

        bool is_null_(ty::TypeId t) const;
        bool is_error_(ty::TypeId t) const;

        bool in_loop_() const { return !loop_stack_.empty() || stmt_loop_depth_ != 0; }
        void note_break_(ty::TypeId t, bool is_value_break);

        // "대입/초기화" 호환성:
        // - exact match OK
        // - null -> T? OK
        // - T -> T? (암시적 승격은 일단 OFF: 필요하면 ON으로 바꾸기 쉬움)
        bool can_assign_(ty::TypeId dst, ty::TypeId src) const;

        // 두 타입을 하나로 합치기(삼항/if-expr 등):
        // - 같으면 그대로
        // - null + T? => T?
        // - null + T  => T? (여기서는 “유틸”로만 제공. 정책은 쉽게 바꿀 수 있음)
        ty::TypeId unify_(ty::TypeId a, ty::TypeId b);

        // 함수 컨텍스트
        struct FnCtx {
            bool in_fn = false;
            bool is_pure = false;
            bool is_comptime = false;
            ty::TypeId ret = ty::kInvalidType;
        };
        FnCtx fn_ctx_{};

        // 심볼 테이블
        sema::SymbolTable sym_;

        // 결과 저장소
        TyckResult result_{};

        // expr_types 캐시
        std::vector<ty::TypeId> expr_type_cache_;
        std::vector<ast::StmtId> expr_overload_target_cache_;
        ast::ExprId current_expr_id_ = ast::k_invalid_expr;

        // for "string literal" placeholder type
        ty::TypeId string_type_ = ty::kInvalidType;


        ast::AstArena& ast_;
        ty::TypePool& types_;
        diag::Bag* diag_bag_ = nullptr;

        // ----------------------------------------
        // Deferred integer inference
        // ----------------------------------------
        struct PendingInt {
            num::BigInt value{};
            bool has_value = false;     // literal-backed only
            bool resolved = false;
            ty::TypeId resolved_type = ty::kInvalidType;
        };

        // By SymbolId: set x = <int literal> ;  (var-level origin)
        std::unordered_map<uint32_t, PendingInt> pending_int_sym_;

        // By ExprId: any integer literal expression (and optionally propagated)
        std::unordered_map<uint32_t, PendingInt> pending_int_expr_;

        // (TypeChecker private) inferred integer("{integer}")를 expected 정수 타입으로 확정 시도
        bool resolve_infer_int_in_context_(ast::ExprId eid, ty::TypeId expected);
        bool infer_int_value_of_expr_(ast::ExprId eid, num::BigInt& out) const;
        static bool fits_builtin_int_big_(const num::BigInt& v, ty::Builtin dst);
        static bool is_field_pod_value_type_(const ty::TypePool& types, ty::TypeId id);

        // ----------------------------------------
        // Mut tracking (tyck-level)
        // ----------------------------------------
        std::unordered_map<uint32_t, bool> sym_is_mut_; // SymbolId -> is_mut

        // name -> overloaded fn decl stmt ids (top-level only)
        // NOTE: std::string을 key로 쓰는 이유:
        // - string_view는 AST storage lifetime에 의존하는데,
        //   향후 AST arena의 내부 저장 방식이 바뀌면 위험해질 수 있음.
        std::unordered_map<std::string, std::vector<ast::StmtId>> fn_decl_by_name_;

        struct ActsOperatorDecl {
            ast::StmtId fn_sid = ast::k_invalid_stmt;
            ty::TypeId owner_type = ty::kInvalidType;
            syntax::TokenKind op_token = syntax::TokenKind::kError;
            bool is_postfix = false;
        };
        std::unordered_map<uint64_t, std::vector<ActsOperatorDecl>> acts_default_operator_map_;

        static uint64_t acts_operator_key_(ty::TypeId owner_type, syntax::TokenKind op_token, bool is_postfix);
        void collect_acts_operator_decl_(const ast::Stmt& acts_decl);
        ast::StmtId resolve_binary_operator_overload_(syntax::TokenKind op, ty::TypeId lhs, ty::TypeId rhs) const;
        ast::StmtId resolve_postfix_operator_overload_(syntax::TokenKind op, ty::TypeId lhs) const;
        static bool type_matches_acts_owner_(const ty::TypePool& types, ty::TypeId owner, ty::TypeId actual);

        bool is_c_abi_safe_type_(ty::TypeId t, bool allow_void) const;
        bool is_c_abi_safe_type_impl_(ty::TypeId t, bool allow_void, std::unordered_set<ty::TypeId>& visiting) const;
        void check_c_abi_global_decl_(const ast::Stmt& s);

        struct FieldAbiMeta {
            ast::StmtId sid = ast::k_invalid_stmt;
            ast::FieldLayout layout = ast::FieldLayout::kNone;
            uint32_t align = 0;
        };

        std::unordered_map<ty::TypeId, FieldAbiMeta> field_abi_meta_by_type_;

    };

} // namespace parus::tyck
