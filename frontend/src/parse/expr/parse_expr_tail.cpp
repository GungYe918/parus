// frontend/src/parse/parse_expr_tail.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/DiagCode.hpp>

#include <string_view>


namespace parus {

    static std::string_view token_display(const Token& t) {
        // lexeme이 비면(EOF 등) kind 이름으로 대체
        if (!t.lexeme.empty()) return t.lexeme;
        return syntax::token_kind_name(t.kind);
    }
    ast::Arg Parser::parse_call_arg(int ternary_depth) {
        ast::Arg a{};
        const Token first = cursor_.peek();

        // labeled arg: Ident ':' (Expr | Hole-as-expr)
        if (first.kind == syntax::TokenKind::kIdent && cursor_.peek(1).kind == syntax::TokenKind::kColon) {
            cursor_.bump(); // label
            cursor_.bump(); // ':'

            a.kind = ast::ArgKind::kLabeled;
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
        a.kind = ast::ArgKind::kPositional;
        a.has_label = false;
        a.expr = parse_expr_pratt(0, ternary_depth);
        a.span = ast_.expr(a.expr).span;
        return a;
    }

    bool Parser::parse_expr_try_call_type_args(uint32_t& out_begin, uint32_t& out_count) {
        using K = syntax::TokenKind;
        out_begin = 0;
        out_count = 0;

        if (!cursor_.at(K::kLt)) return false;

        // speculative pre-scan without diagnostics:
        // accept only if a balanced angle-bracket segment is immediately followed by '('.
        size_t i = cursor_.pos();
        int angle = 0;
        int paren = 0;
        int bracket = 0;
        bool closed = false;
        while (true) {
            const Token t = cursor_.peek(i - cursor_.pos());
            if (t.kind == K::kEof) return false;

            if (t.kind == K::kLt) {
                ++angle;
                ++i;
                continue;
            }
            if (t.kind == K::kGt) {
                --angle;
                ++i;
                if (angle == 0) {
                    closed = true;
                    break;
                }
                if (angle < 0) return false;
                continue;
            }

            if (t.kind == K::kLParen) { ++paren; ++i; continue; }
            if (t.kind == K::kRParen) { --paren; ++i; if (paren < 0) return false; continue; }
            if (t.kind == K::kLBracket) { ++bracket; ++i; continue; }
            if (t.kind == K::kRBracket) { --bracket; ++i; if (bracket < 0) return false; continue; }

            // obvious expression-only operators are not valid in type args.
            if (t.kind == K::kPlus || t.kind == K::kMinus || t.kind == K::kStar ||
                t.kind == K::kSlash || t.kind == K::kPercent || t.kind == K::kEqEq ||
                t.kind == K::kBangEq || t.kind == K::kPipePipe ||
                t.kind == K::kPipeFwd || t.kind == K::kPipeRev ||
                t.kind == K::kKwAnd || t.kind == K::kKwOr) {
                return false;
            }

            ++i;
        }

        if (!closed) return false;
        if (cursor_.peek(i - cursor_.pos()).kind != K::kLParen) return false;

        // real parse
        if (!cursor_.eat(K::kLt)) return false;
        out_begin = static_cast<uint32_t>(ast_.type_args().size());
        while (!cursor_.at(K::kGt) && !cursor_.at(K::kEof)) {
            auto ty = parse_type();
            ast_.add_type_arg(ty.id);
            ++out_count;
            if (cursor_.eat(K::kComma)) {
                if (cursor_.at(K::kGt)) break;
                continue;
            }
            break;
        }

        if (!cursor_.eat(K::kGt)) {
            diag_report(diag::Code::kGenericCallTypeArgParseAmbiguous, cursor_.peek().span);
            recover_to_delim(K::kGt, K::kLParen, K::kSemicolon);
            cursor_.eat(K::kGt);
        }
        return true;
    }

    bool Parser::parse_expr_try_literal_type_args(uint32_t& out_begin, uint32_t& out_count) {
        using K = syntax::TokenKind;
        out_begin = 0;
        out_count = 0;

        if (!cursor_.at(K::kLt)) return false;

        // speculative pre-scan:
        // accept only if balanced angle-bracket segment is immediately followed by '{'.
        size_t i = cursor_.pos();
        int angle = 0;
        int paren = 0;
        int bracket = 0;
        bool closed = false;
        while (true) {
            const Token t = cursor_.peek(i - cursor_.pos());
            if (t.kind == K::kEof) return false;

            if (t.kind == K::kLt) {
                ++angle;
                ++i;
                continue;
            }
            if (t.kind == K::kGt) {
                --angle;
                ++i;
                if (angle == 0) {
                    closed = true;
                    break;
                }
                if (angle < 0) return false;
                continue;
            }

            if (t.kind == K::kLParen) { ++paren; ++i; continue; }
            if (t.kind == K::kRParen) { --paren; ++i; if (paren < 0) return false; continue; }
            if (t.kind == K::kLBracket) { ++bracket; ++i; continue; }
            if (t.kind == K::kRBracket) { --bracket; ++i; if (bracket < 0) return false; continue; }

            if (t.kind == K::kPlus || t.kind == K::kMinus || t.kind == K::kStar ||
                t.kind == K::kSlash || t.kind == K::kPercent || t.kind == K::kEqEq ||
                t.kind == K::kBangEq || t.kind == K::kPipePipe ||
                t.kind == K::kPipeFwd || t.kind == K::kPipeRev ||
                t.kind == K::kKwAnd || t.kind == K::kKwOr) {
                return false;
            }

            ++i;
        }

        if (!closed) return false;
        if (cursor_.peek(i - cursor_.pos()).kind != K::kLBrace) return false;

        // real parse
        if (!cursor_.eat(K::kLt)) return false;
        out_begin = static_cast<uint32_t>(ast_.type_args().size());
        while (!cursor_.at(K::kGt) && !cursor_.at(K::kEof)) {
            auto ty = parse_type();
            ast_.add_type_arg(ty.id);
            ++out_count;
            if (cursor_.eat(K::kComma)) {
                if (cursor_.at(K::kGt)) break;
                continue;
            }
            break;
        }

        if (!cursor_.eat(K::kGt)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ">");
            recover_to_delim(K::kGt, K::kLBrace, K::kSemicolon);
            cursor_.eat(K::kGt);
        }
        return true;
    }

    ast::ExprId Parser::parse_expr_call(ast::ExprId callee,
                                        const Token& lparen_tok,
                                        int ternary_depth,
                                        uint32_t call_type_arg_begin,
                                        uint32_t call_type_arg_count) {
        using K = syntax::TokenKind;

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

        // ------------------------------------------------------------
        // Call mode (v0):
        //   - positional-only: f(e1, e2, ...)
        //   - labeled-only:    f(a: e1, b: e2, ...)
        //   - mixed-tail:      f(e1, e2, x: e3, y: e4)
        //
        // Rule:
        //   positional prefix is allowed, but once labeled starts,
        //   all trailing args must be labeled.
        // ------------------------------------------------------------
        enum class CallMode : uint8_t {
            kUnknown,
            kPositionalPrefix,
            kLabeledTail,
            kInvalidMixed,
        };

        CallMode mode = CallMode::kUnknown;

        auto diag_mix = [&](Span sp) {
            diag_report(diag::Code::kCallArgMixNotAllowed, sp);
        };

        if (!cursor_.at(K::kRParen)) {
            while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                const size_t before = cursor_.pos();

                // normal arg (positional / labeled)
                ast::Arg a = parse_call_arg(ternary_depth);

                const bool is_labeled = (a.kind == ast::ArgKind::kLabeled) || a.has_label;

                if (is_labeled) {
                    if (mode == CallMode::kUnknown) {
                        mode = CallMode::kLabeledTail;
                    } else if (mode == CallMode::kPositionalPrefix) {
                        mode = CallMode::kLabeledTail;
                    } else if (mode == CallMode::kLabeledTail) {
                        // keep
                    } else {
                        diag_mix(a.span.hi ? a.span : cursor_.prev().span);
                        mode = CallMode::kInvalidMixed;
                    }
                } else {
                    if (mode == CallMode::kUnknown) {
                        mode = CallMode::kPositionalPrefix;
                    } else if (mode == CallMode::kPositionalPrefix) {
                        // keep
                    } else if (mode == CallMode::kLabeledTail) {
                        diag_mix(a.span.hi ? a.span : cursor_.prev().span);
                        mode = CallMode::kInvalidMixed;
                    } else {
                        diag_mix(a.span.hi ? a.span : cursor_.prev().span);
                        mode = CallMode::kInvalidMixed;
                    }
                }

                ast_.add_arg(a);
                ++count;

                if (cursor_.eat(K::kComma)) {
                    if (cursor_.at(K::kRParen)) break; // trailing comma
                    continue;
                }

                // no progress recovery
                if (cursor_.pos() == before && !cursor_.at(K::kRParen)) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, token_display(cursor_.peek()));
                    recover_to_delim(K::kComma, K::kRParen);
                    if (cursor_.eat(K::kComma)) continue;
                }

                break;
            }
        }

        Token rp = cursor_.peek();
        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, rp.span, ")");
            recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
            rp = cursor_.peek();
            cursor_.eat(K::kRParen);
        }

        ast::Expr out{};
        out.kind = ast::ExprKind::kCall;
        out.span = span_join(ast_.expr(callee).span, rp.span);
        out.a = callee;
        out.arg_begin = begin;
        out.arg_count = count;
        out.call_type_arg_begin = call_type_arg_begin;
        out.call_type_arg_count = call_type_arg_count;
        return ast_.add_expr(out);
    }

    ast::ExprId Parser::parse_expr_index(ast::ExprId base, const Token& lbracket_tok, int ternary_depth) {
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
            diag_report(diag::Code::kExpectedToken, rb.span, "]");
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

    ast::ExprId Parser::parse_expr_if(int ternary_depth) {
        using K = syntax::TokenKind;

        const Token if_kw = cursor_.bump(); // 'if'

        bool has_paren = false;
        if (cursor_.eat(K::kLParen)) has_paren = true;

        ast::ExprId cond = parse_expr_pratt(0, ternary_depth);

        if (has_paren) {
            if (!cursor_.eat(K::kRParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                recover_to_delim(K::kRParen, K::kLBrace, K::kSemicolon);
                cursor_.eat(K::kRParen);
            }
        }

        // then: 반드시 block
        if (!cursor_.at(K::kLBrace)) {
            diag_report(diag::Code::kIfExprThenExpectedBlock, cursor_.peek().span);
            recover_to_delim(K::kLBrace, K::kKwElse, K::kSemicolon);
        }

        ast::ExprId then_e = ast::k_invalid_expr;
        if (cursor_.at(K::kLBrace)) {
            then_e = parse_expr_block(ternary_depth, BlockTailPolicy::kRequireValueTail);
        } else {
            ast::Expr err{};
            err.kind = ast::ExprKind::kError;
            err.span = if_kw.span;
            err.text = "if_missing_then_block";
            then_e = ast_.add_expr(err);
        }

        // else: if-expr는 else 필수
        ast::ExprId else_e = ast::k_invalid_expr;

        if (!cursor_.at(K::kKwElse)) {
            diag_report(diag::Code::kIfExprMissingElse, if_kw.span);
        } else {
            const Token else_kw = cursor_.bump();

            if (cursor_.at(K::kKwIf)) {
                else_e = parse_expr_if(ternary_depth);
            } else {
                if (!cursor_.at(K::kLBrace)) {
                    diag_report(diag::Code::kIfExprElseExpectedBlock, cursor_.peek().span);
                    recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
                }

                if (cursor_.at(K::kLBrace)) {
                    else_e = parse_expr_block(ternary_depth, BlockTailPolicy::kRequireValueTail);
                } else {
                    ast::Expr err{};
                    err.kind = ast::ExprKind::kError;
                    err.span = else_kw.span;
                    err.text = "if_missing_else_block";
                    else_e = ast_.add_expr(err);
                }
            }
        }

        ast::Expr e{};
        e.kind = ast::ExprKind::kIfExpr;
        e.a = cond;
        e.b = then_e;
        e.c = else_e;

        Span end = ast_.expr(then_e).span;
        if (else_e != ast::k_invalid_expr) end = ast_.expr(else_e).span;

        e.span = span_join(if_kw.span, end);
        return ast_.add_expr(e);
    }

    ast::ExprId Parser::parse_expr_block(int ternary_depth, BlockTailPolicy policy) {
        using K = syntax::TokenKind;

        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        // '{'
        const Token lb = cursor_.peek();
        diag_expect(K::kLBrace);

        // -----------------------------
        // 1) parse block body into StmtKind::kBlock
        // -----------------------------
        const uint32_t child_begin = (uint32_t)ast_.stmt_children().size();
        uint32_t child_count = 0;

        auto add_child_stmt = [&](ast::StmtId sid) {
            ast_.add_stmt_child(sid);
            ++child_count;
        };

        ast::ExprId tail_expr = ast::k_invalid_expr;

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !aborted_) {
            // allow stray ';' as empty stmt
            if (cursor_.at(K::kSemicolon)) {
                Token semi = cursor_.bump();
                ast::Stmt s{};
                s.kind = ast::StmtKind::kEmpty;
                s.span = semi.span;
                add_child_stmt(ast_.add_stmt(s));
                continue;
            }

            const Token first = cursor_.peek();

            // ---- If it's clearly a statement/decl start, parse as stmt ----
            // NOTE: block '{...}' as stmt is common, so treat '{' as stmt-start here.
            const bool stmt_start =
                is_unambiguous_stmt_start(first.kind) ||
                first.kind == K::kLBrace ||
                first.kind == K::kKwIf ||     // stmt-if or if-expr stmt form (we'll still parse via stmt path if needed)
                first.kind == K::kKwLoop;     // loop-expr used as stmt is allowed (no ';')

            if (stmt_start) {
                // We must be careful: 'if' can be a tail expr too.
                // Strategy:
                //  - Parse as expression first only when it can become tail (i.e. it ends right before '}')
                //  - Otherwise, fall back to statement parsing for control-flow constructs.
                //
                // We'll do a lightweight approach:
                //  - For '{', 'loop', and 'if': parse expr, then:
                //      - if next is '}' => tail (stop)
                //      - else if next is ';' => ExprStmt
                //      - else if expr-with-block => ExprStmt (no ';' allowed)
                //      - else => error recovery
                //
                //  - For other stmt-start tokens: parse stmt normally.
                if (first.kind == K::kLBrace || first.kind == K::kKwLoop || first.kind == K::kKwIf) {
                    const size_t before = cursor_.pos();

                    ast::ExprId ex = parse_expr_pratt(0, ternary_depth);
                    ex = parse_expr_postfix(ex, ternary_depth);

                    // tail candidate: directly before '}'
                    if (cursor_.at(K::kRBrace)) {
                        tail_expr = ex;
                        break;
                    }

                    // normal expr-stmt with ';'
                    if (cursor_.eat(K::kSemicolon)) {
                        const Token semi = cursor_.prev();
                        ast::Stmt s{};
                        s.kind = ast::StmtKind::kExprStmt;
                        s.expr = ex;
                        // span: from expr start to semicolon-ish (best-effort)
                        s.span = span_join(ast_.expr(ex).span, semi.span);
                        add_child_stmt(ast_.add_stmt(s));
                        continue;
                    }

                    // expr-with-block can be a statement without ';' (Rust-like)
                    if (is_expr_with_block_kind(ast_.expr(ex).kind)) {
                        ast::Stmt s{};
                        s.kind = ast::StmtKind::kExprStmt;
                        s.expr = ex;
                        s.span = ast_.expr(ex).span;
                        add_child_stmt(ast_.add_stmt(s));
                        continue;
                    }

                    // If we didn't progress, avoid infinite loop
                    if (cursor_.pos() == before) {
                        diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, token_display(cursor_.peek()));
                        cursor_.bump();
                    } else {
                        // Missing ';' after normal expr in block context
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ";");
                        recover_to_delim(K::kSemicolon, K::kRBrace);
                        cursor_.eat(K::kSemicolon);
                    }
                    continue;
                }

                // normal statement path
                ast::StmtId sid = parse_stmt_any();
                add_child_stmt(sid);
                continue;
            }

            // ---- Otherwise parse an expression item ----
            {
                const size_t before = cursor_.pos();

                ast::ExprId ex = parse_expr_pratt(0, ternary_depth);
                ex = parse_expr_postfix(ex, ternary_depth);

                // tail if immediately closed
                if (cursor_.at(K::kRBrace)) {
                    tail_expr = ex;
                    break;
                }

                // expr stmt requires ';'
                if (cursor_.eat(K::kSemicolon)) {
                    const Token semi = cursor_.prev();
                    ast::Stmt s{};
                    s.kind = ast::StmtKind::kExprStmt;
                    s.expr = ex;
                    s.span = span_join(ast_.expr(ex).span, semi.span);
                    add_child_stmt(ast_.add_stmt(s));
                    continue;
                }

                // expr-with-block can omit ';'
                if (is_expr_with_block_kind(ast_.expr(ex).kind)) {
                    ast::Stmt s{};
                    s.kind = ast::StmtKind::kExprStmt;
                    s.expr = ex;
                    s.span = ast_.expr(ex).span;
                    add_child_stmt(ast_.add_stmt(s));
                    continue;
                }

                // recovery
                if (cursor_.pos() == before) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, token_display(cursor_.peek()));
                    cursor_.bump();
                } else {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ";");
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kSemicolon);
                }
                continue;
            }
        }

        // '}' (or EOF)
        Token rb = cursor_.peek();
        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, rb.span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            rb = cursor_.peek();
            cursor_.eat(K::kRBrace);
        }

        // -----------------------------
        // 2) build Block stmt node
        // -----------------------------
        ast::Stmt block_stmt{};
        block_stmt.kind = ast::StmtKind::kBlock;
        block_stmt.span = span_join(lb.span, rb.span);
        block_stmt.stmt_begin = child_begin;
        block_stmt.stmt_count = child_count;

        const ast::StmtId block_sid = ast_.add_stmt(block_stmt);

        // -----------------------------
        // 3) enforce tail policy (parser-level)
        // -----------------------------
        if (policy == BlockTailPolicy::kRequireValueTail && tail_expr == ast::k_invalid_expr) {
            diag_report(diag::Code::kBlockTailExprRequired, rb.span);
            // tail_expr remains invalid; tyck 단계에서도 Slot::kValue에서 추가 진단 가능
        }

        // -----------------------------
        // 4) build BlockExpr node with dedicated payload fields
        // -----------------------------
        ast::Expr out{};
        out.kind = ast::ExprKind::kBlockExpr;
        out.span = span_join(lb.span, rb.span);
        out.block_stmt = block_sid;
        out.block_tail = tail_expr;

        return ast_.add_expr(out);
    }

    ast::ExprId Parser::parse_expr_loop(int ternary_depth) {
        using K = syntax::TokenKind;

        const Token loop_tok = cursor_.bump(); // 'loop'

        ast::Expr e{};
        e.kind = ast::ExprKind::kLoop;
        e.span = loop_tok.span;

        // ---- allow "loop v in xs { ... }" as recovery (missing '(') ----
        if (!cursor_.at(K::kLParen)) {
            if (cursor_.peek().kind == K::kIdent && cursor_.peek(1).kind == K::kKwIn) {
                diag_report(diag::Code::kLoopHeaderExpectedLParen, cursor_.peek().span);
                e.loop_has_header = true;

                const Token v = cursor_.bump(); // ident
                e.loop_var = v.lexeme;

                cursor_.bump(); // 'in'
                e.loop_iter = parse_expr_pratt(0, ternary_depth);
                // no ')'
            }
        }

        // canonical header: loop (v in xs) { ... }
        if (cursor_.at(K::kLParen)) {
            e.loop_has_header = true;
            cursor_.bump(); // '('

            const Token v = cursor_.peek();
            if (v.kind != K::kIdent) {
                diag_report(diag::Code::kLoopHeaderVarExpectedIdent, v.span);
            } else {
                cursor_.bump();
                e.loop_var = v.lexeme;
            }

            if (!cursor_.eat(K::kKwIn)) {
                diag_report(diag::Code::kLoopHeaderExpectedIn, cursor_.peek().span);
                recover_to_delim(K::kRParen, K::kLBrace);
                cursor_.eat(K::kKwIn);
            }

            e.loop_iter = parse_expr_pratt(0, ternary_depth);

            if (!cursor_.eat(K::kRParen)) {
                diag_report(diag::Code::kLoopHeaderExpectedRParen, cursor_.peek().span);
                recover_to_delim(K::kRParen, K::kLBrace);
                cursor_.eat(K::kRParen);
            }
        }

        if (!cursor_.at(K::kLBrace)) {
            diag_report(diag::Code::kLoopBodyExpectedBlock, cursor_.peek().span);
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
        }

        if (cursor_.at(K::kLBrace)) {
            e.loop_body = parse_stmt_block();
            e.span = span_join(loop_tok.span, ast_.stmt(e.loop_body).span);
        } else {
            e.kind = ast::ExprKind::kError;
            e.text = "loop_missing_body";
            e.span = loop_tok.span;
        }

        return ast_.add_expr(e);
    }

} // namespace parus
