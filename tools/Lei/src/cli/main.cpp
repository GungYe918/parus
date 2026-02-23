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
    std::cerr << "  lei-build <config.lei> --out <build.ninja> [--plan <name>]\n";
    std::cerr << "  lei-build --check <config.lei> [--plan <name>]\n";
    std::cerr << "  lei-build <config.lei> --view_graph [--format <json|text|dot>] [--plan <name>]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    bool check_only = false;
    bool view_graph = false;
    bool out_set = false;
    bool format_set = false;
    std::string entry;
    std::string out_path = "build.ninja";
    std::string entry_plan = "master";
    std::string view_format = "json";

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
        if (a == "--view_graph") {
            view_graph = true;
            continue;
        }
        if (a == "--format") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            format_set = true;
            view_format = args[++i];
            continue;
        }
        if (a == "--out") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            out_set = true;
            out_path = args[++i];
            continue;
        }
        if (a == "--plan") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            entry_plan = args[++i];
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
    if (check_only && view_graph) {
        std::cerr << "error: --check and --view_graph cannot be used together\n";
        return 1;
    }
    if (view_graph && out_set) {
        std::cerr << "error: --view_graph and --out cannot be used together\n";
        return 1;
    }
    if (!view_graph && format_set) {
        std::cerr << "error: --format requires --view_graph\n";
        return 1;
    }

    lei::diag::Bag diags;
    lei::eval::EvaluatorBudget budget{};
    auto builtins = lei::eval::make_default_builtin_registry();
    auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator(budget,
                                   diags,
                                   std::move(builtins),
                                   std::move(builtin_plans),
                                   parser_control);

    lei::eval::EvaluateOptions options{};
    options.entry_plan = entry_plan;

    auto value = evaluator.evaluate_entry(std::filesystem::path(entry), options);
    if (!value || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    if (check_only) {
        return 0;
    }

    auto graph = lei::graph::from_entry_plan_value(*value, diags, entry_plan);
    if (!graph || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    if (view_graph) {
        std::optional<std::string> rendered{};
        if (view_format == "json") {
            rendered = lei::graph::emit_graph_json(*graph, diags);
        } else if (view_format == "text") {
            rendered = lei::graph::emit_graph_text(*graph, diags);
        } else if (view_format == "dot") {
            rendered = lei::graph::emit_graph_dot(*graph, diags);
        } else {
            diags.add(lei::diag::Code::B_VIEW_FORMAT_INVALID,
                      "<cli>",
                      1,
                      1,
                      "unsupported --format value: " + view_format);
        }
        if (!rendered || diags.has_error()) {
            std::cerr << diags.render_text();
            return 1;
        }
        std::cout << *rendered;
        return 0;
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
