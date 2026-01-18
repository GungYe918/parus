// tools/slytec/src/main.cpp
#include <iostream>
#include <string_view>
#include <vector>

#include "slyte/Version.hpp"
#include "slyte/lex/Lexer.hpp"
#include "slyte/parse/Parser.hpp"
#include "slyte/ast/Nodes.hpp"
#include "slyte/syntax/TokenKind.hpp"

static void print_usage() {
  std::cout
    << "slytec\n"
    << "  --version\n"
    << "  --expr \"<expr>\"\n";
}

static const char* expr_kind_name(slyte::ast::ExprKind k) {
  using K = slyte::ast::ExprKind;
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

static void dump_expr(const slyte::ast::AstArena& ast, slyte::ast::ExprId id, int indent) {
  const auto& e = ast.expr(id);
  for (int i = 0; i < indent; ++i) std::cout << "  ";

  std::cout << expr_kind_name(e.kind);

  if (e.op != slyte::syntax::TokenKind::kError) {
    std::cout << " op=" << slyte::syntax::token_kind_name(e.op);
  }
  if (!e.text.empty()) {
    std::cout << " text=" << e.text;
  }

  std::cout << " span=[" << e.span.lo << "," << e.span.hi << ")\n";

  switch (e.kind) {
    case slyte::ast::ExprKind::kUnary:
    case slyte::ast::ExprKind::kPostfixUnary:
      dump_expr(ast, e.a, indent + 1);
      break;

    case slyte::ast::ExprKind::kBinary:
      dump_expr(ast, e.a, indent + 1);
      dump_expr(ast, e.b, indent + 1);
      break;

    case slyte::ast::ExprKind::kTernary:
      dump_expr(ast, e.a, indent + 1);
      dump_expr(ast, e.b, indent + 1);
      dump_expr(ast, e.c, indent + 1);
      break;

    case slyte::ast::ExprKind::kCall: {
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

    case slyte::ast::ExprKind::kIndex:
      dump_expr(ast, e.a, indent + 1);
      dump_expr(ast, e.b, indent + 1);
      break;

    default:
      break;
  }
}

int main(int argc, char** argv) {
  if (argc <= 1) {
    std::cout << slyte::k_version_string << "\n";
    print_usage();
    return 0;
  }

  std::vector<std::string_view> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  if (args[0] == "--version") {
    std::cout << slyte::k_version_string << "\n";
    return 0;
  }

  if (args[0] == "--expr") {
    if (args.size() < 2) {
      std::cerr << "error: --expr requires a string\n";
      return 1;
    }

    std::string_view src = args[1];

    slyte::Lexer lex(src, /*file_id=*/0);
    auto tokens = lex.lex_all();

    std::cout << "TOKENS:\n";
    for (const auto& t : tokens) {
      std::cout << "  " << slyte::syntax::token_kind_name(t.kind)
                << " '" << t.lexeme << "'"
                << " [" << t.span.lo << "," << t.span.hi << ")\n";
    }

    slyte::ast::AstArena ast;
    slyte::Parser p(tokens, ast);
    auto root = p.parse_expr();

    std::cout << "\nAST:\n";
    dump_expr(ast, root, 0);
    return 0;
  }

  print_usage();
  return 0;
}
