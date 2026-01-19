#include <iostream>
#include <string_view>
#include <vector>

#include "gaupel/Version.hpp"
#include "gaupel/lex/Lexer.hpp"
#include "gaupel/parse/Parser.hpp"
#include "gaupel/ast/Nodes.hpp"
#include "gaupel/syntax/TokenKind.hpp"

#include "gaupel/diag/Diagnostic.hpp"
#include "gaupel/diag/Render.hpp"
#include "gaupel/diag/DiagCode.hpp"
#include "gaupel/text/SourceManager.hpp"

#include "gaupel/passes/CheckPipeHole.hpp"
#include <gaupel/os/File.hpp>


/// @brief gaupelc 사용법 출력
static void print_usage() {
    std::cout
        << "gaupelc\n"
        << "  --version\n"
        << "  --expr \"<expr>\" [--lang en|ko] [--context N]\n"
        << "  --stmt \"<stmt>\" [--lang en|ko] [--context N]\n"
        << "  --all  \"<program>\" [--lang en|ko] [--context N]\n"
        << "  --file <path> [--lang en|ko] [--context N]\n";
}

static gaupel::diag::Language parse_lang(const std::vector<std::string_view>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--lang") {
            if (args[i + 1] == "ko") return gaupel::diag::Language::kKo;
            return gaupel::diag::Language::kEn;
        }
    }
    return gaupel::diag::Language::kEn;
}

/// @brief 진단 컨텍스트 줄 수를 파싱한다.
static uint32_t parse_context(const std::vector<std::string_view>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--context") {
            try {
                int v = std::stoi(std::string(args[i + 1]));
                if (v < 0) v = 0;
                return static_cast<uint32_t>(v);
            } catch (...) {
                return 2;
            }
        }
    }
    return 2; // 기본: 위/아래 2줄
}

/// @brief 토큰 목록 출력
static void dump_tokens(const std::vector<gaupel::Token>& tokens) {
    std::cout << "TOKENS:\n";
    for (const auto& t : tokens) {
        std::cout << "  " << gaupel::syntax::token_kind_name(t.kind)
                  << " '" << t.lexeme << "'"
                  << " [" << t.span.lo << "," << t.span.hi << ")\n";
    }
}

/// @brief StmtKind를 사람이 읽기 쉬운 이름으로 변경
static const char* stmt_kind_name(gaupel::ast::StmtKind k) {
    using K = gaupel::ast::StmtKind;
    switch (k) {
        case K::kEmpty: return "Empty";
        case K::kExprStmt: return "ExprStmt";
        case K::kBlock: return "Block";
        case K::kLet: return "Let";
        case K::kIf: return "If";
        case K::kWhile: return "While";
        case K::kReturn: return "Return";
        case K::kBreak: return "Break";
        case K::kContinue: return "Continue";
    }
    return "Unknown";
}

static const char* expr_kind_name(gaupel::ast::ExprKind k) {
    using K = gaupel::ast::ExprKind;
    switch (k) {
        case K::kIntLit: return "IntLit";
        case K::kFloatLit: return "FloatLit";
        case K::kStringLit: return "StringLit";
        case K::kBoolLit: return "BoolLit";
        case K::kNullLit: return "NullLit";
        case K::kIdent: return "Ident";
        case K::kHole: return "Hole";
        case K::kUnary: return "Unary";
        case K::kPostfixUnary: return "PostfixUnary";
        case K::kBinary: return "Binary";
        case K::kTernary: return "Ternary";
        case K::kCall: return "Call";
        case K::kIndex: return "Index";
    }
    return "Unknown";
}

/// @brief 진단 출력(컨텍스트 포함) + 종료코드 계산
static int flush_diags(const gaupel::diag::Bag& bag,
                       gaupel::diag::Language lang,
                       const gaupel::SourceManager& sm,
                       uint32_t context_lines) {
    std::cout << "\nDIAGNOSTICS:\n";
    if (bag.diags().empty()) {
        std::cout << "no error.\n";
        return 0;
    }

    for (const auto& d : bag.diags()) {
        std::cerr << gaupel::diag::render_one_context(d, lang, sm, context_lines) << "\n";
    }
    return bag.has_error() ? 1 : 0;
}

static void dump_expr(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId id, int indent) {
    const auto& e = ast.expr(id);
    for (int i = 0; i < indent; ++i) std::cout << "  ";

    std::cout << expr_kind_name(e.kind);

    if (e.op != gaupel::syntax::TokenKind::kError) {
        std::cout << " op=" << gaupel::syntax::token_kind_name(e.op);
    }
    if (!e.text.empty()) {
        std::cout << " text=" << e.text;
    }

    std::cout << " span=[" << e.span.lo << "," << e.span.hi << ")\n";

    switch (e.kind) {
        case gaupel::ast::ExprKind::kUnary:
        case gaupel::ast::ExprKind::kPostfixUnary:
            dump_expr(ast, e.a, indent + 1);
            break;

        case gaupel::ast::ExprKind::kBinary:
            dump_expr(ast, e.a, indent + 1);
            dump_expr(ast, e.b, indent + 1);
            break;

        case gaupel::ast::ExprKind::kTernary:
            dump_expr(ast, e.a, indent + 1);
            dump_expr(ast, e.b, indent + 1);
            dump_expr(ast, e.c, indent + 1);
            break;

        case gaupel::ast::ExprKind::kCall: {
            dump_expr(ast, e.a, indent + 1);
            const auto& args = ast.args();
            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = args[e.arg_begin + i];
                for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                std::cout << "Arg ";
                if (a.has_label) std::cout << a.label << ": ";
                if (a.is_hole) {
                    std::cout << "_\n";
                } else {
                    std::cout << "\n";
                    dump_expr(ast, a.expr, indent + 2);
                }
            }
            break;
        }

        case gaupel::ast::ExprKind::kIndex:
            dump_expr(ast, e.a, indent + 1);
            dump_expr(ast, e.b, indent + 1);
            break;

        default:
            break;
    }
}

/// @brief stmt 1개를 출력하고, 필요한 경우 하위 노드를 출력
static void dump_stmt(const gaupel::ast::AstArena& ast, gaupel::ast::StmtId id, int indent) {
    const auto& s = ast.stmt(id);
    for (int i = 0; i < indent; ++i) std::cout << "  ";

    std::cout << stmt_kind_name(s.kind)
              << " span=[" << s.span.lo << "," << s.span.hi << ")";

    if (s.kind == gaupel::ast::StmtKind::kLet) {
        std::cout << " name=" << s.name << " mut=" << (s.is_mut ? "true" : "false");
    }
    std::cout << "\n";

    switch (s.kind) {
        case gaupel::ast::StmtKind::kExprStmt:
            dump_expr(ast, s.expr, indent + 1);
            break;

        case gaupel::ast::StmtKind::kLet:
            if (s.expr != gaupel::ast::k_invalid_expr) {
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Init:\n";
                dump_expr(ast, s.expr, indent + 2);
            }
            break;

        case gaupel::ast::StmtKind::kIf:
            for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
            std::cout << "Cond:\n";
            dump_expr(ast, s.expr, indent + 2);

            for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
            std::cout << "Then:\n";
            dump_stmt(ast, s.a, indent + 2);

            if (s.b != gaupel::ast::k_invalid_stmt) {
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Else:\n";
                dump_stmt(ast, s.b, indent + 2);
            }
            break;

        case gaupel::ast::StmtKind::kWhile:
            for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
            std::cout << "Cond:\n";
            dump_expr(ast, s.expr, indent + 2);

            for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
            std::cout << "Body:\n";
            dump_stmt(ast, s.a, indent + 2);
            break;

        case gaupel::ast::StmtKind::kReturn:
            if (s.expr != gaupel::ast::k_invalid_expr) {
                dump_expr(ast, s.expr, indent + 1);
            }
            break;

        case gaupel::ast::StmtKind::kBlock: {
            const auto& kids = ast.stmt_children();
            for (uint32_t i = 0; i < s.stmt_count; ++i) {
                dump_stmt(ast, kids[s.stmt_begin + i], indent + 1);
            }
            break;
        }

        default:
            break;
    }
}

static std::vector<gaupel::Token> lex_with_sm(
    gaupel::SourceManager& sm,
    uint32_t file_id
) {
    gaupel::Lexer lex(sm.content(file_id), file_id);
    return lex.lex_all();
}

/// @brief expr 파싱 실행
static int run_expr(std::string_view src_arg, gaupel::diag::Language lang, uint32_t context_lines) {
    gaupel::SourceManager sm;
    std::string src_owned(src_arg);
    const uint32_t file_id = sm.add("<expr>", src_owned);

    auto tokens = lex_with_sm(sm, file_id);
    dump_tokens(tokens);

    gaupel::diag::Bag bag;
    gaupel::ast::AstArena ast;
    gaupel::Parser p(tokens, ast, &bag);

    auto root = p.parse_expr();
    gaupel::passes::check_pipe_hole(ast, root, bag);

    std::cout << "\nAST:\n";
    dump_expr(ast, root, 0);

    return flush_diags(bag, lang, sm, context_lines);
}

/// @brief stmt 1개 파싱 실행
static int run_stmt(std::string_view src_arg, gaupel::diag::Language lang, uint32_t context_lines) {
    gaupel::SourceManager sm;
    std::string src_owned(src_arg);
    const uint32_t file_id = sm.add("<stmt>", src_owned);

    auto tokens = lex_with_sm(sm, file_id);
    dump_tokens(tokens);

    gaupel::diag::Bag bag;
    gaupel::ast::AstArena ast;
    gaupel::Parser p(tokens, ast, &bag);

    auto root = p.parse_stmt();

    std::cout << "\nAST(STMT):\n";
    dump_stmt(ast, root, 0);

    return flush_diags(bag, lang, sm, context_lines);
}


/// @brief 프로그램(여러 stmt) 파싱 실행
static int run_all(std::string_view src_arg, gaupel::diag::Language lang, uint32_t context_lines, std::string_view name) {
    gaupel::SourceManager sm;
    std::string src_owned(src_arg);
    const uint32_t file_id = sm.add(std::string(name), std::move(src_owned));

    auto tokens = lex_with_sm(sm, file_id);
    dump_tokens(tokens);

    gaupel::diag::Bag bag;
    gaupel::ast::AstArena ast;
    gaupel::Parser p(tokens, ast, &bag);

    auto root = p.parse_program();

    std::cout << "\nAST(PROGRAM):\n";
    dump_stmt(ast, root, 0);

    return flush_diags(bag, lang, sm, context_lines);
}


/// @brief 파일을 읽어서 프로그램 모드로 파싱
static int run_file(const std::string& path, gaupel::diag::Language lang, uint32_t context_lines) {
    std::string content;
    std::string err;

    if (!gaupel::open_file(path, content, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::string norm = gaupel::normalize_path(path);
    return run_all(content, lang, context_lines, norm);
}

int main(int argc, char** argv) {
    if (argc <= 1) {
        std::cout << gaupel::k_version_string << "\n";
        print_usage();
        return 0;
    }

    std::vector<std::string_view> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    if (args[0] == "--version") {
        std::cout << gaupel::k_version_string << "\n";
        return 0;
    }

    const auto lang = parse_lang(args);
    const auto context_lines = parse_context(args);

    if (args[0] == "--expr") {
        if (args.size() < 2) {
            std::cerr << "error: --expr requires a string\n";
            return 1;
        }
        return run_expr(args[1], lang, context_lines);
    }

    if (args[0] == "--stmt") {
        if (args.size() < 2) {
            std::cerr << "error: --stmt requires a string\n";
            return 1;
        }
        return run_stmt(args[1], lang, context_lines);
    }

    if (args[0] == "--all") {
        if (args.size() < 2) {
            std::cerr << "error: --all requires a string\n";
            return 1;
        }
        return run_all(args[1], lang, context_lines, "<all>");
    }

    if (args[0] == "--file") {
        if (args.size() < 2) {
            std::cerr << "error: --file requires a path\n";
            return 1;
        }
        return run_file(std::string(args[1]), lang, context_lines);
    }

    print_usage();
    return 0;
}
