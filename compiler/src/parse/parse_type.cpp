// compiler/src/parse/parse_type.cpp
#include <gaupel/parse/Parser.hpp>


namespace gaupel {
    
    Parser::ParsedType Parser::parse_type() {
        using K = syntax::TokenKind;

        const Token start = cursor_.peek();

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

        auto parse_primary = [&]() -> ParsedType {
            const Token s = cursor_.peek();

            // ---- fn(...) -> R ----
            if (cursor_.at(K::kKwFn)) {
                cursor_.bump(); // 'fn'

                // '('
                if (!cursor_.eat(K::kLParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                    recover_to_delim(K::kLParen, K::kArrow, K::kRParen);
                    cursor_.eat(K::kLParen);
                }

                // params
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
                    // 기존 코드처럼 "-""?>" 형태 복구도 가능
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

            // ---- Ident type ----
            if (cursor_.at(K::kIdent)) {
                const Token name = cursor_.bump();

                ParsedType out{};
                out.id = types_.intern_ident(name.lexeme); // builtin or named user
                out.span = name.span;
                return out;
            }

            // ---- error ----
            diag_report(diag::Code::kUnexpectedToken, s.span, "type");
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

                // ---- Array suffix: T[] ----
                if (cursor_.at(K::kLBracket)) {
                    const Token lb = cursor_.bump(); // '['

                    Token rb = cursor_.peek();
                    if (!cursor_.eat(K::kRBracket)) {
                        diag_report(diag::Code::kExpectedToken, rb.span, "]");
                        recover_to_delim(K::kRBracket, K::kQuestion, K::kComma);
                        rb = cursor_.peek();
                        cursor_.eat(K::kRBracket);
                    }

                    base.id = types_.make_array(base.id);
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

        std::vector<PrefixOp> ops;
        for (;;) {
            // single '&' token kind name depends on your TokenKind.
            // 여기서는 kAmp를 가정한다. (기존 punct table에 추가돼 있어야 함)
            if (cursor_.at(K::kAmp)) {
                PrefixOp op{};
                op.kind = PrefixOp::Kind::kBorrow;
                op.tok = cursor_.bump(); // '&'

                // optional 'mut'
                if (cursor_.at(K::kKwMut)) {
                    cursor_.bump();
                    op.is_mut = true;
                }

                ops.push_back(op);
                continue;
            }

            // '&&' (escape)
            if (cursor_.at(K::kAmpAmp)) {
                PrefixOp op{};
                op.kind = PrefixOp::Kind::kEscape;
                op.tok = cursor_.bump(); // '&&'
                ops.push_back(op);
                continue;
            }

            break;
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

        return out;
    }

} // namespace gaupel