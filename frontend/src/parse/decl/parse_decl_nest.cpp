// frontend/src/parse/decl/parse_decl_nest.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus {

    /// @brief `nest` 선언을 파싱한다.
    ///
    /// 지원 형태(v0):
    /// 1) nest foo::bar;
    ///    - 파일 전체 기본 경로를 지정하는 선언형 지시어
    /// 2) nest foo::bar { ... }
    ///    - 해당 경로 하위의 중첩 블록 선언
    ast::StmtId Parser::parse_decl_nest() {
        using K = syntax::TokenKind;

        ast::Stmt s{};
        s.kind = ast::StmtKind::kNestDecl;
        s.use_kind = ast::UseKind::kError;

        // optional export
        if (cursor_.at(K::kKwExport)) {
            s.is_export = true;
            cursor_.bump();
        }

        const Token nest_kw = cursor_.peek();
        if (!cursor_.eat(K::kKwNest)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "nest");
            s.kind = ast::StmtKind::kError;
            s.span = cursor_.peek().span;
            return ast_.add_stmt(s);
        }

        auto [pb, pc] = parse_path_segments();
        s.nest_path_begin = pb;
        s.nest_path_count = pc;

        if (pc == 0) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "nest path");
        }

        // nest foo::bar;
        if (cursor_.eat(K::kSemicolon)) {
            s.nest_is_file_directive = true;
            s.span = span_join(nest_kw.span, cursor_.prev().span);

            // 파일 지시어는 1회만 허용한다.
            if (seen_file_nest_directive_) {
                diag_report(diag::Code::kDuplicateDecl, nest_kw.span, "nest");
            }
            seen_file_nest_directive_ = true;

            return ast_.add_stmt(s);
        }

        // nest foo::bar { ... }
        if (cursor_.at(K::kLBrace)) {
            s.nest_is_file_directive = false;
            s.a = parse_stmt_block(/*allow_macro_decl=*/true);
            s.span = span_join(nest_kw.span, ast_.stmt(s.a).span);
            return ast_.add_stmt(s);
        }

        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "';' or '{'");
        const Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
        s.span = span_join(nest_kw.span, end);
        return ast_.add_stmt(s);
    }

} // namespace parus
