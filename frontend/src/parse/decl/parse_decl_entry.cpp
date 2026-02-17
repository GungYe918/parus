// frontend/src/parse/parse_decl_entry.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>


namespace parus {

    bool Parser::is_decl_start(syntax::TokenKind k) const {
        using K = syntax::TokenKind;
        return k == K::kAt
            || k == K::kKwExport
            || k == K::kKwExtern
            || k == K::kKwFn
            || k == K::kKwField
            || k == K::kKwActs
            || k == K::kKwUse
            || k == K::kKwImport
            || k == K::kKwNest;
    }

    // decl 엔트리.
    ast::StmtId Parser::parse_decl_any() {
        using K = syntax::TokenKind;

        const Token& t = cursor_.peek();

        // ---- use is treated as a top-level decl (policy) ----
        if (t.kind == K::kKwUse) {
            return parse_decl_use();
        }
        if (t.kind == K::kKwImport) {
            return parse_decl_import();
        }
        if (t.kind == K::kKwNest) {
            return parse_decl_nest();
        }

        // direct decl keywords
        if (t.kind == K::kKwField) {
            return parse_decl_field();
        }
        if (t.kind == K::kKwActs) {
            return parse_decl_acts();
        }

        // export-prefixed decls
        if (t.kind == K::kKwExport) {
            const auto k1 = cursor_.peek(1).kind;
            const auto k2 = cursor_.peek(2).kind;
            if (k1 == K::kKwField) return parse_decl_field();
            if (k1 == K::kKwActs)  return parse_decl_acts();
            if (k1 == K::kKwNest)  return parse_decl_nest();
            if (k1 == K::kStringLit && (k2 == K::kKwField || k2 == K::kKwActs || k2 == K::kKwNest)) {
                // `export "C"`는 함수/전역 심볼 선언용이므로 data-decl에는 허용하지 않는다.
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek(1).span,
                            "'export \"C\"' is only allowed on def/global symbol declarations");
            }
            if (k1 == K::kStringLit && (k2 == K::kKwStatic || k2 == K::kKwMut || k2 == K::kKwLet || k2 == K::kKwSet)) {
                return parse_decl_extern_var();
            }
            return parse_decl_fn();
        }

        if (t.kind == K::kKwExtern) {
            const auto k1 = cursor_.peek(1).kind;
            const auto k2 = cursor_.peek(2).kind;
            if (k1 == K::kStringLit) {
                if (k2 == K::kKwFn || k2 == K::kAt) {
                    return parse_decl_fn();
                }
                if (k2 == K::kKwStatic || k2 == K::kKwMut || k2 == K::kKwLet || k2 == K::kKwSet) {
                    return parse_decl_extern_var();
                }
            }
            if (k1 == K::kKwFn || k1 == K::kAt) {
                return parse_decl_fn();
            }
            if (k1 == K::kKwStatic || k1 == K::kKwMut || k1 == K::kKwLet || k1 == K::kKwSet) {
                return parse_decl_extern_var();
            }
            // fallback: def 파서로 넘겨 링크 문법 진단을 일관 처리한다.
            return parse_decl_fn();
        }

        // def decl
        if (t.kind == K::kAt || t.kind == K::kKwFn) {
            return parse_decl_fn();
        }

        diag_report(diag::Code::kDeclExpected, t.span);
        cursor_.bump();

        ast::Stmt s{};
        s.kind = ast::StmtKind::kError;
        s.span = t.span;
        return ast_.add_stmt(s);
    }

    // decl-level use: just forward to stmt use parser (AST node is still StmtKind::kUse)
    ast::StmtId Parser::parse_decl_use() {
        return parse_stmt_use();
    }

    ast::StmtId Parser::parse_decl_import() {
        return parse_stmt_import();
    }

} // namespace parus
