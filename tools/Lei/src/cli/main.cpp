#include <lei/cache/GraphCache.hpp>
#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>
#include <lei/graph/BuildGraph.hpp>
#include <lei/graph/NinjaRunner.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "usage:\n";
    std::cerr << "  lei --help\n";
    std::cerr << "  lei --version\n";
    std::cerr << "  lei <config.lei> --out <build.ninja> [--plan <name>]\n";
    std::cerr << "  lei --check <config.lei> [--plan <name>]\n";
    std::cerr << "  lei <config.lei> --list_sources [--plan <name>]\n";
    std::cerr << "  lei <config.lei> --view_graph [--format <json|text|dot>] [--plan <name>]\n";
    std::cerr << "  lei <config.lei> --build [--out <build.ninja>] [--jobs <N>] [--verbose] [--plan <name>]\n";
}

bool write_text_file(const std::string& path, const std::string& text) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs << text;
    return ofs.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "lei dev\n";
        return 0;
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    bool check_only = false;
    bool list_sources = false;
    bool view_graph = false;
    bool build_now = false;
    bool out_set = false;
    bool format_set = false;
    bool verbose = false;

    std::string entry;
    std::string out_path = "build.ninja";
    std::string entry_plan = "master";
    std::string view_format = "json";
    uint32_t jobs = 1;

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
        if (a == "--list_sources") {
            list_sources = true;
            continue;
        }
        if (a == "--build") {
            build_now = true;
            continue;
        }
        if (a == "--verbose") {
            verbose = true;
            continue;
        }
        if (a == "--jobs") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            try {
                const long long v = std::stoll(args[++i]);
                jobs = static_cast<uint32_t>(v <= 0 ? 1 : v);
            } catch (...) {
                std::cerr << "error: --jobs requires a positive integer\n";
                return 1;
            }
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
    if (check_only && list_sources) {
        std::cerr << "error: --check and --list_sources cannot be used together\n";
        return 1;
    }
    if (list_sources && view_graph) {
        std::cerr << "error: --list_sources and --view_graph cannot be used together\n";
        return 1;
    }
    if (list_sources && build_now) {
        std::cerr << "error: --list_sources and --build cannot be used together\n";
        return 1;
    }
    if (list_sources && out_set) {
        std::cerr << "error: --list_sources and --out cannot be used together\n";
        return 1;
    }
    if (list_sources && format_set) {
        std::cerr << "error: --format requires --view_graph\n";
        return 1;
    }
    if (check_only && build_now) {
        std::cerr << "error: --check and --build cannot be used together\n";
        return 1;
    }
    if (build_now && view_graph) {
        std::cerr << "error: --build and --view_graph cannot be used together\n";
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

    std::error_code ec{};
    const std::string entry_abs = std::filesystem::weakly_canonical(std::filesystem::path(entry), ec).string();
    const std::string entry_norm = ec ? entry : entry_abs;

    lei::diag::Bag diags;

    if (!check_only && !list_sources && !view_graph && !build_now) {
        auto cached = lei::cache::load_graph_cache(entry_norm, entry_plan, diags);
        if (cached.has_value()) {
            const std::string out_ninja = out_set ? out_path : std::string("build.ninja");
            if (!write_text_file(out_ninja, cached->ninja_text)) {
                std::cerr << "error: cannot open output: " << out_ninja << "\n";
                return 1;
            }
            return 0;
        }
    }

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

    auto value = evaluator.evaluate_entry(std::filesystem::path(entry_norm), options);
    if (!value || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    auto graph = lei::graph::from_entry_plan_value(*value, diags, entry_plan);
    if (!graph || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    if (check_only) {
        return 0;
    }

    if (list_sources) {
        std::set<std::string> srcs{};
        for (const auto& b : graph->bundles) {
            for (const auto& s : b.sources) srcs.insert(s);
        }
        for (const auto& s : srcs) {
            std::cout << s << "\n";
        }
        return 0;
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

    auto exec_graph = lei::graph::lower_exec_graph(*graph, diags);
    if (!exec_graph || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    auto ninja = lei::graph::emit_ninja(*exec_graph, diags);
    if (!ninja || diags.has_error()) {
        std::cerr << diags.render_text();
        return 1;
    }

    const std::string graph_json = lei::graph::emit_graph_json(*graph, diags).value_or("{}");
    lei::cache::GraphCacheMeta cache_meta{};
    cache_meta.entry_file = entry_norm;
    cache_meta.entry_plan = entry_plan;
    cache_meta.builtin_fingerprint = "lei-builtins-v1";
    for (const auto& path : evaluator.loaded_module_paths()) {
        lei::cache::ModuleHash m{};
        m.path = path;
        m.hash = lei::cache::hash_file(path);
        cache_meta.modules.push_back(std::move(m));
    }

    (void)lei::cache::store_graph_cache(entry_norm, entry_plan, cache_meta, graph_json, *ninja, diags);

    std::string ninja_out = out_set ? out_path : "build.ninja";
    if (build_now && !out_set) {
        const auto key = lei::cache::make_cache_key(entry_norm, entry_plan);
        ninja_out = (lei::cache::ninja_cache_dir() / (key + ".ninja")).string();
    }

    if (!write_text_file(ninja_out, *ninja)) {
        std::cerr << "error: cannot open output: " << ninja_out << "\n";
        return 1;
    }

    if (build_now) {
        if (!lei::graph::run_embedded_ninja(std::filesystem::path(ninja_out), jobs, verbose, diags)) {
            std::cerr << diags.render_text();
            return 1;
        }
    }

    return 0;
}
