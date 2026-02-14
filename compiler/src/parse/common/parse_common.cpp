// compiler/src/parse/parse_common.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>
#include <gaupel/diag/DiagCode.hpp>


namespace gaupel {

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

} // namespace gaupel
