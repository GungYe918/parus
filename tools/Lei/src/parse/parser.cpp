#include <lei/parse/Parser.hpp>

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

const syntax::Token& Parser::peek(size_t k) const { return cursor_.peek(k); }
bool Parser::at(K k) const { return peek().kind == k; }
const syntax::Token& Parser::bump() { return cursor_.bump(); }

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

void Parser::diag_legacy(const syntax::Token& t, std::string_view legacy) {
    diags_.add(diag::Code::C_LEGACY_SYNTAX_REMOVED,
               file_path_,
               t.loc.line,
               t.loc.column,
               "legacy syntax removed: " + std::string(legacy));
}

void Parser::diag_reserved_identifier(const syntax::Token& t, std::string_view where) {
    diags_.add(diag::Code::C_RESERVED_IDENTIFIER,
               file_path_,
               t.loc.line,
               t.loc.column,
               "reserved identifier cannot be used as " + std::string(where) + ": '" + t.lexeme + "'");
}

bool Parser::is_reserved_name(std::string_view name) const {
    return name == "bundle" || name == "master" || name == "task" || name == "codegen";
}

bool Parser::validate_decl_name(const syntax::Token& t,
                                std::string_view where,
                                bool allow_master_plan) {
    if (!is_reserved_name(t.lexeme)) return true;
    if (allow_master_plan && t.lexeme == "master") return true;
    diag_reserved_identifier(t, where);
    return false;
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
    if (at(K::kKwImport)) return parse_import_alias();
    if (at(K::kKwProto)) return parse_proto_decl();
    if (at(K::kKwPlan)) return parse_plan_decl(false);
    if (at(K::kKwLet)) return parse_binding(false);
    if (at(K::kKwVar)) return parse_binding(true);
    if (at(K::kKwDef)) return parse_def_decl();
    if (at(K::kKwAssert)) return parse_assert_item();
    if (at(K::kKwExport)) return parse_export_stmt();

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

ast::Item Parser::parse_import_alias() {
    auto start = bump(); // import
    ast::Item it{};
    it.kind = ast::ItemKind::kImportAlias;
    it.span = span_from(start);

    if (at(K::kLBrace)) {
        diag_legacy(peek(), "named import 'import {A} from ...' is removed; use 'import alias from ...;'");
    }

    if (at(K::kIdent)) {
        const auto tok = bump();
        it.import_alias.alias = tok.lexeme;
        (void)validate_decl_name(tok, "import alias");
    } else {
        diag_expected(peek(), "import alias identifier");
    }

    expect(K::kKwFrom, "'from'");
    if (at(K::kStringLit)) {
        it.import_alias.from_path = bump().lexeme;
    } else {
        diag_expected(peek(), "import path string");
    }
    expect(K::kSemicolon, "';'");
    return it;
}

ast::TypeNode Parser::parse_type_required(std::string_view where) {
    auto ty = parse_type();
    if (ty.has_value()) return std::move(*ty);

    diags_.add(diag::Code::C_UNEXPECTED_TOKEN,
               file_path_,
               peek().loc.line,
               peek().loc.column,
               "invalid type in " + std::string(where));

    ast::TypeNode fallback{};
    fallback.kind = ast::TypeNode::Kind::kString;
    return fallback;
}

std::optional<ast::TypeNode> Parser::parse_type() {
    ast::TypeNode ty{};
    if (eat(K::kKwInt)) {
        ty.kind = ast::TypeNode::Kind::kInt;
        return ty;
    }
    if (eat(K::kKwFloat)) {
        ty.kind = ast::TypeNode::Kind::kFloat;
        return ty;
    }
    if (eat(K::kKwString)) {
        ty.kind = ast::TypeNode::Kind::kString;
        return ty;
    }
    if (eat(K::kKwBool)) {
        ty.kind = ast::TypeNode::Kind::kBool;
        return ty;
    }
    if (eat(K::kLBracket)) {
        ty.kind = ast::TypeNode::Kind::kArray;
        auto elem = parse_type();
        if (!elem.has_value()) {
            diag_expected(peek(), "element type");
            return std::nullopt;
        }
        ty.element = std::make_unique<ast::TypeNode>(std::move(*elem));
        expect(K::kRBracket, "']'");
        return ty;
    }
    return std::nullopt;
}

ast::Item Parser::parse_proto_decl() {
    auto start = bump(); // proto
    ast::Item it{};
    it.kind = ast::ItemKind::kProto;
    it.span = span_from(start);

    if (at(K::kIdent)) {
        const auto tok = bump();
        it.proto.name = tok.lexeme;
        (void)validate_decl_name(tok, "proto name");
    } else {
        diag_expected(peek(), "proto name");
    }

    expect(K::kLBrace, "'{' in proto");
    while (!at(K::kRBrace) && !at(K::kEof)) {
        ast::ProtoField f{};
        if (at(K::kIdent)) {
            f.name = bump().lexeme;
        } else {
            diag_expected(peek(), "proto field name");
            break;
        }
        expect(K::kColon, "':'");
        f.type = parse_type_required("proto field");
        if (eat(K::kAssign)) {
            f.default_value = parse_expr();
        }
        expect(K::kSemicolon, "';'");
        it.proto.fields.push_back(std::move(f));
    }
    expect(K::kRBrace, "'}'");
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Path Parser::parse_path_required(std::string_view where) {
    ast::Path p{};
    if (!at(K::kIdent)) {
        diag_expected(peek(), where);
        return p;
    }

    ast::PathSegment root{};
    root.kind = ast::PathSegment::Kind::kField;
    root.field = bump().lexeme;
    p.segments.push_back(std::move(root));

    while (true) {
        if (eat(K::kDot)) {
            ast::PathSegment s{};
            s.kind = ast::PathSegment::Kind::kField;
            if (at(K::kIdent)) {
                s.field = bump().lexeme;
            } else {
                diag_expected(peek(), "field name after '.'");
            }
            p.segments.push_back(std::move(s));
            continue;
        }
        if (eat(K::kLBracket)) {
            ast::PathSegment s{};
            s.kind = ast::PathSegment::Kind::kIndex;
            auto idx = parse_expr();
            s.index = std::shared_ptr<ast::Expr>(idx.release());
            expect(K::kRBracket, "']'");
            p.segments.push_back(std::move(s));
            continue;
        }
        break;
    }

    return p;
}

ast::Item Parser::parse_plan_decl(bool is_export) {
    auto start = bump(); // plan
    ast::Item it{};
    it.kind = is_export ? ast::ItemKind::kExportPlan : ast::ItemKind::kPlan;
    it.span = span_from(start);

    if (at(K::kIdent)) {
        const auto tok = bump();
        it.plan.name = tok.lexeme;
        (void)validate_decl_name(tok, "plan name", true);
    } else {
        diag_expected(peek(), "plan name");
    }

    if (eat(K::kAssign)) {
        it.plan.is_expr_form = true;
        it.plan.expr = parse_expr();
        expect(K::kSemicolon, "';'");
        return it;
    }

    expect(K::kLBrace, "'{' in plan");
    while (!at(K::kRBrace) && !at(K::kEof)) {
        ast::PlanAssign a{};
        a.path = parse_path_required("plan assignment path");
        expect(K::kAssign, "'='");
        a.value = parse_expr();
        expect(K::kSemicolon, "';'");
        it.plan.body_items.push_back(std::move(a));
    }
    expect(K::kRBrace, "'}'");
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Item Parser::parse_export_stmt() {
    auto start = bump(); // export
    (void)start;

    if (at(K::kIdent) && peek().lexeme == "build") {
        diag_legacy(peek(), "'export build ...;' is removed; use 'plan master = ...;' and profile entry selection");
    }

    if (!at(K::kKwPlan)) {
        diag_expected(peek(), "'plan' after export");
        ast::Item bad{};
        bad.kind = ast::ItemKind::kAssert;
        bad.span = span_from(peek());
        return bad;
    }

    bump(); // plan

    if (!at(K::kIdent)) {
        diag_expected(peek(), "plan name");
        ast::Item bad{};
        bad.kind = ast::ItemKind::kAssert;
        bad.span = span_from(peek());
        return bad;
    }

    const auto name_tok = bump();
    (void)validate_decl_name(name_tok, "plan name", true);

    if (eat(K::kSemicolon)) {
        ast::Item it{};
        it.kind = ast::ItemKind::kExportPlanRef;
        it.span = span_from(name_tok);
        it.export_plan_ref = name_tok.lexeme;
        return it;
    }

    ast::Item it{};
    it.kind = ast::ItemKind::kExportPlan;
    it.span = span_from(name_tok);
    it.plan.name = name_tok.lexeme;

    if (eat(K::kAssign)) {
        it.plan.is_expr_form = true;
        it.plan.expr = parse_expr();
        expect(K::kSemicolon, "';'");
        return it;
    }

    expect(K::kLBrace, "'{' in export plan");
    while (!at(K::kRBrace) && !at(K::kEof)) {
        ast::PlanAssign a{};
        a.path = parse_path_required("plan assignment path");
        expect(K::kAssign, "'='");
        a.value = parse_expr();
        expect(K::kSemicolon, "';'");
        it.plan.body_items.push_back(std::move(a));
    }
    expect(K::kRBrace, "'}'");
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Item Parser::parse_binding(bool is_var) {
    auto start = bump(); // let / var
    ast::Item it{};
    it.kind = is_var ? ast::ItemKind::kVar : ast::ItemKind::kLet;
    it.span = span_from(start);

    if (at(K::kIdent)) {
        const auto tok = bump();
        it.binding.name = tok.lexeme;
        (void)validate_decl_name(tok, "binding name");
    } else {
        diag_expected(peek(), "binding name");
    }

    if (eat(K::kColon)) {
        it.binding.type = parse_type_required("binding type");
    }

    expect(K::kAssign, "'='");
    it.binding.value = parse_expr();
    expect(K::kSemicolon, "';'");
    return it;
}

ast::Item Parser::parse_def_decl() {
    auto start = bump(); // def
    ast::Item it{};
    it.kind = ast::ItemKind::kDef;
    it.span = span_from(start);

    if (at(K::kIdent)) {
        const auto tok = bump();
        it.def.name = tok.lexeme;
        (void)validate_decl_name(tok, "function name");
    } else {
        diag_expected(peek(), "function name");
    }

    expect(K::kLParen, "'('");
    if (!at(K::kRParen)) {
        while (true) {
            ast::Param p{};
            if (at(K::kIdent)) {
                const auto tok = bump();
                p.name = tok.lexeme;
                (void)validate_decl_name(tok, "parameter name");
            } else {
                diag_expected(peek(), "parameter name");
                break;
            }
            if (eat(K::kColon)) {
                p.type = parse_type_required("parameter type");
            }
            it.def.params.push_back(std::move(p));

            if (eat(K::kComma)) {
                if (at(K::kRParen)) break;
                continue;
            }
            break;
        }
    }
    expect(K::kRParen, "')'");

    if (eat(K::kArrow)) {
        it.def.return_type = parse_type_required("return type");
    } else if (at(K::kAssign)) {
        diag_legacy(peek(), "expression-body function 'def f(...) => ...;' is removed; use block body");
    }

    it.def.body = parse_block();
    return it;
}

ast::Item Parser::parse_assert_item() {
    auto start = bump();
    ast::Item it{};
    it.kind = ast::ItemKind::kAssert;
    it.span = span_from(start);
    it.expr = parse_expr();
    expect(K::kSemicolon, "';'");
    return it;
}

std::shared_ptr<ast::Block> Parser::parse_block() {
    auto b = std::make_shared<ast::Block>();
    expect(K::kLBrace, "'{' block start");
    while (!at(K::kRBrace) && !at(K::kEof)) {
        if (eat(K::kSemicolon)) continue;
        b->statements.push_back(parse_stmt());
    }
    expect(K::kRBrace, "'}' block end");
    return b;
}

ast::Stmt Parser::parse_stmt() {
    if (at(K::kKwLet)) return parse_let_stmt(false);
    if (at(K::kKwVar)) return parse_let_stmt(true);
    if (at(K::kKwFor)) return parse_for_stmt();
    if (at(K::kKwIf)) return parse_if_stmt();
    if (at(K::kKwReturn)) return parse_return_stmt();
    if (at(K::kKwAssert)) return parse_assert_stmt();
    return parse_expr_or_assign_stmt();
}

ast::Stmt Parser::parse_let_stmt(bool is_var) {
    auto start = bump();
    ast::Stmt s{};
    s.kind = is_var ? ast::StmtKind::kVar : ast::StmtKind::kLet;
    s.span = span_from(start);

    if (at(K::kIdent)) {
        const auto tok = bump();
        s.let_decl.name = tok.lexeme;
        (void)validate_decl_name(tok, "binding name");
    } else {
        diag_expected(peek(), "binding name");
    }

    if (eat(K::kColon)) {
        s.let_decl.type = parse_type_required("statement binding type");
    }

    expect(K::kAssign, "'='");
    s.let_decl.value = parse_expr();
    expect(K::kSemicolon, "';'");
    return s;
}

ast::Stmt Parser::parse_for_stmt() {
    auto start = bump();
    ast::Stmt s{};
    s.kind = ast::StmtKind::kFor;
    s.span = span_from(start);

    if (at(K::kIdent)) {
        const auto tok = bump();
        s.for_stmt.iter_name = tok.lexeme;
        (void)validate_decl_name(tok, "for iterator name");
    } else {
        diag_expected(peek(), "for iterator name");
    }

    expect(K::kKwIn, "'in'");
    s.for_stmt.iterable = parse_expr();
    s.for_stmt.body = parse_block();
    return s;
}

ast::Stmt Parser::parse_if_stmt() {
    auto start = bump();
    ast::Stmt s{};
    s.kind = ast::StmtKind::kIf;
    s.span = span_from(start);

    s.if_stmt.cond = parse_expr();
    s.if_stmt.then_block = parse_block();
    if (eat(K::kKwElse)) {
        s.if_stmt.else_block = parse_block();
    }
    return s;
}

ast::Stmt Parser::parse_return_stmt() {
    auto start = bump();
    ast::Stmt s{};
    s.kind = ast::StmtKind::kReturn;
    s.span = span_from(start);
    s.ret.value = parse_expr();
    expect(K::kSemicolon, "';'");
    return s;
}

ast::Stmt Parser::parse_assert_stmt() {
    auto start = bump();
    ast::Stmt s{};
    s.kind = ast::StmtKind::kAssert;
    s.span = span_from(start);
    s.expr = parse_expr();
    expect(K::kSemicolon, "';'");
    return s;
}

bool Parser::looks_like_path_assign_stmt() const {
    if (peek().kind != K::kIdent) return false;
    size_t i = 1;
    while (true) {
        auto t = peek(i);
        if (t.kind == K::kDot) {
            if (peek(i + 1).kind != K::kIdent) return false;
            i += 2;
            continue;
        }
        if (t.kind == K::kLBracket) {
            int depth = 1;
            ++i;
            while (depth > 0) {
                auto k = peek(i).kind;
                if (k == K::kEof) return false;
                if (k == K::kLBracket) ++depth;
                if (k == K::kRBracket) --depth;
                ++i;
            }
            continue;
        }
        break;
    }
    return peek(i).kind == K::kAssign;
}

ast::Stmt Parser::parse_expr_or_assign_stmt() {
    if (looks_like_path_assign_stmt()) {
        auto first = peek();
        ast::Stmt s{};
        s.kind = ast::StmtKind::kAssign;
        s.span = span_from(first);
        s.assign.path = parse_path_required("assignment path");
        expect(K::kAssign, "'='");
        s.assign.value = parse_expr();
        expect(K::kSemicolon, "';'");
        return s;
    }

    auto first = peek();
    ast::Stmt s{};
    s.kind = ast::StmtKind::kExpr;
    s.span = span_from(first);
    s.expr = parse_expr();
    expect(K::kSemicolon, "';'");
    return s;
}

std::unique_ptr<ast::Expr> Parser::parse_expr() { return parse_merge(); }

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
        bool eq = eat(K::kEqEq);
        if (!eq) (void)eat(K::kBangEq);
        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = eq ? "==" : "!=";
        op->lhs = std::move(lhs);
        op->rhs = parse_add();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_add() {
    auto lhs = parse_mul();
    while (at(K::kPlus) || at(K::kMinus)) {
        bool add = eat(K::kPlus);
        if (!add) (void)eat(K::kMinus);
        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = add ? "+" : "-";
        op->lhs = std::move(lhs);
        op->rhs = parse_mul();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_mul() {
    auto lhs = parse_unary();
    while (at(K::kStar) || at(K::kSlash)) {
        bool mul = eat(K::kStar);
        if (!mul) (void)eat(K::kSlash);
        auto op = make_expr(ast::ExprKind::kBinary, lhs->span);
        op->text = mul ? "*" : "/";
        op->lhs = std::move(lhs);
        op->rhs = parse_unary();
        lhs = std::move(op);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_unary() {
    if (at(K::kMinus) || at(K::kBang)) {
        auto tok = bump();
        auto e = make_expr(ast::ExprKind::kUnary, span_from(tok));
        e->text = tok.kind == K::kMinus ? "-" : "!";
        e->rhs = parse_unary();
        return e;
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
            auto m = make_expr(ast::ExprKind::kMember, e->span);
            m->lhs = std::move(e);
            if (at(K::kIdent)) {
                m->text = bump().lexeme;
            } else {
                diag_expected(peek(), "member name");
            }
            e = std::move(m);
            continue;
        }
        if (eat(K::kLBracket)) {
            auto idx = make_expr(ast::ExprKind::kIndex, e->span);
            idx->lhs = std::move(e);
            idx->rhs = parse_expr();
            expect(K::kRBracket, "']'");
            e = std::move(idx);
            continue;
        }
        break;
    }
    return e;
}

bool Parser::looks_like_plan_patch() const {
    if (peek().kind != K::kLBrace) return false;
    if (peek(1).kind == K::kRBrace) return true;
    if (peek(1).kind != K::kIdent) return false;

    size_t i = 2;
    while (true) {
        auto t = peek(i);
        if (t.kind == K::kDot) {
            if (peek(i + 1).kind != K::kIdent) return false;
            i += 2;
            continue;
        }
        if (t.kind == K::kLBracket) {
            int depth = 1;
            ++i;
            while (depth > 0) {
                auto k = peek(i).kind;
                if (k == K::kEof) return false;
                if (k == K::kLBracket) ++depth;
                if (k == K::kRBracket) --depth;
                ++i;
            }
            continue;
        }
        break;
    }

    return peek(i).kind == K::kAssign;
}

std::unique_ptr<ast::Expr> Parser::parse_primary() {
    auto t = peek();

    if (eat(K::kIntLit)) {
        auto e = make_expr(ast::ExprKind::kInt, span_from(t));
        auto n = strip_underscores(t.lexeme);
        e->int_value = std::strtoll(n.c_str(), nullptr, 10);
        return e;
    }

    if (eat(K::kFloatLit)) {
        auto e = make_expr(ast::ExprKind::kFloat, span_from(t));
        auto n = strip_underscores(t.lexeme);
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

    if (at(K::kIdent) && peek(1).kind == K::kColonColon) {
        auto e = make_expr(ast::ExprKind::kNamespaceRef, span_from(t));
        e->ns_parts.push_back(bump().lexeme);
        while (eat(K::kColonColon)) {
            if (!at(K::kIdent)) {
                diag_expected(peek(), "identifier after '::'");
                break;
            }
            e->ns_parts.push_back(bump().lexeme);
        }
        return e;
    }

    if (eat(K::kIdent)) {
        auto e = make_expr(ast::ExprKind::kIdent, span_from(t));
        e->text = t.lexeme;
        return e;
    }

    if (at(K::kLBrace)) {
        if (looks_like_plan_patch()) return parse_plan_patch_lit();
        return parse_object_lit();
    }
    if (at(K::kLBracket)) return parse_array_lit();

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

std::unique_ptr<ast::Expr> Parser::parse_object_lit() {
    auto start = bump(); // {
    auto e = make_expr(ast::ExprKind::kObject, span_from(start));

    if (!at(K::kRBrace)) {
        while (true) {
            ast::ObjectItem item{};
            if (at(K::kStringLit) || at(K::kIdent)) {
                item.key = bump().lexeme;
            } else {
                diag_expected(peek(), "object key");
                break;
            }
            expect(K::kColon, "':'");
            item.value = parse_expr();
            e->object_items.push_back(std::move(item));
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

std::unique_ptr<ast::Expr> Parser::parse_plan_patch_lit() {
    auto start = bump(); // {
    auto e = make_expr(ast::ExprKind::kPlanPatch, span_from(start));

    while (!at(K::kRBrace) && !at(K::kEof)) {
        ast::PlanAssign a{};
        a.path = parse_path_required("plan patch path");
        expect(K::kAssign, "'='");
        a.value = parse_expr();
        expect(K::kSemicolon, "';'");
        e->plan_patch_items.push_back(std::move(a));
    }

    expect(K::kRBrace, "'}'");
    return e;
}

std::unique_ptr<ast::Expr> Parser::parse_array_lit() {
    auto start = bump();
    auto e = make_expr(ast::ExprKind::kArray, span_from(start));

    if (!at(K::kRBracket)) {
        while (true) {
            e->array_items.push_back(parse_expr());
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
