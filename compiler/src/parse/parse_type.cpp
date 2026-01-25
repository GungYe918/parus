// compiler/src/parse/parse_type.cpp
#include <gaupel/parse/Parser.hpp>


namespace gaupel {

    ast::TypeId Parser::parse_type() {
        using K = syntax::TokenKind;

        const Token t = cursor_.peek();

        // NamedType: ident
        if (t.kind != K::kIdent) {
            // 전문 에러 메세지
            diag_report(diag::Code::kTypeNameExpected, t.span);

            // 타입 실패 시, "타입이 끝나는 지점"까지 스킵해서 연쇄 오류 방지
            // (함수 파라미터/리턴 타입 컨텍스트에서 자주 나오는 follow set)
            recover_to_delim(K::kComma, K::kRParen, K::kLBrace);

            ast::Type ty{};
            ty.kind = ast::TypeKind::kError;
            ty.span = t.span;
            ty.text = "type_error";
            return ast_.add_type(ty);
        }

        cursor_.bump(); // consume ident

        ast::Type base{};
        base.kind = ast::TypeKind::kNamed;
        base.span = t.span;
        base.text = t.lexeme;

        ast::TypeId cur = ast_.add_type(base);

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

                ast::Type arr{};
                arr.kind = ast::TypeKind::kArray;
                arr.elem = cur;
                arr.span = span_join(lb.span, rb.span);

                cur = ast_.add_type(arr);
                continue;
            }

            // T?
            if (cursor_.at(K::kQuestion)) {
                const Token q = cursor_.bump();

                if (saw_optional) {
                    diag_report(diag::Code::kTypeOptionalDuplicate, q.span);
                }
                saw_optional = true;

                ast::Type opt{};
                opt.kind = ast::TypeKind::kOptional;
                opt.elem = cur;
                opt.span = span_join(ast_.type_node(cur).span, q.span);

                cur = ast_.add_type(opt);
                continue;
            }

            break;
        }

        return cur;
    }

} // namespace gaupel