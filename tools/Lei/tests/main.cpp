#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>
#include <lei/graph/BuildGraph.hpp>
#include <lei/parse/Parser.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace {

bool has_code(const lei::diag::Bag& bag, lei::diag::Code code) {
    for (const auto& d : bag.all()) {
        if (d.code == code) return true;
    }
    return false;
}

bool run_ok_case(const std::filesystem::path& path) {
    lei::diag::Bag bag;
    auto builtins = lei::eval::make_default_builtin_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), parser_control);
    auto v = evaluator.evaluate_entry(path);
    if (!v || bag.has_error()) {
        std::cerr << "unexpected failure:\n" << bag.render_text();
        return false;
    }

    auto conventions = lei::graph::make_default_build_conventions();
    auto graph = lei::graph::from_build_value(*v, bag, conventions);
    if (!graph || bag.has_error()) {
        std::cerr << "graph failure:\n" << bag.render_text();
        return false;
    }

    auto ninja = lei::graph::emit_ninja(*graph, bag);
    if (!ninja || bag.has_error()) {
        std::cerr << "ninja emit failure:\n" << bag.render_text();
        return false;
    }

    if (ninja->find("build all: phony") == std::string::npos) {
        std::cerr << "ninja output missing default phony target\n";
        return false;
    }

    return true;
}

bool run_err_case(const std::filesystem::path& path, lei::diag::Code expected) {
    lei::diag::Bag bag;
    auto builtins = lei::eval::make_default_builtin_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), parser_control);
    auto v = evaluator.evaluate_entry(path);
    (void)v;

    if (!bag.has_error()) {
        std::cerr << "expected failure but got success: " << path << "\n";
        return false;
    }

    if (!has_code(bag, expected)) {
        std::cerr << "expected diagnostic code not found: " << lei::diag::code_name(expected) << "\n";
        std::cerr << bag.render_text();
        return false;
    }

    return true;
}

bool run_builtin_api_case(const std::filesystem::path& path) {
    lei::diag::Bag bag;
    auto builtins = lei::eval::make_default_builtin_registry();
    builtins.register_native_function(
        "make_profile",
        [](const std::vector<lei::eval::Value>& args, const lei::ast::Span&, lei::diag::Bag&) -> std::optional<lei::eval::Value> {
            if (!args.empty()) return std::nullopt;
            lei::eval::Value v;
            v.data = std::string("debug");
            return v;
        });

    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), parser_control);
    auto v = evaluator.evaluate_entry(path);
    if (!v || bag.has_error()) {
        std::cerr << "builtin api case failed:\n" << bag.render_text();
        return false;
    }
    return true;
}

bool run_invalid_utf8_case() {
    std::string source = "export build \"";
    source.push_back(static_cast<char>(0xFF));
    source += "\";";

    lei::diag::Bag bag;
    lei::parse::ParserControl parser_control{};
    (void)lei::parse::parse_source(source, "<invalid-utf8>", bag, parser_control);

    if (!bag.has_error()) {
        std::cerr << "expected invalid utf8 parse failure but got success\n";
        return false;
    }
    if (!has_code(bag, lei::diag::Code::C_UNEXPECTED_TOKEN)) {
        std::cerr << "invalid utf8 case did not emit expected diagnostic code\n";
        std::cerr << bag.render_text();
        return false;
    }
    return true;
}

bool is_code_file(const std::filesystem::path& p) {
    const auto ext = p.extension().string();
    return ext == ".h" || ext == ".hpp" || ext == ".hh" ||
           ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
           ext == ".ipp" || ext == ".inl";
}

bool run_no_parus_include_rule() {
    const std::filesystem::path project_dir = LEI_PROJECT_DIR;
    const std::filesystem::path include_dir = project_dir / "include";
    const std::filesystem::path src_dir = project_dir / "src";

    for (const auto& root : {include_dir, src_dir}) {
        for (const auto& ent : std::filesystem::recursive_directory_iterator(root)) {
            if (!ent.is_regular_file()) continue;
            const auto& p = ent.path();
            if (!is_code_file(p)) continue;

            std::ifstream ifs(p, std::ios::binary);
            if (!ifs) continue;
            const std::string text((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());

            if (text.find("#include <parus/") != std::string::npos) {
                std::cerr << "forbidden include found in LEI source: " << p << "\n";
                return false;
            }
        }
    }

    return true;
}

} // namespace

int main() {
    const std::filesystem::path cases = LEI_TEST_CASE_DIR;

    const bool ok1 = run_ok_case(cases / "app_import_ok.lei");
    const bool err1 = run_err_case(cases / "err_cycle_a.lei", lei::diag::Code::L_IMPORT_CYCLE);
    const bool err2 = run_err_case(cases / "err_recursion.lei", lei::diag::Code::L_RECURSION_FORBIDDEN);
    const bool err3 = run_err_case(cases / "err_intrinsic_removed.lei", lei::diag::Code::C_UNEXPECTED_TOKEN);
    const bool err4 = run_err_case(cases / "err_import_missing.lei", lei::diag::Code::L_IMPORT_NOT_FOUND);
    const bool ok2 = run_builtin_api_case(cases / "api_builtin_fn_ok.lei");
    const bool ok3 = run_invalid_utf8_case();
    const bool ok4 = run_no_parus_include_rule();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !err1 || !err2 || !err3 || !err4) {
        return 1;
    }

    std::cout << "lei tests passed\n";
    return 0;
}
