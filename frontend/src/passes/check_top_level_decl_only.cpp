// frontend/src/passes/check_top_level_decl_only.cpp
#include <parus/passes/CheckTopLevelDeclOnly.hpp>
#include <parus/diag/DiagCode.hpp>

#include <cstdint>
#include <string_view>


namespace parus::passes {

    static void report(diag::Bag& bag, diag::Code code, Span span) {
        diag::Diagnostic d(diag::Severity::kError, code, span);
        bag.add(std::move(d));
    }

    static void report_msg(diag::Bag& bag, Span span, std::string_view msg) {
        diag::Diagnostic d(diag::Severity::kError, diag::Code::kTypeErrorGeneric, span);
        d.add_arg(msg);
        bag.add(std::move(d));
    }

    enum class ItemScope : uint8_t {
        kTopLevel,
        kNestBody,
    };

    static bool is_allowed_item_(const ast::Stmt& s, ItemScope scope) {
        switch (s.kind) {
            case ast::StmtKind::kFnDecl:
            case ast::StmtKind::kFieldDecl:
            case ast::StmtKind::kActsDecl:
            case ast::StmtKind::kNestDecl:
            case ast::StmtKind::kUse:
                return true;

            case ast::StmtKind::kVar:
                if (scope == ItemScope::kTopLevel) {
                    // Top-level variable declarations remain declaration items only for static/ABI globals.
                    // let/set local declarations are block-local only.
                    return s.is_static || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
                }
                // nest body allows item declarations only; variable item requires static storage.
                return s.is_static;

            default:
                return false;
        }
    }

    static void check_item_block_(const ast::AstArena& ast, ast::StmtId block_id, ItemScope scope, diag::Bag& bag) {
        if (block_id == ast::k_invalid_stmt) return;
        const auto& block = ast.stmt(block_id);
        if (block.kind != ast::StmtKind::kBlock) return;

        const auto& kids = ast.stmt_children();
        const uint64_t begin = block.stmt_begin;
        const uint64_t end = begin + block.stmt_count;
        if (begin > kids.size() || end > kids.size()) return;

        for (uint32_t i = 0; i < block.stmt_count; ++i) {
            const auto sid = kids[block.stmt_begin + i];
            if (sid == ast::k_invalid_stmt) continue;

            const auto& s = ast.stmt(sid);
            if (!is_allowed_item_(s, scope)) {
                if (scope == ItemScope::kTopLevel && s.kind == ast::StmtKind::kVar) {
                    report_msg(
                        bag,
                        s.span,
                        "top-level variable declaration must be static/global (let/set declarations are local-only)"
                    );
                } else if (scope == ItemScope::kNestBody && s.kind == ast::StmtKind::kVar && !s.is_static) {
                    report_msg(
                        bag,
                        s.span,
                        "variables in nest body must be static (const will be introduced later)"
                    );
                } else if (scope == ItemScope::kNestBody) {
                    report_msg(bag, s.span, "nest body allows item declarations only");
                } else {
                    report(bag, diag::Code::kTopLevelDeclOnly, s.span);
                }
                continue;
            }

            if (s.kind == ast::StmtKind::kNestDecl && !s.nest_is_file_directive && s.a != ast::k_invalid_stmt) {
                check_item_block_(ast, s.a, ItemScope::kNestBody, bag);
            }
        }
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

        check_item_block_(ast, program_root, ItemScope::kTopLevel, bag);
    }

}
