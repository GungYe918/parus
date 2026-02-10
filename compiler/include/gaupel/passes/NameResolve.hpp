// compiler/include/gaupel/passes/NameResolve.hpp
#pragma once
#include <cstdint>
#include <vector>

#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/sema/SymbolTable.hpp>


namespace gaupel::passes {

    // -----------------------------------------------------------------------------
    // shadowing 처리 정책
    // -----------------------------------------------------------------------------
    enum class ShadowingMode : uint8_t {
        kAllow,   // 기본: 허용, 진단 없음
        kWarn,    // 경고
        kError,   // 에러(하지만 “금지”와 동일한 의미로 쓰진 않음: 선언 자체는 계속 진행 가능)
    };

    struct NameResolveOptions {
        ShadowingMode shadowing = ShadowingMode::kAllow;
    };

    // -----------------------------------------------------------------------------
    // NameResolveResult
    //
    // - expr_to_symbol[eid] : 해당 ExprId(kIdent)가 resolve된 SymbolId
    // - stmt_to_symbol[sid] : (선택) decl 성격 stmt(kVar/kFnDecl 등)가 등록한 SymbolId
    //
    // NOTE:
    // - SIR builder가 스코프 정보를 다시 만들지 않도록, “해결된 결과”를 보존한다.
    // - 미해결/비식별자 expr는 k_invalid_symbol로 유지된다.
    // -----------------------------------------------------------------------------
    struct NameResolveResult {
        using SymbolId = uint32_t;
        static constexpr SymbolId k_invalid_symbol = 0xFFFF'FFFFu;

        std::vector<SymbolId> expr_to_symbol; // size = ast.exprs().size()
        std::vector<SymbolId> stmt_to_symbol; // size = ast.stmts().size() (decl stmt만 채움)

        void reset_sizes(uint32_t expr_count, uint32_t stmt_count) {
            expr_to_symbol.assign(expr_count, k_invalid_symbol);
            stmt_to_symbol.assign(stmt_count, k_invalid_symbol);
        }
    };

    // 신규 API(단일): out_result는 “참조”로 강제한다. (포인터/optional 금지)
    void name_resolve_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out_result
    );

} // namespace gaupel::passes