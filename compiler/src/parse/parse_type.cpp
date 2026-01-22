// compiler/src/parse/parse_type.cpp
#include <gaupel/parse/Parser.hpp>


namespace gaupel {

    ast::TypeId Parser::parse_type() {
        // v0: NamedType only
        const Token& t = cursor_.peek();

        if (t.kind == syntax::TokenKind::kIdent) {
            cursor_.bump();
            ast::Type ty{};
            ty.kind = ast::TypeKind::kNamed;
            ty.span = t.span;
            ty.text = t.lexeme;
            return ast_.add_type(ty);
        }

        // error type
        diag_report(diag::Code::kUnexpectedToken, t.span, "type");
        ast::Type ty{};
        ty.kind = ast::TypeKind::kError;
        ty.span = t.span;
        ty.text = {};
        return ast_.add_type(ty);
    }

} // namespace gaupel