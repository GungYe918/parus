// frontend/src/sir/lower/sir_builder_internal.hpp
#pragma once
#include <parus/sir/Builder.hpp>


namespace parus::sir::detail {

    TypeId active_builtin_type(parus::ty::Builtin b);
    bool type_is_escape_for_sir_build(TypeId t);
    TypeId array_elem_type_for_sir_build(TypeId t);

    /// @brief Tyck 결과에서 AST expression의 최종 타입을 조회한다.
    TypeId type_of_ast_expr(const tyck::TyckResult& tyck, parus::ast::ExprId eid);

    /// @brief Tyck cache가 비어 있어도 AST target_type / resolved symbol type로 보강 조회한다.
    TypeId best_effort_type_of_ast_expr(
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId eid
    );

    /// @brief AST expression 노드에서 name-resolve 심볼을 찾는다.
    SymbolId resolve_symbol_from_expr(
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId eid
    );

    /// @brief loop header binder에 대응하는 name-resolve 심볼을 찾는다.
    SymbolId resolve_loop_symbol_from_expr(
        const passes::NameResolveResult& nres,
        parus::ast::ExprId eid
    );
    SymbolId resolve_loop_symbol_from_stmt(
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    );

    /// @brief place expression의 root 심볼(ident/index.base)을 찾는다.
    SymbolId resolve_root_place_symbol_from_expr(
        const parus::ast::AstArena& ast,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId eid
    );

    /// @brief AST statement 노드에서 name-resolve 심볼을 찾는다.
    SymbolId resolve_symbol_from_stmt(
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid
    );
    SymbolId resolve_symbol_from_stmt(const passes::NameResolveResult& nres, parus::ast::StmtId sid);

    /// @brief AST 파라미터 인덱스에서 name-resolve 심볼을 찾는다.
    SymbolId resolve_symbol_from_param_index(
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        uint32_t param_index
    );

    /// @brief 동일 심볼의 사용 위치를 기준으로 선언 타입을 보강 추론한다.
    TypeId resolve_decl_type_from_symbol_uses(
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        SymbolId sym_id
    );

    void set_active_type_pool_for_sir_build_(ty::TypePool* types);

    /// @brief AST expression이 place(local/index/...)인지 분류한다.
    PlaceClass classify_place_from_ast(const parus::ast::AstArena& ast, parus::ast::ExprId eid);

    /// @brief ValueKind 기반의 효과 분류(Pure/MayWrite/Unknown)를 계산한다.
    EffectClass classify_effect(ValueKind k);

    /// @brief 두 효과 클래스를 보수적으로 합친다.
    EffectClass join_effect_(EffectClass a, EffectClass b);

    /// @brief SIR statement 단위의 효과를 계산한다.
    EffectClass effect_of_stmt_(const Module& m, const Stmt& s);

    /// @brief SIR block 단위의 효과를 계산한다.
    EffectClass effect_of_block_(const Module& m, BlockId bid);

    /// @brief ExprId 유효성 여부를 검사한다.
    bool is_valid_expr_id_(const parus::ast::AstArena& ast, parus::ast::ExprId id);

    /// @brief StmtId 유효성 여부를 검사한다.
    bool is_valid_stmt_id_(const parus::ast::AstArena& ast, parus::ast::StmtId id);

    /// @brief AST block stmt를 SIR block으로 lower한다.
    BlockId lower_block_stmt(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId block_sid
    );

    /// @brief AST block(+tail)을 SIR block-expression value로 lower한다.
    ValueId lower_block_value_(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId block_sid,
        parus::ast::ExprId tail_eid,
        parus::Span span,
        TypeId forced_type
    );

    /// @brief ExprId/StmtId quirk를 흡수하여 값 lowering 경로를 통일한다.
    ValueId lower_expr_or_stmt_as_value_(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId maybe_expr,
        parus::Span span,
        TypeId expected
    );

    /// @brief AST expression을 SIR value로 lower한다.
    ValueId lower_expr(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId eid
    );

    /// @brief AST statement를 SIR statement로 lower한다.
    Stmt lower_stmt_(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid
    );

    /// @brief AST 함수 모드를 SIR 함수 모드로 변환한다.
    FnMode lower_fn_mode(parus::ast::FnMode m);

    /// @brief AST 함수 선언을 SIR 함수로 lower한다.
    FuncId lower_func_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid,
        bool is_acts_member,
        ActsId owner_acts,
        bool is_actor_member = false,
        bool is_actor_init = false,
        TypeId actor_owner_type = k_invalid_type
    );

    /// @brief AST field 선언을 SIR field 메타로 lower한다.
    FieldId lower_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    );

} // namespace parus::sir::detail
