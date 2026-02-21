// frontend/src/parse/parse_common.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/DiagCode.hpp>


namespace parus {

    namespace {
        uint64_t make_diag_key_(diag::Severity sev, diag::Code code, Span span) {
            uint64_t h = 1469598103934665603ull;
            const auto mix = [&](uint64_t v) {
                h ^= (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
            };
            mix(static_cast<uint64_t>(sev));
            mix(static_cast<uint64_t>(code));
            mix(static_cast<uint64_t>(span.file_id));
            mix(static_cast<uint64_t>(span.lo));
            mix(static_cast<uint64_t>(span.hi));
            return h;
        }
    } // namespace

    void parus::Parser::diag_report(diag::Code code, Span span, std::string_view a0) {
        if (!diags_ || aborted_) return;

        // 동일 severity/code/span 진단은 전역적으로 한 번만 보고한다.
        const uint64_t key = make_diag_key_(diag::Severity::kError, code, span);
        if (!seen_diag_keys_.insert(key).second) return;

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

    void Parser::diag_report_warn(diag::Code code, Span span, std::string_view a0) {
        if (!diags_ || aborted_) return;

        // warning도 동일 severity/code/span은 1회만 보고한다.
        const uint64_t key = make_diag_key_(diag::Severity::kWarning, code, span);
        if (!seen_diag_keys_.insert(key).second) return;

        // 경고도 같은 위치/코드 중복 출력은 억제한다.
        if (span.lo == last_diag_lo_ && code == last_diag_code_) return;
        last_diag_lo_ = span.lo;
        last_diag_code_ = code;

        diag::Diagnostic d(diag::Severity::kWarning, code, span);
        if (!a0.empty()) d.add_arg(a0);
        diags_->add(std::move(d));
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

    bool Parser::is_unambiguous_stmt_start(syntax::TokenKind k) const {
        using K = syntax::TokenKind;

        if (is_decl_start(k)) return true;
        if (k == K::kSemicolon) return true;

        // 확실한 stmt 키워드
        if (k == K::kKwLet || k == K::kKwSet || k == K::kKwStatic)          return true;
        if (k == K::kKwMut) {
            const auto k1 = cursor_.peek(1).kind;
            if (k1 == K::kKwLet || k1 == K::kKwSet || k1 == K::kKwStatic) return true;
        }
        if (k == K::kKwReturn || k == K::kKwBreak || k == K::kKwContinue)   return true;
        if (k == K::kKwWhile || k == K::kKwDo || k == K::kKwSwitch)         return true;
        if (k == K::kKwManual)                                               return true;
        if (k == K::kKwUse)                                                 return true;

        // if/loop/{...} 는 expr도 될 수 있으므로 여기 넣지 않음
        return false;
    }

    bool Parser::is_context_keyword(const Token& t, std::string_view kw) const {
        if (t.kind != syntax::TokenKind::kIdent) return false;
        return t.lexeme == kw;
    }

    bool Parser::is_macro_decl_start() const {
        return is_context_keyword(cursor_.peek(), "macro");
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

    bool Parser::parse_macro_call_path(uint32_t& out_path_begin, uint32_t& out_path_count, Span& out_span) {
        using K = syntax::TokenKind;
        out_path_begin = static_cast<uint32_t>(ast_.path_segs().size());
        out_path_count = 0;

        const Token first = cursor_.peek();
        if (first.kind != K::kIdent) {
            diag_report(diag::Code::kUnexpectedToken, first.span, "macro path segment");
            out_span = first.span;
            return false;
        }

        cursor_.bump();
        ast_.add_path_seg(first.lexeme);
        out_path_count = 1;
        out_span = first.span;

        auto eat_coloncolon = [&]() -> bool {
            if (cursor_.eat(K::kColonColon)) return true;
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
                diag_report(diag::Code::kUnexpectedToken, seg.span, "macro path segment");
                return false;
            }
            cursor_.bump();
            ast_.add_path_seg(seg.lexeme);
            ++out_path_count;
            out_span = span_join(out_span, seg.span);
        }

        return true;
    }

    std::pair<uint32_t, uint32_t> Parser::parse_macro_call_arg_tokens() {
        using K = syntax::TokenKind;
        const uint32_t begin = static_cast<uint32_t>(ast_.macro_tokens().size());
        uint32_t count = 0;

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            return {begin, count};
        }

        int paren = 1;
        int brace = 0;
        int bracket = 0;

        while (!cursor_.at(K::kEof) && paren > 0) {
            const Token t = cursor_.peek();

            if (t.kind == K::kLParen) {
                ++paren;
                ast_.add_macro_token(cursor_.bump());
                ++count;
                continue;
            }
            if (t.kind == K::kRParen) {
                --paren;
                if (paren == 0) {
                    cursor_.bump(); // consume closing ')'
                    break;
                }
                ast_.add_macro_token(cursor_.bump());
                ++count;
                continue;
            }
            if (t.kind == K::kLBrace) {
                ++brace;
                ast_.add_macro_token(cursor_.bump());
                ++count;
                continue;
            }
            if (t.kind == K::kRBrace) {
                if (brace > 0) --brace;
                ast_.add_macro_token(cursor_.bump());
                ++count;
                continue;
            }
            if (t.kind == K::kLBracket) {
                ++bracket;
                ast_.add_macro_token(cursor_.bump());
                ++count;
                continue;
            }
            if (t.kind == K::kRBracket) {
                if (bracket > 0) --bracket;
                ast_.add_macro_token(cursor_.bump());
                ++count;
                continue;
            }

            if (t.kind == K::kEof) break;

            ast_.add_macro_token(cursor_.bump());
            ++count;
        }

        if (paren != 0) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
        }

        return {begin, count};
    }

    ast::ExprId Parser::parse_macro_call_expr() {
        using K = syntax::TokenKind;
        const Token dol = cursor_.peek();
        if (!cursor_.eat(K::kDollar)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "$");
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = cursor_.peek().span;
            e.text = "macro_call_expected_dollar";
            return ast_.add_expr(e);
        }

        uint32_t path_begin = 0;
        uint32_t path_count = 0;
        Span path_sp = dol.span;
        if (!parse_macro_call_path(path_begin, path_count, path_sp)) {
            ast::Expr e{};
            e.kind = ast::ExprKind::kError;
            e.span = span_join(dol.span, cursor_.prev().span);
            e.text = "macro_call_bad_path";
            return ast_.add_expr(e);
        }

        auto [arg_begin, arg_count] = parse_macro_call_arg_tokens();

        ast::Expr e{};
        e.kind = ast::ExprKind::kMacroCall;
        e.macro_path_begin = path_begin;
        e.macro_path_count = path_count;
        e.macro_token_begin = arg_begin;
        e.macro_token_count = arg_count;
        e.span = span_join(dol.span, cursor_.prev().span);
        return ast_.add_expr(e);
    }

} // namespace parus
