// frontend/src/parse/parse_type.cpp
#include <parus/parse/Parser.hpp>

#include <charconv>
#include <string>

namespace parus {

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

        auto make_error_type_ = [&](Span sp) -> ParsedType {
            ast::TypeNode n{};
            n.kind = ast::TypeNodeKind::kError;
            n.span = sp;
            n.resolved_type = types_.error();

            ParsedType out{};
            out.node = ast_.add_type_node(n);
            out.id = types_.error();
            out.span = sp;
            return out;
        };

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
        //      'def' '(' TypeList? ')' '->' Type
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

            // ---- def(...) -> R ----
            // IMPORTANT:
            // 타입 문맥에서만 "def (" 형태를 함수 타입으로 인정한다.
            // "def Ident ..." (함수 선언 헤더처럼 보이는 형태)는 타입 파서가 과소비하지 않도록
            // 여기서 즉시 에러로 처리하고 토큰을 많이 먹지 않는다.
            if (cursor_.at(K::kKwFn)) {
                if (cursor_.peek(1).kind != K::kLParen) {
                    // def 타입이 아닌데 type 위치에 등장한 경우: 과도한 recover 금지
                    diag_report(diag::Code::kTypeFnSignatureExpected, s.span);
                    cursor_.bump(); // consume only 'def' to ensure progress
                    return make_error_type_(s.span);
                }

                cursor_.bump(); // 'def'

                // '('
                cursor_.bump(); // '(' guaranteed by lookahead

                // params (TypeList?)
                std::vector<ParsedType> params;
                Span last = s.span;

                if (!cursor_.at(K::kRParen)) {
                    while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                        auto pt = parse_type();
                        params.push_back(pt);
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

                std::vector<ty::TypeId> param_ids{};
                param_ids.reserve(params.size());
                for (const auto& p : params) {
                    param_ids.push_back(p.id);
                }

                ty::TypeId fn_id = ty::kInvalidType;
                if (!param_ids.empty()) {
                    fn_id = types_.make_fn(rt.id, param_ids.data(), (uint32_t)param_ids.size());
                } else {
                    fn_id = types_.make_fn(rt.id, nullptr, 0);
                }

                ast::TypeNode fn_node{};
                fn_node.kind = ast::TypeNodeKind::kFn;
                fn_node.span = span_join(s.span, rt.span.hi ? rt.span : cursor_.prev().span);
                fn_node.fn_ret = rt.node;
                fn_node.fn_param_begin = static_cast<uint32_t>(ast_.type_node_children().size());
                fn_node.fn_param_count = static_cast<uint32_t>(params.size());
                fn_node.resolved_type = fn_id;
                for (const auto& p : params) {
                    ast_.add_type_node_child(p.node);
                }

                ParsedType out{};
                out.node = ast_.add_type_node(fn_node);
                out.id = fn_id;
                out.span = fn_node.span;
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
                out.node = inner.node;
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

                ast::TypeNode n{};
                n.kind = ast::TypeNodeKind::kArray;
                n.span = span_join(lb.span, rb.span.hi ? rb.span : elem.span);
                n.elem = elem.node;
                n.array_has_size = false;
                n.array_size = 0;
                n.resolved_type = types_.make_array(elem.id);

                ParsedType out{};
                out.node = ast_.add_type_node(n);
                out.id = n.resolved_type;
                out.span = n.span;
                return out;
            }

            // ---- ptr T / ptr mut T ----
            if (cursor_.at(K::kIdent) && cursor_.peek().lexeme == "ptr") {
                const Token ptr_tok = cursor_.bump(); // 'ptr'
                bool is_mut = false;
                if (cursor_.at(K::kKwMut)) {
                    is_mut = true;
                    cursor_.bump();
                }
                auto elem = parse_type();
                if (elem.id == ty::kInvalidType) elem.id = types_.error();

                ast::TypeNode n{};
                n.kind = ast::TypeNodeKind::kPtr;
                n.span = span_join(ptr_tok.span, elem.span.hi ? elem.span : ptr_tok.span);
                n.elem = elem.node;
                n.is_mut = is_mut;
                n.resolved_type = types_.make_ptr(elem.id, is_mut);

                ParsedType out{};
                out.node = ast_.add_type_node(n);
                out.id = n.resolved_type;
                out.span = n.span;
                return out;
            }

            // ---- Type macro call: $path(...) ----
            if (cursor_.at(K::kDollar)) {
                const Token dol = cursor_.bump();

                uint32_t path_begin = 0;
                uint32_t path_count = 0;
                Span path_span = dol.span;
                if (!parse_macro_call_path(path_begin, path_count, path_span)) {
                    return make_error_type_(span_join(dol.span, cursor_.prev().span));
                }

                const auto [arg_begin, arg_count] = parse_macro_call_arg_tokens();
                const Span out_sp = span_join(dol.span, cursor_.prev().span);

                ast::TypeNode n{};
                n.kind = ast::TypeNodeKind::kMacroCall;
                n.span = out_sp;
                n.macro_path_begin = path_begin;
                n.macro_path_count = path_count;
                n.macro_arg_begin = arg_begin;
                n.macro_arg_count = arg_count;
                n.resolved_type = types_.error();

                ParsedType out{};
                out.node = ast_.add_type_node(n);
                out.id = n.resolved_type;
                out.span = out_sp;
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

                std::vector<ParsedType> generic_args{};
                if (cursor_.eat(K::kLt)) {
                    while (!cursor_.at(K::kGt) && !cursor_.at(K::kEof)) {
                        ParsedType at = parse_type();
                        generic_args.push_back(at);
                        if (cursor_.eat(K::kComma)) {
                            if (cursor_.at(K::kGt)) break;
                            continue;
                        }
                        break;
                    }

                    if (!cursor_.eat(K::kGt)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ">");
                        recover_to_delim(K::kGt, K::kQuestion, K::kLBracket);
                        cursor_.eat(K::kGt);
                    }
                    last_span = cursor_.prev().span;
                }

                if (segs.size() == 1 && segs[0] == "unit") {
                    diag_report(diag::Code::kTypeInternalNameReserved, first.span, "unit");
                    return make_error_type_(span_join(first.span, last_span));
                }

                const uint32_t pb = static_cast<uint32_t>(ast_.path_segs().size());
                for (const auto seg : segs) {
                    ast_.add_path_seg(seg);
                }

                ast::TypeNode n{};
                n.kind = ast::TypeNodeKind::kNamedPath;
                n.span = span_join(first.span, last_span);
                n.path_begin = pb;
                n.path_count = static_cast<uint32_t>(segs.size());
                n.generic_arg_begin = static_cast<uint32_t>(ast_.type_node_children().size());
                n.generic_arg_count = static_cast<uint32_t>(generic_args.size());
                for (const auto& ga : generic_args) {
                    ast_.add_type_node_child(ga.node);
                }

                if (generic_args.empty()) {
                    n.resolved_type = types_.intern_path(segs.data(), static_cast<uint32_t>(segs.size()));
                } else {
                    std::string flat{};
                    for (size_t i = 0; i < segs.size(); ++i) {
                        if (i) flat += "::";
                        flat += std::string(segs[i]);
                    }
                    flat += "<";
                    for (size_t i = 0; i < generic_args.size(); ++i) {
                        if (i) flat += ",";
                        flat += types_.to_string(generic_args[i].id);
                    }
                    flat += ">";
                    n.resolved_type = types_.intern_ident(ast_.add_owned_string(std::move(flat)));
                }

                ParsedType out{};
                out.node = ast_.add_type_node(n);
                out.id = n.resolved_type;
                out.span = n.span;
                return out;
            }

            // ---- error ----
            diag_report(diag::Code::kTypeNameExpected, s.span);
            if (!cursor_.at(K::kEof)) cursor_.bump();

            return make_error_type_(s.span);
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
                    const Span sp = span_join(base.span, q.span);
                    ast::TypeNode n{};
                    n.kind = ast::TypeNodeKind::kOptional;
                    n.span = sp;
                    n.elem = base.node;
                    n.resolved_type = types_.make_optional(base.id);

                    base.node = ast_.add_type_node(n);
                    base.id = n.resolved_type;
                    base.span = sp;
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

                    const Span sp = span_join(base.span, rb.span.hi ? rb.span : lb.span);
                    ast::TypeNode n{};
                    n.kind = ast::TypeNodeKind::kArray;
                    n.span = sp;
                    n.elem = base.node;
                    n.array_has_size = has_size;
                    n.array_size = size;
                    n.resolved_type = types_.make_array(base.id, has_size, size);

                    base.node = ast_.add_type_node(n);
                    base.id = n.resolved_type;
                    base.span = sp;
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
                const Span sp = span_join(op.tok.span, out.span);
                ast::TypeNode n{};
                n.kind = ast::TypeNodeKind::kBorrow;
                n.span = sp;
                n.elem = out.node;
                n.is_mut = op.is_mut;
                n.resolved_type = types_.make_borrow(out.id, op.is_mut);
                out.node = ast_.add_type_node(n);
                out.id = n.resolved_type;
                out.span = sp;
            } else {
                const Span sp = span_join(op.tok.span, out.span);
                ast::TypeNode n{};
                n.kind = ast::TypeNodeKind::kEscape;
                n.span = sp;
                n.elem = out.node;
                n.resolved_type = types_.make_escape(out.id);
                out.node = ast_.add_type_node(n);
                out.id = n.resolved_type;
                out.span = sp;
            }
        }

        if (saw_ambiguous_amp_run) {
            // Force error type id, but keep the best-effort span.
            out.id = types_.error();
            if (out.node != ast::k_invalid_type_node &&
                static_cast<size_t>(out.node) < ast_.type_nodes().size()) {
                ast_.type_node_mut(out.node).resolved_type = out.id;
            }
        }

        return out;
    }

    ast::TypeNodeId Parser::parse_type_full_for_macro(ty::TypeId* out_type) {
        const auto p = parse_type();
        if (out_type) *out_type = p.id;

        if (!cursor_.at(syntax::TokenKind::kEof)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "end of type");
            if (out_type) *out_type = types_.error();
            return ast::k_invalid_type_node;
        }
        return p.node;
    }

} // namespace parus
