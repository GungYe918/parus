// compiler/src/parse/parse_decl_entry.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>


namespace gaupel {

    bool Parser::is_decl_start(syntax::TokenKind k) const {
        using K = syntax::TokenKind;
        return k == K::kAt
            || k == K::kKwExport
            || k == K::kKwFn
            || k == K::kKwField
            || k == K::kKwActs
            || k == K::kKwUse;
    }

    // decl 엔트리.
    ast::StmtId Parser::parse_decl_any() {
        using K = syntax::TokenKind;

        const Token& t = cursor_.peek();

        // ---- use is treated as a top-level decl (policy) ----
        if (t.kind == K::kKwUse) {
            return parse_decl_use();
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
            if (k1 == K::kKwField) return parse_decl_field();
            if (k1 == K::kKwActs)  return parse_decl_acts();
            return parse_decl_fn();
        }

        // fn decl
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

} // namespace gaupel
