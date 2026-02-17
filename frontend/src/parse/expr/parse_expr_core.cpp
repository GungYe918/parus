// frontend/src/parse/parse_expr_core.cpp
#include <parus/parse/Parser.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/syntax/Precedence.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/DiagCode.hpp>

#include <cctype>
#include <string_view>
#include <vector>


namespace parus {

    static std::string_view token_display(const Token& t) {
        // lexeme이 비면(EOF 등) kind 이름으로 대체
        if (!t.lexeme.empty()) return t.lexeme;
        return syntax::token_kind_name(t.kind);
    }

    static bool starts_with_(std::string_view s, std::string_view pfx) {
        return s.size() >= pfx.size() && s.substr(0, pfx.size()) == pfx;
    }

    static bool ends_with_(std::string_view s, std::string_view sfx) {
        return s.size() >= sfx.size() && s.substr(s.size() - sfx.size()) == sfx;
    }

    static std::string_view trim_ascii_ws_(std::string_view s) {
        size_t lo = 0;
        while (lo < s.size() && std::isspace(static_cast<unsigned char>(s[lo]))) lo++;
        size_t hi = s.size();
        while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1]))) hi--;
        return s.substr(lo, hi - lo);
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

    ast::ExprId Parser::parse_expr_full() {
        ast::ExprId e = parse_expr_pratt(/*min_prec=*/0, /*ternary_depth=*/0);
        if (cursor_.peek().kind == syntax::TokenKind::kEof) return e;

        ast::Expr err{};
        err.kind = ast::ExprKind::kError;
        err.span = cursor_.peek().span;
        err.text = "expr_trailing_tokens";
        return ast_.add_expr(err);
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
            e.string_is_raw = starts_with_(t.lexeme, "R\"\"\"");
            e.string_is_format = starts_with_(t.lexeme, "F\"\"\"");

            // F"""...{expr}...""" parsing:
            // - literal braces: '{{' / '}}'
            // - interpolation: '{ expr }'
            // - parts are stored in ast.fstring_parts()
            if (e.string_is_format && ends_with_(t.lexeme, "\"\"\"") && t.lexeme.size() >= 7) {
                const std::string_view body = t.lexeme.substr(4, t.lexeme.size() - 7);
                const uint32_t base_lo = t.span.lo + 4;

                e.string_part_begin = static_cast<uint32_t>(ast_.fstring_parts().size());
                e.string_part_count = 0;

                std::string literal_buf;
                size_t literal_start = 0;
                bool has_literal_start = false;

                auto flush_literal = [&](size_t end_pos) {
                    if (literal_buf.empty()) return;
                    ast::FStringPart p{};
                    p.is_expr = false;
                    p.text = ast_.add_owned_string(std::move(literal_buf));
                    p.span = Span{
                        t.span.file_id,
                        static_cast<uint32_t>(base_lo + static_cast<uint32_t>(literal_start)),
                        static_cast<uint32_t>(base_lo + static_cast<uint32_t>(end_pos))
                    };
                    ast_.add_fstring_part(p);
                    e.string_part_count += 1;
                    literal_buf.clear();
                    has_literal_start = false;
                };

                auto parse_embedded_expr = [&](std::string_view expr_src, uint32_t abs_lo, uint32_t abs_hi) -> ast::ExprId {
                    const size_t expr_before = ast_.exprs().size();

                    Lexer nested_lexer(expr_src, t.span.file_id, nullptr);
                    const auto nested_tokens = nested_lexer.lex_all();

                    Parser nested_parser(nested_tokens, ast_, types_, nullptr);
                    const ast::ExprId inner = nested_parser.parse_expr_full();

                    const size_t expr_after = ast_.exprs().size();
                    for (size_t i = expr_before; i < expr_after; ++i) {
                        auto& ne = ast_.expr_mut(static_cast<ast::ExprId>(i));
                        ne.span.file_id = t.span.file_id;
                        ne.span.lo = abs_lo + ne.span.lo;
                        ne.span.hi = abs_lo + ne.span.hi;
                    }

                    const bool ok =
                        inner != ast::k_invalid_expr &&
                        static_cast<size_t>(inner) < ast_.exprs().size() &&
                        ast_.expr(inner).kind != ast::ExprKind::kError;
                    if (!ok) {
                        diag_report(diag::Code::kUnexpectedToken, Span{t.span.file_id, abs_lo, abs_hi}, "fstring interpolation expression");
                    }
                    return inner;
                };

                size_t i = 0;
                while (i < body.size()) {
                    const char c = body[i];

                    if (c == '{') {
                        if (i + 1 < body.size() && body[i + 1] == '{') {
                            if (!has_literal_start) {
                                literal_start = i;
                                has_literal_start = true;
                            }
                            literal_buf.push_back('{');
                            i += 2;
                            continue;
                        }

                        flush_literal(i);
                        const size_t open_pos = i;
                        i += 1; // after '{'
                        const size_t expr_begin = i;

                        const std::string_view remain = body.substr(expr_begin);
                        Lexer scan_lexer(remain, t.span.file_id, nullptr);
                        const auto scan_tokens = scan_lexer.lex_all();

                        bool found_close = false;
                        size_t close_rel_lo = 0; // relative to `remain`
                        size_t close_rel_hi = 0; // relative to `remain`
                        int depth = 1;

                        for (const auto& st : scan_tokens) {
                            if (st.kind == syntax::TokenKind::kEof) break;
                            if (st.kind == syntax::TokenKind::kLBrace) {
                                depth += 1;
                                continue;
                            }
                            if (st.kind == syntax::TokenKind::kRBrace) {
                                depth -= 1;
                                if (depth == 0) {
                                    found_close = true;
                                    close_rel_lo = static_cast<size_t>(st.span.lo);
                                    close_rel_hi = static_cast<size_t>(st.span.hi);
                                    break;
                                }
                            }
                        }

                        if (!found_close) {
                            const uint32_t lo = static_cast<uint32_t>(base_lo + static_cast<uint32_t>(open_pos));
                            const uint32_t hi = static_cast<uint32_t>(lo + 1);
                            diag_report(diag::Code::kExpectedToken, Span{t.span.file_id, lo, hi}, "}");
                            break;
                        }

                        const size_t expr_end = expr_begin + close_rel_lo; // before closing '}'
                        std::string_view expr_text = body.substr(expr_begin, expr_end - expr_begin);
                        const std::string_view trimmed = trim_ascii_ws_(expr_text);

                        if (trimmed.empty()) {
                            const uint32_t lo = static_cast<uint32_t>(base_lo + static_cast<uint32_t>(expr_begin));
                            const uint32_t hi = static_cast<uint32_t>(base_lo + static_cast<uint32_t>(expr_end));
                            diag_report(diag::Code::kUnexpectedToken, Span{t.span.file_id, lo, hi}, "fstring interpolation expression");
                        } else {
                            const size_t trim_off = static_cast<size_t>(trimmed.data() - body.data());
                            const uint32_t abs_lo = static_cast<uint32_t>(base_lo + static_cast<uint32_t>(trim_off));
                            const uint32_t abs_hi = static_cast<uint32_t>(abs_lo + static_cast<uint32_t>(trimmed.size()));

                            ast::FStringPart p{};
                            p.is_expr = true;
                            p.expr = parse_embedded_expr(trimmed, abs_lo, abs_hi);
                            p.span = Span{t.span.file_id, abs_lo, abs_hi};
                            ast_.add_fstring_part(p);
                            e.string_part_count += 1;
                        }

                        i = expr_begin + close_rel_hi; // consume closing '}'
                        literal_start = i;
                        has_literal_start = false;
                        continue;
                    }

                    if (c == '}') {
                        if (i + 1 < body.size() && body[i + 1] == '}') {
                            if (!has_literal_start) {
                                literal_start = i;
                                has_literal_start = true;
                            }
                            literal_buf.push_back('}');
                            i += 2;
                            continue;
                        }
                        const uint32_t lo = static_cast<uint32_t>(base_lo + static_cast<uint32_t>(i));
                        diag_report(diag::Code::kUnexpectedToken, Span{t.span.file_id, lo, lo + 1}, "}");
                        i += 1;
                        continue;
                    }

                    if (!has_literal_start) {
                        literal_start = i;
                        has_literal_start = true;
                    }
                    literal_buf.push_back(c);
                    i += 1;
                }

                flush_literal(body.size());
            }
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
            // Path primary:
            //   Ident ('::' Ident)*
            // 를 하나의 식별 경로 토큰으로 파싱한다.
            cursor_.bump();

            std::string joined(t.lexeme);
            Span path_sp = t.span;
            bool has_path_tail = false;

            auto eat_coloncolon = [&]() -> bool {
                if (cursor_.eat(syntax::TokenKind::kColonColon)) return true;
                if (cursor_.at(syntax::TokenKind::kColon) && cursor_.peek(1).kind == syntax::TokenKind::kColon) {
                    cursor_.bump();
                    cursor_.bump();
                    return true;
                }
                return false;
            };

            while (eat_coloncolon()) {
                const Token seg = cursor_.peek();
                // explicit acts path segment:
                //   TypePath::acts(NameOrPath)::member
                if (seg.kind == syntax::TokenKind::kKwActs &&
                    cursor_.peek(1).kind == syntax::TokenKind::kLParen) {
                    const Token acts_kw = cursor_.bump(); // acts
                    const Token lp = cursor_.bump();      // '('

                    std::string acts_set;
                    Span acts_end = lp.span;

                    auto parse_acts_set_path = [&]() -> bool {
                        const Token first_set = cursor_.peek();
                        if (first_set.kind != syntax::TokenKind::kIdent) {
                            diag_report(diag::Code::kUnexpectedToken, first_set.span, "acts name identifier");
                            return false;
                        }

                        cursor_.bump();
                        acts_set.assign(first_set.lexeme.data(), first_set.lexeme.size());
                        acts_end = first_set.span;

                        while (eat_coloncolon()) {
                            const Token set_seg = cursor_.peek();
                            if (set_seg.kind != syntax::TokenKind::kIdent) {
                                diag_report(diag::Code::kUnexpectedToken, set_seg.span, "acts name path segment");
                                return false;
                            }
                            cursor_.bump();
                            acts_set += "::";
                            acts_set.append(set_seg.lexeme.data(), set_seg.lexeme.size());
                            acts_end = set_seg.span;
                        }
                        return true;
                    };

                    bool set_ok = parse_acts_set_path();
                    if (!set_ok) {
                        recover_to_delim(syntax::TokenKind::kRParen, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRBrace);
                    }

                    if (!cursor_.eat(syntax::TokenKind::kRParen)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                        recover_to_delim(syntax::TokenKind::kRParen, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRBrace);
                        cursor_.eat(syntax::TokenKind::kRParen);
                    } else {
                        acts_end = cursor_.prev().span;
                    }

                    joined += "::acts(";
                    joined += acts_set;
                    joined += ")";
                    path_sp = span_join(path_sp, acts_end.hi ? acts_end : acts_kw.span);
                    has_path_tail = true;
                    continue;
                }

                if (seg.kind != syntax::TokenKind::kIdent) {
                    diag_report(diag::Code::kUnexpectedToken, seg.span, "identifier (path segment)");
                    break;
                }
                cursor_.bump();
                joined += "::";
                joined.append(seg.lexeme.data(), seg.lexeme.size());
                path_sp = span_join(path_sp, seg.span);
                has_path_tail = true;
            }

            const std::string_view path_text = has_path_tail
                ? ast_.add_owned_string(std::move(joined))
                : t.lexeme;

            // Field literal primary:
            //   TypePath{ name: expr, ... }
            if (cursor_.at(syntax::TokenKind::kLBrace)) {
                const Token lb = cursor_.bump(); // '{'
                const uint32_t begin = static_cast<uint32_t>(ast_.field_init_entries().size());
                uint32_t count = 0;

                while (!cursor_.at(syntax::TokenKind::kRBrace) &&
                       !cursor_.at(syntax::TokenKind::kEof) &&
                       !is_aborted()) {
                    if (cursor_.eat(syntax::TokenKind::kComma)) {
                        continue;
                    }

                    const Token name_tok = cursor_.peek();
                    if (name_tok.kind != syntax::TokenKind::kIdent) {
                        diag_report(diag::Code::kFieldMemberNameExpected, name_tok.span);
                        recover_to_delim(syntax::TokenKind::kComma, syntax::TokenKind::kRBrace, syntax::TokenKind::kSemicolon);
                        if (cursor_.eat(syntax::TokenKind::kComma)) continue;
                        break;
                    }
                    cursor_.bump();

                    if (!cursor_.eat(syntax::TokenKind::kColon)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                        recover_to_delim(syntax::TokenKind::kComma, syntax::TokenKind::kRBrace, syntax::TokenKind::kSemicolon);
                        if (cursor_.eat(syntax::TokenKind::kComma)) continue;
                        break;
                    }

                    ast::ExprId rhs = parse_expr_pratt(0, ternary_depth);
                    rhs = parse_expr_postfix(rhs, ternary_depth);

                    ast::FieldInitEntry ent{};
                    ent.name = name_tok.lexeme;
                    ent.expr = rhs;
                    ent.span = span_join(name_tok.span, ast_.expr(rhs).span);
                    ast_.add_field_init_entry(ent);
                    ++count;

                    if (cursor_.eat(syntax::TokenKind::kComma)) {
                        if (cursor_.at(syntax::TokenKind::kRBrace)) break; // trailing comma
                        continue;
                    }
                    break;
                }

                Span end_span = lb.span;
                if (!cursor_.eat(syntax::TokenKind::kRBrace)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
                    recover_to_delim(syntax::TokenKind::kRBrace, syntax::TokenKind::kSemicolon, syntax::TokenKind::kRParen);
                    if (cursor_.eat(syntax::TokenKind::kRBrace)) {
                        end_span = cursor_.prev().span;
                    } else {
                        end_span = cursor_.peek().span;
                    }
                } else {
                    end_span = cursor_.prev().span;
                }

                ast::Expr e{};
                e.kind = ast::ExprKind::kFieldInit;
                e.text = path_text;
                e.field_init_begin = begin;
                e.field_init_count = count;
                e.span = span_join(path_sp, end_span);
                return ast_.add_expr(e);
            }

            ast::Expr e{};
            e.kind = ast::ExprKind::kIdent;
            e.span = path_sp;
            e.text = path_text;
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

            // field/method access chain:
            //   base.ident
            //
            // 현재 AST에서는 전용 노드 대신 Binary(op='.')로 표현한다.
            if (t.kind == K::kDot) {
                const Token dot = cursor_.bump();
                const Token seg = cursor_.peek();
                if (seg.kind != K::kIdent) {
                    diag_report(diag::Code::kUnexpectedToken, seg.span, "identifier after '.'");
                    recover_to_delim(K::kComma, K::kSemicolon, K::kRParen);
                    continue;
                }
                cursor_.bump();

                ast::Expr rhs{};
                rhs.kind = ast::ExprKind::kIdent;
                rhs.span = seg.span;
                rhs.text = seg.lexeme;
                const ast::ExprId rhs_id = ast_.add_expr(rhs);

                ast::Expr e{};
                e.kind = ast::ExprKind::kBinary;
                e.op = K::kDot;
                e.a = base;
                e.b = rhs_id;
                e.span = span_join(ast_.expr(base).span, seg.span);
                (void)dot;
                base = ast_.add_expr(e);
                continue;
            }

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
