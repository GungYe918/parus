#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ParseOnlyProgram {
    parus::ast::AstArena ast;
    parus::ty::TypePool types;
    parus::diag::Bag bag;
    std::vector<parus::Token> tokens;
    parus::ast::StmtId root = parus::ast::k_invalid_stmt;
};

struct CaseExpectation {
    bool has_directive = false;
    bool expect_no_parser_error = false;
    std::vector<std::string> error_codes;
    std::vector<std::string> warning_codes;
};

static bool require_(bool cond, const char* msg) {
    if (cond) return true;
    std::cerr << "  - " << msg << "\n";
    return false;
}

static bool read_text_file_(const std::filesystem::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

static std::string trim_(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

static bool starts_with_(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

static ParseOnlyProgram parse_program_(const std::string& src) {
    ParseOnlyProgram p{};
    parus::Lexer lx(src, /*file_id=*/1, &p.bag);
    p.tokens = lx.lex_all();

    parus::Parser parser(p.tokens, p.ast, p.types, &p.bag, /*max_errors=*/256);
    p.root = parser.parse_program();
    return p;
}

static bool has_diag_code_name_(
    const parus::diag::Bag& bag,
    std::string_view code_name,
    parus::diag::Severity sev
) {
    for (const auto& d : bag.diags()) {
        if (d.severity() != sev) continue;
        if (parus::diag::code_name(d.code()) == code_name) return true;
    }
    return false;
}

static bool has_any_warning_(const parus::diag::Bag& bag) {
    for (const auto& d : bag.diags()) {
        if (d.severity() == parus::diag::Severity::kWarning) return true;
    }
    return false;
}

static CaseExpectation parse_expectation_directives_(std::string_view src) {
    CaseExpectation out{};
    std::istringstream iss{std::string(src)};
    std::string line;

    constexpr std::string_view kErr = "//@expect-error ";
    constexpr std::string_view kWarn = "//@expect-warning ";
    constexpr std::string_view kNoErr = "//@expect-no-parser-error";

    while (std::getline(iss, line)) {
        const std::string t = trim_(line);
        if (starts_with_(t, kErr)) {
            out.has_directive = true;
            out.error_codes.push_back(trim_(std::string_view(t).substr(kErr.size())));
            continue;
        }
        if (starts_with_(t, kWarn)) {
            out.has_directive = true;
            out.warning_codes.push_back(trim_(std::string_view(t).substr(kWarn.size())));
            continue;
        }
        if (t == kNoErr) {
            out.has_directive = true;
            out.expect_no_parser_error = true;
            continue;
        }
    }

    return out;
}

static bool validate_directive_expectation_(
    const parus::diag::Bag& bag,
    const CaseExpectation& exp,
    const std::filesystem::path& path
) {
    bool ok = true;

    if (exp.expect_no_parser_error) {
        ok &= require_(!bag.has_error(), "directive expects no parser error, but errors were emitted");
    }

    for (const auto& code : exp.error_codes) {
        const bool matched =
            has_diag_code_name_(bag, code, parus::diag::Severity::kError)
            || has_diag_code_name_(bag, code, parus::diag::Severity::kFatal);
        if (!matched) {
            std::cerr << "  - directive expected parser error code not found: " << code << "\n";
            ok = false;
        }
    }

    for (const auto& code : exp.warning_codes) {
        const bool matched = has_diag_code_name_(bag, code, parus::diag::Severity::kWarning);
        if (!matched) {
            std::cerr << "  - directive expected parser warning code not found: " << code << "\n";
            ok = false;
        }
    }

    if (!ok) {
        std::cerr << "    file: " << path.filename().string() << "\n";
    }
    return ok;
}

static bool has_top_level_fn_named_(
    const parus::ast::AstArena& ast,
    parus::ast::StmtId root,
    std::string_view name
) {
    if (root == parus::ast::k_invalid_stmt) return false;
    const auto& r = ast.stmt(root);
    if (r.kind != parus::ast::StmtKind::kBlock) return false;

    const auto& children = ast.stmt_children();
    if (r.stmt_begin + r.stmt_count > children.size()) return false;
    for (uint32_t i = 0; i < r.stmt_count; ++i) {
        const auto sid = children[r.stmt_begin + i];
        if (sid == parus::ast::k_invalid_stmt) continue;
        const auto& st = ast.stmt(sid);
        if (st.kind == parus::ast::StmtKind::kFnDecl && st.name == name) {
            return true;
        }
    }
    return false;
}

static bool run_file_case_(const std::filesystem::path& p) {
    std::string src;
    if (!read_text_file_(p, src)) {
        std::cerr << "  - failed to read case file: " << p.string() << "\n";
        return false;
    }

    const auto exp = parse_expectation_directives_(src);
    auto prog = parse_program_(src);

    bool ok = true;
    ok &= require_(prog.root != parus::ast::k_invalid_stmt, "parser must return a valid program root");
    if (prog.root != parus::ast::k_invalid_stmt) {
        ok &= require_(prog.ast.stmt(prog.root).kind == parus::ast::StmtKind::kBlock,
                       "program root must be block stmt");
    }

    if (exp.has_directive) {
        ok &= validate_directive_expectation_(prog.bag, exp, p);
        return ok;
    }

    const std::string file_name = p.filename().string();
    const bool expect_error = starts_with_(file_name, "err_");
    const bool expect_warning = (!expect_error && starts_with_(file_name, "warn_"));

    if (expect_error) {
        ok &= require_(prog.bag.has_error(), "expected parser diagnostics for err_ case, but none were emitted");
    } else if (expect_warning) {
        ok &= require_(!prog.bag.has_error(), "warn_ case emitted parser error diagnostics");
        ok &= require_(has_any_warning_(prog.bag), "expected warning diagnostics for warn_ case, but none were emitted");
    } else {
        ok &= require_(!prog.bag.has_error(), "ok_ case emitted parser error diagnostics");
    }

    if (!ok) {
        std::cerr << "    file: " << p.filename().string() << "\n";
    }
    return ok;
}

static bool test_parser_aborted_guard_no_infinite_loop() {
    parus::ast::AstArena ast;
    parus::ty::TypePool types;
    parus::diag::Bag bag;

    constexpr parus::Span bad_span{1, 0, 1};
    bag.add(parus::diag::Diagnostic(
        parus::diag::Severity::kError,
        parus::diag::Code::kInvalidUtf8,
        bad_span
    ));

    std::vector<parus::Token> toks{
        parus::Token{parus::syntax::TokenKind::kIdent, bad_span, "x"},
        parus::Token{parus::syntax::TokenKind::kEof, parus::Span{1, 1, 1}, ""}
    };

    parus::Parser parser(toks, ast, types, &bag);
    const auto root = parser.parse_program();

    bool ok = true;
    ok &= require_(root != parus::ast::k_invalid_stmt, "aborted parser must still return a program root");
    ok &= require_(ast.stmt(root).kind == parus::ast::StmtKind::kBlock, "program root must remain block stmt");
    ok &= require_(ast.stmt(root).stmt_count == 0, "aborted parser should not keep parsing non-EOF tokens");
    ok &= require_(bag.has_code(parus::diag::Code::kInvalidUtf8), "invalid utf8 diagnostic must be preserved");
    return ok;
}

static bool test_recovery_suffix_decl_survives() {
    const std::string src = R"(
        def broken(a: i32 -> i32 {
            return a;
        }

        def alive() -> i32 {
            return 1i32;
        }
    )";

    auto prog = parse_program_(src);

    bool ok = true;
    ok &= require_(prog.bag.has_error(), "broken source must emit parser diagnostics");
    ok &= require_(has_top_level_fn_named_(prog.ast, prog.root, "alive"),
                   "parser recovery must preserve suffix declaration after broken declaration");
    return ok;
}

static bool test_recovery_nested_delim_survives() {
    const std::string src = R"(
        def broken() -> i32 {
            set x = foo(a: (1 + (2 * 3]), b: 4);
            return x;
        }

        def tail_ok() -> i32 {
            return 7i32;
        }
    )";

    auto prog = parse_program_(src);

    bool ok = true;
    ok &= require_(prog.bag.has_error(), "delimiter mismatch source must emit parser diagnostics");
    ok &= require_(has_top_level_fn_named_(prog.ast, prog.root, "tail_ok"),
                   "parser recovery should continue and parse following top-level declaration");
    return ok;
}

static bool test_expect_directive_precedence() {
    const std::string src = R"(
        //@expect-error ExpectedToken
        def main( -> i32 {
            return 0i32;
        }
    )";

    const auto exp = parse_expectation_directives_(src);
    auto prog = parse_program_(src);

    bool ok = true;
    ok &= require_(exp.has_directive, "directive parser must detect //@expect-* metadata");
    ok &= require_(validate_directive_expectation_(prog.bag, exp, "inline"),
                   "directive validation must succeed for matching parser diagnostic");
    return ok;
}

static bool test_file_cases_directory() {
#ifndef PARUS_PARSER_CASE_DIR
    std::cerr << "  - PARUS_PARSER_CASE_DIR is not defined\n";
    return false;
#else
    const std::filesystem::path case_dir{PARUS_PARSER_CASE_DIR};
    bool ok = true;

    ok &= require_(std::filesystem::exists(case_dir), "parser case directory does not exist");
    ok &= require_(std::filesystem::is_directory(case_dir), "parser case directory path is not a directory");
    if (!ok) return false;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(case_dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() == ".pr") files.push_back(p);
    }

    std::sort(files.begin(), files.end());
    ok &= require_(files.size() >= 5, "at least 5 parser case files are required");
    if (!ok) return false;

    for (const auto& p : files) {
        std::cout << "  [CASE] " << p.filename().string() << "\n";
        ok &= run_file_case_(p);
    }
    return ok;
#endif
}

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"parser_aborted_guard_no_infinite_loop", test_parser_aborted_guard_no_infinite_loop},
        {"recovery_suffix_decl_survives", test_recovery_suffix_decl_survives},
        {"recovery_nested_delim_survives", test_recovery_nested_delim_survives},
        {"expect_directive_precedence", test_expect_directive_precedence},
        {"file_cases_directory", test_file_cases_directory},
    };

    int failed = 0;
    for (const auto& tc : cases) {
        std::cout << "[TEST] " << tc.name << "\n";
        const bool ok = tc.fn();
        if (!ok) {
            ++failed;
            std::cout << "  -> FAIL\n";
        } else {
            std::cout << "  -> PASS\n";
        }
    }

    if (failed != 0) {
        std::cout << "FAILED: " << failed << " test(s)\n";
        return 1;
    }

    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
