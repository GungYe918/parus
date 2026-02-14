// compiler/include/gaupel/tyck/TypeCheck.hpp
#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/ty/TypePool.hpp>
#include <gaupel/sema/SymbolTable.hpp>
#include <gaupel/text/Span.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/num/BigInt.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map> 


namespace gaupel::tyck {

    // tyck의 에러는 우선 compile-safe한 독립 포맷으로 저장
    // (diag::Bag 연동은 프로젝트 내 Diagnostic API가 확정되면 쉽게 브릿지 가능)
    struct TyError {
        Span span{};
        std::string message{};
    };

    struct TyckResult {
        bool ok = true;
        std::vector<ty::TypeId> expr_types; // ast.exprs() index에 대응
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
        void check_stmt_return_(const ast::Stmt& s);
        void check_stmt_switch_(const ast::Stmt& s);
        void check_stmt_fn_decl_(const ast::Stmt& s);

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

        // ----------------------------------------
        // Mut tracking (tyck-level)
        // ----------------------------------------
        std::unordered_map<uint32_t, bool> sym_is_mut_; // SymbolId -> is_mut

        // name -> fn decl stmt id (top-level only)
        // NOTE: std::string을 key로 쓰는 이유:
        // - string_view는 AST storage lifetime에 의존하는데,
        //   향후 AST arena의 내부 저장 방식이 바뀌면 위험해질 수 있음.
        std::unordered_map<std::string, ast::StmtId> fn_decl_by_name_;

    };

} // namespace gaupel::tyck
