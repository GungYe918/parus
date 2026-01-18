// compiler/src/parse/parse_expr.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/Precedence.hpp>
#include <gaupel/syntax/TokenKind.hpp>
#include <gaupel/diag/DiagCode.hpp>


namespace gaupel {

    void Parser::report(diag::Code code, Span span, std::string_view a0) {
        if (!diags_) return;
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
        if (cursor_.eat(k)) return true;
        const Token t = cursor_.peek();
        report(diag::Code::kExpectedToken, t.span, syntax::token_kind_name(k));
        return false;
    }

    Span Parser::span_join(Span a, Span b) const {
        Span s = a;
        
        if (s.file_id == 0) s.file_id = b.file_id;
        s.lo = (a.lo < b.lo) ? a.lo : b.lo;
        s.hi = (a.hi > b.hi) ? a.hi : b.hi;
        
        return s;
    }

    ast::ExprId Parser::parse_expr() {
        return parse_expr_pratt(/*min_prec=*/0, /*ternary_depth=*/0);
    }

    ast::ExprId Parser::parse_expr_pratt(int min_prec, int ternary_depth) {
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
            lhs = ast_.add_expr(e);
        }

        return lhs;
    }

    ast::ExprId Parser::parse_prefix(int ternary_depth) {
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

        // parenthesized
        if (cursor_.eat(syntax::TokenKind::kLParen)) {
            ast::ExprId inner = parse_expr_pratt(0, ternary_depth);
            expect(syntax::TokenKind::kRParen);
            return inner;
        }

        // fallback: create error-ish hole node
        report(diag::Code::kUnexpectedToken, t.span, t.lexeme);
        cursor_.bump();
        ast::Expr e{};
        e.kind = ast::ExprKind::kIdent;
        e.span = t.span;
        e.text = "<error>";
        return ast_.add_expr(e);
    }

    ast::ExprId Parser::parse_postfix(ast::ExprId base, int ternary_depth) {
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
        (void)lparen_tok;
        uint32_t begin = static_cast<uint32_t>(ast_.args().size());
        uint32_t count = 0;
    
        if (!cursor_.at(syntax::TokenKind::kRParen)) {
            while (1) {
                ast::Arg a = parse_arg(ternary_depth);
                ast_.add_arg(a);
                ++count;

                if (cursor_.eat(syntax::TokenKind::kComma)) continue;
                break;
            }
        }

        Token rp = cursor_.peek();
        expect(syntax::TokenKind::kRParen);

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
        ast::ExprId idx = parse_expr_pratt(0, ternary_depth);
        Token rb = cursor_.peek();
        expect(syntax::TokenKind::kRBracket);

        ast::Expr e{};
        e.kind = ast::ExprKind::kIndex;
        e.span = span_join(ast_.expr(base).span, rb.span);
        e.a = base;
        e.b = idx;
        
        return ast_.add_expr(e);
    }


} // namespace gaupel