// compiler/src/parse/parse_type.cpp
#include <gaupel/parse/Parser.hpp>


namespace gaupel {

    Parser::ParsedType Parser::parse_type() {
        using K = syntax::TokenKind;

        const Token t = cursor_.peek();

        // NamedType: ident
        if (t.kind != K::kIdent) {
            // 전문 에러 메세지
            diag_report(diag::Code::kTypeNameExpected, t.span);

            // 타입 실패 시, "타입이 끝나는 지점"까지 스킵해서 연쇄 오류 방지
            // (함수 파라미터/리턴 타입 컨텍스트에서 자주 나오는 follow set)
            recover_to_delim(K::kComma, K::kRParen, K::kLBrace);

            ParsedType bad{};
            bad.id = types_.error();
            bad.span = t.span;
            return bad;
        }

        cursor_.bump(); // consume ident

        // base: builtin or named user
        ty::TypeId cur = types_.intern_ident(t.lexeme);
        Span cur_span = t.span;

        bool saw_optional = false;

        // Suffix chain: [] and ?
        while (1) {
            // T[]
            if (cursor_.at(K::kLBracket)) {
                const Token lb = cursor_.bump();

                if (!cursor_.eat(K::kRBracket)) {
                    // 기존 generic ExpectedToken("]") 대신 전문화
                    diag_report(diag::Code::kTypeArrayMissingRBracket, cursor_.peek().span);
                    recover_to_delim(K::kRBracket, K::kComma, K::kRParen);
                    cursor_.eat(K::kRBracket);
                }
                const Token rb = cursor_.prev();

                cur = types_.make_array(cur);
                cur_span = span_join(cur_span, rb.span);
                (void)lb; // lb는 span_join에 필요하면 확장 가능. v0는 rb 기준으로 충분.

                continue;
            }

            // T?
            if (cursor_.at(K::kQuestion)) {
                const Token q = cursor_.bump();

                if (saw_optional) {
                    diag_report(diag::Code::kTypeOptionalDuplicate, q.span);
                }
                saw_optional = true;

                cur = types_.make_optional(cur);
                cur_span = span_join(cur_span, q.span);

                continue;
            }

            break;
        }

        ParsedType out{};
        out.id = cur;
        out.span = cur_span;
        return out;
    }

} // namespace gaupel