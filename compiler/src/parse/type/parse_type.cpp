// compiler/src/parse/parse_type.cpp
#include <gaupel/parse/Parser.hpp>

#include <charconv>
#include <string>

namespace gaupel {

    namespace {

        /// @brief 타입 배열 길이 리터럴(예: `3`, `1_024`)을 `u32`로 파싱한다.
        bool parse_array_size_u32_(std::string_view text, uint32_t& out) {
            std::string digits;
            digits.reserve(text.size());

            bool saw_digit = false;
            for (char ch : text) {
                if (ch == '_') continue;
                if (ch >= '0' && ch <= '9') {
                    digits.push_back(ch);
                    saw_digit = true;
                    continue;
                }
                return false;
            }
            if (!saw_digit) return false;

            uint32_t v = 0;
            const char* begin = digits.data();
            const char* end = begin + digits.size();
            auto [ptr, ec] = std::from_chars(begin, end, v, 10);
            if (ec != std::errc{} || ptr != end) return false;
            out = v;
            return true;
        }

    } // namespace

    Parser::ParsedType Parser::parse_type() {
        using K = syntax::TokenKind;

        const Token start = cursor_.peek();
        (void)start;

        // --------------------
        // type grammar (v0)
        // --------------------
        //
        //  Type :=
        //      PrefixType
        //
        //  PrefixType :=
        //      ( '&' ['mut'] | '&&' )* SuffixType
        //
        //  SuffixType :=
        //      PrimaryType ( '?' | '[]' )*
        //
        //  PrimaryType :=
        //      'fn' '(' TypeList? ')' '->' Type
        //    | Ident
        //    | '(' Type ')'
        //
        // precedence (tight -> loose):
        //   Primary  >  Suffix(?,[])  >  Prefix(&,&&)
        //
        // so: &&int?  == &&(int?)
        // and user can override by parentheses: (&&int)? , &&(int?) , etc.
        //
        // EXTRA RULE (requested):
        //   To avoid visually/semantically ambiguous chains like "&&&T" or "&&&&T",
        //   we forbid 3 or more consecutive '&' characters that come from adjacent
        //   prefix tokens without parentheses separation.
        //
        //   Allowed examples:
        //     &&( &T )
        //     (&(&T))          // uses parentheses to make intent explicit
        //     &mut &&T         // "mut" breaks adjacency visually, allowed by default
        //
        //   Forbidden examples:
        //     &&&T
        //     &&&&T
        //     &&&T?
        //
        // If forbidden pattern appears, we emit an error and return error type
        // (but still try to recover by parsing the rest).

        auto parse_primary = [&]() -> ParsedType {
            const Token s = cursor_.peek();

            // ---- fn(...) -> R ----
            // IMPORTANT:
            // 타입 문맥에서만 "fn (" 형태를 함수 타입으로 인정한다.
            // "fn Ident ..." (함수 선언 헤더처럼 보이는 형태)는 타입 파서가 과소비하지 않도록
            // 여기서 즉시 에러로 처리하고 토큰을 많이 먹지 않는다.
            if (cursor_.at(K::kKwFn)) {
                if (cursor_.peek(1).kind != K::kLParen) {
                    // fn 타입이 아닌데 type 위치에 등장한 경우: 과도한 recover 금지
                    diag_report(diag::Code::kTypeFnSignatureExpected, s.span);
                    cursor_.bump(); // consume only 'fn' to ensure progress

                    ParsedType out{};
                    out.id = types_.error();
                    out.span = s.span;
                    return out;
                }

                cursor_.bump(); // 'fn'

                // '('
                cursor_.bump(); // '(' guaranteed by lookahead

                // params (TypeList?)
                std::vector<ty::TypeId> params;
                Span last = s.span;

                if (!cursor_.at(K::kRParen)) {
                    while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                        auto pt = parse_type();
                        if (pt.id != ty::kInvalidType) params.push_back(pt.id);
                        last = pt.span.hi ? pt.span : last;

                        if (cursor_.eat(K::kComma)) {
                            if (cursor_.at(K::kRParen)) break;
                            continue;
                        }
                        break;
                    }
                }

                Token rp = cursor_.peek();
                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, rp.span, ")");
                    recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
                    rp = cursor_.peek();
                    cursor_.eat(K::kRParen);
                }

                // '->'
                if (!cursor_.at(K::kArrow)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "->");
                    recover_to_delim(K::kArrow, K::kLBrace, K::kSemicolon);
                    cursor_.eat(K::kArrow);
                } else {
                    cursor_.bump();
                }

                auto rt = parse_type();
                if (rt.id == ty::kInvalidType) rt.id = types_.error();

                ty::TypeId fn_id = ty::kInvalidType;
                if (!params.empty()) {
                    fn_id = types_.make_fn(rt.id, params.data(), (uint32_t)params.size());
                } else {
                    fn_id = types_.make_fn(rt.id, nullptr, 0);
                }

                ParsedType out{};
                out.id = fn_id;
                out.span = span_join(s.span, rt.span.hi ? rt.span : cursor_.prev().span);
                return out;
            }

            // ---- ( Type ) ----
            if (cursor_.at(K::kLParen)) {
                const Token lp = cursor_.bump(); // '('

                auto inner = parse_type();
                if (inner.id == ty::kInvalidType) inner.id = types_.error();

                Token rp = cursor_.peek();
                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, rp.span, ")");
                    recover_to_delim(K::kRParen, K::kQuestion, K::kLBracket);
                    rp = cursor_.peek();
                    cursor_.eat(K::kRParen);
                }

                ParsedType out{};
                out.id = inner.id;
                out.span = span_join(lp.span, rp.span.hi ? rp.span : inner.span);
                return out;
            }

            // ---- Slice element type sugar: [T] ----
            // v0에서는 &[T], &mut [T] 표기를 지원하기 위해 타입 1개를 받는다.
            if (cursor_.at(K::kLBracket)) {
                const Token lb = cursor_.bump(); // '['

                auto elem = parse_type();
                if (elem.id == ty::kInvalidType) elem.id = types_.error();

                Token rb = cursor_.peek();
                if (!cursor_.eat(K::kRBracket)) {
                    diag_report(diag::Code::kExpectedToken, rb.span, "]");
                    recover_to_delim(K::kRBracket, K::kQuestion, K::kComma);
                    rb = cursor_.peek();
                    cursor_.eat(K::kRBracket);
                }

                ParsedType out{};
                out.id = types_.make_array(elem.id);
                out.span = span_join(lb.span, rb.span.hi ? rb.span : elem.span);
                return out;
            }

            // ---- Ident / Path type ----
            //   Ident ('::' Ident)*
            if (cursor_.at(K::kIdent)) {
                const Token first = cursor_.bump();

                std::vector<std::string_view> segs;
                segs.reserve(4);
                segs.push_back(first.lexeme);

                Span last_span = first.span;

                // Path tail: :: Ident
                while (cursor_.at(K::kColonColon)) {
                    const Token cc = cursor_.bump(); // '::'
                    (void)cc;

                    const Token next = cursor_.peek();
                    if (!cursor_.at(K::kIdent)) {
                        // recover: path expects an ident after '::'
                        diag_report(diag::Code::kExpectedToken, next.span, "identifier after '::'");
                        break;
                    }

                    const Token seg = cursor_.bump();
                    segs.push_back(seg.lexeme);
                    last_span = seg.span;
                }

                if (segs.size() == 1 && segs[0] == "unit") {
                    diag_report(diag::Code::kTypeInternalNameReserved, first.span, "unit");
                    ParsedType out{};
                    out.id = types_.error();
                    out.span = span_join(first.span, last_span);
                    return out;
                }

                ParsedType out{};
                out.id = types_.intern_path(segs.data(), (uint32_t)segs.size());
                out.span = span_join(first.span, last_span);
                return out;
            }

            // ---- error ----
            diag_report(diag::Code::kTypeNameExpected, s.span);
            if (!cursor_.at(K::kEof)) cursor_.bump();

            ParsedType out{};
            out.id = types_.error();
            out.span = s.span;
            return out;
        };

        auto parse_suffix = [&]() -> ParsedType {
            // PrimaryType
            auto base = parse_primary();
            if (base.id == ty::kInvalidType) base.id = types_.error();

            // ( '?' | '[]' )*
            for (;;) {
                // ---- Optional suffix: T? ----
                if (cursor_.at(K::kQuestion)) {
                    const Token q = cursor_.bump(); // '?'
                    base.id = types_.make_optional(base.id);
                    base.span = span_join(base.span, q.span);
                    continue;
                }

                // ---- Array suffix: T[] / T[N] ----
                if (cursor_.at(K::kLBracket)) {
                    const Token lb = cursor_.bump(); // '['

                    bool has_size = false;
                    uint32_t size = 0;

                    if (!cursor_.at(K::kRBracket)) {
                        const Token n = cursor_.peek();
                        if (n.kind == K::kIntLit) {
                            cursor_.bump();
                            if (!parse_array_size_u32_(n.lexeme, size)) {
                                diag_report(diag::Code::kArraySizeInvalidLiteral, n.span, n.lexeme);
                            } else {
                                has_size = true;
                            }
                        } else {
                            diag_report(diag::Code::kArraySizeExpectedIntLiteral, n.span);
                        }
                    }

                    Token rb = cursor_.peek();
                    if (!cursor_.eat(K::kRBracket)) {
                        diag_report(diag::Code::kExpectedToken, rb.span, "]");
                        recover_to_delim(K::kRBracket, K::kQuestion, K::kComma);
                        rb = cursor_.peek();
                        cursor_.eat(K::kRBracket);
                    }

                    base.id = types_.make_array(base.id, has_size, size);
                    base.span = span_join(base.span, rb.span.hi ? rb.span : lb.span);
                    continue;
                }

                break;
            }

            return base;
        };

        // ---- prefix chain: (& ['mut'] | &&)* ----
        // NOTE: suffix binds tighter than prefix.
        // so: &&int? == &&(int?) by default.
        struct PrefixOp {
            enum class Kind : uint8_t { kBorrow, kEscape } kind;
            bool is_mut = false; // only for borrow
            Token tok{};
        };

        // Track adjacency amp-run to forbid "&&&" style.
        // We count how many '&' characters appear consecutively from adjacent prefix tokens.
        // 'mut' breaks adjacency (it is a separate token), so "&mut&&T" is not considered "&&&".
        bool saw_ambiguous_amp_run = false;
        int  amp_run_chars = 0;       // consecutive '&' chars from adjacent prefix tokens
        bool prev_was_amp_token = false;

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

        std::vector<PrefixOp> ops;
        for (;;) {
            if (cursor_.at(K::kAmp)) {
                PrefixOp op{};
                op.kind = PrefixOp::Kind::kBorrow;
                op.tok = cursor_.bump(); // '&'
                bump_amp_run(op.tok);

                // optional 'mut'
                if (cursor_.at(K::kKwMut)) {
                    cursor_.bump();
                    op.is_mut = true;

                    // "mut" visually/lexically breaks "&" adjacency, so reset run here.
                    break_amp_run();
                }

                ops.push_back(op);
                continue;
            }

            if (cursor_.at(K::kAmpAmp)) {
                PrefixOp op{};
                op.kind = PrefixOp::Kind::kEscape;
                op.tok = cursor_.bump(); // '&&'
                bump_amp_run(op.tok);

                ops.push_back(op);
                continue;
            }

            // any other token breaks adjacency
            break_amp_run();
            break;
        }

        // If we saw "&&&" (or worse) as an adjacent prefix run, force parentheses usage.
        // We still parse the suffix to recover, but the resulting type becomes error.
        if (saw_ambiguous_amp_run) {
            Span sp = amp_run_start;
            if (amp_run_end.hi) sp = span_join(amp_run_start, amp_run_end);

            diag_report(diag::Code::kAmbiguousAmpPrefixChain, sp);
        }

        // operand is suffix-type (so suffix is tighter than prefix)
        auto out = parse_suffix();
        if (out.id == ty::kInvalidType) out.id = types_.error();

        // apply prefixes from right-to-left: && &mut & T  => &&(&mut(&T))
        for (int i = (int)ops.size() - 1; i >= 0; --i) {
            const auto& op = ops[(size_t)i];
            if (op.kind == PrefixOp::Kind::kBorrow) {
                out.id = types_.make_borrow(out.id, op.is_mut);
                out.span = span_join(op.tok.span, out.span);
            } else {
                out.id = types_.make_escape(out.id);
                out.span = span_join(op.tok.span, out.span);
            }
        }

        if (saw_ambiguous_amp_run) {
            // Force error type id, but keep the best-effort span.
            out.id = types_.error();
        }

        return out;
    }

} // namespace gaupel
