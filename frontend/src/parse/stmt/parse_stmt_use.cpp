// frontend/src/parse/parse_stmt_use.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>


namespace parus::detail {

    static bool is_lower_alpha_only(std::string_view s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (!(c >= 'a' && c <= 'z')) return false;
        }
        return true;
    }

    static bool contains_digit(std::string_view s) {
        for (char c : s) {
            if (c >= '0' && c <= '9') return true;
        }
        return false;
    }

    static bool is_textsubst_literal_tok(syntax::TokenKind k) {
        using K = syntax::TokenKind;
        return k == K::kIntLit
            || k == K::kFloatLit
            || k == K::kStringLit
            || k == K::kCharLit
            || k == K::kKwTrue
            || k == K::kKwFalse
            || k == K::kKwNull;
    }
} // namespace parus::detail

namespace parus {
    ast::ExprId Parser::parse_use_literal_expr_or_error() {
        using K = syntax::TokenKind;

        const Token t = cursor_.peek();
        if (!detail::is_textsubst_literal_tok(t.kind)) {
            diag_report(diag::Code::kUnexpectedToken, t.span, "literal (use substitution)");
            // recovery: try to parse an expr to move forward, but this may emit extra diags.
            // safer recovery: just bump one token if not EOF
            if (!cursor_.at(K::kEof)) cursor_.bump();

            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = t.span;
            e.text = "use_textsubst_non_literal";
            return ast_.add_expr(e);
        }

        cursor_.bump();

        ast::Expr e{};
        e.span = t.span;
        e.text = t.lexeme;

        switch (t.kind) {
            case K::kIntLit:    e.kind = ast::ExprKind::kIntLit;    break;
            case K::kFloatLit:  e.kind = ast::ExprKind::kFloatLit;  break;
            case K::kStringLit: e.kind = ast::ExprKind::kStringLit; break;
            case K::kCharLit:   e.kind = ast::ExprKind::kCharLit;   break;
            case K::kKwTrue:
            case K::kKwFalse:   e.kind = ast::ExprKind::kBoolLit;   break;
            case K::kKwNull:    e.kind = ast::ExprKind::kNullLit;   break;
            default:
                e.kind = ast::ExprKind::kError;
                e.text = "use_textsubst_unreachable";
                break;
        }

        return ast_.add_expr(e);
    }
    std::pair<uint32_t, uint32_t> Parser::parse_path_segments() {
        using K = syntax::TokenKind;

        // Path := Ident (('::' | ':' ':') Ident)*
        uint32_t begin = (uint32_t)ast_.path_segs().size();
        uint32_t count = 0;

        const Token first = cursor_.peek();
        if (first.kind != K::kIdent) {
            diag_report(diag::Code::kUnexpectedToken, first.span, "identifier (path segment)");
            return { begin, count };
        }

        cursor_.bump();
        ast_.add_path_seg(first.lexeme);
        ++count;

        auto eat_coloncolon = [&]() -> bool {
            if (cursor_.at(K::kColonColon)) {
                cursor_.bump();
                return true;
            }
            // lexer가 '::'를 ':' ':'로 쪼개서 내는 경우 흡수
            if (cursor_.at(K::kColon) && cursor_.peek(1).kind == K::kColon) {
                cursor_.bump();
                cursor_.bump();
                return true;
            }
            return false;
        };

        while (eat_coloncolon()) {
            const Token seg = cursor_.peek();
            if (seg.kind != K::kIdent) {
                diag_report(diag::Code::kUnexpectedToken, seg.span, "identifier (path segment)");
                break;
            }
            cursor_.bump();
            ast_.add_path_seg(seg.lexeme);
            ++count;
        }

        return { begin, count };
    }

    /// @brief import 선언을 파싱한다.
    ///
    /// 문법(v0):
    ///   import foo;
    ///   import foo::bar as fb;
    ///
    /// 규칙:
    /// - import는 외부 의존성 alias를 현재 스코프에 1개만 도입한다.
    /// - alias 미지정 시 마지막 path segment를 alias로 사용한다.
    ast::StmtId Parser::parse_stmt_import() {
        using K = syntax::TokenKind;

        const Token import_kw = cursor_.bump(); // 'import'

        ast::Stmt s{};
        s.kind = ast::StmtKind::kUse;
        s.use_kind = ast::UseKind::kImport;
        s.span = import_kw.span;

        auto [pb, pc] = parse_path_segments();
        s.use_path_begin = pb;
        s.use_path_count = pc;

        if (pc == 0) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "module path");
            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(import_kw.span, end);
            return ast_.add_stmt(s);
        }

        std::string_view alias = ast_.path_segs()[pb + pc - 1];
        if (cursor_.eat(K::kKwAs)) {
            const Token al = cursor_.peek();
            if (al.kind != K::kIdent) {
                diag_report(diag::Code::kUnexpectedToken, al.span, "identifier (import alias)");
            } else {
                cursor_.bump();
                alias = al.lexeme;
            }
        }

        s.use_rhs_ident = alias;
        Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
        s.span = span_join(import_kw.span, end);
        return ast_.add_stmt(s);
    }

    // stmt 경계까지 스킵
    void Parser::stmt_sync_to_boundary() {
        while (!cursor_.at(syntax::TokenKind::kSemicolon) &&
               !cursor_.at(syntax::TokenKind::kRBrace) &&
               !cursor_.at(syntax::TokenKind::kEof)) {
            cursor_.bump();
        }
    }

    ast::StmtId Parser::parse_stmt_use() {
        using K = syntax::TokenKind;

        const Token use_kw = cursor_.bump(); // 'use'

        ast::Stmt s{};
        s.kind = ast::StmtKind::kUse;
        s.span = use_kw.span;
        s.use_kind = ast::UseKind::kError;

        // use module ... 는 즉시 폐기(deprecated -> removed)
        if (cursor_.at(K::kKwModule)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "use module is removed; use 'import foo [as alias];'");
            stmt_sync_to_boundary();
            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // use acts Name for T;
        if (cursor_.at(K::kKwActs)) {
            cursor_.bump(); // acts

            auto [pb, pc] = parse_path_segments();
            s.use_path_begin = pb;
            s.use_path_count = pc;
            if (pc > 0) {
                const auto& segs = ast_.path_segs();
                s.use_name = segs[pb + pc - 1];
            }

            const auto is_for_token = [](const Token& t) -> bool {
                return t.kind == K::kIdent && t.lexeme == "for";
            };

            if (!is_for_token(cursor_.peek())) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "for");
                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            }
            cursor_.bump(); // for

            const auto ty = parse_type();
            s.acts_target_type = ty.id;
            if (s.acts_target_type == ast::k_invalid_type) {
                diag_report(diag::Code::kActsForTypeExpected, ty.span);
            }

            s.use_kind = ast::UseKind::kActsEnable;
            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // ------------------------------------------------------------
        // 2) non-module forms must start with Ident (path head)
        //    - TypeAlias:   use NewT (=|as) Type;
        //    - PathAlias:   use A::B (=|as) name;
        //    - TextSubst:   use PI 3.14;
        // ------------------------------------------------------------
        const Token first = cursor_.peek();
        if (first.kind != K::kIdent) {
            diag_report(diag::Code::kUnexpectedToken, first.span, "identifier (use target)");
            if (!cursor_.at(K::kEof)) cursor_.bump();

            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // parse path: Ident ('::' Ident)*
        auto [pb, pc] = parse_path_segments();

        // separator can be '=' or 'as'
        const bool has_assign = cursor_.at(K::kAssign);
        const bool has_as     = cursor_.at(K::kKwAs);

        // ------------------------------------------------------------
        // 2-A) alias forms: ( '=' | 'as' )
        // ------------------------------------------------------------
        if (has_assign || has_as) {
            if (has_assign) {
                cursor_.bump(); // '='
            } else {
                cursor_.bump(); // 'as'
            }

            // ---- PathAlias: pc >= 2, RHS must be Ident ----
            if (pc >= 2) {
                const Token rhs = cursor_.peek();
                if (rhs.kind != K::kIdent) {
                    diag_report(diag::Code::kUnexpectedToken, rhs.span, "identifier (use path alias name)");
                } else {
                    cursor_.bump();

                    s.use_kind = ast::UseKind::kPathAlias;
                    s.use_path_begin = pb;
                    s.use_path_count = pc;
                    s.use_rhs_ident = rhs.lexeme;
                }

                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            }

            // ---- pc == 1: TypeAlias (but keep v0 heuristic: forbid value-alias-looking cases) ----
            {
                const std::string_view lhs = ast_.path_segs()[pb];

                // If RHS is Ident and looks like value-alias (lowercase-only), keep old policy:
                // forbid `use foo (=|as) bar;` when both are lowercase-only and rhs has no digit.
                const Token rhs0 = cursor_.peek();
                if (rhs0.kind == K::kIdent) {
                    const std::string_view rhs = rhs0.lexeme;

                    const bool looks_like_value_alias =
                        detail::is_lower_alpha_only(lhs) &&
                        detail::is_lower_alpha_only(rhs) &&
                        !detail::contains_digit(rhs);

                    if (looks_like_value_alias) {
                        diag_report(diag::Code::kUnexpectedToken, rhs0.span,
                                    "value-like alias is not allowed here (use TypeAlias or PathAlias).");
                        // recovery: consume rhs ident to stabilize stream
                        cursor_.bump();

                        Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                        s.span = span_join(use_kw.span, end);
                        return ast_.add_stmt(s);
                    }
                }

                // Parse Type (v0: NamedType). This also happily consumes Ident RHS.
                auto ty = parse_type();

                s.use_kind = ast::UseKind::kTypeAlias;
                s.use_name = lhs;
                s.type = ty.id;

                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            }
        }

        // ------------------------------------------------------------
        // 2-B) Text substitution: use NAME LITERAL;
        //      Only allowed when pc == 1 (single ident)
        // ------------------------------------------------------------
        if (pc != 1) {
            // e.g. "use A::B 123;" is nonsensical in v0
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "expected '=' or 'as' after use-path");
            // recovery: sync to ';'
            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // TextSubst payload must be a literal token (not general expr) in v0.
        {
            const std::string_view name = ast_.path_segs()[pb];
            s.use_kind = ast::UseKind::kTextSubst;
            s.use_name = name;

            // literal-only payload
            s.expr = parse_use_literal_expr_or_error();

            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }
    }

} // namespace parus
