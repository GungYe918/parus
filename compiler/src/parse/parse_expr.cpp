// compiler/src/parse/parse_expr.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/Precedence.hpp>
#include <gaupel/syntax/TokenKind.hpp>
#include <gaupel/diag/DiagCode.hpp>


namespace gaupel {

    static std::string_view token_display(const Token& t) {
        // lexeme이 비면(EOF 등) kind 이름으로 대체
        if (!t.lexeme.empty()) return t.lexeme;
        return syntax::token_kind_name(t.kind);
    }

    void Parser::report(diag::Code code, Span span, std::string_view a0) {
        if (!diags_) return;
        if (aborted_) return;

        // lexer-fatal 이후엔 파서 진단 억제
        if (lexer_fatal_) {
            if (code == diag::Code::kUnexpectedToken) return;
            if (code == diag::Code::kExpectedToken) return;
            if (code == diag::Code::kUnexpectedEof) return;
        }

        // 동일 위치/동일 코드 중복 억제
        if (code == last_diag_code_ && span.lo == last_diag_lo_) return;
        last_diag_code_ = code;
        last_diag_lo_ = span.lo;

        // max-errors 도달 시 중단
        // (현재 진단을 찍기 전에 체크해서 "정확히 N개"를 보장)
        if (diags_->issue_count() >= max_errors_) {
            if (!too_many_errors_emitted_) {
                too_many_errors_emitted_ = true;
                diag::Diagnostic stop(diag::Severity::kFatal, diag::Code::kTooManyErrors, span);
                diags_->add(std::move(stop));
            }
            aborted_ = true;
            return;
        }

        diag::Diagnostic d(diag::Severity::kError, code, span);
        if (!a0.empty()) d.add_arg(a0);
        diags_->add(std::move(d));
    }

    void Parser::report_int(diag::Code code, Span span, int v0) {
        if (!diags_) return;
        diag::Diagnostic d(diag::Severity::kError, code, span);
        d.add_arg_int(v0);
        diags_->add(std::move(d));
    }

    bool Parser::expect(syntax::TokenKind k) {
        if (aborted_) return false;

        if (cursor_.at(k)) {
            cursor_.bump();
            return true;
        }

        const Token& got = cursor_.peek();
        if (got.kind == syntax::TokenKind::kEof) {
            report(diag::Code::kUnexpectedEof, got.span, syntax::token_kind_name(k));
            aborted_ = true;
            return false;
        }

        report(diag::Code::kExpectedToken, got.span, syntax::token_kind_name(k));
        return false;
    }

    void Parser::recover_to_delim(
        syntax::TokenKind stop0,
        syntax::TokenKind stop1,
        syntax::TokenKind stop2
    ) {
        int paren = 0, brack = 0, brace = 0;

        auto is_stop = [&](syntax::TokenKind k) {
            return (k == stop0) || (stop1 != syntax::TokenKind::kError && k == stop1)
                || (stop2 != syntax::TokenKind::kError && k == stop2);
        };

        while (!cursor_.at(syntax::TokenKind::kEof)) {
            auto k = cursor_.peek().kind;

            // only stop when we're at top-level of the current construct
            if (paren == 0 && brack == 0 && brace == 0 && is_stop(k)) return;

            // maintain nesting
            if (k == syntax::TokenKind::kLParen) ++paren;
            else if (k == syntax::TokenKind::kRParen && paren > 0) --paren;
            else if (k == syntax::TokenKind::kLBracket) ++brack;
            else if (k == syntax::TokenKind::kRBracket && brack > 0) --brack;
            else if (k == syntax::TokenKind::kLBrace) ++brace;
            else if (k == syntax::TokenKind::kRBrace && brace > 0) --brace;

            cursor_.bump();
        }
    }

    Span Parser::span_join(Span a, Span b) const {
        Span s = a;
        
        if (s.file_id == 0) s.file_id = b.file_id;
        s.lo = (a.lo < b.lo) ? a.lo : b.lo;
        s.hi = (a.hi > b.hi) ? a.hi : b.hi;
        
        return s;
    }

    static constexpr bool is_assign_op(gaupel::syntax::TokenKind k) {
        using K = gaupel::syntax::TokenKind;
        switch (k) {
            case K::kAssign:
            case K::kPlusAssign:
            case K::kMinusAssign:
            case K::kStarAssign:
            case K::kSlashAssign:
            case K::kPercentAssign:
                return true;
            default:
                return false;
        }
    }

    ast::ExprId Parser::parse_expr() {
        return parse_expr_pratt(/*min_prec=*/0, /*ternary_depth=*/0);
    }

    ast::ExprId Parser::parse_expr_pratt(int min_prec, int ternary_depth) {
        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        ast::ExprId lhs = parse_prefix(ternary_depth);
        lhs = parse_postfix(lhs, ternary_depth);

        
        while (1) {
            const auto& tok = cursor_.peek();

            // ternary ?: (non-nestable)
            if (tok.kind == syntax::TokenKind::kQuestion) {
                if (ternary_depth > 0) {
                    report(diag::Code::kNestedTernaryNotAllowed, tok.span);
                    cursor_.bump();
                } else {
                    cursor_.bump(); // '?'
                    ast::ExprId then_e = parse_expr_pratt(0, ternary_depth + 1);
                    expect(syntax::TokenKind::kColon);
                    ast::ExprId else_e = parse_expr_pratt(0, ternary_depth + 1);

                    ast::Expr e{};
                    e.kind = ast::ExprKind::kTernary;
                    e.span = span_join(ast_.expr(lhs).span, ast_.expr(else_e).span);
                    e.a = lhs;
                    e.b = then_e;
                    e.c = else_e;
                    lhs = ast_.add_expr(e);
                    continue;
                }
            }

            auto info = syntax::infix_info(tok.kind);
            if (!info.has_value()) break;

            int prec = info->prec;
            if (prec < min_prec) break;

            const Token op_tok = cursor_.bump();
            int next_min = (info->assoc == syntax::Assoc::kLeft) ? (prec + 1) : prec;

            ast::ExprId rhs = parse_expr_pratt(next_min, ternary_depth);
            rhs = parse_postfix(rhs, ternary_depth);

            ast::Expr e{};
            e.kind = ast::ExprKind::kBinary;
            e.op = op_tok.kind;
            e.a = lhs;
            e.b = rhs;
            e.span = span_join(ast_.expr(lhs).span, ast_.expr(rhs).span);

            if (is_assign_op(op_tok.kind)) {
                e.kind = ast::ExprKind::kAssign;
            } else {
                e.kind = ast::ExprKind::kBinary;
            }

            lhs = ast_.add_expr(e);
        }

        return lhs;
    }

    ast::ExprId Parser::parse_prefix(int ternary_depth) {
        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        const Token t = cursor_.peek();

        if (auto p = syntax::prefix_info(t.kind); p.has_value()) {
            const Token op = cursor_.bump();
            ast::ExprId rhs = parse_prefix(ternary_depth);
            rhs = parse_postfix(rhs, ternary_depth);

            ast::Expr e{};
            e.kind = ast::ExprKind::kUnary;
            e.op = op.kind;
            e.a = rhs;
            e.span = span_join(op.span, ast_.expr(rhs).span);
            
            return ast_.add_expr(e);
        }

        return parse_primary(ternary_depth);
    }

    ast::ExprId Parser::parse_primary(int ternary_depth) {
        const Token t = cursor_.peek();

        // literals / ident / hole
        if (t.kind == syntax::TokenKind::kIntLit) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kIntLit;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        }

        if (t.kind == syntax::TokenKind::kFloatLit) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kFloatLit;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        } 

        if (t.kind == syntax::TokenKind::kStringLit) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kStringLit;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        }
        if (t.kind == syntax::TokenKind::kKwTrue || t.kind == syntax::TokenKind::kKwFalse) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kBoolLit;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        }

        if (t.kind == syntax::TokenKind::kKwNull) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kNullLit;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        }

        if (t.kind == syntax::TokenKind::kIdent) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kIdent;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        }

        if (t.kind == syntax::TokenKind::kHole) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kHole;
            e.span = t.span;
            e.text = t.lexeme;
            return ast_.add_expr(e);
        }

        if (t.kind == syntax::TokenKind::kEof) {
            if (!lexer_fatal_) {
                report(diag::Code::kUnexpectedEof, t.span, "expression");
            }
            aborted_ = true;

            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = t.span;
            e.text = "eof";
            return ast_.add_expr(e);
        }

        // parenthesized
        if (cursor_.eat(syntax::TokenKind::kLParen)) {
            ast::ExprId inner = parse_expr_pratt(0, ternary_depth);
            if (!cursor_.eat(syntax::TokenKind::kRParen)) {
                report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                recover_to_delim(syntax::TokenKind::kRParen, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRBrace);
                cursor_.eat(syntax::TokenKind::kRParen);
            }
            return inner;
        }

        // fallback: create error-ish hole node
        report(diag::Code::kUnexpectedToken, t.span, token_display(t));
        cursor_.bump();

        ast::Expr e{};
        e.kind = ast::ExprKind::kError;
        e.span = t.span;
        e.text = token_display(t);
        return ast_.add_expr(e);
    }

    ast::ExprId Parser::parse_postfix(ast::ExprId base, int ternary_depth) {
        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        while (1) {
            const Token t = cursor_.peek();

            if (t.kind == syntax::TokenKind::kLParen) {
                const Token lp = cursor_.bump();
                base = parse_call(base, lp, ternary_depth);
                continue;
            }

            if (t.kind == syntax::TokenKind::kLBracket) {
                const Token lb = cursor_.bump();
                base = parse_index(base, lb, ternary_depth);
                continue;
            }

            if (t.kind == syntax::TokenKind::kPlusPlus) {
                const Token op = cursor_.bump();
                ast::Expr e{};
                e.kind = ast::ExprKind::kPostfixUnary;
                e.op = op.kind;
                e.a = base;
                e.span = span_join(ast_.expr(base).span, op.span);
                base = ast_.add_expr(e);
                continue;
            }

            break;
        }

        return base;
    }

    ast::Arg Parser::parse_arg(int ternary_depth) {
        ast::Arg a{};
        const Token first = cursor_.peek();

        // labeled arg: Ident ':' (Expr | Hole-as-expr)
        if (first.kind == syntax::TokenKind::kIdent && cursor_.peek(1).kind == syntax::TokenKind::kColon) {
            cursor_.bump(); // label
            cursor_.bump(); // ':'
            a.has_label = true;
            a.label = first.lexeme;

            const Token next = cursor_.peek();
            if (next.kind == syntax::TokenKind::kHole) {
                cursor_.bump();
                a.is_hole = true;
                a.expr = ast::k_invalid_expr;
                a.span = span_join(first.span, next.span);
                return a;
            }

            a.expr = parse_expr_pratt(0, ternary_depth);
            a.span = span_join(first.span, ast_.expr(a.expr).span);
            
            return a;
        }

        // positional arg: Expr
        a.has_label = false;
        a.expr = parse_expr_pratt(0, ternary_depth);
        a.span = ast_.expr(a.expr).span;
        return a;
    }

    ast::ExprId Parser::parse_call(ast::ExprId callee, const Token& lparen_tok, int ternary_depth) {
        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        (void)lparen_tok;
        uint32_t begin = static_cast<uint32_t>(ast_.args().size());
        uint32_t count = 0;

        if (!cursor_.at(syntax::TokenKind::kRParen)) {
            while (!cursor_.at(syntax::TokenKind::kRParen) && !cursor_.at(syntax::TokenKind::kEof)) {
                size_t before = cursor_.pos();

                // try parse one arg
                ast::Arg a = parse_arg(ternary_depth);
                ast_.add_arg(a);
                ++count;

                // normal separator
                if (cursor_.eat(syntax::TokenKind::kComma)) continue;

                // if we didn't progress (or we hit junk), recover to ',' or ')'
                if (cursor_.pos() == before && !cursor_.at(syntax::TokenKind::kRParen)) {
                    report(diag::Code::kUnexpectedToken, cursor_.peek().span, token_display(cursor_.peek()));
                    recover_to_delim(syntax::TokenKind::kComma, syntax::TokenKind::kRParen);
                    if (cursor_.eat(syntax::TokenKind::kComma)) continue;
                }

                // otherwise break (expect ')')
                break;
            }
        }

        // closing ')'
        Token rp = cursor_.peek();
        if (!cursor_.eat(syntax::TokenKind::kRParen)) {
            report(diag::Code::kExpectedToken, rp.span, ")");
            recover_to_delim(syntax::TokenKind::kRParen, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRBrace);
            rp = cursor_.peek();
            cursor_.eat(syntax::TokenKind::kRParen); // consume if present
        }

        ast::Expr e{};
        e.kind = ast::ExprKind::kCall;
        e.span = span_join(ast_.expr(callee).span, rp.span);
        e.a = callee;
        e.arg_begin = begin;
        e.arg_count = count;
        return ast_.add_expr(e);
    }


    ast::ExprId Parser::parse_index(ast::ExprId base, const Token& lbracket_tok, int ternary_depth) {
        (void)lbracket_tok;
        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        ast::ExprId idx = parse_expr_pratt(0, ternary_depth);

        Token rb = cursor_.peek();
        if (!cursor_.eat(syntax::TokenKind::kRBracket)) {
            report(diag::Code::kExpectedToken, rb.span, "]");
            recover_to_delim(syntax::TokenKind::kRBracket, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRBrace);
            rb = cursor_.peek();
            cursor_.eat(syntax::TokenKind::kRBracket);
        }

        ast::Expr e{};
        e.kind = ast::ExprKind::kIndex;
        e.span = span_join(ast_.expr(base).span, rb.span);
        e.a = base;
        e.b = idx;
        return ast_.add_expr(e);
    }


} // namespace gaupel