#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ParseOnlyProgram {
    parus::ast::AstArena ast;
    parus::ty::TypePool types;
    parus::diag::Bag bag;
    std::vector<parus::Token> tokens;
    parus::ast::StmtId root = parus::ast::k_invalid_stmt;
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

static ParseOnlyProgram parse_program_(const std::string& src) {
    ParseOnlyProgram p{};
    parus::Lexer lx(src, /*file_id=*/1, &p.bag);
    p.tokens = lx.lex_all();
    parus::Parser parser(p.tokens, p.ast, p.types, &p.bag, /*max_errors=*/256);
    p.root = parser.parse_program();
    return p;
}

static bool run_crash_case_(const std::filesystem::path& p) {
    std::string src;
    if (!read_text_file_(p, src)) {
        std::cerr << "  - failed to read crash case file: " << p.string() << "\n";
        return false;
    }

    const auto begin = std::chrono::steady_clock::now();
    auto prog = parse_program_(src);
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

    bool ok = true;
    ok &= require_(elapsed_ms <= 2000, "parse exceeded crash-case timeout budget (2000ms)");
    ok &= require_(prog.root != parus::ast::k_invalid_stmt, "crash case must still return program root");
    if (prog.root != parus::ast::k_invalid_stmt) {
        ok &= require_(prog.ast.stmt(prog.root).kind == parus::ast::StmtKind::kBlock,
                       "crash case program root must be block stmt");
    }

    if (!ok) {
        std::cerr << "    file: " << p.filename().string() << "\n";
        std::cerr << "    elapsed_ms: " << elapsed_ms << "\n";
        std::cerr << "    diag_count: " << prog.bag.diags().size() << "\n";
    }
    return ok;
}

static bool test_parser_crash_cases() {
#ifndef PARUS_PARSER_CRASH_CASE_DIR
    std::cerr << "  - PARUS_PARSER_CRASH_CASE_DIR is not defined\n";
    return false;
#else
    const std::filesystem::path case_dir{PARUS_PARSER_CRASH_CASE_DIR};
    bool ok = true;

    ok &= require_(std::filesystem::exists(case_dir), "crash case directory does not exist");
    ok &= require_(std::filesystem::is_directory(case_dir), "crash case directory path is not a directory");
    if (!ok) return false;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(case_dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() == ".pr") files.push_back(p);
    }

    std::sort(files.begin(), files.end());
    ok &= require_(!files.empty(), "at least one parser crash case is required");
    if (!ok) return false;

    for (const auto& p : files) {
        std::cout << "  [CRASH-CASE] " << p.filename().string() << "\n";
        ok &= run_crash_case_(p);
    }
    return ok;
#endif
}

} // namespace

int main() {
    std::cout << "[TEST] parser_crash_cases\n";
    if (!test_parser_crash_cases()) {
        std::cout << "  -> FAIL\n";
        std::cout << "FAILED: 1 test(s)\n";
        return 1;
    }

    std::cout << "  -> PASS\n";
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
