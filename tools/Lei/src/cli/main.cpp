#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>
#include <lei/graph/BuildGraph.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "usage:\n";
    std::cerr << "  lei-build <entry.lei> --out <build.ninja>\n";
    std::cerr << "  lei-build --check <entry.lei>\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    bool check_only = false;
    std::string entry;
    std::string out_path = "build.ninja";

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--check") {
            check_only = true;
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            entry = args[++i];
            continue;
        }
        if (a == "--out") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            out_path = args[++i];
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option: " << a << "\n";
            print_usage();
            return 1;
        }
        if (entry.empty()) {
            entry = a;
        } else {
            std::cerr << "error: multiple entry files are not supported\n";
            return 1;
        }
    }

    if (entry.empty()) {
        print_usage();
        return 1;
    }

    lei::diag::Bag diags;
    lei::eval::EvaluatorBudget budget{};
    auto builtins = lei::eval::make_default_builtin_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator(budget, diags, std::move(builtins), parser_control);

    auto value = evaluator.evaluate_entry(std::filesystem::path(entry));
    if (!value || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    if (check_only) {
        return 0;
    }

    auto conventions = lei::graph::make_default_build_conventions();
    auto graph = lei::graph::from_build_value(*value, diags, conventions);
    if (!graph || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    auto ninja = lei::graph::emit_ninja(*graph, diags);
    if (!ninja || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    std::ofstream ofs(out_path);
    if (!ofs) {
        std::cerr << "error: cannot open output: " << out_path << "\n";
        return 1;
    }
    ofs << *ninja;

    return 0;
}
