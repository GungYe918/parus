// compiler/include/gaupel/passes/Passes.hpp
#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/sema/SymbolTable.hpp>
#include <gaupel/passes/NameResolve.hpp>


namespace gaupel::passes {

    struct PassOptions {
        NameResolveOptions name_resolve{};
    };

    // 프로그램(= parse_program 결과 root) 기준으로 모든 passes 실행
    // - Top-level decl only 체크 포함
    // - NameResolve 포함(심볼테이블 생성/채움)
    void run_all_on_program(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        sema::SymbolTable& out_sym,
        diag::Bag& bag,
        const PassOptions& opt
    );

    // stmt 트리에 대한 passes
    void run_all_on_stmt(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& out_sym,
        diag::Bag& bag,
        const PassOptions& opt
    );

    // expr 루트에 대한 passes
    void run_all_on_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace gaupel::passes