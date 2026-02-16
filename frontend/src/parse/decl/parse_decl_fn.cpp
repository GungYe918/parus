// frontend/src/parse/parse_decl_fn.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/ty/Type.hpp>
#include <parus/ty/TypePool.hpp>

#include <unordered_set>
#include <vector>


namespace parus {

    namespace {

        /// @brief 링크 ABI 문자열 토큰이 C ABI(`"C"`)인지 검사한다.
        bool is_c_abi_lit_(const Token& t) {
            if (t.kind != syntax::TokenKind::kStringLit) return false;
            return t.lexeme == "\"C\"";
        }

    } // namespace
    
    // '@attr' 리스트를 파싱하여 AST arena에 저장한다.
    // 정책(v0): '@' 뒤는 ident만 허용. 그 외는 진단 후 회복.
    std::pair<uint32_t, uint32_t> Parser::parse_decl_fn_attr_list() {
        using K = syntax::TokenKind;

        const uint32_t begin = static_cast<uint32_t>(ast_.fn_attrs().size());
        uint32_t count = 0;

        while (cursor_.at(K::kAt)) {
            cursor_.bump(); // '@'

            const Token name_tok = cursor_.peek();
            if (name_tok.kind == K::kIdent) {
                cursor_.bump();

                ast::Attr a{};
                a.name = name_tok.lexeme;
                a.span = name_tok.span;
                ast_.add_fn_attr(a);
                ++count;
                continue;
            }

            diag_report(diag::Code::kAttrNameExpectedAfterAt, name_tok.span);

            //  무한루프 방지를 위해 EOF가 아니면 1토큰 전진
            if (!cursor_.at(K::kEof)) cursor_.bump();
        }

        return { begin, count };
    }

    // 파라미터 1개(Ident ':' Type ['=' Expr])를 파싱한다.
    bool Parser::parse_decl_fn_one_param(bool is_named_group, std::string_view* out_name, bool* out_is_self) {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start_span = start_tok.span;

        // ---- optional 'mut' before param name ----
        bool is_mut = false;
        if (cursor_.at(K::kKwMut)) {
            is_mut = true;
            start_span = cursor_.bump().span; // start span becomes 'mut'
        }

        bool is_self = false;
        Token name_tok = cursor_.peek();

        // self receiver marker: `self x: T`
        if (name_tok.kind == K::kIdent && name_tok.lexeme == "self") {
            is_self = true;
            start_span = is_mut ? start_span : name_tok.span;
            cursor_.bump(); // self
            name_tok = cursor_.peek();
        }

        std::string_view name{};
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kFnParamNameExpected, name_tok.span);
            recover_to_delim(K::kComma, K::kRParen, K::kRBrace);
            return false;
        }

        if (out_name) *out_name = name;
        if (out_is_self) *out_is_self = is_self;

        // ':'
        if (!cursor_.eat(K::kColon)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
            recover_to_delim(K::kComma, K::kRParen, K::kRBrace);
            return false;
        }

        // Type
        auto ty = parse_type(); // ParsedType { id, span }

        // default
        //
        // POLICY CHANGE:
        // - default 값은 named-group 파라미터에서만 허용한다.
        // - positional 파라미터에서 '='를 만나면 진단 후 recovery를 위해 식은 소비하되,
        //   AST에는 default를 기록하지 않는다.
        bool has_default = false;
        ast::ExprId def = ast::k_invalid_expr;

        bool saw_eq = false;
        Span eq_span = ty.span; // placeholder

        if (cursor_.at(K::kAssign)) {
            const Token eq = cursor_.bump(); // '='
            saw_eq = true;
            eq_span = eq.span;

            if (!is_named_group) {
                // 위치 파라미터 default 금지
                diag_report(diag::Code::kFnParamDefaultNotAllowedOutsideNamedGroup, eq_span);

                // recovery: 가능한 경우 우변 식을 소비해 다음 파라미터/토큰으로 정렬
                const auto nk = cursor_.peek().kind;
                if (!(nk == K::kComma || nk == K::kRParen || nk == K::kRBrace || nk == K::kEof)) {
                    (void)parse_expr();
                }

                // has_default/def는 기록하지 않음 (정책 강제)
                has_default = false;
                def = ast::k_invalid_expr;
            } else {
                has_default = true;

                const auto nk = cursor_.peek().kind;
                if (nk == K::kComma || nk == K::kRParen || nk == K::kRBrace || nk == K::kEof) {
                    // "= <expr>" 에서 expr 누락
                    diag_report(diag::Code::kFnParamDefaultExprExpected, eq_span);
                    def = ast::k_invalid_expr;
                } else {
                    def = parse_expr();
                }
            }
        }

        ast::Param p{};
        p.name = name;
        p.type = ty.id;
        p.is_mut = is_mut;
        p.is_self = is_self;
        p.is_named_group = is_named_group;
        p.has_default = has_default;
        p.default_expr = def;

        // span end
        Span end = ty.span;
        if (has_default && def != ast::k_invalid_expr) {
            end = ast_.expr(def).span;
        } else if (saw_eq) {
            // '='를 봤지만 default를 기록하지 않았거나(expr 누락/금지), 최소 '='까지는 span에 포함
            end = eq_span;
            if (cursor_.prev().span.hi >= eq_span.hi) {
                end = cursor_.prev().span;
            }
        }

        // start span: either 'mut' span (if present) or name span
        Span real_start = is_mut ? start_span : name_tok.span;
        p.span = span_join(real_start, end);

        ast_.add_param(p);
        return true;
    }

    // 함수 파라미터 목록을 파싱한다. (positional + optional named-group)
    void Parser::parse_decl_fn_params(
        uint32_t& out_param_begin,
        uint32_t& out_param_count,
        uint32_t& out_positional_count,
        bool& out_has_named_group
    ) {
        using K = syntax::TokenKind;

        out_param_begin = static_cast<uint32_t>(ast_.params().size());
        out_param_count = 0;
        out_positional_count = 0;
        out_has_named_group = false;

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            recover_to_delim(K::kLParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kLParen);
        }

        if (cursor_.at(K::kRParen)) {
            cursor_.bump();
            return;
        }

        std::unordered_set<std::string_view> seen_pos;
        std::unordered_set<std::string_view> seen_named;

        bool consumed_named_group = false;

        auto parse_named_group_section = [&]() {
            // entering at '{'
            if (consumed_named_group) {
                diag_report(diag::Code::kFnOnlyOneNamedGroupAllowed, cursor_.peek().span);
                cursor_.bump(); // '{'
                recover_to_delim(K::kRBrace, K::kRParen);
                cursor_.eat(K::kRBrace);
                out_has_named_group = true;
                consumed_named_group = true;
                return;
            }

            consumed_named_group = true;
            out_has_named_group = true;

            cursor_.bump(); // '{'

            while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof)) {
                std::string_view pname{};
                const bool ok = parse_decl_fn_one_param(/*is_named_group=*/true, &pname);
                if (ok) {
                    if (!seen_named.insert(pname).second) {
                        // NOTE: 전용 diag code가 있으면 그걸 쓰는 게 더 좋다.
                        diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                                    "duplicate named-group parameter");
                    }
                    ++out_param_count;
                }

                if (cursor_.eat(K::kComma)) {
                    if (cursor_.at(K::kRBrace)) break; // trailing comma
                    continue;
                }
                break;
            }

            if (!cursor_.eat(K::kRBrace)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
                recover_to_delim(K::kRBrace, K::kRParen, K::kArrow);
                cursor_.eat(K::kRBrace);
            }
        };

        while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {

            // allow: "(, { ... })" or "(..., { ... })"  => optional comma before '{' (SPEC NamedGroupOpt)
            if (cursor_.at(K::kComma) && cursor_.peek(1).kind == K::kLBrace) {
                cursor_.bump();
            }

            // named-group section
            if (cursor_.at(K::kLBrace)) {
                parse_named_group_section();

                // named-group 이후에는 ')'만 와야 한다 (엄격, SPEC상 NamedGroupOpt는 마지막)
                if (cursor_.eat(K::kComma)) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                                "no parameters allowed after named-group");
                    recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
                }
                break;
            }

            // positional param
            {
                std::string_view pname{};
                const bool ok = parse_decl_fn_one_param(/*is_named_group=*/false, &pname);
                if (ok) {
                    if (!seen_pos.insert(pname).second) {
                        diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                                    "duplicate positional parameter");
                    }
                    ++out_param_count;
                    ++out_positional_count;
                }
            }

            if (cursor_.eat(K::kComma)) {
                if (cursor_.at(K::kRParen)) break;
                continue;
            }
            break;
        }

        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
            recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kRParen);
        }
    }

    // 함수 선언(스펙 6.1)을 파싱한다.
    // - qualifier 순서 유연화(가능 범위 내에서 전진하며 수집)
    ast::StmtId Parser::parse_decl_fn() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        // 1) @attribute*
        auto [attr_begin, attr_count] = parse_decl_fn_attr_list();

        // 2) linkage prefix (export / extern)
        bool is_export = false;
        bool is_extern = false;
        ast::LinkAbi link_abi = ast::LinkAbi::kNone;
        if (cursor_.at(K::kKwExport) || cursor_.at(K::kKwExtern)) {
            const Token lk = cursor_.bump();
            is_export = (lk.kind == K::kKwExport);
            is_extern = (lk.kind == K::kKwExtern);

            // ABI spec v0.0.1:
            // - extern "C" / export "C"를 C ABI 경계로 취급한다.
            // - export 단독은 기존 내부 export 의미를 유지한다.
            if (cursor_.at(K::kStringLit)) {
                const Token abi_tok = cursor_.peek();
                if (is_c_abi_lit_(abi_tok)) {
                    link_abi = ast::LinkAbi::kC;
                } else {
                    diag_report(diag::Code::kUnexpectedToken, abi_tok.span, "only \"C\" ABI is supported");
                }
                cursor_.bump();
            } else if (is_extern) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "\"C\"");
            }
        }

        // 3) fn
        if (!cursor_.at(K::kKwFn)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "fn");

            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }
        cursor_.bump(); // 'fn'

        // 4) qualifier*
        bool is_pure_kw = false;
        bool is_comptime_kw = false;
        bool is_commit = false;
        bool is_recast = false;

        for (;;) {
            bool progressed = false;
            if (cursor_.at(K::kKwPure)) {
                is_pure_kw = true; cursor_.bump(); progressed = true;
            } else if (cursor_.at(K::kKwComptime)) {
                is_comptime_kw = true; cursor_.bump(); progressed = true;
            } else if (cursor_.at(K::kKwCommit)) {
                is_commit = true; cursor_.bump(); progressed = true;
            } else if (cursor_.at(K::kKwRecast)) {
                is_recast = true; cursor_.bump(); progressed = true;
            }
            if (!progressed) break;
        }

        // 5) Name
        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kFnNameExpected, name_tok.span);
        }

        // 6) '?' (throwing)
        bool is_throwing = false;
        if (cursor_.at(K::kQuestion)) {
            is_throwing = true;
            cursor_.bump();
        }

        // 7) params
        uint32_t param_begin = 0, param_count = 0, positional_count = 0;
        bool has_named_group = false;
        parse_decl_fn_params(param_begin, param_count, positional_count, has_named_group);

        // 8) '->' ReturnType
        if (!cursor_.at(K::kArrow)) {
            if (cursor_.at(K::kMinus) && cursor_.peek(1).kind == K::kGt) {
                cursor_.bump(); cursor_.bump();
            } else {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "->");
                recover_to_delim(K::kArrow, K::kLBrace, K::kSemicolon);
                cursor_.eat(K::kArrow);
            }
        } else {
            cursor_.bump();
        }

        auto ret_ty = parse_type();

        // ------------------------------------------------------------------
        // FIX (핵심):
        // TypePool의 fn 시그니처에는 "positional 파라미터"만 포함한다.
        // named-group 파라미터는 시그니처에 넣지 말고,
        // FnDecl 메타(param list + flags)로만 보관한 뒤 tyck에서 별도 검증한다.
        //
        // 예)
        //   fn sub(a,b,{clamp})  -> sig: fn(i32,i32)->i32   (positional_count=2)
        //   fn mul({a,b})        -> sig: fn()->i32          (positional_count=0)
        //   fn div(a,b,{rounding=0,bias}) -> sig: fn(i32,i32)->i32
        // ------------------------------------------------------------------
        ty::TypeId sig_id = ty::kInvalidType;
        {
            std::vector<ty::TypeId> pts;
            pts.reserve(positional_count);

            for (uint32_t i = 0; i < positional_count; ++i) {
                const auto& p = ast_.params()[param_begin + i];
                pts.push_back(p.type);
            }

            sig_id = types_.make_fn(
                ret_ty.id,
                pts.empty() ? nullptr : pts.data(),
                (uint32_t)pts.size()
            );
        }

        // 9) body/prototype
        ast::StmtId body = ast::k_invalid_stmt;
        Span end_sp = ret_ty.span.hi ? ret_ty.span : cursor_.prev().span;
        if (is_extern) {
            // extern 선언은 body 없이 ';'로 끝난다.
            if (cursor_.at(K::kLBrace)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "extern function declaration must not have a body");
                (void)parse_stmt_required_block("extern fn");
            }
            if (cursor_.at(K::kSemicolon)) {
                end_sp = cursor_.bump().span;
            } else {
                end_sp = stmt_consume_semicolon_or_recover(end_sp);
            }
        } else {
            body = parse_stmt_required_block("fn");
            end_sp = ast_.stmt(body).span;
            if (cursor_.at(K::kSemicolon)) {
                end_sp = cursor_.bump().span;
            }
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFnDecl;
        s.span = span_join(start, end_sp);

        s.name = name;

        s.type = sig_id;          // signature: positional-only
        s.fn_ret = ret_ty.id;

        s.a = body;

        s.is_export = is_export;
        s.is_extern = is_extern;
        s.link_abi = link_abi;
        s.fn_mode = ast::FnMode::kNone;

        s.is_throwing = is_throwing;
        s.is_pure = is_pure_kw;
        s.is_comptime = is_comptime_kw;
        s.is_commit = is_commit;
        s.is_recast = is_recast;

        s.attr_begin = attr_begin;
        s.attr_count = attr_count;

        s.param_begin = param_begin;
        s.param_count = param_count;                 // total params (positional + named-group)
        s.positional_param_count = positional_count; // positional only
        s.has_named_group = has_named_group;

        return ast_.add_stmt(s);
    }

} // namespace parus
