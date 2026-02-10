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

    // Pass 결과를 “한 번에” 묶어서 제공
    struct PassResults {
        sema::SymbolTable sym;
        NameResolveResult name_resolve;
    };

    // expr 루트에 대한 passes (expr-only: pipe-hole 등)
    void run_on_expr(
        const ast::AstArena& ast,
        ast::ExprId root,
        diag::Bag& bag
    );

    // stmt 트리에 대한 passes (NameResolve 포함)
    PassResults run_on_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        diag::Bag& bag,
        const PassOptions& opt
    );

    // 프로그램(= parse_program 결과 root) 기준 passes
    // - Top-level decl only 체크 포함
    PassResults run_on_program(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        diag::Bag& bag,
        const PassOptions& opt
    );

} // namespace gaupel::passes