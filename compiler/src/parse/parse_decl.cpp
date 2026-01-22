// compiler/src/parse/parse_decl.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <utility>
#include <unordered_set>


namespace gaupel {

    bool Parser::is_decl_start(syntax::TokenKind k) const {
        using K = syntax::TokenKind;
        return k == K::kAt
            || k == K::kKwExport
            || k == K::kKwFn;
            // pub/sub는 class 내부에서만: 여기서 제거
    }

    // decl 엔트리. 현재는 fn decl만 decl로 취급한다.
    ast::StmtId Parser::parse_decl_any() {
        using K = syntax::TokenKind;

        const Token& t = cursor_.peek();

        if (t.kind == K::kAt || t.kind == K::kKwExport || t.kind == K::kKwFn) {
            return parse_decl_fn();
        }

        diag_report(diag::Code::kUnexpectedToken, t.span, "declaration");
        cursor_.bump();

        ast::Stmt s{};
        s.kind = ast::StmtKind::kError;
        s.span = t.span;
        return ast_.add_stmt(s);
    }

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
    bool Parser::parse_decl_fn_one_param(bool is_named_group, std::string_view* out_name) {
        using K = syntax::TokenKind;

        const Token first = cursor_.peek();

        std::string_view name{};
        if (first.kind == K::kIdent) {
            name = first.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kUnexpectedToken, first.span, "identifier (param name)");
            recover_to_delim(K::kComma, K::kRParen, K::kRBrace);
            return false;
        }

        if (out_name) *out_name = name;

        if (!cursor_.eat(K::kColon)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
            recover_to_delim(K::kComma, K::kRParen, K::kRBrace);
            return false;
        }

        ast::TypeId ty = parse_type();

        bool has_default = false;
        ast::ExprId def = ast::k_invalid_expr;

        // '=' 위치(span) 기억: 기본값 식이 누락됐을 때 span 계산에 사용
        bool saw_eq = false;
        Span eq_span = ast_.type_node(ty).span; // placeholder

        if (cursor_.at(K::kAssign)) {
            const Token eq = cursor_.bump(); // '='
            saw_eq = true;
            eq_span = eq.span;

            // ---- 새 룰: 기본값은 named-group '{...}' 안에서만 허용 ----
            if (!is_named_group) {
                diag_report(diag::Code::kUnexpectedToken, eq.span,
                            "default value is only allowed inside named-group '{...}'");

                // 복구: "= expr" 형태면 expr는 소비해서 토큰 흐름을 안정화
                // ("= , ) } eof" 는 expr를 소비하지 않음)
                const auto nk = cursor_.peek().kind;
                if (!(nk == K::kComma || nk == K::kRParen || nk == K::kRBrace || nk == K::kEof)) {
                    (void)parse_expr(); // discard
                }

                // 저장은 금지: AST에는 기본값이 없는 것으로 남긴다.
                has_default = false;
                def = ast::k_invalid_expr;
            } else {
                // named-group 안에서는 정상적으로 기본값 파싱
                has_default = true;

                // ---- 기본값 식 누락 방지: "= , ) }" 같은 케이스 ----
                const auto nk = cursor_.peek().kind;
                if (nk == K::kComma || nk == K::kRParen || nk == K::kRBrace || nk == K::kEof) {
                    // 새 diag code가 없으면 kUnexpectedToken/kExpectedToken 재활용 가능
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "default expression");
                    // 복구: 그냥 invalid_expr로 두고 계속 진행
                } else {
                    def = parse_expr();
                }
            }
        }

        ast::Param p{};
        p.name = name;
        p.type = ty;
        p.is_named_group = is_named_group;
        p.has_default = has_default;
        p.default_expr = def;

        // span 계산:
        // - 기본: type 끝까지
        // - named-group 기본값 + expr 있음: expr 끝까지
        // - named-group 기본값 + expr 없음: '=' 까지
        // - positional에서 '=' 금지였던 케이스: 소비한 토큰까지(span을 최대한 넓혀서 에러 구간 포함)
        Span end = ast_.type_node(ty).span;

        if (has_default && def != ast::k_invalid_expr) {
            end = ast_.expr(def).span;
        } else if (has_default && def == ast::k_invalid_expr) {
            end = eq_span;
        } else if (!has_default && saw_eq && !is_named_group) {
            // 금지된 "= ..." 를 읽고 버린 경우, 최소 '=' 또는 그 뒤 expr까지 포함
            end = cursor_.prev().span;
        }

        p.span = span_join(first.span, end);
        ast_.add_param(p);
        return true;
    }

    // 함수 파라미터 목록을 파싱한다. (positional + optional named-group)
    // 정책(v0): named-group은 최대 1개만 허용.
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

        // ---- 중복 검사 (positional / named-group 분리) ----
        std::unordered_set<std::string_view> seen_pos;
        std::unordered_set<std::string_view> seen_named;

        bool consumed_named_group = false;

        while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {

            // allow ", { ... }"
            if (cursor_.at(K::kComma) && cursor_.peek(1).kind == K::kLBrace) {
                cursor_.bump();
            }

            if (cursor_.at(K::kLBrace)) {
                if (consumed_named_group) {
                    // named-group 2개 이상 금지
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                                "only one named-group '{...}' is allowed");
                    // 복구: 다음 '}' 또는 ')'까지 스킵
                    cursor_.bump();
                    recover_to_delim(K::kRBrace, K::kRParen);
                    cursor_.eat(K::kRBrace);
                    // 이후 계속 파싱하지 않고 종료(정상 흐름도 named-group 이후 종료)
                    consumed_named_group = true;
                    out_has_named_group = true;
                    break;
                }

                consumed_named_group = true;
                out_has_named_group = true;

                cursor_.bump(); // '{'

                if (!cursor_.at(K::kRBrace)) {
                    while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof)) {
                        std::string_view pname{};
                        const bool ok = parse_decl_fn_one_param(/*is_named_group=*/true, &pname);
                        if (ok) {
                            if (!seen_named.insert(pname).second) {
                                diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                                            "duplicate named-group parameter");
                            }
                            ++out_param_count;
                        } else {
                            // 실패해도 카운트는 올리지 않음
                        }

                        if (cursor_.eat(K::kComma)) {
                            if (cursor_.at(K::kRBrace)) break; // trailing comma
                            continue;
                        }
                        break;
                    }
                }

                if (!cursor_.eat(K::kRBrace)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
                    recover_to_delim(K::kRBrace, K::kRParen, K::kArrow);
                    cursor_.eat(K::kRBrace);
                }

                // named-group 이후는 ')'만 기대
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

        // 2) export?
        bool is_export = false;
        if (cursor_.at(K::kKwExport)) {
            is_export = true;
            cursor_.bump();
        }

        // 3) fn (NOTE: pub/sub는 class 전용이므로 여기서 금지)
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
                if (!is_pure_kw) is_pure_kw = true;
                cursor_.bump();
                progressed = true;
            } else if (cursor_.at(K::kKwComptime)) {
                if (!is_comptime_kw) is_comptime_kw = true;
                cursor_.bump();
                progressed = true;
            } else if (cursor_.at(K::kKwCommit)) {
                if (!is_commit) is_commit = true;
                cursor_.bump();
                progressed = true;
            } else if (cursor_.at(K::kKwRecast)) {
                if (!is_recast) is_recast = true;
                cursor_.bump();
                progressed = true;
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
            diag_report(diag::Code::kUnexpectedToken, name_tok.span, "identifier (function name)");
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

        ast::TypeId ret_ty = parse_type();

        // 9) Block
        ast::StmtId body = parse_stmt_required_block("fn");

        Span end_sp = ast_.stmt(body).span;
        if (cursor_.at(K::kSemicolon)) {
            end_sp = cursor_.bump().span;
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFnDecl;
        s.span = span_join(start, end_sp);

        s.name = name;
        s.type = ret_ty;
        s.a = body;

        s.is_export = is_export;

        // ---- IMPORTANT: class에서만 pub/sub 허용. 여기선 None 고정 ----
        s.fn_mode = ast::FnMode::kNone;

        s.is_throwing = is_throwing;
        s.is_pure = is_pure_kw;
        s.is_comptime = is_comptime_kw;
        s.is_commit = is_commit;
        s.is_recast = is_recast;

        s.attr_begin = attr_begin;
        s.attr_count = attr_count;

        s.param_begin = param_begin;
        s.param_count = param_count;
        s.positional_param_count = positional_count;
        s.has_named_group = has_named_group;

        return ast_.add_stmt(s);
    }

} // namespace gaupel