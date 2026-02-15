// frontend/include/parus/passes/CheckTopLevelDeclOnly.hpp
#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>

namespace parus::passes {

    // 프로그램 최상위는 decl만 허용
    // v0에서는 허용: FnDecl, Use
    void check_top_level_decl_only(const ast::AstArena& ast, ast::StmtId program_root, diag::Bag& bag);

}