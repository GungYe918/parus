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


static void print_usage() {
  std::cout
    << "gaupelc\n"
    << "  --version\n"
    << "  --expr \"<expr>\" [--lang en|ko]\n";
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

static int run_expr(std::string_view src_arg, gaupel::diag::Language lang) {
  // 1) SourceManager에 소스를 "등록"해서 file_id를 얻는다.
  gaupel::SourceManager sm;
  std::string src_owned(src_arg); // 소유권 확보 (SourceManager가 내부에 복사/보관)
  const uint32_t file_id = sm.add("<expr>", src_owned);

  // 2) Lexer는 반드시 SourceManager의 content 뷰 + file_id로 동작
  gaupel::Lexer lex(sm.content(file_id), file_id);
  auto tokens = lex.lex_all();

  std::cout << "TOKENS:\n";
  for (const auto& t : tokens) {
    std::cout << "  " << gaupel::syntax::token_kind_name(t.kind)
              << " '" << t.lexeme << "'"
              << " [" << t.span.lo << "," << t.span.hi << ")\n";
  }

  gaupel::diag::Bag bag;

  gaupel::ast::AstArena ast;
  gaupel::Parser p(tokens, ast, &bag);
  auto root = p.parse_expr();

  // 핵심 정적 규칙: pipe + hole 검사
  gaupel::passes::check_pipe_hole(ast, root, bag);

  std::cout << "\nAST:\n";
  dump_expr(ast, root, 0);

  std::cout << "\nDIAGNOSTICS:\n";
  if (bag.diags().empty()) {
    std::cout << "no error.\n";
    return 0;
  }

  for (const auto& d : bag.diags()) {
    // SourceManager 기반 렌더
    std::cerr << gaupel::diag::render_one(d, lang, sm) << "\n";
  }

  return bag.has_error() ? 1 : 0;
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

  if (args[0] == "--expr") {
    if (args.size() < 2) {
      std::cerr << "error: --expr requires a string\n";
      return 1;
    }
    const auto lang = parse_lang(args);
    return run_expr(args[1], lang);
  }

  print_usage();
  return 0;
}