// compiler/include/gaupel/tyck/TypeCheck.hpp
#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/ty/TypePool.hpp>
#include <gaupel/sema/SymbolTable.hpp>
#include <gaupel/text/Span.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>


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

        // program(StmtId) 하나를 타입체크
        TyckResult check_program(ast::StmtId program_stmt);

    private:
        // --------------------
        // core
        // --------------------
        void first_pass_collect_top_level_(ast::StmtId program_stmt);
        void second_pass_check_program_(ast::StmtId program_stmt);

        // stmt
        void check_stmt_(ast::StmtId sid);
        void check_stmt_block_(const ast::Stmt& s);
        void check_stmt_var_(const ast::Stmt& s);
        void check_stmt_if_(const ast::Stmt& s);
        void check_stmt_while_(const ast::Stmt& s);
        void check_stmt_return_(const ast::Stmt& s);
        void check_stmt_switch_(const ast::Stmt& s);
        void check_stmt_fn_decl_(const ast::Stmt& s);

        // expr
        ty::TypeId check_expr_(ast::ExprId eid);
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

        // --------------------
        // helpers
        // --------------------
        void err_(Span sp, std::string msg);

        bool is_place_expr_(ast::ExprId eid) const;

        bool is_optional_(ty::TypeId t) const;
        ty::TypeId optional_elem_(ty::TypeId opt) const;

        bool is_null_(ty::TypeId t) const;
        bool is_error_(ty::TypeId t) const;

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
    };

} // namespace gaupel::tyck