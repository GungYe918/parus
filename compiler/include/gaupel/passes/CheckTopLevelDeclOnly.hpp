// compiler/include/gaupel/passes/CheckTopLevelDeclOnly.hpp
#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>

namespace gaupel::passes {

    // 프로그램 최상위는 decl만 허용
    // v0에서는 허용: FnDecl, Use
    void check_top_level_decl_only(const ast::AstArena& ast, ast::StmtId program_root, diag::Bag& bag);

}