// compiler/src/passes/check_top_level_decl_only.cpp
#include <gaupel/passes/CheckTopLevelDeclOnly.hpp>
#include <gaupel/diag/DiagCode.hpp>


namespace gaupel::passes {

    static void report(diag::Bag& bag, diag::Code code, Span span) {
        diag::Diagnostic d(diag::Severity::kError, code, span);
        bag.add(std::move(d));
    }

    void check_top_level_decl_only(const ast::AstArena& ast, ast::StmtId program_root, diag::Bag& bag) {
        if (program_root == ast::k_invalid_stmt) return;

        const auto& root = ast.stmt(program_root);

        // parse_program()은 보통 block로 감싸서 root.kind==kBlock일 가능성이 큼
        if (root.kind != ast::StmtKind::kBlock) {
            // 그래도 안전하게: block이 아니면 자체 span을 에러 처리
            report(bag, diag::Code::kTopLevelMustBeBlock, root.span);
            return;
        }

        const auto& kids = ast.stmt_children();
        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const auto sid = kids[root.stmt_begin + i];
            if (sid == ast::k_invalid_stmt) continue;

            const auto& s = ast.stmt(sid);

            const bool ok =
                (s.kind == ast::StmtKind::kVar) ||
                (s.kind == ast::StmtKind::kFnDecl) ||
                (s.kind == ast::StmtKind::kFieldDecl) ||
                (s.kind == ast::StmtKind::kActsDecl) ||
                (s.kind == ast::StmtKind::kUse);

            if (!ok) {
                // 최상위에서 stmt 사용 금지
                report(bag, diag::Code::kTopLevelDeclOnly, s.span);
            }
        }
    }

}
