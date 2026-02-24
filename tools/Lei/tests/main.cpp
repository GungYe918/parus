#include <lei/cache/GraphCache.hpp>
#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>
#include <lei/graph/BuildGraph.hpp>
#include <lei/parse/Parser.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <cstdio>
#include <cstdlib>

namespace {

bool has_code(const lei::diag::Bag& bag, lei::diag::Code code) {
    for (const auto& d : bag.all()) {
        if (d.code == code) return true;
    }
    return false;
}

bool run_ok_case(const std::filesystem::path& path, std::string entry_plan = "master") {
    lei::diag::Bag bag;
    auto builtins = lei::eval::make_default_builtin_registry();
    auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), std::move(builtin_plans), parser_control);

    lei::eval::EvaluateOptions opts{};
    opts.entry_plan = std::move(entry_plan);

    auto v = evaluator.evaluate_entry(path, opts);
    if (!v || bag.has_error()) {
        std::cerr << "unexpected failure:\n" << bag.render_text();
        return false;
    }

    auto graph = lei::graph::from_entry_plan_value(*v, bag, opts.entry_plan);
    if (!graph || bag.has_error()) {
        std::cerr << "graph failure:\n" << bag.render_text();
        return false;
    }

    auto exec_graph = lei::graph::lower_exec_graph(*graph, path.parent_path().lexically_normal().string(), bag);
    if (!exec_graph || bag.has_error()) {
        std::cerr << "exec graph failure:\n" << bag.render_text();
        return false;
    }

    auto ninja = lei::graph::emit_ninja(*exec_graph, bag);
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

bool run_err_case(const std::filesystem::path& path,
                  lei::diag::Code expected,
                  std::string entry_plan = "master") {
    lei::diag::Bag bag;
    auto builtins = lei::eval::make_default_builtin_registry();
    auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), std::move(builtin_plans), parser_control);

    lei::eval::EvaluateOptions opts{};
    opts.entry_plan = std::move(entry_plan);

    auto v = evaluator.evaluate_entry(path, opts);
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

bool run_graph_err_case(const std::filesystem::path& path,
                        lei::diag::Code expected,
                        std::string entry_plan = "master") {
    lei::diag::Bag bag;
    auto builtins = lei::eval::make_default_builtin_registry();
    auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), std::move(builtin_plans), parser_control);

    lei::eval::EvaluateOptions opts{};
    opts.entry_plan = std::move(entry_plan);

    auto v = evaluator.evaluate_entry(path, opts);
    if (!v || bag.has_error()) {
        std::cerr << "expected graph-stage failure but evaluation failed first:\n";
        std::cerr << bag.render_text();
        return false;
    }

    (void)lei::graph::from_entry_plan_value(*v, bag, opts.entry_plan);
    if (!bag.has_error()) {
        std::cerr << "expected graph failure but got success: " << path << "\n";
        return false;
    }

    if (!has_code(bag, expected)) {
        std::cerr << "expected graph diagnostic code not found: " << lei::diag::code_name(expected) << "\n";
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
        [](const std::vector<lei::eval::Value>& args,
           const lei::ast::Span&,
           lei::diag::Bag&) -> std::optional<lei::eval::Value> {
            if (!args.empty()) return std::nullopt;
            lei::eval::Value v;
            v.data = std::string("debug");
            return v;
        });

    auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
    lei::parse::ParserControl parser_control{};
    lei::eval::Evaluator evaluator({}, bag, std::move(builtins), std::move(builtin_plans), parser_control);

    lei::eval::EvaluateOptions opts{};
    opts.entry_plan = "master";

    auto v = evaluator.evaluate_entry(path, opts);
    if (!v || bag.has_error()) {
        std::cerr << "builtin api case failed:\n" << bag.render_text();
        return false;
    }
    return true;
}

bool run_invalid_utf8_case() {
    std::string source = "plan master {";
    source.push_back(static_cast<char>(0xFF));
    source += "};";

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

std::pair<int, std::string> run_cli_capture(const std::string& command) {
    const std::string tmp_path = "/tmp/lei_cli_capture.txt";
    const std::string full = command + " > " + tmp_path + " 2>&1";
    const int rc = std::system(full.c_str());

    std::ifstream ifs(tmp_path, std::ios::binary);
    std::string out;
    if (ifs) {
        out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    std::remove(tmp_path.c_str());
    return {rc, out};
}

bool run_cli_view_graph_cases(const std::filesystem::path& path) {
    const std::string bin = LEI_BUILD_BIN;
    const std::string src = path.string();

    auto [rc_json, out_json] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --view_graph");
    if (rc_json != 0 || out_json.find("\"bundles\"") == std::string::npos) {
        std::cerr << "cli --view_graph json failed\n" << out_json;
        return false;
    }

    auto [rc_text, out_text] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --view_graph --format text");
    if (rc_text != 0 || out_text.find("bundles=") == std::string::npos) {
        std::cerr << "cli --view_graph text failed\n" << out_text;
        return false;
    }

    auto [rc_dot, out_dot] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --view_graph --format dot");
    if (rc_dot != 0 || out_dot.find("digraph lei_build") == std::string::npos) {
        std::cerr << "cli --view_graph dot failed\n" << out_dot;
        return false;
    }

    auto [rc_conflict1, out_conflict1] = run_cli_capture("\"" + bin + "\" --check \"" + src + "\" --view_graph");
    if (rc_conflict1 == 0 || out_conflict1.find("--check and --view_graph") == std::string::npos) {
        std::cerr << "cli conflict (--check/--view_graph) should fail\n" << out_conflict1;
        return false;
    }

    auto [rc_conflict2, out_conflict2] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --view_graph --out /tmp/out.ninja");
    if (rc_conflict2 == 0 || out_conflict2.find("--view_graph and --out") == std::string::npos) {
        std::cerr << "cli conflict (--view_graph/--out) should fail\n" << out_conflict2;
        return false;
    }

    auto [rc_bad_fmt, out_bad_fmt] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --view_graph --format bad");
    if (rc_bad_fmt == 0 || out_bad_fmt.find("B_VIEW_FORMAT_INVALID") == std::string::npos) {
        std::cerr << "cli bad format should fail with B_VIEW_FORMAT_INVALID\n" << out_bad_fmt;
        return false;
    }

    return true;
}

bool run_cli_build_smoke(const std::filesystem::path& path) {
    const std::string bin = LEI_BUILD_BIN;
    const std::string src = path.string();

    auto [rc_build, out_build] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --build");
    if (rc_build != 0) {
        std::cerr << "cli --build failed\n" << out_build;
        return false;
    }

    const std::string out_ninja = "/tmp/lei_build_smoke.ninja";
    auto [rc_build_out, out_build_out] =
        run_cli_capture("\"" + bin + "\" \"" + src + "\" --build --out " + out_ninja);
    if (rc_build_out != 0) {
        std::cerr << "cli --build --out failed\n" << out_build_out;
        return false;
    }
    if (!std::filesystem::exists(out_ninja)) {
        std::cerr << "cli --build --out did not create ninja file\n";
        return false;
    }
    return true;
}

bool run_cli_list_sources_case(const std::filesystem::path& path) {
    const std::string bin = LEI_BUILD_BIN;
    const std::string src = path.string();

    auto [rc_ls, out_ls] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --list_sources");
    if (rc_ls != 0 || out_ls.find("src/main.pr") == std::string::npos) {
        std::cerr << "cli --list_sources failed\n" << out_ls;
        return false;
    }

    auto [rc_conflict, out_conflict] =
        run_cli_capture("\"" + bin + "\" \"" + src + "\" --list_sources --view_graph");
    if (rc_conflict == 0 || out_conflict.find("--list_sources and --view_graph") == std::string::npos) {
        std::cerr << "cli conflict (--list_sources/--view_graph) should fail\n" << out_conflict;
        return false;
    }

    auto [rc_ver, out_ver] = run_cli_capture("\"" + bin + "\" --version");
    if (rc_ver != 0 || out_ver.find("lei") == std::string::npos) {
        std::cerr << "cli --version failed\n" << out_ver;
        return false;
    }

    auto [rc_help, out_help] = run_cli_capture("\"" + bin + "\" --help");
    if (rc_help != 0 || out_help.find("usage:") == std::string::npos) {
        std::cerr << "cli --help failed\n" << out_help;
        return false;
    }

    return true;
}

bool run_cli_cache_smoke(const std::filesystem::path& path) {
    const std::string bin = LEI_BUILD_BIN;
    const std::string src = path.string();

    auto [rc1, out1] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --out /tmp/lei_cache_smoke.ninja");
    if (rc1 != 0) {
        std::cerr << "first cache warmup failed\n" << out1;
        return false;
    }
    auto [rc2, out2] = run_cli_capture("\"" + bin + "\" \"" + src + "\" --out /tmp/lei_cache_smoke_2.ninja");
    if (rc2 != 0) {
        std::cerr << "second cache run failed\n" << out2;
        return false;
    }

    std::error_code ec{};
    const auto abs = std::filesystem::weakly_canonical(path, ec);
    const std::string entry = ec ? path.string() : abs.string();
    const std::string key = lei::cache::make_cache_key(entry, "master");
    const auto graph_meta = lei::cache::graph_cache_dir() / (key + ".meta.json");
    const auto graph_json = lei::cache::graph_cache_dir() / (key + ".json");
    const auto ninja = lei::cache::ninja_cache_dir() / (key + ".ninja");

    if (!std::filesystem::exists(graph_meta) ||
        !std::filesystem::exists(graph_json) ||
        !std::filesystem::exists(ninja)) {
        std::cerr << "cache artifacts were not created for key: " << key << "\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    const std::filesystem::path cases = LEI_TEST_CASE_DIR;

    const bool ok1 = run_ok_case(cases / "ok_master_graph.lei");
    const bool ok2 = run_ok_case(cases / "ok_proto_bundle_merge.lei");
    const bool ok3 = run_ok_case(cases / "ok_task_codegen.lei");
    const bool ok4 = run_ok_case(cases / "ok_plan_export_ref.lei");
    const bool ok5 = run_ok_case(cases / "ok_plan_master_allowed.lei");
    const bool ok6 = run_ok_case(cases / "ok_builtin_constants_namespaces.lei");
    const bool ok7 = run_ok_case(cases / "ok_builtin_str_arr_obj.lei");
    const bool ok8 = run_ok_case(cases / "ok_builtin_path_fs_glob.lei");
    const bool ok9 = run_ok_case(cases / "ok_builtin_semver_subset.lei");
    const bool ok10 = run_ok_case(cases / "ok_builtin_parus_helpers.lei");
    const bool ok11 = run_ok_case(cases / "ok_bundle_bin_with_lib_closure.lei");
    const bool ok12 = run_ok_case(cases / "ok_codegen_then_compile.lei");
    const bool ok13 = run_ok_case(cases / "ok_module_import_canonicalization.lei");
    const bool ok14 = run_ok_case(cases / "ok_short_circuit_logic.lei");

    const bool err1 = run_err_case(cases / "err_legacy_export_build.lei", lei::diag::Code::C_LEGACY_SYNTAX_REMOVED);
    const bool err2 = run_err_case(cases / "err_legacy_fatarrow.lei", lei::diag::Code::C_LEGACY_SYNTAX_REMOVED);
    const bool err3 = run_err_case(cases / "err_proto_required_missing.lei", lei::diag::Code::L_PROTO_REQUIRED_FIELD_MISSING);
    const bool err4 = run_err_case(cases / "err_builtin_schema_violation_task.lei", lei::diag::Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION);
    const bool err5 = run_err_case(cases / "err_builtin_schema_violation_codegen.lei", lei::diag::Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION);
    const bool err6 = run_err_case(cases / "err_master_export_forbidden.lei", lei::diag::Code::L_MASTER_EXPORT_FORBIDDEN);
    const bool err7 = run_err_case(cases / "err_plan_not_found.lei", lei::diag::Code::L_PLAN_NOT_FOUND, "missing");
    const bool err8 = run_err_case(cases / "err_reserved_ident_let_bundle.lei", lei::diag::Code::C_RESERVED_IDENTIFIER);
    const bool err9 = run_err_case(cases / "err_reserved_ident_proto_codegen.lei", lei::diag::Code::C_RESERVED_IDENTIFIER);
    const bool err10 = run_err_case(cases / "err_reserved_ident_import_task.lei", lei::diag::Code::C_RESERVED_IDENTIFIER);
    const bool err11 = run_err_case(cases / "err_legacy_explicit_graph_removed.lei",
                                    lei::diag::Code::L_LEGACY_EXPLICIT_GRAPH_REMOVED);
    const bool err12 = run_err_case(cases / "err_builtin_bad_args.lei", lei::diag::Code::L_TYPE_MISMATCH);
    const bool err13 = run_graph_err_case(cases / "err_bundle_dep_cycle.lei", lei::diag::Code::B_INVALID_BUILD_SHAPE);
    const bool err14 = run_graph_err_case(cases / "err_module_head_removed.lei",
                                          lei::diag::Code::B_MODULE_HEAD_REMOVED);
    const bool err15 = run_graph_err_case(cases / "err_module_import_invalid.lei",
                                          lei::diag::Code::B_MODULE_IMPORT_INVALID);
    const bool err16 = run_graph_err_case(cases / "err_module_top_head_collision.lei",
                                          lei::diag::Code::B_MODULE_TOP_HEAD_COLLISION);

    const bool ok_builtin = run_builtin_api_case(cases / "ok_builtin_fn_in_master.lei");
    const bool ok_cli = run_cli_view_graph_cases(cases / "ok_master_graph.lei");
    const bool ok_list_sources = run_cli_list_sources_case(cases / "ok_task_codegen.lei");
    const bool ok_build = run_cli_build_smoke(cases / "ok_build_task_true.lei");
    const bool ok_cache = run_cli_cache_smoke(cases / "ok_build_empty.lei");
    const bool ok_utf8 = run_invalid_utf8_case();
    const bool ok_boundary = run_no_parus_include_rule();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 || !ok9 || !ok10 || !ok11 || !ok12 || !ok13 || !ok14 ||
        !err1 || !err2 || !err3 || !err4 || !err5 || !err6 || !err7 || !err8 || !err9 || !err10 || !err11 || !err12 || !err13 || !err14 || !err15 || !err16 ||
        !ok_builtin || !ok_cli || !ok_list_sources || !ok_build || !ok_cache || !ok_utf8 || !ok_boundary) {
        return 1;
    }

    std::cout << "lei tests passed\n";
    return 0;
}
