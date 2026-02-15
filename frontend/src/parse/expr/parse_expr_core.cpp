// frontend/src/parse/parse_expr_core.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/Precedence.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/DiagCode.hpp>

#include <string_view>
#include <vector>


namespace parus {

    static std::string_view token_display(const Token& t) {
        // lexeme이 비면(EOF 등) kind 이름으로 대체
        if (!t.lexeme.empty()) return t.lexeme;
        return syntax::token_kind_name(t.kind);
    }
    static constexpr bool is_assign_op(parus::syntax::TokenKind k) {
        using K = parus::syntax::TokenKind;
        switch (k) {
            case K::kAssign:
            case K::kPlusAssign:
            case K::kMinusAssign:
            case K::kStarAssign:
            case K::kSlashAssign:
            case K::kPercentAssign:
            case K::kQuestionQuestionAssign:
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
            bool is_mut = false; // for '&mut'
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
            PrefixOp p{};
            p.kind = op.kind;
            p.span = op.span;

            const bool has_mut_suffix = (op.kind == K::kAmp && cursor_.at(K::kKwMut));
            if (has_mut_suffix) {
                // "&mut <place>" expression
                cursor_.bump();
                p.is_mut = true;
            }

            ops.push_back(p);

            if (op.kind == K::kAmp || op.kind == K::kAmpAmp) {
                bump_amp_run(op);
                if (has_mut_suffix) {
                    // "mut" token breaks "& adjacency" intentionally.
                    break_amp_run();
                }
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
            e.unary_is_mut = ops[(size_t)i].is_mut;
            e.a = rhs;
            e.span = span_join(ops[(size_t)i].span, ast_.expr(rhs).span);
            rhs = ast_.add_expr(e);
        }

        // 4) emit diagnostic for ambiguous "&&&" run
        if (saw_ambiguous_amp_run) {
            Span sp = amp_run_start;
            if (amp_run_end.hi) sp = span_join(amp_run_start, amp_run_end);

            diag_report(diag::Code::kAmbiguousAmpPrefixChain, sp);

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

    /// @brief 배열 리터럴(`[e0, e1, ...]`)을 파싱한다.
    ast::ExprId Parser::parse_expr_array_lit(int ternary_depth) {
        using K = syntax::TokenKind;

        const Token lb = cursor_.peek();
        diag_expect(K::kLBracket);

        const uint32_t begin = static_cast<uint32_t>(ast_.args().size());
        uint32_t count = 0;

        if (!cursor_.at(K::kRBracket)) {
            while (!cursor_.at(K::kRBracket) && !cursor_.at(K::kEof)) {
                ast::ExprId item = parse_expr_pratt(0, ternary_depth);
                item = parse_expr_postfix(item, ternary_depth);

                ast::Arg a{};
                a.kind = ast::ArgKind::kPositional;
                a.expr = item;
                a.span = ast_.expr(item).span;
                ast_.add_arg(a);
                ++count;

                if (cursor_.eat(K::kComma)) {
                    if (cursor_.at(K::kRBracket)) break; // trailing comma
                    continue;
                }
                break;
            }
        }

        Token rb = cursor_.peek();
        if (!cursor_.eat(K::kRBracket)) {
            diag_report(diag::Code::kExpectedToken, rb.span, "]");
            recover_to_delim(K::kRBracket, K::kSemicolon, K::kRBrace);
            rb = cursor_.peek();
            cursor_.eat(K::kRBracket);
        }

        ast::Expr out{};
        out.kind = ast::ExprKind::kArrayLit;
        out.arg_begin = begin;
        out.arg_count = count;
        out.span = span_join(lb.span, rb.span);
        return ast_.add_expr(out);
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

        if (t.kind == syntax::TokenKind::kLBracket) {
            return parse_expr_array_lit(ternary_depth);
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

        using K = syntax::TokenKind;

        // helper: 다음 토큰이 "표현식 시작"인지(= ternary의 then-branch 시작 가능) 판정
        auto is_expr_start = [&](K k) -> bool {
            // prefix 연산자로 시작 가능하면 expr-start
            if (syntax::prefix_info(k).has_value()) return true;

            switch (k) {
                // primary starters
                case K::kIntLit:
                case K::kFloatLit:
                case K::kStringLit:
                case K::kCharLit:
                case K::kIdent:
                case K::kHole:
                case K::kKwTrue:
                case K::kKwFalse:
                case K::kKwNull:
                case K::kKwIf:
                case K::kKwLoop:
                case K::kLParen:
                case K::kLBrace:
                case K::kLBracket:
                    return true;
                default:
                    return false;
            }
        };

        // helper: 다음 토큰이 "타입 시작"인지 판정
        auto is_type_start = [&](K k) -> bool {
            switch (k) {
                case K::kAmp:
                case K::kAmpAmp:
                case K::kKwFn:
                case K::kLParen:
                case K::kLBracket:
                case K::kIdent:
                    return true;
                default:
                    return false;
            }
        };

        while (1) {
            const Token t = cursor_.peek();

            // call
            if (t.kind == K::kLParen) {
                const Token lp = cursor_.bump();
                base = parse_expr_call(base, lp, ternary_depth);
                continue;
            }

            // index
            if (t.kind == K::kLBracket) {
                const Token lb = cursor_.bump();
                base = parse_expr_index(base, lb, ternary_depth);
                continue;
            }

            // postfix ++
            if (t.kind == K::kPlusPlus) {
                const Token op = cursor_.bump();
                ast::Expr e{};
                e.kind = ast::ExprKind::kPostfixUnary;
                e.op = op.kind;
                e.a = base;
                e.span = span_join(ast_.expr(base).span, op.span);
                base = ast_.add_expr(e);
                continue;
            }

            // ----------------------------
            // cast postfix  (as / as? / as!)
            //   expr as T
            //   expr as? T
            //   expr as! T
            // ----------------------------
            if (t.kind == K::kKwAs) {
                const Token as_kw = cursor_.bump(); // 'as'

                ast::CastKind ck = ast::CastKind::kAs;
                Span op_span = as_kw.span;

                // as? / as!
                if (cursor_.at(K::kQuestion)) {
                    const Token q = cursor_.bump();
                    ck = ast::CastKind::kAsOptional;
                    op_span = span_join(op_span, q.span);
                } else if (cursor_.at(K::kBang)) {
                    const Token b = cursor_.bump();
                    ck = ast::CastKind::kAsForce;
                    op_span = span_join(op_span, b.span);
                }

                const Token ty_first = cursor_.peek();
                ParsedType parsed_ty{};
                if (!is_type_start(ty_first.kind)) {
                    // "as" 뒤에 타입 시작 토큰이 없으면 전용 진단을 우선 보고한다.
                    diag_report(diag::Code::kCastTargetTypeExpected, ty_first.span);
                    parsed_ty.id = types_.error();
                    parsed_ty.span = op_span;
                } else {
                    parsed_ty = parse_type();
                    if (parsed_ty.id == ty::kInvalidType) {
                        diag_report(diag::Code::kCastTargetTypeExpected, ty_first.span);
                    }
                }

                ast::Expr e{};
                e.kind = ast::ExprKind::kCast;
                e.a = base;
                e.cast_kind = ck;
                e.cast_type = parsed_ty.id;

                Span end = parsed_ty.span.hi ? parsed_ty.span : op_span;
                e.span = span_join(ast_.expr(base).span, end);

                base = ast_.add_expr(e);
                continue;
            }

            // ----------------------------
            // postfix '?' / '!'  (unwrap / try / force-unwrap 계열)
            //
            // IMPORTANT:
            // '?'는 ternary (?:) 와 충돌할 수 있음.
            // - 다음 토큰이 expr-start 이면: "a ? b : c"로 보고 postfix 단계에서 먹지 않는다.
            // - 다음 토큰이 expr-start가 아니면: "a?" postfix로 소비한다.
            //
            // 예)
            //   cond ? 1 : 2      -> 여기 '?' 다음은 intLit(=expr-start) => ternary
            //   foo()? : bar()    -> '?' 다음은 ':'(=expr-start 아님) => postfix '?' 허용
            //   (x?) + 1          -> '?' 다음은 '+'(prefix_info('+')는 있을 수 있으나,
            //                         여기서는 peek(1)이 '+'이므로 expr-start로 보지 않음)
            // ----------------------------
            if (t.kind == K::kQuestion) {
                const Token next = cursor_.peek(1);

                // ternary로 시작할 가능성이 높으면 postfix '?'는 여기서 소비하지 않음
                if (is_expr_start(next.kind)) {
                    break; // pratt loop에서 ternary 처리
                }

                const Token op = cursor_.bump(); // '?'
                ast::Expr e{};
                e.kind = ast::ExprKind::kPostfixUnary;
                e.op = op.kind;
                e.a = base;
                e.span = span_join(ast_.expr(base).span, op.span);
                base = ast_.add_expr(e);
                continue;
            }

            if (t.kind == K::kBang) {
                const Token op = cursor_.bump(); // '!'
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


} // namespace parus
