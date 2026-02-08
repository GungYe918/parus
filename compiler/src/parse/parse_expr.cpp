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

    void gaupel::Parser::diag_report(diag::Code code, Span span, std::string_view a0) {
        if (!diags_ || aborted_) return;

        // 같은 위치/같은 코드 중복 스팸 방지
        if (span.lo == last_diag_lo_ && code == last_diag_code_) return;
        last_diag_lo_ = span.lo;
        last_diag_code_ = code;

        diag::Severity sev = diag::Severity::kError;

        diag::Diagnostic d(sev, code, span);
        if (!a0.empty()) d.add_arg(a0);
        diags_->add(std::move(d));

        ++parse_error_count_;

        // 하드 세이프티 캡 (무한 루프 방지)
        if (parse_error_count_ >= kMaxParseErrors) {
            aborted_ = true;
            return;
        }

        // -fmax-errors= 제한
        if (!too_many_errors_emitted_ && parse_error_count_ >= max_errors_) {
            too_many_errors_emitted_ = true;

            Span sp = span;
            if (cursor_.peek().kind != syntax::TokenKind::kEof) sp = cursor_.peek().span;

            diag::Diagnostic stop(diag::Severity::kFatal, diag::Code::kTooManyErrors, sp);
            diags_->add(std::move(stop));

            aborted_ = true;
        }
    }


    void Parser::diag_report_int(diag::Code code, Span span, int v0) {
        std::string tmp = std::to_string(v0);
        diag_report(code, span, tmp);
    }

    bool Parser::diag_expect(syntax::TokenKind k) {
        if (aborted_) return false;

        if (cursor_.at(k)) {
            cursor_.bump();
            return true;
        }

        const Token& got = cursor_.peek();
        if (got.kind == syntax::TokenKind::kEof) {
            diag_report(diag::Code::kUnexpectedEof, got.span, syntax::token_kind_name(k));
            aborted_ = true;
            return false;
        }

        diag_report(diag::Code::kExpectedToken, got.span, syntax::token_kind_name(k));
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

    bool Parser::is_unambiguous_stmt_start(syntax::TokenKind k) const {
        using K = syntax::TokenKind;

        if (is_decl_start(k)) return true;
        if (k == K::kSemicolon) return true;

        // 확실한 stmt 키워드
        if (k == K::kKwLet || k == K::kKwSet)                               return true;
        if (k == K::kKwReturn || k == K::kKwBreak || k == K::kKwContinue)   return true;
        if (k == K::kKwWhile || k == K::kKwSwitch)                          return true;
        if (k == K::kKwUse)                                                 return true;

        // if/loop/{...} 는 expr도 될 수 있으므로 여기 넣지 않음
        return false;
    }

    bool Parser::is_expr_with_block_kind(ast::ExprKind k) {
        switch (k) {
            case ast::ExprKind::kLoop:
            case ast::ExprKind::kIfExpr:
            case ast::ExprKind::kBlockExpr:
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

        ast::ExprId lhs = parse_expr_prefix(ternary_depth);
        lhs = parse_expr_postfix(lhs, ternary_depth);

        
        while (1) {
            const auto& tok = cursor_.peek();

            // ternary ?: (non-nestable)
            if (tok.kind == syntax::TokenKind::kQuestion) {
                if (ternary_depth > 0) {
                    diag_report(diag::Code::kNestedTernaryNotAllowed, tok.span);
                    cursor_.bump();
                } else {
                    cursor_.bump(); // '?'
                    ast::ExprId then_e = parse_expr_pratt(0, ternary_depth + 1);
                    diag_expect(syntax::TokenKind::kColon);
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
            rhs = parse_expr_postfix(rhs, ternary_depth);

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

    ast::ExprId Parser::parse_expr_prefix(int ternary_depth) {
        if (aborted_) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "aborted";
            return ast_.add_expr(e);
        }

        using K = syntax::TokenKind;

        // NOTE:
        // 기존 구현은 prefix_info가 있는 토큰을 재귀로 계속 먹어 단항 체인을 만들었고,
        // 그 과정에서 "&&&a" 같은 케이스가
        //   kAmpAmp("&&") + kAmp("&") 로 토큰이 분리되더라도
        // "연속된 & 문자(run)" 검사가 없어서 에러 없이 통과했다.
        //
        // 여기서는 prefix 토큰들을 먼저 수집한 뒤(반복),
        // '&' / '&&' 가 연속으로 등장하는 경우를 "문자 개수" 기준으로 누적한다:
        //   '&'  => +1
        //   '&&' => +2
        // 연속 run이 3 이상이면(예: &&& , &&&& 등) 모호하므로 파싱 단계에서 진단한다.
        //
        // 괄호로 명시적으로 끊으면(run이 끊기므로) 허용:
        //   &&(&a)  // 토큰: &&, (, &, a, )
        //
        // (추가로, spec상 '&&'의 의미 제약(escape는 place만 / borrow에는 불가 등)은
        //  이후 semantic/pass에서 더 엄격히 체크하는 게 좋다. 여기선 "&&&" 모호성만 차단.)

        struct PrefixOp {
            K kind{};
            Span span{};
        };

        std::vector<PrefixOp> ops;
        ops.reserve(4);

        // Track adjacency amp-run to forbid "&&&" style.
        bool saw_ambiguous_amp_run = false;
        int  amp_run_chars = 0;          // consecutive '&' chars from adjacent prefix tokens
        bool prev_was_amp_token = false; // previous prefix token was & or &&

        Span amp_run_start{};
        Span amp_run_end{};

        auto bump_amp_run = [&](const Token& amp_tok) {
            int add = 0;
            if (amp_tok.kind == K::kAmp) add = 1;
            else if (amp_tok.kind == K::kAmpAmp) add = 2;

            if (prev_was_amp_token) {
                amp_run_chars += add;
                amp_run_end = amp_tok.span;
            } else {
                amp_run_chars = add;
                amp_run_start = amp_tok.span;
                amp_run_end = amp_tok.span;
            }

            prev_was_amp_token = true;

            if (amp_run_chars >= 3) {
                saw_ambiguous_amp_run = true;
            }
        };

        auto break_amp_run = [&]() {
            prev_was_amp_token = false;
            amp_run_chars = 0;
            amp_run_start = {};
            amp_run_end = {};
        };

        // 1) collect prefix ops
        while (true) {
            const Token t = cursor_.peek();
            if (!syntax::prefix_info(t.kind).has_value()) break;

            const Token op = cursor_.bump();
            ops.push_back(PrefixOp{ op.kind, op.span });

            if (op.kind == K::kAmp || op.kind == K::kAmpAmp) {
                bump_amp_run(op);
            } else {
                // other prefix ops break "& run"
                break_amp_run();
            }
        }

        // 2) parse operand (postfix binds tighter than prefix)
        ast::ExprId rhs = parse_expr_primary(ternary_depth);
        rhs = parse_expr_postfix(rhs, ternary_depth);

        // 3) apply prefixes right-to-left
        for (int i = (int)ops.size() - 1; i >= 0; --i) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kUnary;
            e.op = ops[(size_t)i].kind;
            e.a = rhs;
            e.span = span_join(ops[(size_t)i].span, ast_.expr(rhs).span);
            rhs = ast_.add_expr(e);
        }

        // 4) emit diagnostic for ambiguous "&&&" run
        if (saw_ambiguous_amp_run) {
            Span sp = amp_run_start;
            if (amp_run_end.hi) sp = span_join(amp_run_start, amp_run_end);

            diag_report(
                diag::Code::kUnexpectedToken,
                sp,
                "ambiguous '&' prefix chain (3+ consecutive '&'). Use parentheses, e.g. &&(&x) or &(&&x)"
            );

            // 파서는 최대한 AST를 유지하되, 이후 단계에서 확실히 막고 싶다면
            // 여기서 Error 노드로 감싸도 된다. (지금은 진단만 발생시키고 rhs 반환)
            // ast::Expr err{};
            // err.kind = ast::ExprKind::kError;
            // err.span = sp;
            // err.text = "ambiguous_amp_prefix_chain";
            // return ast_.add_expr(err);
        }

        return rhs;
    }

    ast::ExprId Parser::parse_expr_primary(int ternary_depth) {
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

        if (t.kind == syntax::TokenKind::kCharLit) {
            cursor_.bump();
            ast::Expr e{};
            e.kind = ast::ExprKind::kCharLit;
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

        if (t.kind == syntax::TokenKind::kKwLoop) {
            return parse_expr_loop(ternary_depth);
        }

        // ---- if-expression (Rust style) ----
        if (t.kind == syntax::TokenKind::kKwIf) {
            return parse_expr_if(ternary_depth);
        }

        // ---- block-expression "{ ... }" ----
        if (t.kind == syntax::TokenKind::kLBrace) {
            return parse_expr_block(ternary_depth, BlockTailPolicy::kAllowEmptyTail);
        }

        if (t.kind == syntax::TokenKind::kEof) {
            if (!lexer_fatal_) {
                diag_report(diag::Code::kUnexpectedEof, t.span, "expression");
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
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                recover_to_delim(syntax::TokenKind::kRParen, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRBrace);
                cursor_.eat(syntax::TokenKind::kRParen);
            }
            return inner;
        }

        // fallback: create error-ish hole node
        diag_report(diag::Code::kUnexpectedToken, t.span, token_display(t));
        cursor_.bump();

        ast::Expr e{};
        e.kind = ast::ExprKind::kError;
        e.span = t.span;
        e.text = token_display(t);
        return ast_.add_expr(e);
    }

    ast::ExprId Parser::parse_expr_postfix(ast::ExprId base, int ternary_depth) {
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
                base = parse_expr_call(base, lp, ternary_depth);
                continue;
            }

            if (t.kind == syntax::TokenKind::kLBracket) {
                const Token lb = cursor_.bump();
                base = parse_expr_index(base, lb, ternary_depth);
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

    ast::Arg Parser::parse_call_named_group_arg(int ternary_depth) {
        using K = syntax::TokenKind;

        const Token lb = cursor_.bump(); // '{'
        uint32_t begin = (uint32_t)ast_.named_group_args().size();
        uint32_t count = 0;

        if (!cursor_.at(K::kRBrace)) {
            while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof)) {
                const Token first = cursor_.peek();

                if (!(first.kind == K::kIdent && cursor_.peek(1).kind == K::kColon)) {
                    diag_report(diag::Code::kNamedGroupEntryExpectedColon, first.span);
                    recover_to_delim(K::kComma, K::kRBrace);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }

                cursor_.bump(); // label
                cursor_.bump(); // ':'

                ast::Arg entry{};
                entry.kind = ast::ArgKind::kLabeled;
                entry.has_label = true;
                entry.label = first.lexeme;

                const Token next = cursor_.peek();
                if (next.kind == K::kHole) {
                    cursor_.bump();
                    entry.is_hole = true;
                    entry.expr = ast::k_invalid_expr;
                    entry.span = span_join(first.span, next.span);
                } else {
                    entry.expr = parse_expr_pratt(0, ternary_depth);
                    entry.span = span_join(first.span, ast_.expr(entry.expr).span);
                }

                ast_.add_named_group_arg(entry);
                ++count;

                if (cursor_.eat(K::kComma)) {
                    if (cursor_.at(K::kRBrace)) break; // trailing comma
                    continue;
                }
                break;
            }
        }

        Token rb = cursor_.peek();
        diag_expect(K::kRBrace);

        ast::Arg g{};
        g.kind = ast::ArgKind::kNamedGroup;
        g.child_begin = begin;
        g.child_count = count;
        g.span = span_join(lb.span, rb.span);
        return g;
    }

    ast::ExprId Parser::parse_expr_call(ast::ExprId callee, const Token& lparen_tok, int ternary_depth) {
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

        bool seen_named_group = false;

        if (!cursor_.at(K::kRParen)) {
            while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                size_t before = cursor_.pos();

                ast::Arg a{};

                // named-group arg: '{' ... '}'
                if (cursor_.at(K::kLBrace)) {
                    if (seen_named_group) {
                        diag_report(diag::Code::kCallOnlyOneNamedGroupAllowed, cursor_.peek().span);
                        recover_to_delim(K::kComma, K::kRParen);
                        cursor_.eat(K::kComma);
                        continue;
                    }

                    seen_named_group = true;
                    a = parse_call_named_group_arg(ternary_depth);
                } else {
                    // normal arg (positional or labeled)
                    a = parse_call_arg(ternary_depth);
                }

                ast_.add_arg(a);
                ++count;

                // normal separator
                if (cursor_.eat(K::kComma)) {
                    // allow trailing comma before ')'
                    if (cursor_.at(K::kRParen)) break;
                    continue;
                }

                // if we didn't progress (or we hit junk), recover to ',' or ')'
                if (cursor_.pos() == before && !cursor_.at(K::kRParen)) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, token_display(cursor_.peek()));
                    recover_to_delim(K::kComma, K::kRParen);
                    if (cursor_.eat(K::kComma)) continue;
                }

                break;
            }
        }

        // closing ')'
        Token rp = cursor_.peek();
        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, rp.span, ")");
            recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
            rp = cursor_.peek();
            cursor_.eat(K::kRParen);
        }

        // ---- call arg mix rule: labeled + positional mixing not allowed ----
        {
            uint32_t labeled = 0;
            uint32_t positional = 0;

            const auto& args = ast_.args();
            for (uint32_t i = 0; i < count; ++i) {
                const auto& a = args[begin + i];

                // named-group 자체는 mix 판정에서 제외(원하면 포함시켜도 됨)
                if (a.kind == ast::ArgKind::kNamedGroup) continue;

                if (a.has_label) ++labeled;
                else ++positional;
            }

            if (labeled > 0 && positional > 0) {
                // 어디를 span으로 찍을지: call의 '(' 위치 or 첫 섞인 arg 위치
                // 여기선 call 전체 범위의 시작( lparen_tok ) 근처를 사용
                diag_report(diag::Code::kCallArgMixNotAllowed, lparen_tok.span);
            }
        }

        ast::Expr e{};
        e.kind = ast::ExprKind::kCall;
        e.span = span_join(ast_.expr(callee).span, rp.span);
        e.a = callee;
        e.arg_begin = begin;
        e.arg_count = count;
        return ast_.add_expr(e);
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

        const Token lb = cursor_.peek();
        diag_expect(K::kLBrace);

        std::vector<ast::StmtId> local;
        local.reserve(16);

        ast::ExprId tail = ast::k_invalid_expr;

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            const auto k = cursor_.peek().kind;

            // 1) unambiguous stmt starters
            if (is_unambiguous_stmt_start(k)) {
                local.push_back(parse_stmt_any());
                continue;
            }

            // 2) otherwise: parse expression first
            const Token start = cursor_.peek();
            ast::ExprId e = parse_expr_pratt(0, ternary_depth);

            const auto expr_end = ast_.expr(e).span;
            const bool expr_with_block = is_expr_with_block_kind(ast_.expr(e).kind);

            // 2-a) expr ';'
            if (cursor_.at(K::kSemicolon)) {
                const Token semi = cursor_.bump();

                // --- 핵심: tail 위치에서 ';'가 붙고 바로 '}'라면 ---
                //   진단 후: expr-stmt로 만들지 말고 tail로 복구
                if (policy == BlockTailPolicy::kRequireValueTail && cursor_.at(K::kRBrace)) {
                    diag_report(diag::Code::kBlockTailSemicolonNotAllowed, semi.span);
                    tail = e;
                    break;
                }

                ast::Stmt s{};
                s.kind = ast::StmtKind::kExprStmt;
                s.expr = e;
                s.span = span_join(start.span, semi.span);
                local.push_back(ast_.add_stmt(s));
                continue;
            }

            // 2-b) expr '}'  => tail
            if (cursor_.at(K::kRBrace)) {
                tail = e;
                break;
            }

            // 2-c) expr-with-block + (not ';', not '}') => stmt 로 허용 (Rust 스타일)
            // 예: if (...) { ... } else { ... }  <newline>  let x: ...
            if (expr_with_block) {
                ast::Stmt s{};
                s.kind = ast::StmtKind::kExprStmt;
                s.expr = e;
                s.span = span_join(start.span, expr_end);
                local.push_back(ast_.add_stmt(s));
                continue;
            }

            // 2-d) 그 외는 문법 오류
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "';' or '}'");
            recover_to_delim(K::kSemicolon, K::kRBrace, K::kEof);
            if (cursor_.at(K::kSemicolon)) cursor_.bump();
        }

        const Token rb = cursor_.peek();
        diag_expect(K::kRBrace);

        // --- policy: value tail required ---
        if (policy == BlockTailPolicy::kRequireValueTail && tail == ast::k_invalid_expr) {
            // '}' 위치를 찍어도 되고, 마지막 토큰을 찍어도 됨. 여기선 '}'.
            diag_report(diag::Code::kIfExprBranchValueExpected, rb.span);
        }

        // commit stmt children slice
        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (auto id : local) ast_.add_stmt_child(id);

        ast::Stmt blk{};
        blk.kind = ast::StmtKind::kBlock;
        blk.span = span_join(lb.span, rb.span);
        blk.stmt_begin = begin;
        blk.stmt_count = static_cast<uint32_t>(local.size());
        ast::StmtId blk_id = ast_.add_stmt(blk);

        ast::Expr out{};
        out.kind = ast::ExprKind::kBlockExpr;
        out.span = span_join(lb.span, rb.span);
        out.a = blk_id; // (설계상 Expr.a에 StmtId 저장)
        out.b = tail;
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

} // namespace gaupel