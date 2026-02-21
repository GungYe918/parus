#include <lei/parse/Parser.hpp>

#include <charconv>
#include <cstdlib>

namespace lei::parse {

namespace {

std::unique_ptr<ast::Expr> make_expr(ast::ExprKind k, const ast::Span& sp) {
    auto e = std::make_unique<ast::Expr>();
    e->kind = k;
    e->span = sp;
    return e;
}

std::string strip_underscores(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

} // namespace

const syntax::Token& Parser::peek(size_t k) const {
    const size_t idx = (pos_ + k < tokens_.size()) ? (pos_ + k) : (tokens_.size() - 1);
    return tokens_[idx];
}

bool Parser::at(K k) const {
    return peek().kind == k;
}

const syntax::Token& Parser::bump() {
    const auto& t = peek();
    if (pos_ < tokens_.size()) ++pos_;
    return t;
}

bool Parser::eat(K k) {
    if (!at(k)) return false;
    bump();
    return true;
}

bool Parser::expect(K k, std::string_view what) {
    if (eat(k)) return true;
    diag_expected(peek(), what);
    return false;
}

ast::Span Parser::span_from(const syntax::Token& t) const {
    return ast::Span{file_path_, t.loc.line, t.loc.column};
}

void Parser::diag_expected(const syntax::Token& t, std::string_view what) {
    diags_.add(diag::Code::C_UNEXPECTED_TOKEN,
               file_path_,
               t.loc.line,
               t.loc.column,
               "expected " + std::string(what) + ", got '" + std::string(syntax::token_kind_name(t.kind)) + "'");
}

ast::Program Parser::parse_program() {
    ast::Program p{};
    while (!at(K::kEof)) {
        if (eat(K::kSemicolon)) continue;
        p.items.push_back(parse_item());
    }
    return p;
}

ast::Item Parser::parse_item() {
    if (at(K::kKwImport)) return parse_import();
    if (at(K::kKwLet)) return parse_binding(false, false);
    if (at(K::kKwConst)) return parse_binding(true, false);
    if (at(K::kKwDef)) return parse_def(false);
    if (at(K::kKwAssert)) return parse_assert();

    if (eat(K::kKwExport)) {
        if (at(K::kKwBuild)) return parse_export_build();
        if (at(K::kKwLet)) return parse_binding(false, true);
        if (at(K::kKwConst)) return parse_binding(true, true);
        if (at(K::kKwDef)) return parse_def(true);

        diag_expected(peek(), "'build', 'let', 'const', or 'def' after export");
    }

    auto t = bump();
    diags_.add(diag::Code::C_UNEXPECTED_TOKEN,
               file_path_,
               t.loc.line,
               t.loc.column,
               "unexpected token at top-level");

    ast::Item bad{};
    bad.kind = ast::ItemKind::kAssert;
    bad.span = span_from(t);
    return bad;
}

ast::Item Parser::parse_import() {
    auto start = bump(); // import

    ast::Item it{};
    it.span = span_from(start);
    it.kind = ast::ItemKind::kImportFrom;

    // Removed syntax guard:
    //   import intrinsic { base };
    if (control_.reject_removed_intrinsic_syntax
        && at(K::kIdent)
        && peek().lexeme == "intrinsic") {
        const auto bad = bump();
        diags_.add(diag::Code::C_UNEXPECTED_TOKEN,
                   file_path_,
                   bad.loc.line,
                   bad.loc.column,
                   "keyword 'intrinsic' was removed; use 'base' directly without import");
        while (!at(K::kSemicolon) && !at(K::kEof)) {
            bump();
        }
        (void)eat(K::kSemicolon);
        it.kind = ast::ItemKind::kAssert;
        return it;
    }

    expect(K::kLBrace, "'{' in named import");
    if (!at(K::kRBrace)) {
        while (true) {
            if (!at(K::kIdent)) {
                diag_expected(peek(), "import symbol name");
                break;
            }
            it.import_spec.names.push_back(bump().lexeme);
            if (eat(K::kComma)) {
                if (at(K::kRBrace)) break;
                continue;
            }
            break;
        }
    }
    expect(K::kRBrace, "'}'");
    expect(K::kKwFrom, "'from'");
    if (at(K::kStringLit)) {
        it.import_spec.from_path = bump().lexeme;
    } else {
        diag_expected(peek(), "import path string");
    }
    expect(K::kSemicolon, "';'");
    return it;
}

std::optional<std::string> Parser::parse_type_name() {
    if (eat(K::kKwInt)) return std::string("int");
    if (eat(K::kKwFloat)) return std::string("float");
    if (eat(K::kKwString)) return std::string("string");
    if (eat(K::kKwBool)) return std::string("bool");
    diag_expected(peek(), "type name (int/float/string/bool)");
    return std::nullopt;
}

ast::Item Parser::parse_binding(bool is_const, bool is_export) {
    auto start = bump(); // let/const

    ast::Item it{};
    it.kind = is_const ? ast::ItemKind::kConst : ast::ItemKind::kLet;
    it.span = span_from(start);
    it.binding.is_const = is_const;
    it.binding.is_export = is_export;

    if (at(K::kIdent)) {
        it.binding.name = bump().lexeme;
    } else {
        diag_expected(peek(), "binding name");
    }

    if (eat(K::kColon)) {
        it.binding.annotated_type = parse_type_name();
    }

    expect(K::kAssign, "'='");
    it.binding.value = parse_expr();
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Item Parser::parse_def(bool is_export) {
    auto start = bump(); // def

    ast::Item it{};
    it.kind = ast::ItemKind::kDef;
    it.span = span_from(start);
    it.def.is_export = is_export;

    if (at(K::kIdent)) {
        it.def.name = bump().lexeme;
    } else {
        diag_expected(peek(), "function name");
    }

    expect(K::kLParen, "'('");
    if (!at(K::kRParen)) {
        while (true) {
            if (!at(K::kIdent)) {
                diag_expected(peek(), "parameter name");
                break;
            }
            it.def.params.push_back(bump().lexeme);
            if (eat(K::kComma)) {
                if (at(K::kRParen)) break;
                continue;
            }
            break;
        }
    }
    expect(K::kRParen, "')'");
    expect(K::kFatArrow, "'=>'");
    it.def.body = parse_expr();
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Item Parser::parse_assert() {
    auto start = bump();
    ast::Item it{};
    it.kind = ast::ItemKind::kAssert;
    it.span = span_from(start);
    it.expr = parse_expr();
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Item Parser::parse_export_build() {
    auto start = bump(); // build
    ast::Item it{};
    it.kind = ast::ItemKind::kExportBuild;
    it.span = span_from(start);
    it.expr = parse_expr();
    expect(K::kSemicolon, "';'");
    return it;
}

std::unique_ptr<ast::Expr> Parser::parse_expr() {
    if (at(K::kKwIf)) return parse_if_expr();
    if (at(K::kKwMatch)) return parse_match_expr();
    return parse_default_overlay();
}

std::unique_ptr<ast::Expr> Parser::parse_if_expr() {
    auto start = bump(); // if
    auto e = make_expr(ast::ExprKind::kIf, span_from(start));
    e->cond = parse_expr();
    expect(K::kKwThen, "'then'");
    e->then_expr = parse_expr();
    expect(K::kKwElse, "'else'");
    e->else_expr = parse_expr();
    return e;
}

std::unique_ptr<ast::Expr> Parser::parse_match_expr() {
    auto start = bump(); // match
    auto e = make_expr(ast::ExprKind::kMatch, span_from(start));
    e->cond = parse_expr();

    expect(K::kLBrace, "'{' in match");
    if (!at(K::kRBrace)) {
        while (true) {
            ast::MatchArm arm{};
            if (eat(K::kUnderscore)) {
                arm.pattern.wildcard = true;
            } else if (at(K::kIntLit)) {
                std::string n = strip_underscores(bump().lexeme);
                arm.pattern.int_value = std::strtoll(n.c_str(), nullptr, 10);
            } else if (at(K::kFloatLit)) {
                std::string n = strip_underscores(bump().lexeme);
                arm.pattern.float_value = std::strtod(n.c_str(), nullptr);
            } else if (at(K::kStringLit)) {
                arm.pattern.string_value = bump().lexeme;
            } else if (at(K::kKwTrue) || at(K::kKwFalse)) {
                const auto btok = bump();
                arm.pattern.bool_value = (btok.kind == K::kKwTrue);
            } else {
                diag_expected(peek(), "match pattern");
            }

            expect(K::kFatArrow, "'=>'");
            arm.value = parse_expr();
            e->match_arms.push_back(std::move(arm));

            if (eat(K::kComma)) {
                if (at(K::kRBrace)) break;
                continue;
            }
            break;
        }
    }
    expect(K::kRBrace, "'}'");
    return e;
}

std::unique_ptr<ast::Expr> Parser::parse_default_overlay() {
    auto lhs = parse_merge();
    if (!eat(K::kDefaultOverlay)) return lhs;

    auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
    op->text = "?=";
    op->lhs = std::move(lhs);
    op->rhs = parse_merge();
    return op;
}

std::unique_ptr<ast::Expr> Parser::parse_merge() {
    auto lhs = parse_or();
    while (eat(K::kAmp)) {
        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = "&";
        op->lhs = std::move(lhs);
        op->rhs = parse_or();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_or() {
    auto lhs = parse_and();
    while (eat(K::kOrOr)) {
        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = "||";
        op->lhs = std::move(lhs);
        op->rhs = parse_and();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_and() {
    auto lhs = parse_eq();
    while (eat(K::kAndAnd)) {
        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = "&&";
        op->lhs = std::move(lhs);
        op->rhs = parse_eq();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_eq() {
    auto lhs = parse_add();
    while (at(K::kEqEq) || at(K::kBangEq)) {
        const bool is_eq = at(K::kEqEq);
        bump();

        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = is_eq ? "==" : "!=";
        op->lhs = std::move(lhs);
        op->rhs = parse_add();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_add() {
    auto lhs = parse_mul();
    while (at(K::kPlus) || at(K::kMinus)) {
        const bool is_add = at(K::kPlus);
        bump();

        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = is_add ? "+" : "-";
        op->lhs = std::move(lhs);
        op->rhs = parse_mul();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_mul() {
    auto lhs = parse_unary();
    while (at(K::kStar) || at(K::kSlash)) {
        const bool is_mul = at(K::kStar);
        bump();

        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = is_mul ? "*" : "/";
        op->lhs = std::move(lhs);
        op->rhs = parse_unary();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_unary() {
    if (at(K::kMinus) || at(K::kBang)) {
        const auto tok = bump();
        const bool is_neg = (tok.kind == K::kMinus);
        auto u = make_expr(ast::ExprKind::kUnary, span_from(tok));
        u->text = is_neg ? "-" : "!";
        u->rhs = parse_unary();
        return u;
    }
    return parse_postfix();
}

std::unique_ptr<ast::Expr> Parser::parse_postfix() {
    auto e = parse_primary();

    while (true) {
        if (eat(K::kLParen)) {
            auto c = make_expr(ast::ExprKind::kCall, e->span);
            c->lhs = std::move(e);
            if (!at(K::kRParen)) {
                while (true) {
                    c->call_args.push_back(parse_expr());
                    if (eat(K::kComma)) {
                        if (at(K::kRParen)) break;
                        continue;
                    }
                    break;
                }
            }
            expect(K::kRParen, "')'");
            e = std::move(c);
            continue;
        }

        if (eat(K::kDot)) {
            if (!at(K::kIdent)) {
                diag_expected(peek(), "member name");
                break;
            }
            auto m = make_expr(ast::ExprKind::kMember, e->span);
            m->lhs = std::move(e);
            m->text = bump().lexeme;
            e = std::move(m);
            continue;
        }

        break;
    }

    return e;
}

std::unique_ptr<ast::Expr> Parser::parse_primary() {
    auto t = peek();

    if (eat(K::kIntLit)) {
        auto e = make_expr(ast::ExprKind::kInt, span_from(t));
        std::string n = strip_underscores(t.lexeme);
        e->int_value = std::strtoll(n.c_str(), nullptr, 10);
        return e;
    }

    if (eat(K::kFloatLit)) {
        auto e = make_expr(ast::ExprKind::kFloat, span_from(t));
        std::string n = strip_underscores(t.lexeme);
        e->float_value = std::strtod(n.c_str(), nullptr);
        return e;
    }

    if (eat(K::kStringLit)) {
        auto e = make_expr(ast::ExprKind::kString, span_from(t));
        e->text = t.lexeme;
        return e;
    }

    if (eat(K::kKwTrue) || eat(K::kKwFalse)) {
        auto e = make_expr(ast::ExprKind::kBool, span_from(t));
        e->bool_value = (t.kind == K::kKwTrue);
        return e;
    }

    if (eat(K::kIdent)) {
        auto e = make_expr(ast::ExprKind::kIdent, span_from(t));
        e->text = t.lexeme;
        return e;
    }

    if (at(K::kLBrace)) return parse_object();
    if (at(K::kLBracket)) return parse_array();

    if (eat(K::kLParen)) {
        auto e = parse_expr();
        expect(K::kRParen, "')'");
        return e;
    }

    diag_expected(peek(), "expression");
    auto bad = make_expr(ast::ExprKind::kIdent, span_from(peek()));
    bad->text = "__error__";
    bump();
    return bad;
}

std::unique_ptr<ast::Expr> Parser::parse_object() {
    auto start = bump(); // {
    auto e = make_expr(ast::ExprKind::kObject, span_from(start));

    if (!at(K::kRBrace)) {
        while (true) {
            std::string key;
            if (at(K::kStringLit)) {
                key = bump().lexeme;
            } else if (at(K::kIdent)) {
                key = bump().lexeme;
                while (eat(K::kColonColon)) {
                    if (!at(K::kIdent)) {
                        diag_expected(peek(), "identifier after '::'");
                        break;
                    }
                    key += "::";
                    key += bump().lexeme;
                }
            } else {
                diag_expected(peek(), "object key");
                break;
            }

            expect(K::kColon, "':'");
            auto v = parse_expr();
            e->object_items.push_back(ast::ObjectItem{std::move(key), std::move(v)});

            if (eat(K::kComma)) {
                if (at(K::kRBrace)) break;
                continue;
            }
            break;
        }
    }
    expect(K::kRBrace, "'}'");
    return e;
}

std::unique_ptr<ast::Expr> Parser::parse_array() {
    auto start = bump(); // [
    auto e = make_expr(ast::ExprKind::kArray, span_from(start));

    if (!at(K::kRBracket)) {
        while (true) {
            bool spread = eat(K::kEllipsis);
            auto v = parse_expr();
            e->array_items.push_back(ast::ArrayItem{spread, std::move(v)});

            if (eat(K::kComma)) {
                if (at(K::kRBracket)) break;
                continue;
            }
            break;
        }
    }
    expect(K::kRBracket, "']'");
    return e;
}

ast::Program parse_source(std::string_view source,
                          std::string_view file_path,
                          diag::Bag& diags,
                          ParserControl control) {
    auto toks = lex(source, file_path, diags);
    Parser p(std::move(toks), std::string(file_path), diags, control);
    return p.parse_program();
}

} // namespace lei::parse
