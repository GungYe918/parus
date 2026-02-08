// compiler/include/gaupel/passes/NameResolve.hpp
#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/sema/SymbolTable.hpp>

namespace gaupel::passes {

    // shadowing 처리 정책
    enum class ShadowingMode : uint8_t {
        kAllow,   // 기본: 허용, 진단 없음
        kWarn,    // 경고
        kError,   // 에러(하지만 “금지”와 동일한 의미로 쓰진 않음: 선언 자체는 계속 진행 가능)
    };

    struct NameResolveOptions {
        ShadowingMode shadowing = ShadowingMode::kAllow;
    };

    void name_resolve_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    );

} // namespace gaupel::passes