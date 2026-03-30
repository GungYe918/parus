#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

namespace {

std::pair<int, std::string> run_capture(const std::string& command) {
    const std::string tmp = "/tmp/parus_cli_capture.txt";
    const std::string full = "(" + command + ") > " + tmp + " 2>&1";
    const int rc = std::system(full.c_str());

    std::ifstream ifs(tmp, std::ios::binary);
    std::string out;
    if (ifs) {
        out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    std::remove(tmp.c_str());
    return {rc, out};
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

size_t count_occurrences(const std::string& s, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs << text;
    return ofs.good();
}

struct ExportIndexSourceSeed {
    std::filesystem::path path{};
    std::string module_head{};
    std::vector<std::string> module_imports{};
};

bool emit_bundle_export_index_from_fragments(
    const std::string& bin,
    const std::string& bundle_name,
    const std::filesystem::path& bundle_root,
    const std::vector<ExportIndexSourceSeed>& sources_with_heads,
    const std::filesystem::path& out_index,
    std::string& out_log
) {
    out_log.clear();
    if (sources_with_heads.empty()) {
        out_log = "no bundle sources";
        return false;
    }

    std::vector<std::filesystem::path> fragments{};
    fragments.reserve(sources_with_heads.size());
    for (size_t i = 0; i < sources_with_heads.size(); ++i) {
        const auto& seed = sources_with_heads[i];
        const auto& src = seed.path;
        const auto& module_head = seed.module_head;
        const auto frag = out_index.parent_path() / (out_index.stem().string() + ".frag." + std::to_string(i) + ".json");
        fragments.push_back(frag);

        std::string cmd =
            "\"" + bin + "\" tool parusc -- \"" + src.string() + "\"" +
            " --bundle-name " + bundle_name +
            " --bundle-root \"" + bundle_root.string() + "\"" +
            " --module-head " + module_head +
            " --emit-export-index \"" + frag.string() + "\"";
        for (const auto& all_seed : sources_with_heads) {
            cmd += " --bundle-source \"" + all_seed.path.string() + "\"";
        }
        for (const auto& import_head : seed.module_imports) {
            cmd += " --module-import " + import_head;
        }
        auto [rc, out] = run_capture(cmd);
        out_log += out;
        if (rc != 0) return false;
    }

    std::string merge_cmd =
        "\"" + bin + "\" tool parusc -- \"" + sources_with_heads.front().path.string() + "\"" +
        " --bundle-name " + bundle_name +
        " --bundle-root \"" + bundle_root.string() + "\"" +
        " --module-head " + sources_with_heads.front().module_head +
        " --emit-export-index \"" + out_index.string() + "\"";
    for (const auto& all_seed : sources_with_heads) {
        merge_cmd += " --bundle-source \"" + all_seed.path.string() + "\"";
    }
    for (const auto& import_head : sources_with_heads.front().module_imports) {
        merge_cmd += " --module-import " + import_head;
    }
    for (const auto& frag : fragments) {
        merge_cmd += " --load-export-index \"" + frag.string() + "\"";
    }
    auto [merge_rc, merge_out] = run_capture(merge_cmd);
    out_log += merge_out;
    return merge_rc == 0;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

std::string parse_default_target_triple(const std::string& manifest_text) {
    const std::string key = "\"default_target_triple\"";
    const size_t key_pos = manifest_text.find(key);
    if (key_pos == std::string::npos) return {};
    const size_t colon = manifest_text.find(':', key_pos + key.size());
    if (colon == std::string::npos) return {};
    const size_t first_quote = manifest_text.find('"', colon + 1);
    if (first_quote == std::string::npos) return {};
    const size_t second_quote = manifest_text.find('"', first_quote + 1);
    if (second_quote == std::string::npos || second_quote <= first_quote + 1) return {};
    return manifest_text.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::optional<std::pair<std::string, std::string>> resolve_installed_sysroot_and_target() {
    const char* env_sysroot = std::getenv("PARUS_SYSROOT");
    if (env_sysroot != nullptr && env_sysroot[0] != '\0') {
        const std::filesystem::path sysroot = env_sysroot;
        const std::string target = parse_default_target_triple(read_text(sysroot / "manifest.json"));
        if (!target.empty()) return std::make_pair(sysroot.string(), target);
    }

    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return std::nullopt;

    const std::filesystem::path toolchains = std::filesystem::path(home) / ".local/share/parus/toolchains";
    std::error_code ec{};
    if (!std::filesystem::exists(toolchains, ec) || ec) return std::nullopt;

    std::vector<std::filesystem::path> candidates{};
    for (const auto& entry : std::filesystem::directory_iterator(toolchains, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const auto sysroot = entry.path() / "sysroot";
        const auto manifest = sysroot / "manifest.json";
        if (std::filesystem::exists(manifest, ec) && !ec) {
            candidates.push_back(sysroot);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const std::string target = parse_default_target_triple(read_text(*it / "manifest.json"));
        if (!target.empty()) return std::make_pair(it->string(), target);
    }
    return std::nullopt;
}

bool test_help_and_version() {
    const std::string bin = PARUS_BUILD_BIN;

    auto [rc_help, out_help] = run_capture("\"" + bin + "\" --help");
    if (rc_help != 0 || !contains(out_help, "Commands:")) {
        std::cerr << "help failed\n" << out_help;
        return false;
    }

    auto [rc_ver, out_ver] = run_capture("\"" + bin + "\" --version");
    if (rc_ver != 0 || !contains(out_ver, "parus v")) {
        std::cerr << "version failed\n" << out_ver;
        return false;
    }

    return true;
}

bool test_build_and_graph() {
    const std::string bin = PARUS_BUILD_BIN;
    const std::string lei_case = std::string(PARUS_LEI_CASE_DIR) + "/ok_build_empty.lei";

    const std::string out_ninja = "/tmp/parus_cli_build.ninja";
    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" build \"" + lei_case + "\" --out \"" + out_ninja + "\"");
    if (rc_build != 0 || !std::filesystem::exists(out_ninja)) {
        std::cerr << "build failed\n" << out_build;
        return false;
    }

    const std::string graph_case = std::string(PARUS_LEI_CASE_DIR) + "/ok_master_graph.lei";
    auto [rc_graph, out_graph] = run_capture(
        "\"" + bin + "\" graph \"" + graph_case + "\" --format dot");
    if (rc_graph != 0 || !contains(out_graph, "digraph lei_build")) {
        std::cerr << "graph failed\n" << out_graph;
        return false;
    }

    return true;
}

bool test_check_pr() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-check-pr";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto pr_path = temp_root / "main.pr";
    const std::string pr_src =
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(pr_path, pr_src)) {
        std::cerr << "failed to write temp .pr file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + pr_path.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc != 0) {
        std::cerr << "check .pr failed\n" << out;
        return false;
    }
    return true;
}

bool test_check_lei_project() {
    const std::string bin = PARUS_BUILD_BIN;

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-check";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto pr = temp_root / "main.pr";
    const auto lei = temp_root / "config.lei";

    const std::string pr_src =
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    const std::string lei_src =
        "plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"main.pr\"];\n"
        "      imports = [];\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"check-proj\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(pr, pr_src) || !write_text(lei, lei_src)) {
        std::cerr << "failed to write temp project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + lei.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        std::cerr << "check .lei failed\n" << out;
        return false;
    }
    return true;
}

bool test_doctor_json_and_global_json_reject() {
    const std::string bin = PARUS_BUILD_BIN;

    auto [rc_doc, out_doc] = run_capture("\"" + bin + "\" doctor --json");
    if (rc_doc != 0 || !contains(out_doc, "\"items\"")) {
        std::cerr << "doctor --json failed\n" << out_doc;
        return false;
    }

    auto [rc_bad, out_bad] = run_capture("\"" + bin + "\" --json doctor");
    if (rc_bad == 0 || !contains(out_bad, "--json is only available")) {
        std::cerr << "global --json reject failed\n" << out_bad;
        return false;
    }

    return true;
}

bool test_tool_passthrough() {
    const std::string bin = PARUS_BUILD_BIN;
    auto [rc, out] = run_capture("\"" + bin + "\" tool parusc -- --version");
    if (rc != 0 || !contains(out, "parus v")) {
        std::cerr << "tool passthrough failed\n" << out;
        return false;
    }
    return true;
}

bool test_bundle_strict_export_violation() {
    const std::string bin = PARUS_BUILD_BIN;

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-bundle-strict";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto a = temp_root / "a.pr";
    const auto b = temp_root / "b.pr";
    const auto lei = temp_root / "config.lei";

    const std::string a_src =
        "nest pkg;\n"
        "def hidden() -> i32 {\n"
        "  return 1i32;\n"
        "}\n";

    const std::string b_src =
        "nest pkg;\n"
        "def use_hidden() -> i32 {\n"
        "  return hidden();\n"
        "}\n";

    const std::string lei_src =
        "plan pkg_bundle = bundle & {\n"
        "  name = \"pkg\";\n"
        "  kind = \"lib\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"a.pr\", \"b.pr\"];\n"
        "      imports = [];\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"strict-bundle\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [pkg_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(a, a_src) || !write_text(b, b_src) || !write_text(lei, lei_src)) {
        std::cerr << "failed to write strict bundle project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + lei.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "strict bundle visibility expected failure but passed\n" << out;
        return false;
    }
    if (!contains(out, "SymbolNotExportedFileScope")) {
        std::cerr << "strict bundle failure did not report SymbolNotExportedFileScope\n" << out;
        return false;
    }
    return true;
}

bool test_bundle_build_strict_export_violation() {
    const std::string bin = PARUS_BUILD_BIN;

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-bundle-build-strict";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto a = temp_root / "a.pr";
    const auto b = temp_root / "b.pr";
    const auto lei = temp_root / "config.lei";

    const std::string a_src =
        "nest pkg;\n"
        "def main() -> i32 {\n"
        "  return hidden();\n"
        "}\n";

    const std::string b_src =
        "nest pkg;\n"
        "def hidden() -> i32 {\n"
        "  return 1i32;\n"
        "}\n";

    const std::string lei_src =
        "plan pkg_bundle = bundle & {\n"
        "  name = \"pkg\";\n"
        "  kind = \"bin\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"a.pr\", \"b.pr\"];\n"
        "      imports = [];\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"strict-bundle-build\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [pkg_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(a, a_src) || !write_text(b, b_src) || !write_text(lei, lei_src)) {
        std::cerr << "failed to write strict build project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd = "cd \"" + temp_root.string() + "\" && \"" + bin + "\" build config.lei";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "strict build visibility expected failure but passed\n" << out;
        return false;
    }
    if (!contains(out, "SymbolNotExportedFileScope")) {
        std::cerr << "strict build failure did not report SymbolNotExportedFileScope\n" << out;
        return false;
    }
    return true;
}

bool test_bundle_dep_import_not_declared() {
    const std::string bin = PARUS_BUILD_BIN;

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-dep-import";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto pr = temp_root / "main.pr";
    const auto lei = temp_root / "config.lei";

    const std::string pr_src =
        "import math as m;\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    const std::string lei_src =
        "plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"main.pr\"];\n"
        "      imports = [];\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"dep-import\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(pr, pr_src) || !write_text(lei, lei_src)) {
        std::cerr << "failed to write dep import project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + lei.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "deps import expected failure but passed\n" << out;
        return false;
    }
    if (!contains(out, "ImportDepNotDeclared")) {
        std::cerr << "deps import failure did not report ImportDepNotDeclared\n" << out;
        return false;
    }
    return true;
}

bool test_cross_bundle_non_export_violation() {
    const std::string bin = PARUS_BUILD_BIN;

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cross-bundle-export";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto math_dir = temp_root / "math";
    const auto app_dir = temp_root / "app";
    const auto math_src_dir = math_dir / "src";
    const auto app_src_dir = app_dir / "src";
    std::filesystem::create_directories(math_src_dir, ec);
    std::filesystem::create_directories(app_src_dir, ec);
    if (ec) {
        std::cerr << "temp subdir create failed\n";
        return false;
    }

    const auto lib = math_src_dir / "lib.pr";
    const auto app = app_src_dir / "main.pr";
    const auto math_lei = math_dir / "math.lei";
    const auto app_lei = app_dir / "app.lei";
    const auto lei = temp_root / "config.lei";

    const std::string lib_src =
        "nest math::arith;\n"
        "def hidden(a: i32, b: i32) -> i32 {\n"
        "  return a + b;\n"
        "}\n";

    const std::string app_src =
        "import math as m;\n"
        "def main() -> i32 {\n"
        "  return m::arith::hidden(1i32, 2i32);\n"
        "}\n";

    const std::string math_lei_src =
        "export plan math_bundle = bundle & {\n"
        "  name = \"math\";\n"
        "  kind = \"lib\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"math/src/lib.pr\"];\n"
        "      imports = [];\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n";

    const std::string app_lei_src =
        "export plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"app/src/main.pr\"];\n"
        "      imports = [\"math\"];\n"
        "    },\n"
        "  ];\n"
        "  deps = [\"math\"];\n"
        "};\n";

    const std::string lei_src =
        "import math from \"./math/math.lei\";\n"
        "import app from \"./app/app.lei\";\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"cross-bundle-export\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [math::math_bundle, app::app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(lib, lib_src) ||
        !write_text(app, app_src) ||
        !write_text(math_lei, math_lei_src) ||
        !write_text(app_lei, app_lei_src) ||
        !write_text(lei, lei_src)) {
        std::cerr << "failed to write cross bundle project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + lei.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "cross bundle non-export expected failure but passed\n" << out;
        return false;
    }
    if (!contains(out, "SymbolNotExportedBundleScope")) {
        std::cerr << "cross bundle failure did not report SymbolNotExportedBundleScope\n" << out;
        return false;
    }
    return true;
}

bool test_check_capability_diagnostic_surface() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-capability-diag";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto pr = temp_root / "main.pr";
    const std::string pr_src =
        "def main() -> i32 {\n"
        "  let x: i32 = 1i32;\n"
        "  let h: ~i32 = ~x;\n"
        "  let p: &i32 = &h;\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(pr, pr_src)) {
        std::cerr << "failed to write capability diagnostic seed file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" tool parusc -- \"" + pr.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "capability diagnostic seed should fail but passed (parusc)\n" << out;
        return false;
    }
    if (!contains(out, "BorrowOperandMustBeOwnedPlace")) {
        std::cerr << "capability failure did not surface detailed diagnostic\n" << out;
        return false;
    }
    return true;
}

bool test_cross_bundle_export_runtime_call() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cross-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "math/src", ec);
    std::filesystem::create_directories(temp_root / "app/src", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto lib = temp_root / "math/src/lib.pr";
    const auto app = temp_root / "app/src/main.pr";
    const auto math_lei = temp_root / "math/math.lei";
    const auto app_lei = temp_root / "app/app.lei";
    const auto lei = temp_root / "config.lei";

    const std::string lib_src =
        "nest math::arith;\n"
        "export def add(a: i32, b: i32) -> i32 {\n"
        "  return a + b;\n"
        "}\n";

    const std::string app_src =
        "import math as m;\n"
        "def main() -> i32 {\n"
        "  return m::arith::add(5i32, 6i32);\n"
        "}\n";

    const std::string math_lei_src =
        "export plan math_bundle = bundle & {\n"
        "  name = \"math\";\n"
        "  kind = \"lib\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"math/src/lib.pr\"];\n"
        "      imports = [];\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n";

    const std::string app_lei_src =
        "export plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"app/src/main.pr\"];\n"
        "      imports = [\"math\"];\n"
        "    },\n"
        "  ];\n"
        "  deps = [\"math\"];\n"
        "};\n";

    const std::string lei_src =
        "import math from \"./math/math.lei\";\n"
        "import app from \"./app/app.lei\";\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"cross-runtime\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [math::math_bundle, app::app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(lib, lib_src) ||
        !write_text(app, app_src) ||
        !write_text(math_lei, math_lei_src) ||
        !write_text(app_lei, app_lei_src) ||
        !write_text(lei, lei_src)) {
        std::cerr << "failed to write cross runtime project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string build_cmd = "cd \"" + temp_root.string() + "\" && \"" + bin + "\" build config.lei";
    auto [rc_build, out_build] = run_capture(build_cmd);
    if (rc_build != 0) {
        std::cerr << "cross runtime build failed\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string run_cmd =
        "cd \"" + temp_root.string() + "\" && \"target/parus/out/bin/app\"; echo EXIT:$?";
    auto [rc_run, out_run] = run_capture(run_cmd);
    std::filesystem::remove_all(temp_root, ec);

    if (rc_run != 0) {
        std::cerr << "cross runtime run command failed\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:11")) {
        std::cerr << "cross runtime exit mismatch (expected 11)\n" << out_run;
        return false;
    }
    return true;
}

bool test_same_bundle_multi_module_runtime_call() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-same-bundle-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "util/src", ec);
    std::filesystem::create_directories(temp_root / "app/src", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto util_src = temp_root / "util/src/lib.pr";
    const auto app_src = temp_root / "app/src/main.pr";
    const auto lei = temp_root / "config.lei";

    const std::string util_pr =
        "nest util;\n"
        "export def helper() -> i32 {\n"
        "  return 7i32;\n"
        "}\n";

    const std::string app_pr =
        "import util as u;\n"
        "def main() -> i32 {\n"
        "  return u::helper();\n"
        "}\n";

    const std::string lei_src =
        "plan util_module = module & {\n"
        "  sources = [\"util/src/lib.pr\"];\n"
        "  imports = [];\n"
        "};\n"
        "\n"
        "plan app_module = module & {\n"
        "  sources = [\"app/src/main.pr\"];\n"
        "  imports = [\"util\"];\n"
        "};\n"
        "\n"
        "plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  modules = [util_module, app_module];\n"
        "  deps = [];\n"
        "};\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"same-bundle-runtime\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    if (!write_text(util_src, util_pr) ||
        !write_text(app_src, app_pr) ||
        !write_text(lei, lei_src)) {
        std::cerr << "failed to write same bundle runtime project files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string build_cmd = "cd \"" + temp_root.string() + "\" && \"" + bin + "\" build config.lei";
    auto [rc_build, out_build] = run_capture(build_cmd);
    if (rc_build != 0) {
        std::cerr << "same bundle runtime build failed\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string run_cmd =
        "cd \"" + temp_root.string() + "\" && \"target/parus/out/bin/app\"; echo EXIT:$?";
    auto [rc_run, out_run] = run_capture(run_cmd);
    std::filesystem::remove_all(temp_root, ec);

    if (rc_run != 0) {
        std::cerr << "same bundle runtime run command failed\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:7")) {
        std::cerr << "same bundle runtime exit mismatch (expected 7)\n" << out_run;
        return false;
    }
    return true;
}

bool test_builtin_acts_policy_core_gate() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-builtin-acts";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto non_core = temp_root / "non_core.pr";
    const auto non_core_marker_only = temp_root / "non_core_marker_only.pr";
    const auto core_no_marker = temp_root / "core_no_marker.pr";
    const auto core_ok = temp_root / "core_ok.pr";
    const auto core_dup = temp_root / "core_dup.pr";

    const std::string base_decl =
        "acts for i32 {\n"
        "  def size(self) -> i32 {\n"
        "    return 4i32;\n"
        "  }\n"
        "};\n";

    const std::string marker_decl =
        "$![Impl::Core];\n"
        "\n"
        "acts for i32 {\n"
        "  def size(self) -> i32 {\n"
        "    return 4i32;\n"
        "  }\n"
        "};\n";

    const std::string dup_decl =
        "$![Impl::Core];\n"
        "\n"
        "acts for i32 {\n"
        "  def size(self) -> i32 {\n"
        "    return 4i32;\n"
        "  }\n"
        "};\n"
        "\n"
        "acts for i32 {\n"
        "  def bits(self) -> i32 {\n"
        "    return 32i32;\n"
        "  }\n"
        "};\n";

    const std::string marker_only =
        "$![Impl::Core];\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(non_core, base_decl) ||
        !write_text(non_core_marker_only, marker_only) ||
        !write_text(core_no_marker, base_decl) ||
        !write_text(core_ok, marker_decl) ||
        !write_text(core_dup, dup_decl)) {
        std::cerr << "failed to write builtin acts policy files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto run_tool = [&](const std::filesystem::path& pr,
                        const std::string& bundle_name) -> std::pair<int, std::string> {
        const std::string cmd =
            "\"" + bin + "\" tool parusc -- \"" + pr.string() + "\" -fsyntax-only" +
            " --bundle-name " + bundle_name +
            " --bundle-root \"" + temp_root.string() + "\"" +
            " --module-head " + bundle_name +
            " --bundle-source \"" + pr.string() + "\"";
        return run_capture(cmd);
    };

    auto [rc_non_core, out_non_core] = run_tool(non_core, "app");
    if (rc_non_core == 0 ||
        !contains(out_non_core, "requires bundle 'core' and file marker '$![Impl::Core];'")) {
        std::cerr << "non-core builtin acts should fail\n" << out_non_core;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_no_marker, out_core_no_marker] = run_tool(core_no_marker, "core");
    if (rc_core_no_marker == 0 ||
        !contains(out_core_no_marker, "requires bundle 'core' and file marker '$![Impl::Core];'")) {
        std::cerr << "core builtin acts without marker should fail\n" << out_core_no_marker;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_ok, out_core_ok] = run_tool(core_ok, "core");
    if (rc_core_ok != 0) {
        std::cerr << "core builtin acts should pass\n" << out_core_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_dup, out_core_dup] = run_tool(core_dup, "core");
    if (rc_core_dup == 0 || !contains(out_core_dup, "duplicate default acts declaration for type i32")) {
        std::cerr << "core duplicate builtin acts should fail\n" << out_core_dup;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_non_core_marker, out_non_core_marker] = run_tool(non_core_marker_only, "app");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_non_core_marker == 0 ||
        !contains(out_non_core_marker, "$![Impl::Core]; is allowed only when bundle-name is 'core'")) {
        std::cerr << "non-core marker-only file should fail marker policy\n" << out_non_core_marker;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    std::filesystem::remove_all(temp_root, ec);
    return true;
}

bool test_impl_binding_surface_policy() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-impl-binding-policy";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto non_core = temp_root / "non_core.pr";
    const auto non_core_with_body = temp_root / "non_core_with_body.pr";
    const auto non_core_unknown_with_body = temp_root / "non_core_unknown_with_body.pr";
    const auto core_no_marker = temp_root / "core_no_marker.pr";
    const auto core_ok = temp_root / "core_ok.pr";
    const auto core_with_body = temp_root / "core_with_body.pr";
    const auto core_bad_sig = temp_root / "core_bad_sig.pr";
    const auto core_non_fn = temp_root / "core_non_fn.pr";
    const auto ordinary_bodyless = temp_root / "ordinary_bodyless.pr";

    const std::string impl_decls =
        "$![Impl::SizeOf]\n"
        "export def size_of<T>() -> usize;\n"
        "\n"
        "$![Impl::AlignOf]\n"
        "export def align_of<T>() -> usize;\n"
        "\n"
        "$![Impl::SpinLoop]\n"
        "export def spin_loop() -> void;\n";

    const std::string core_ok_src =
        "$![Impl::Core];\n"
        "\n" + impl_decls;

    const std::string core_with_body_src =
        "$![Impl::Core];\n"
        "\n"
        "$![Impl::SpinLoop]\n"
        "export def spin_loop() -> void {\n"
        "}\n";

    const std::string non_core_with_body_src =
        "$![Impl::SizeOf]\n"
        "export def size_of<T>() -> usize {\n"
        "  return 0usize;\n"
        "}\n";

    const std::string non_core_unknown_with_body_src =
        "$![Impl::Spawn]\n"
        "export def spawn() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    const std::string core_bad_sig_src =
        "$![Impl::Core];\n"
        "\n"
        "$![Impl::SizeOf]\n"
        "export def size_of<T>(x: i32) -> usize;\n";

    const std::string core_non_fn_src =
        "$![Impl::Core];\n"
        "\n"
        "$![Impl::SpinLoop]\n"
        "const X: i32 = 1i32;\n";

    const std::string ordinary_bodyless_src =
        "export def nope() -> void;\n";

    if (!write_text(non_core, impl_decls) ||
        !write_text(non_core_with_body, non_core_with_body_src) ||
        !write_text(non_core_unknown_with_body, non_core_unknown_with_body_src) ||
        !write_text(core_no_marker, impl_decls) ||
        !write_text(core_ok, core_ok_src) ||
        !write_text(core_with_body, core_with_body_src) ||
        !write_text(core_bad_sig, core_bad_sig_src) ||
        !write_text(core_non_fn, core_non_fn_src) ||
        !write_text(ordinary_bodyless, ordinary_bodyless_src)) {
        std::cerr << "failed to write Impl::* policy sources\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto run_tool = [&](const std::filesystem::path& pr,
                        const std::string& bundle_name) -> std::pair<int, std::string> {
        const std::string cmd =
            "\"" + bin + "\" tool parusc -- \"" + pr.string() + "\" -fsyntax-only" +
            " --bundle-name " + bundle_name +
            " --bundle-root \"" + temp_root.string() + "\"" +
            " --module-head core::mem" +
            " --bundle-source \"" + pr.string() + "\"";
        return run_capture(cmd);
    };

    auto [rc_non_core, out_non_core] = run_tool(non_core, "app");
    if (rc_non_core == 0 ||
        !contains(out_non_core, "bodyless recognized $![Impl::*] binding requires bundle 'core' and file marker '$![Impl::Core];'")) {
        std::cerr << "non-core bodyless recognized Impl::* surface should fail\n" << out_non_core;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_non_core_with_body, out_non_core_with_body] = run_tool(non_core_with_body, "app");
    if (rc_non_core_with_body != 0) {
        std::cerr << "non-core recognized Impl::* with body should pass as library-owned surface\n" << out_non_core_with_body;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_non_core_unknown_with_body, out_non_core_unknown_with_body] = run_tool(non_core_unknown_with_body, "app");
    if (rc_non_core_unknown_with_body != 0) {
        std::cerr << "non-core unknown Impl::* with body should pass as metadata-only surface\n" << out_non_core_unknown_with_body;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_no_marker, out_core_no_marker] = run_tool(core_no_marker, "core");
    if (rc_core_no_marker == 0 ||
        !contains(out_core_no_marker, "bodyless recognized $![Impl::*] binding requires bundle 'core' and file marker '$![Impl::Core];'")) {
        std::cerr << "core Impl::* surface without marker should fail\n" << out_core_no_marker;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_ok, out_core_ok] = run_tool(core_ok, "core");
    if (rc_core_ok != 0) {
        std::cerr << "core Impl::* surface should pass\n" << out_core_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_with_body, out_core_with_body] = run_tool(core_with_body, "core");
    if (rc_core_with_body != 0) {
        std::cerr << "recognized Impl::* with body should pass as library-owned surface\n" << out_core_with_body;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_bad_sig, out_core_bad_sig] = run_tool(core_bad_sig, "core");
    if (rc_core_bad_sig == 0 ||
        !contains(out_core_bad_sig, "$![Impl::SizeOf] requires signature 'def size_of<T>() -> usize'")) {
        std::cerr << "recognized Impl::* with wrong signature should fail\n" << out_core_bad_sig;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_non_fn, out_core_non_fn] = run_tool(core_non_fn, "core");
    if (rc_core_non_fn == 0 ||
        !contains(out_core_non_fn, "recognized $![Impl::*] binding is only allowed on function declarations")) {
        std::cerr << "recognized Impl::* on non-function should fail\n" << out_core_non_fn;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ordinary_bodyless, out_ordinary_bodyless] = run_tool(ordinary_bodyless, "app");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_ordinary_bodyless == 0) {
        std::cerr << "ordinary bodyless def must remain a syntax error\n" << out_ordinary_bodyless;
        return false;
    }
    return true;
}

bool test_impl_binding_library_owned_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-impl-binding-library-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "app";
    const std::string src =
        "static mut touched: i32 = 0i32;\n"
        "\n"
        "$![Impl::SpinLoop]\n"
        "export def spin_loop() -> void {\n"
        "  touched = 1i32;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  spin_loop();\n"
        "  return touched;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write Impl::* runtime sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for Impl::* runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "library-owned Impl::* runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "library-owned Impl::* runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:1")) {
        std::cerr << "library-owned Impl::* must run ordinary body, not compiler intrinsic\n" << out_run;
        return false;
    }
    return true;
}

bool test_auto_core_export_index_loaded_for_non_core_bundle() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-auto-core";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "sysroot/.cache/exports", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto core_index = temp_root / "sysroot/.cache/exports/core.exports.json";
    const auto main_pr = temp_root / "main.pr";

    const std::string core_index_src =
        "{\n"
        "  \"version\": 1,\n"
        "  \"bundle\": \"core\",\n"
        "  \"exports\": [\n"
        "    {\n"
        "      \"kind\": \"act\",\n"
        "      \"path\": \"i32\",\n"
        "      \"link_name\": \"\",\n"
        "      \"module_head\": \"num\",\n"
        "      \"decl_dir\": \"sysroot/core/num\",\n"
        "      \"type_repr\": \"i32\",\n"
        "      \"inst_payload\": \"\",\n"
        "      \"decl_span\": {\"file\": \"sysroot/core/num/i32.pr\", \"line\": 1, \"col\": 1},\n"
        "      \"is_export\": true\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fn\",\n"
        "      \"path\": \"i32::size\",\n"
        "      \"link_name\": \"core_i32_size\",\n"
        "      \"module_head\": \"num\",\n"
        "      \"decl_dir\": \"sysroot/core/num\",\n"
        "      \"type_repr\": \"def(i32) -> i32\",\n"
        "      \"inst_payload\": \"parus_builtin_acts|owner=i32|member=size|self=1\",\n"
        "      \"decl_span\": {\"file\": \"sysroot/core/num/i32.pr\", \"line\": 3, \"col\": 3},\n"
        "      \"is_export\": true\n"
        "    }\n"
        "  ]\n"
        "}\n";

    const std::string main_src =
        "def main() -> i32 {\n"
        "  let x: i32 = 123i32;\n"
        "  return x.size();\n"
        "}\n";

    if (!write_text(core_index, core_index_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write auto core index test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string compile_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main.o").string() + "\"";
    auto [rc_compile, out_compile] = run_capture(compile_cmd);
    if (rc_compile != 0) {
        std::cerr << "non-core bundle compile should auto-load core export-index\n" << out_compile;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string stale_core_index_src =
        "{\n"
        "  \"version\": 7,\n"
        "  \"bundle\": \"core\",\n"
        "  \"exports\": []\n"
        "}\n";
    if (!write_text(core_index, stale_core_index_src)) {
        std::cerr << "failed to rewrite stale auto core export-index fixture\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_stale, out_stale] = run_capture(compile_cmd);
    if (rc_stale == 0 || !contains(out_stale, "unsupported export-index version (expected v1)")) {
        std::cerr << "stale v7 core export-index must be rejected\n" << out_stale;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    if (!write_text(core_index, core_index_src)) {
        std::cerr << "failed to restore v1 auto core export-index fixture\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string compile_no_core_cmd =
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main.no_core.o").string() + "\" -fno-core";
    auto [rc_no_core, out_no_core] = run_capture(compile_no_core_cmd);
    if (rc_no_core == 0) {
        std::cerr << "-fno-core must disable implicit core export-index injection\n" << out_no_core;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto missing_root = temp_root / "missing";
    std::filesystem::create_directories(missing_root / "core", ec);
    const std::string compile_missing_idx_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\"" +
        " --sysroot \"" + missing_root.string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main.missing.o").string() + "\"";
    auto [rc_missing_idx, out_missing_idx] = run_capture(compile_missing_idx_cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc_missing_idx == 0) {
        std::cerr << "missing core export-index should not inject builtin acts implicitly\n" << out_missing_idx;
        return false;
    }
    if (contains(out_missing_idx, "ExportIndexMissing")) {
        std::cerr << "missing core export-index should not be a hard error anymore\n" << out_missing_idx;
        return false;
    }
    return true;
}

bool test_core_ext_scaffold_and_auto_injection() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-core-ext";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "sysroot/.cache/exports", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const std::filesystem::path repo_root = std::filesystem::path(PARUS_MAIN_PR).parent_path();
    const std::filesystem::path core_root = repo_root / "sysroot/core";
    const std::filesystem::path ext_types = core_root / "ext/types.pr";
    const std::filesystem::path ext_cstr = core_root / "ext/cstr.pr";
    const std::filesystem::path ext_errors = core_root / "ext/errors.pr";
    if (!std::filesystem::exists(ext_types, ec) ||
        !std::filesystem::exists(ext_cstr, ec) ||
        !std::filesystem::exists(ext_errors, ec)) {
        std::cerr << "core::ext scaffold source files are missing\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    std::filesystem::create_directories(temp_root / "sysroot/core/ext", ec);
    if (ec ||
        !std::filesystem::copy_file(ext_types, temp_root / "sysroot/core/ext/types.pr",
                                    std::filesystem::copy_options::overwrite_existing, ec) ||
        ec ||
        !std::filesystem::copy_file(ext_errors, temp_root / "sysroot/core/ext/errors.pr",
                                    std::filesystem::copy_options::overwrite_existing, ec) ||
        ec ||
        !std::filesystem::copy_file(ext_cstr, temp_root / "sysroot/core/ext/cstr.pr",
                                    std::filesystem::copy_options::overwrite_existing, ec) ||
        ec) {
        std::cerr << "failed to stage core::ext macro/type sources in temp sysroot\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto core_index = temp_root / "sysroot/.cache/exports/core.exports.json";
    std::string out_emit{};
    if (!emit_bundle_export_index_from_fragments(
            bin,
            "core",
            core_root,
            {
                {ext_types, "ext", {}},
                {ext_cstr, "ext", {}},
                {ext_errors, "ext", {}},
            },
            core_index,
            out_emit)) {
        std::cerr << "failed to emit core export-index from core::ext scaffold\n" << out_emit;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string core_index_text = read_text(core_index);
    if (!contains(core_index_text, "\"path\":\"c_int\"")) {
        std::cerr << "core export-index must include core::ext::c_int\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto app_main = temp_root / "main.pr";
    const std::string app_src =
        "import ext as ext;\n"
        "\n"
        "def keep(x: ext::c_int, sz: ext::c_size, ssz: ext::c_ssize, diff: ext::c_ptrdiff) -> i32 {\n"
        "  return 0i32;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(app_main, app_src)) {
        std::cerr << "failed to write core::ext app source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string compile_with_core_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + app_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main.o").string() + "\"";
    auto [rc_with_core, out_with_core] = run_capture(compile_with_core_cmd);
    if (rc_with_core != 0) {
        std::cerr << "non-core bundle must resolve core::ext via auto-injected core index\n" << out_with_core;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto hidden_fn_main = temp_root / "main_hidden_fn.pr";
    const std::string hidden_fn_src =
        "import ext as ext;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set x = ext::make(\"x\", 1usize);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(hidden_fn_main, hidden_fn_src)) {
        std::cerr << "failed to write hidden core::ext fn visibility source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string hidden_fn_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + hidden_fn_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main_hidden_fn.o").string() + "\"";
    auto [rc_hidden_fn, out_hidden_fn] = run_capture(hidden_fn_cmd);
    if (rc_hidden_fn == 0 ||
        (!contains(out_hidden_fn, "SymbolNotExportedFileScope") &&
         !contains(out_hidden_fn, "UndefinedName"))) {
        std::cerr << "non-export core function must not be visible externally\n" << out_hidden_fn;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto hidden_macro_main = temp_root / "main_hidden_macro.pr";
    const std::string hidden_macro_src =
        "def main() -> i32 {\n"
        "  set x = $__core_ext_identity(1i32);\n"
        "  return x;\n"
        "}\n";
    if (!write_text(hidden_macro_main, hidden_macro_src)) {
        std::cerr << "failed to write hidden core::ext macro visibility source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string hidden_macro_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + hidden_macro_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main_hidden_macro.o").string() + "\"";
    auto [rc_hidden_macro, out_hidden_macro] = run_capture(hidden_macro_cmd);
    if (rc_hidden_macro == 0 || !contains(out_hidden_macro, "MacroNoMatch")) {
        std::cerr << "non-export core macro must not be visible externally\n" << out_hidden_macro;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string compile_no_core_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + app_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main.no_core.o").string() + "\" -fno-core";
    auto [rc_no_core, out_no_core] = run_capture(compile_no_core_cmd);
    (void)rc_no_core;
    (void)out_no_core;

    const auto va_ok_main = temp_root / "main_va_ok.pr";
    const std::string va_ok_src =
        "import ext as ext;\n"
        "\n"
        "extern \"C\" def vprintf(fmt: *const ext::c_char, ap: ext::vaList) -> ext::c_int;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(va_ok_main, va_ok_src)) {
        std::cerr << "failed to write core::ext vaList allowed-usage test source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string va_ok_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + va_ok_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main_va_ok.o").string() + "\"";
    auto [rc_va_ok, out_va_ok] = run_capture(va_ok_cmd);
    if (rc_va_ok != 0) {
        std::cerr << "core::ext vaList should be allowed on C ABI parameter signatures\n" << out_va_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto va_fail_main = temp_root / "main_va_fail.pr";
    const std::string va_fail_src =
        "import ext as ext;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let x: ext::vaList;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(va_fail_main, va_fail_src)) {
        std::cerr << "failed to write core::ext vaList forbidden-usage test source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string va_fail_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + va_fail_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main_va_fail.o").string() + "\"";
    auto [rc_va_fail, out_va_fail] = run_capture(va_fail_cmd);
    if (rc_va_fail == 0 || !contains(out_va_fail, "vaList may only appear in C ABI function parameter types")) {
        std::cerr << "core::ext vaList local binding must be rejected\n" << out_va_fail;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto cimport_main = temp_root / "main_stdio.pr";
    const std::string cimport_src =
        "import \"stdio.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  manual[abi] {\n"
        "    c::printf(\"%s\\n\", c\"ok\");\n"
        "    c::printf(\"%d\\n\", 5);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(cimport_main, cimport_src)) {
        std::cerr << "failed to write cimport/core::ext integration source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cimport_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + cimport_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main_stdio.o").string() + "\"";
    auto [rc_cimport, out_cimport] = run_capture(cimport_cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc_cimport != 0) {
        std::cerr << "stdio + core::ext interop syntax/type check must pass\n" << out_cimport;
        return false;
    }
    return true;
}

bool test_core_seed_export_index_and_auto_injection() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-core-seed";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "sysroot/.cache/exports", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const std::filesystem::path repo_root = std::filesystem::path(PARUS_MAIN_PR).parent_path();
    const std::filesystem::path core_root = repo_root / "sysroot/core";
    const std::filesystem::path ext_types = core_root / "ext/types.pr";
    const std::filesystem::path ext_cstr = core_root / "ext/cstr.pr";
    const std::filesystem::path ext_errors = core_root / "ext/errors.pr";
    const std::filesystem::path cmp_ordering = core_root / "cmp/ordering.pr";
    const std::filesystem::path bool_acts = core_root / "bool/bool.pr";
    const std::filesystem::path convert_mod = core_root / "convert/convert.pr";
    const std::filesystem::path num_int = core_root / "num/int.pr";
    const std::filesystem::path num_float = core_root / "num/float.pr";
    const std::filesystem::path constraints_proto = core_root / "constraints/proto.pr";
    const std::filesystem::path constraints_inst = core_root / "constraints/inst.pr";
    const std::filesystem::path char_ascii = core_root / "char/ascii.pr";
    const std::filesystem::path text_view = core_root / "text/text.pr";
    const std::filesystem::path mem_mod = core_root / "mem/mem.pr";
    const std::filesystem::path hint_mod = core_root / "hint/hint.pr";
    const std::filesystem::path iter_mod = core_root / "iter/iter.pr";
    const std::filesystem::path range_mod = core_root / "range/range.pr";
    const std::filesystem::path slice_mod = core_root / "slice/slice.pr";
    if (!std::filesystem::exists(ext_types, ec) ||
        !std::filesystem::exists(ext_cstr, ec) ||
        !std::filesystem::exists(ext_errors, ec) ||
        !std::filesystem::exists(cmp_ordering, ec) ||
        !std::filesystem::exists(bool_acts, ec) ||
        !std::filesystem::exists(convert_mod, ec) ||
        !std::filesystem::exists(num_int, ec) ||
        !std::filesystem::exists(num_float, ec) ||
        !std::filesystem::exists(constraints_proto, ec) ||
        !std::filesystem::exists(constraints_inst, ec) ||
        !std::filesystem::exists(char_ascii, ec) ||
        !std::filesystem::exists(text_view, ec) ||
        !std::filesystem::exists(mem_mod, ec) ||
        !std::filesystem::exists(hint_mod, ec) ||
        !std::filesystem::exists(iter_mod, ec) ||
        !std::filesystem::exists(range_mod, ec) ||
        !std::filesystem::exists(slice_mod, ec)) {
        std::cerr << "core seed source files are missing\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto core_index = temp_root / "sysroot/.cache/exports/core.exports.json";
    std::string out_emit{};
    if (!emit_bundle_export_index_from_fragments(
            bin,
            "core",
            core_root,
            {
                {ext_types, "ext", {}},
                {ext_cstr, "ext", {}},
                {ext_errors, "ext", {}},
                {cmp_ordering, "cmp", {"constraints"}},
                {bool_acts, "bool", {"cmp"}},
                {convert_mod, "convert", {}},
                {num_int, "num", {"cmp"}},
                {num_float, "num", {"cmp"}},
                {constraints_proto, "constraints", {}},
                {constraints_inst, "constraints", {}},
                {char_ascii, "char", {"cmp"}},
                {text_view, "text", {}},
                {mem_mod, "mem", {}},
                {hint_mod, "hint", {}},
                {iter_mod, "iter", {"constraints"}},
                {range_mod, "range", {"constraints", "iter"}},
                {slice_mod, "slice", {"constraints"}},
            },
            core_index,
            out_emit)) {
        std::cerr << "failed to emit core export-index from core seed\n" << out_emit;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string core_index_text = read_text(core_index);
    if (!contains(core_index_text, "\"path\":\"Ordering\"")) {
        std::cerr << "core export-index must include core::cmp::Ordering\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "parus_builtin_acts|owner=bool|member=cmp|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=bool|member=is_true|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=bool|member=is_false|self=1")) {
        std::cerr << "core export-index must include bool builtin acts payloads\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "parus_builtin_acts|owner=i32|member=min|self=1")) {
        std::cerr << "core export-index must include i32.min builtin acts payload\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "parus_builtin_acts|owner=i32|member=is_even|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=u32|member=pow|self=1")) {
        std::cerr << "core export-index must include integer helper builtin acts payloads\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "parus_builtin_acts|owner=f32|member=partial_cmp|self=1")) {
        std::cerr << "core export-index must include f32.partial_cmp builtin acts payload\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "parus_builtin_acts|owner=char|member=is_ascii_hexdigit|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=char|member=to_digit|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=char|member=len_utf8|self=1")) {
        std::cerr << "core export-index must include char builtin acts payloads\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "parus_builtin_acts|owner=text|member=is_empty|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=text|member=len_bytes|self=1") ||
        !contains(core_index_text, "parus_builtin_acts|owner=text|member=as_ptr|self=1")) {
        std::cerr << "core export-index must include text builtin acts payloads\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "\"path\":\"size_of\"") ||
        !contains(core_index_text, "\"path\":\"align_of\"") ||
        !contains(core_index_text, "\"path\":\"swap\"") ||
        !contains(core_index_text, "\"path\":\"replace\"") ||
        !contains(core_index_text, "\"path\":\"take\"") ||
        !contains(core_index_text, "\"path\":\"unreachable\"") ||
        !contains(core_index_text, "\"path\":\"spin_loop\"") ||
        !contains(core_index_text, "parus_impl_binding|key=Impl::SizeOf|mode=compiler") ||
        !contains(core_index_text, "parus_impl_binding|key=Impl::AlignOf|mode=compiler") ||
        !contains(core_index_text, "parus_impl_binding|key=Impl::SpinLoop|mode=compiler")) {
        std::cerr << "core export-index must include mem/hint helper symbols and impl bindings\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (!contains(core_index_text, "\"path\":\"Range\"") ||
        !contains(core_index_text, "\"path\":\"Into\"") ||
        !contains(core_index_text, "\"path\":\"into\"") ||
        !contains(core_index_text, "\"path\":\"RangeInclusive\"") ||
        !contains(core_index_text, "\"path\":\"range\"") ||
        !contains(core_index_text, "\"path\":\"range_inclusive\"") ||
        !contains(core_index_text, "\"path\":\"Iterator\"") ||
        !contains(core_index_text, "\"path\":\"IterSource\"") ||
        !contains(core_index_text, "\"path\":\"IterSourceMut\"") ||
        !contains(core_index_text, "\"path\":\"IterSourceMove\"") ||
        !contains(core_index_text, "\"path\":\"RangeIter\"") ||
        !contains(core_index_text, "\"path\":\"RangeInclusiveIter\"") ||
        !contains(core_index_text, "\"path\":\"step_next\"") ||
        !contains(core_index_text, "\"path\":\"Comparable\"") ||
        !contains(core_index_text, "\"path\":\"BinaryInteger\"") ||
        !contains(core_index_text, "\"path\":\"SignedInteger\"") ||
        !contains(core_index_text, "\"path\":\"UnsignedInteger\"") ||
        !contains(core_index_text, "\"path\":\"Step\"") ||
        !contains(core_index_text, "\"path\":\"BinaryFloatingPoint\"") ||
        !contains(core_index_text, "parus_impl_binding|key=Impl::StepNext|mode=compiler") ||
        !contains(core_index_text, "impl_proto=core::iter::IterSourceMove") ||
        !contains(core_index_text, "impl_proto=core::iter::Iterator") ||
        !contains(core_index_text, "assoc_type=Item%2C") ||
        !contains(core_index_text, "assoc_type=Iter%2C") ||
        !contains(core_index_text, "gconstraint=proto,T,core::constraints::Step") ||
        contains(core_index_text, "gconstraint=proto,T,constraints::Step") ||
        contains(core_index_text, "\"path\":\"Sequence\"") ||
        contains(core_index_text, "\"path\":\"AsRef\"") ||
        contains(core_index_text, "\"path\":\"AsMut\"") ||
        contains(core_index_text, "\"path\":\"SignedInt\"") ||
        contains(core_index_text, "\"path\":\"UnsignedInt\"") ||
        contains(core_index_text, "\"path\":\"Integral\"") ||
        contains(core_index_text, "\"path\":\"FloatLike\"") ||
        contains(core_index_text, "\"path\":\"RangeBound\"")) {
        std::cerr << "core export-index must include range types, constructors, and builtin proto declarations\n" << core_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto app_main = temp_root / "main.pr";
    const std::string app_src =
        "import cmp as cmp;\n"
        "import constraints as constraints;\n"
        "import convert as convert;\n"
        "import ext as ext;\n"
        "import hint as hint;\n"
        "import iter as iter;\n"
        "import mem as mem;\n"
        "import range as range;\n"
        "import slice as slice;\n"
        "\n"
        "struct ConvertBox : convert::Into<i32> {\n"
        "  value: i32;\n"
        "};\n"
        "acts for ConvertBox {\n"
        "  def into(self move) -> i32 { return self.value; }\n"
        "};\n"
        "\n"
        "def cold(flag: bool) -> i32 {\n"
        "  if (flag) { return 7i32; }\n"
        "  hint::unreachable();\n"
        "}\n"
        "def main() -> i32 {\n"
        "  let truth: bool = true;\n"
        "  let bool_ord: core::cmp::Ordering = truth.cmp(false);\n"
        "  let bool_true: bool = truth.is_true();\n"
        "  let bool_false: bool = false.is_false();\n"
        "  let lhs: i32 = 1i32;\n"
        "  let rhs: i32 = 2i32;\n"
        "  let a: i32 = lhs.min(rhs);\n"
        "  let even_ok: bool = 4i32.is_even();\n"
        "  let odd_ok: bool = 5i32.is_odd();\n"
        "  let pow_ok: u32 = 3u32.pow(4u32);\n"
        "  let raw: f32 = 1.0f32;\n"
        "  let b: core::cmp::Ordering? = raw.partial_cmp(2.0f32);\n"
        "  let b0: core::cmp::Ordering = b ?? cmp::ordering_equal();\n"
        "  let raw_nan: bool = raw.is_nan();\n"
        "  let c: char = 'f';\n"
        "  let hex: bool = c.is_ascii_hexdigit();\n"
        "  let digit: u32? = c.to_digit(16u32);\n"
        "  let utf1: usize = 'A'.len_utf8();\n"
        "  let utf2: usize = 'é'.len_utf8();\n"
        "  let utf3: usize = '한'.len_utf8();\n"
        "  let utf4: usize = '😀'.len_utf8();\n"
        "  let s: text = \"abc\";\n"
        "  let empty: text = \"\";\n"
        "  let s_len: usize = s.len_bytes();\n"
        "  let s_empty: bool = empty.is_empty();\n"
        "  let s_ptr: *const u8 = s.as_ptr();\n"
        "  let convert_i: i32 = convert::into<i32, ConvertBox>(ConvertBox{ value: 5i32 });\n"
        "  let arr: i32[4] = [1i32, 2i32, 3i32, 4i32];\n"
        "  let xs: i32[] = arr;\n"
        "  let empty_xs: i32[] = arr[0i32..0i32];\n"
        "  let prefix: i32[] = arr[0i32..2i32];\n"
        "  let middle: i32[] = arr[1i32..3i32];\n"
        "  let suffix: i32[] = arr[2i32..4i32];\n"
        "  let xs_len: usize = slice::len(xs);\n"
        "  let xs_empty: bool = slice::is_empty(empty_xs);\n"
        "  let xs_ptr: *const i32 = slice::as_ptr(xs);\n"
        "  let mid_slice: i32[] = slice::subslice(xs, 1usize, 3usize);\n"
        "  let front: i32[] = slice::take_front(xs, 2usize);\n"
        "  let dropf: i32[] = slice::drop_front(xs, 2usize);\n"
        "  let back: i32[] = slice::take_back(xs, 2usize);\n"
        "  let dropb: i32[] = slice::drop_back(xs, 2usize);\n"
        "  let full: i32[] = slice::take_front(xs, 99usize);\n"
        "  let none: i32[] = slice::drop_front(xs, 99usize);\n"
        "  let slice_eq: bool = slice::equal(front, prefix);\n"
        "  let slice_sub_eq: bool = slice::equal(mid_slice, middle);\n"
        "  let slice_dropf_eq: bool = slice::equal(dropf, suffix);\n"
        "  let slice_back_eq: bool = slice::equal(back, suffix);\n"
        "  let slice_dropb_eq: bool = slice::equal(dropb, prefix);\n"
        "  let slice_full_eq: bool = slice::equal(full, xs);\n"
        "  let slice_starts: bool = slice::starts_with(xs, prefix);\n"
        "  let slice_ends: bool = slice::ends_with(xs, suffix);\n"
        "  let cmp_min_v: i32 = cmp::min(3i32, 7i32);\n"
        "  let cmp_max_v: char = cmp::max('a', 'z');\n"
        "  let cmp_clamp_v: i32 = cmp::clamp(5i32, 1i32, 4i32);\n"
        "  let ord_less: bool = cmp::ordering_less().is_less();\n"
        "  let ord_eq: bool = cmp::ordering_equal().is_equal();\n"
        "  let ord_gt: bool = cmp::ordering_greater().is_greater();\n"
        "  let ord_rev: core::cmp::Ordering = cmp::ordering_greater().reverse();\n"
        "  let sz_i32: usize = mem::size_of<i32>();\n"
        "  let sz_text: usize = mem::size_of<text>();\n"
        "  let al_i32: usize = mem::align_of<i32>();\n"
        "  let al_text: usize = mem::align_of<text>();\n"
        "  set mut swap_a = 1i32;\n"
        "  set mut swap_b = 2i32;\n"
        "  mem::swap<i32>(&mut swap_a, &mut swap_b);\n"
        "  set mut repl = \"abc\";\n"
        "  let repl_old: text = mem::replace<text>(&mut repl, \"x\");\n"
        "  hint::spin_loop();\n"
        "  let cstr: core::ext::CStr = ext::from_raw_parts(c\"Hello\", 6usize);\n"
        "  let bridged: text = ext::to_text(cstr);\n"
        "  let bridged_len: usize = bridged.len_bytes();\n"
        "  let rx: core::range::Range<i32> = range::range(1i32, 4i32);\n"
        "  let ri: core::range::RangeInclusive<u32> = range::range_inclusive(1u32, 4u32);\n"
        "  let rc: core::range::Range<char> = range::range('a', 'z');\n"
        "  let rci: core::range::RangeInclusive<char> = range::range_inclusive('k', 'k');\n"
        "  let rx_empty: bool = rx.is_empty();\n"
        "  let rx_has_mid: bool = rx.contains(2i32);\n"
        "  let rx_has_hi: bool = rx.contains(4i32);\n"
        "  let rx_contains_range: bool = rx.contains_range(range::range(2i32, 3i32));\n"
        "  let rx_intersects_tail: bool = rx.intersects(range::range(4i32, 8i32));\n"
        "  let ri_has_hi: bool = ri.contains(4u32);\n"
        "  let ri_empty: bool = range::range_inclusive(5i32, 4i32).is_empty();\n"
        "  let rc_has_mid: bool = rc.contains('m');\n"
        "  let rc_has_hi: bool = rc.contains('z');\n"
        "  let rci_single: bool = rci.is_singleton();\n"
        "  let rci_intersects: bool = rci.intersects(range::range_inclusive('k', 'm'));\n"
        "  set mut it = rx.into_iter();\n"
        "  set mut it1 = 0i32;\n"
        "  set mut it2 = 0i32;\n"
        "  set mut it3 = 0i32;\n"
        "  set mut it4 = 0i32;\n"
        "  let ok_it1: bool = it.next(&mut it1);\n"
        "  let ok_it2: bool = it.next(&mut it2);\n"
        "  let ok_it3: bool = it.next(&mut it3);\n"
        "  let ok_it4: bool = it.next(&mut it4);\n"
        "  set mut single_char = rci.into_iter();\n"
        "  set mut rc1 = 'x';\n"
        "  set mut rc2 = 'x';\n"
        "  let ok_rc1: bool = single_char.next(&mut rc1);\n"
        "  let ok_rc2: bool = single_char.next(&mut rc2);\n"
        "  set mut loop_sum = 0i32;\n"
        "  loop (x in range::range(1i32, 4i32)) {\n"
        "    set loop_sum = loop_sum + x;\n"
        "  }\n"
        "  set mut loop_inc_sum = 0i32;\n"
        "  loop (x in range::range_inclusive(1i32, 3i32)) {\n"
        "    set loop_inc_sum = loop_inc_sum + x;\n"
        "  }\n"
        "  let ord: core::cmp::Ordering = a.cmp(0i32);\n"
        "  set mut bool_ok = false;\n"
        "  switch (bool_ord) {\n"
        "  case core::cmp::Ordering::Greater: { bool_ok = true; }\n"
        "  default: {}\n"
        "  }\n"
        "  set mut ord_ok = false;\n"
        "  switch (ord) {\n"
        "  case core::cmp::Ordering::Greater: { ord_ok = true; }\n"
        "  default: {}\n"
        "  }\n"
        "  set mut partial_ok = false;\n"
        "  switch (b0) {\n"
        "  case core::cmp::Ordering::Less: { partial_ok = true; }\n"
        "  default: {}\n"
        "  }\n"
        "  set mut ord_rev_ok = false;\n"
        "  switch (ord_rev) {\n"
        "  case core::cmp::Ordering::Less: { ord_rev_ok = true; }\n"
        "  default: {}\n"
        "  }\n"
        "  if (bool_ok and ord_ok and partial_ok and ord_less and ord_eq and ord_gt and ord_rev_ok and"
        " even_ok and odd_ok and pow_ok == 81u32 and hex and bool_true and bool_false and not raw_nan and"
        " utf1 == 1usize and utf2 == 2usize and utf3 == 3usize and utf4 == 4usize and"
        " s_len == 3usize and s_empty and convert_i == 5i32 and (digit ?? 0u32) == 15u32 and bridged_len == 5usize and"
        " xs_len == 4usize and xs_empty and slice_eq and slice_sub_eq and"
        " slice_dropf_eq and slice_back_eq and slice_dropb_eq and slice_full_eq and slice::is_empty(none) and"
        " slice_starts and slice_ends and cmp_min_v == 3i32 and cmp_max_v == 'z' and cmp_clamp_v == 4i32 and"
        " not rx_empty and rx_has_mid and not rx_has_hi and rx_contains_range and not rx_intersects_tail and"
        " ri_has_hi and ri_empty and rc_has_mid and not rc_has_hi and rci_single and rci_intersects and"
        " ok_it1 and ok_it2 and ok_it3 and not ok_it4 and"
        " it1 == 1i32 and it2 == 2i32 and it3 == 3i32 and"
        " ok_rc1 and rc1 == 'k' and not ok_rc2 and"
        " loop_sum == 6i32 and loop_inc_sum == 6i32 and"
        " sz_i32 == 4usize and sz_text == 16usize and al_i32 == 4usize and al_text == 8usize and"
        " swap_a == 2i32 and swap_b == 1i32 and repl_old.len_bytes() == 3usize and repl.len_bytes() == 1usize and"
        " cold(true) == 7i32) {\n"
        "    return 42i32;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(app_main, app_src)) {
        std::cerr << "failed to write core seed app source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string compile_with_core_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + app_main.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main.o").string() + "\"";
    auto [rc_with_core, out_with_core] = run_capture(compile_with_core_cmd);
    const auto app_fail = temp_root / "main_float_cmp_fail.pr";
    const std::string fail_src =
        "def main() -> i32 {\n"
        "  let x: f32 = 1.0f32;\n"
        "  let ord: core::cmp::Ordering = x.cmp(2.0f32);\n"
        "  switch (ord) {\n"
        "  case core::cmp::Ordering::Less: { return 42i32; }\n"
        "  default: {}\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(app_fail, fail_src)) {
        std::cerr << "failed to write float cmp rejection sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string compile_fail_cmd =
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + app_fail.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " --emit-object -o \"" + (temp_root / "main_fail.o").string() + "\"";
    auto [rc_fail, out_fail] = run_capture(compile_fail_cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc_with_core != 0) {
        std::cerr << "non-core bundle must resolve core seed acts via auto-injected core index\n" << out_with_core;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "float.cmp must be rejected after core float acts redesign\n" << out_fail;
        return false;
    }
    return true;
}

bool test_core_seed_runtime_smoke() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-core-seed-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const std::filesystem::path repo_root = std::filesystem::path(PARUS_MAIN_PR).parent_path();
    const std::filesystem::path core_root = repo_root / "sysroot/core";
    const std::array<std::filesystem::path, 17> core_seed_sources = {
        core_root / "ext/types.pr",
        core_root / "ext/cstr.pr",
        core_root / "ext/errors.pr",
        core_root / "cmp/ordering.pr",
        core_root / "bool/bool.pr",
        core_root / "convert/convert.pr",
        core_root / "num/int.pr",
        core_root / "num/float.pr",
        core_root / "constraints/proto.pr",
        core_root / "constraints/inst.pr",
        core_root / "char/ascii.pr",
        core_root / "text/text.pr",
        core_root / "mem/mem.pr",
        core_root / "hint/hint.pr",
        core_root / "iter/iter.pr",
        core_root / "range/range.pr",
        core_root / "slice/slice.pr",
    };

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "import cmp as cmp;\n"
        "import constraints as constraints;\n"
        "import convert as convert;\n"
        "import ext as ext;\n"
        "import hint as hint;\n"
        "import iter as iter;\n"
        "import mem as mem;\n"
        "import range as range;\n"
        "import slice as slice;\n"
        "\n"
        "struct ConvertBox : convert::Into<i32> {\n"
        "  value: i32;\n"
        "};\n"
        "acts for ConvertBox {\n"
        "  def into(self move) -> i32 { return self.value; }\n"
        "};\n"
        "\n"
        "def cold(flag: bool) -> i32 {\n"
        "  if (flag) { return 7i32; }\n"
        "  hint::unreachable();\n"
        "}\n"
        "def main() -> i32 {\n"
        "  let bo: core::cmp::Ordering = true.cmp(false);\n"
        "  let bt: bool = true.is_true();\n"
        "  let bf: bool = false.is_false();\n"
        "  let neg: i32 = -3i32;\n"
        "  let mag: i32 = neg.abs();\n"
        "  let even_ok: bool = 4i32.is_even();\n"
        "  let odd_ok: bool = 5i32.is_odd();\n"
        "  let pow_i: i32 = 3i32.pow(3u32);\n"
        "  let pow_u: u32 = 3u32.pow(4u32);\n"
        "  let pow8: bool = 8u32.is_power_of_two();\n"
        "  let c: char = 'a';\n"
        "  let up: char = c.to_ascii_upper();\n"
        "  let digit_hex: u32? = 'F'.to_digit(16u32);\n"
        "  let digit_oob: u32? = '8'.to_digit(8u32);\n"
        "  let utf1: usize = 'A'.len_utf8();\n"
        "  let utf2: usize = 'é'.len_utf8();\n"
        "  let utf3: usize = '한'.len_utf8();\n"
        "  let utf4: usize = '😀'.len_utf8();\n"
        "  let s: text = \"abc\";\n"
        "  let empty: text = \"\";\n"
        "  let s_len: usize = s.len_bytes();\n"
        "  let s_empty: bool = empty.is_empty();\n"
        "  let s_ptr: *const u8 = s.as_ptr();\n"
        "  let convert_i: i32 = convert::into<i32, ConvertBox>(ConvertBox{ value: 5i32 });\n"
        "  let arr: i32[4] = [1i32, 2i32, 3i32, 4i32];\n"
        "  let xs: i32[] = arr;\n"
        "  let empty_xs: i32[] = arr[0i32..0i32];\n"
        "  let prefix: i32[] = arr[0i32..2i32];\n"
        "  let middle: i32[] = arr[1i32..3i32];\n"
        "  let suffix: i32[] = arr[2i32..4i32];\n"
        "  let xs_len: usize = slice::len(xs);\n"
        "  let xs_empty: bool = slice::is_empty(empty_xs);\n"
        "  let xs_ptr: *const i32 = slice::as_ptr(xs);\n"
        "  let mid_slice: i32[] = slice::subslice(xs, 1usize, 3usize);\n"
        "  let front: i32[] = slice::take_front(xs, 2usize);\n"
        "  let dropf: i32[] = slice::drop_front(xs, 2usize);\n"
        "  let back: i32[] = slice::take_back(xs, 2usize);\n"
        "  let dropb: i32[] = slice::drop_back(xs, 2usize);\n"
        "  let full: i32[] = slice::take_front(xs, 99usize);\n"
        "  let none: i32[] = slice::drop_front(xs, 99usize);\n"
        "  let slice_eq: bool = slice::equal(front, prefix);\n"
        "  let slice_sub_eq: bool = slice::equal(mid_slice, middle);\n"
        "  let slice_dropf_eq: bool = slice::equal(dropf, suffix);\n"
        "  let slice_back_eq: bool = slice::equal(back, suffix);\n"
        "  let slice_dropb_eq: bool = slice::equal(dropb, prefix);\n"
        "  let slice_full_eq: bool = slice::equal(full, xs);\n"
        "  let slice_starts: bool = slice::starts_with(xs, prefix);\n"
        "  let slice_ends: bool = slice::ends_with(xs, suffix);\n"
        "  let cmp_min_v: i32 = cmp::min(3i32, 7i32);\n"
        "  let cmp_max_v: char = cmp::max('a', 'z');\n"
        "  let cmp_clamp_v: i32 = cmp::clamp(5i32, 1i32, 4i32);\n"
        "  let ord_less: bool = cmp::ordering_less().is_less();\n"
        "  let ord_eq: bool = cmp::ordering_equal().is_equal();\n"
        "  let ord_gt: bool = cmp::ordering_greater().is_greater();\n"
        "  let ord_rev: core::cmp::Ordering = cmp::ordering_greater().reverse();\n"
        "  let sz_i32: usize = mem::size_of<i32>();\n"
        "  let sz_text: usize = mem::size_of<text>();\n"
        "  let sz_arr: usize = mem::size_of<i32[4]>();\n"
        "  let al_i32: usize = mem::align_of<i32>();\n"
        "  let al_text: usize = mem::align_of<text>();\n"
        "  set mut swap_a = 1i32;\n"
        "  set mut swap_b = 2i32;\n"
        "  mem::swap<i32>(&mut swap_a, &mut swap_b);\n"
        "  set mut repl = \"abc\";\n"
        "  let repl_old: text = mem::replace<text>(&mut repl, \"x\");\n"
        "  hint::spin_loop();\n"
        "  let cs: core::ext::CStr = ext::from_raw_parts(c\"Hello\", 6usize);\n"
        "  let tv: text = ext::to_text(cs);\n"
        "  let tv_len: usize = tv.len_bytes();\n"
        "  let rx: core::range::Range<i32> = range::range(1i32, 4i32);\n"
        "  let ri: core::range::RangeInclusive<u32> = range::range_inclusive(1u32, 4u32);\n"
        "  let rc: core::range::Range<char> = range::range('a', 'z');\n"
        "  let rci: core::range::RangeInclusive<char> = range::range_inclusive('k', 'k');\n"
        "  let rx_empty: bool = rx.is_empty();\n"
        "  let rx_has_mid: bool = rx.contains(2i32);\n"
        "  let rx_has_hi: bool = rx.contains(4i32);\n"
        "  let rx_contains_range: bool = rx.contains_range(range::range(2i32, 3i32));\n"
        "  let rx_intersects_tail: bool = rx.intersects(range::range(4i32, 8i32));\n"
        "  let ri_has_hi: bool = ri.contains(4u32);\n"
        "  let ri_empty: bool = range::range_inclusive(5i32, 4i32).is_empty();\n"
        "  let rc_has_mid: bool = rc.contains('m');\n"
        "  let rc_has_hi: bool = rc.contains('z');\n"
        "  let rci_single: bool = rci.is_singleton();\n"
        "  let rci_intersects: bool = rci.intersects(range::range_inclusive('k', 'm'));\n"
        "  set mut it = rx.into_iter();\n"
        "  set mut it1 = 0i32;\n"
        "  set mut it2 = 0i32;\n"
        "  set mut it3 = 0i32;\n"
        "  set mut it4 = 0i32;\n"
        "  let ok_it1: bool = it.next(&mut it1);\n"
        "  let ok_it2: bool = it.next(&mut it2);\n"
        "  let ok_it3: bool = it.next(&mut it3);\n"
        "  let ok_it4: bool = it.next(&mut it4);\n"
        "  set mut char_it = rci.into_iter();\n"
        "  set mut rc1 = 'x';\n"
        "  set mut rc2 = 'x';\n"
        "  let ok_rc1: bool = char_it.next(&mut rc1);\n"
        "  let ok_rc2: bool = char_it.next(&mut rc2);\n"
        "  set mut loop_sum = 0i32;\n"
        "  loop (x in range::range(1i32, 4i32)) {\n"
        "    set loop_sum = loop_sum + x;\n"
        "  }\n"
        "  set mut loop_inc_sum = 0i32;\n"
        "  loop (x in range::range_inclusive(1i32, 3i32)) {\n"
        "    set loop_inc_sum = loop_inc_sum + x;\n"
        "  }\n"
        "  set mut bo_ok = false;\n"
        "  switch (bo) {\n"
        "  case core::cmp::Ordering::Greater: { bo_ok = true; }\n"
        "  default: {}\n"
        "  }\n"
        "  set mut ord_rev_ok = false;\n"
        "  switch (ord_rev) {\n"
        "  case core::cmp::Ordering::Less: { ord_rev_ok = true; }\n"
        "  default: {}\n"
        "  }\n"
        "  manual[abi] {\n"
        "    let shadow: text = text{ data: s_ptr, len: s_len };\n"
        "    let shadow_ptr: *const i32 = xs_ptr;\n"
        "    if (shadow.len_bytes() != 3usize) { return 0i32; }\n"
        "  }\n"
        "  if (bo_ok\n"
        "      and ord_less\n"
        "      and ord_eq\n"
        "      and ord_gt\n"
        "      and ord_rev_ok\n"
        "      and bt\n"
        "      and bf\n"
        "      and mag == 3i32\n"
        "      and even_ok\n"
        "      and odd_ok\n"
        "      and pow_i == 27i32\n"
        "      and pow_u == 81u32\n"
        "      and pow8\n"
        "      and up == 'A'\n"
        "      and utf1 == 1usize\n"
        "      and utf2 == 2usize\n"
        "      and utf3 == 3usize\n"
        "      and utf4 == 4usize\n"
        "      and convert_i == 5i32\n"
        "      and (digit_hex ?? 0u32) == 15u32\n"
        "      and (digit_oob ?? 99u32) == 99u32\n"
        "      and s_len == 3usize\n"
        "      and s_empty\n"
        "      and xs_len == 4usize\n"
        "      and xs_empty\n"
        "      and slice_eq\n"
        "      and slice_sub_eq\n"
        "      and slice_dropf_eq\n"
        "      and slice_back_eq\n"
        "      and slice_dropb_eq\n"
        "      and slice_full_eq\n"
        "      and slice::is_empty(none)\n"
        "      and slice_starts\n"
        "      and slice_ends\n"
        "      and cmp_min_v == 3i32\n"
        "      and cmp_max_v == 'z'\n"
        "      and cmp_clamp_v == 4i32\n"
        "      and tv_len == 5usize\n"
        "      and not rx_empty\n"
        "      and rx_has_mid\n"
        "      and not rx_has_hi\n"
        "      and rx_contains_range\n"
        "      and not rx_intersects_tail\n"
        "      and ri_has_hi\n"
        "      and ri_empty\n"
        "      and rc_has_mid\n"
        "      and not rc_has_hi\n"
        "      and rci_single\n"
        "      and rci_intersects\n"
        "      and ok_it1\n"
        "      and ok_it2\n"
        "      and ok_it3\n"
        "      and not ok_it4\n"
        "      and it1 == 1i32\n"
        "      and it2 == 2i32\n"
        "      and it3 == 3i32\n"
        "      and ok_rc1\n"
        "      and rc1 == 'k'\n"
        "      and not ok_rc2\n"
        "      and loop_sum == 6i32\n"
        "      and loop_inc_sum == 6i32\n"
        "      and sz_i32 == 4usize\n"
        "      and sz_text == 16usize\n"
        "      and sz_arr == 16usize\n"
        "      and al_i32 == 4usize\n"
        "      and al_text == 8usize\n"
        "      and swap_a == 2i32\n"
        "      and swap_b == 1i32\n"
        "      and repl_old.len_bytes() == 3usize\n"
        "      and repl.len_bytes() == 1usize\n"
        "      and cold(true) == 7i32) {\n"
        "    return 42i32;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write core seed runtime sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for core seed runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;
    const std::filesystem::path installed_core_index_path =
        std::filesystem::path(sysroot) / ".cache/exports/core.exports.json";
    const std::filesystem::path installed_core_lib_path =
        std::filesystem::path(sysroot) / "targets" / target / "lib" / "libcore_ext.a";
    if (!std::filesystem::exists(installed_core_index_path, ec) ||
        !std::filesystem::exists(installed_core_lib_path, ec)) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    std::error_code time_ec{};
    const auto installed_index_time = std::filesystem::last_write_time(installed_core_index_path, time_ec);
    if (time_ec) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    const auto installed_lib_time = std::filesystem::last_write_time(installed_core_lib_path, time_ec);
    if (time_ec) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    for (const auto& src : core_seed_sources) {
        const auto src_time = std::filesystem::last_write_time(src, time_ec);
        if (time_ec) {
            std::filesystem::remove_all(temp_root, ec);
            return true;
        }
        if (src_time > installed_index_time || src_time > installed_lib_time) {
            std::filesystem::remove_all(temp_root, ec);
            return true;
        }
    }

    const std::string installed_core_index = read_text(installed_core_index_path);
    if (!contains(installed_core_index, "parus_builtin_acts|owner=bool|member=cmp|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=bool|member=is_true|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=bool|member=is_false|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=i32|member=div_euclid|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=i32|member=is_even|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=u32|member=pow|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=u32|member=is_power_of_two|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=f32|member=partial_cmp|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=char|member=eq_ignore_ascii_case|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=char|member=to_digit|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=char|member=len_utf8|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=text|member=len_bytes|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=text|member=as_ptr|self=1") ||
        !contains(installed_core_index, "\"path\":\"size_of\"") ||
        !contains(installed_core_index, "\"path\":\"align_of\"") ||
        !contains(installed_core_index, "\"path\":\"swap\"") ||
        !contains(installed_core_index, "\"path\":\"replace\"") ||
        !contains(installed_core_index, "\"path\":\"take\"") ||
        !contains(installed_core_index, "\"path\":\"unreachable\"") ||
        !contains(installed_core_index, "\"path\":\"spin_loop\"") ||
        !contains(installed_core_index, "\"path\":\"Range\"") ||
        !contains(installed_core_index, "\"path\":\"RangeInclusive\"") ||
        !contains(installed_core_index, "\"path\":\"range\"") ||
        !contains(installed_core_index, "\"path\":\"range_inclusive\"") ||
        !contains(installed_core_index, "\"path\":\"Iterator\"") ||
        !contains(installed_core_index, "\"path\":\"IterSource\"") ||
        !contains(installed_core_index, "\"path\":\"IterSourceMut\"") ||
        !contains(installed_core_index, "\"path\":\"IterSourceMove\"") ||
        !contains(installed_core_index, "\"path\":\"RangeIter\"") ||
        !contains(installed_core_index, "\"path\":\"RangeInclusiveIter\"") ||
        !contains(installed_core_index, "\"path\":\"step_next\"") ||
        !contains(installed_core_index, "\"path\":\"SignedInteger\"") ||
        !contains(installed_core_index, "impl_proto=core::iter::IterSourceMove") ||
        !contains(installed_core_index, "impl_proto=core::iter::Iterator") ||
        !contains(installed_core_index, "assoc_type=Item%2C") ||
        !contains(installed_core_index, "assoc_type=Iter%2C") ||
        !contains(installed_core_index, "parus_impl_binding|key=Impl::SizeOf|mode=compiler") ||
        !contains(installed_core_index, "parus_impl_binding|key=Impl::AlignOf|mode=compiler") ||
        !contains(installed_core_index, "parus_impl_binding|key=Impl::SpinLoop|mode=compiler") ||
        !contains(installed_core_index, "parus_impl_binding|key=Impl::StepNext|mode=compiler") ||
        contains(installed_core_index, "\"path\":\"Sequence\"")) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "core seed runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "core seed runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:42")) {
        std::cerr << "core seed runtime exit mismatch (expected 42)\n" << out_run;
        return false;
    }
    return true;
}

bool test_text_view_cstr_preflight_syntax_only() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-text-cstr-preflight";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    const auto& [sysroot, target] = *sysroot_and_target;
    const std::filesystem::path installed_core_index_path =
        std::filesystem::path(sysroot) / ".cache/exports/core.exports.json";
    const std::filesystem::path installed_core_lib_path =
        std::filesystem::path(sysroot) / "targets" / target / "lib" / "libcore_ext.a";
    if (!std::filesystem::exists(installed_core_index_path, ec) ||
        !std::filesystem::exists(installed_core_lib_path, ec)) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    const std::string installed_core_index = read_text(installed_core_index_path);
    if (!contains(installed_core_index, "\"path\":\"Comparable\"") ||
        !contains(installed_core_index, "\"path\":\"BinaryInteger\"") ||
        !contains(installed_core_index, "\"path\":\"SignedInteger\"") ||
        !contains(installed_core_index, "\"path\":\"UnsignedInteger\"") ||
        !contains(installed_core_index, "\"path\":\"BinaryFloatingPoint\"") ||
        contains(installed_core_index, "\"path\":\"SignedInt\"") ||
        contains(installed_core_index, "\"path\":\"UnsignedInt\"") ||
        contains(installed_core_index, "\"path\":\"Integral\"") ||
        contains(installed_core_index, "\"path\":\"FloatLike\"") ||
        contains(installed_core_index, "\"path\":\"RangeBound\"")) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    if (!contains(installed_core_index, "parus_builtin_acts|owner=text|member=len_bytes|self=1") ||
        !contains(installed_core_index, "parus_builtin_acts|owner=text|member=is_empty|self=1")) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "import ext as ext;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let c: core::ext::CStr = ext::from_raw_parts(c\"Hello\", 6usize);\n"
        "  let t: text = ext::to_text(c);\n"
        "  let n: usize = t.len_bytes();\n"
        "  if (n != 5usize or t.is_empty()) { return 0i32; }\n"
        "  return 42i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write text/CStr preflight sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + main_pr.string() +
        "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        std::cerr << "text view CStr preflight should typecheck with installed core\n" << out;
        return false;
    }
    return true;
}

bool test_cstr_private_fields_hidden_but_helpers_work() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cstr-private";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    const auto& [sysroot, target] = *sysroot_and_target;
    const std::filesystem::path installed_core_index_path =
        std::filesystem::path(sysroot) / ".cache/exports/core.exports.json";
    const std::filesystem::path installed_core_lib_path =
        std::filesystem::path(sysroot) / "targets" / target / "lib" / "libcore_ext.a";
    if (!std::filesystem::exists(installed_core_index_path, ec) ||
        !std::filesystem::exists(installed_core_lib_path, ec)) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    const auto ok_pr = temp_root / "ok.pr";
    const std::string ok_src =
        "import ext as ext;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let c: core::ext::CStr = ext::from_raw_parts(c\"Hello\", 6usize);\n"
        "  let n: usize = ext::len(c);\n"
        "  let t: text = ext::to_text(c);\n"
        "  if (t.len_bytes() == 5usize and n == 5usize) { return 42i32; }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(ok_pr, ok_src)) {
        std::cerr << "failed to write cstr helper sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + ok_pr.string() +
        "\" -fsyntax-only");
    if (rc_ok != 0) {
        std::cerr << "CStr helper API should remain usable after private field migration\n" << out_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_pr = temp_root / "bad.pr";
    const std::string bad_src =
        "import ext as ext;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let c: core::ext::CStr = ext::from_ptr(\"Hello\");\n"
        "  set p = c.ptr_;\n"
        "  let n: usize = c.len_;\n"
        "  if (p == null and n == 0usize) { return 0i32; }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(bad_pr, bad_src)) {
        std::cerr << "failed to write cstr private field negative sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_bad, out_bad] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_pr.string() +
        "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (rc_bad == 0) {
        std::cerr << "CStr private fields must not be accessible from user code\n";
        return false;
    }
    if (!contains(out_bad, "ClassPrivateMemberAccessDenied") &&
        !contains(out_bad, "member access is only available")) {
        std::cerr << "CStr private field access must be rejected for installed core users\n" << out_bad;
        return false;
    }
    return true;
}

bool test_external_generic_constraints_v2_work() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-external-generic-v2";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    const auto& [sysroot, target] = *sysroot_and_target;
    const std::filesystem::path installed_core_index_path =
        std::filesystem::path(sysroot) / ".cache/exports/core.exports.json";
    const std::filesystem::path installed_core_lib_path =
        std::filesystem::path(sysroot) / "targets" / target / "lib" / "libcore_ext.a";
    if (!std::filesystem::exists(installed_core_index_path, ec) ||
        !std::filesystem::exists(installed_core_lib_path, ec)) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }
    const std::string installed_core_index = read_text(installed_core_index_path);
    if (!contains(installed_core_index, "\"path\":\"Comparable\"") ||
        !contains(installed_core_index, "\"path\":\"BinaryInteger\"") ||
        !contains(installed_core_index, "\"path\":\"SignedInteger\"") ||
        !contains(installed_core_index, "\"path\":\"UnsignedInteger\"") ||
        !contains(installed_core_index, "\"path\":\"BinaryFloatingPoint\"") ||
        contains(installed_core_index, "\"path\":\"SignedInt\"") ||
        contains(installed_core_index, "\"path\":\"UnsignedInt\"") ||
        contains(installed_core_index, "\"path\":\"Integral\"") ||
        contains(installed_core_index, "\"path\":\"FloatLike\"") ||
        contains(installed_core_index, "\"path\":\"RangeBound\"")) {
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    const auto lib_pr = temp_root / "lib.pr";
    const std::string lib_src =
        "nest api;\n"
        "import constraints as constraints;\n"
        "import ext as ext;\n"
        "import range as range;\n"
        "extern \"C\" def puts(s: *const ext::c_char) -> ext::c_int;\n"
        "struct OnlyI32<T> with [T == i32] {\n"
        "  value: T;\n"
        "};\n"
        "struct Box<T> {\n"
        "  value: T;\n"
        "};\n"
        "export class GenericBox<T> {\n"
        "  private:\n"
        "    value: T;\n"
        "\n"
        "  public:\n"
        "    static const ZERO: i32 = 5i32;\n"
        "\n"
        "    init(v: T) {\n"
        "      self.value = v;\n"
        "    }\n"
        "\n"
        "    static def bonus() -> i32 {\n"
        "      return 6i32;\n"
        "    }\n"
        "\n"
        "    def value_or_zero(self) -> i32 {\n"
        "      return 7i32;\n"
        "    }\n"
        "\n"
        "    deinit() {\n"
        "      puts(\"drop\");\n"
        "    }\n"
        "};\n"
        "export proto BaseProto {\n"
        "  provide def base_value() -> i32 {\n"
        "    return 9i32;\n"
        "  }\n"
        "};\n"
        "export proto Defaulted<T> {\n"
        "  provide const ZERO: i32 = 4i32;\n"
        "  provide def value_or_zero() -> i32 {\n"
        "    return 7i32;\n"
        "  }\n"
        "};\n"
        "proto HiddenProto {\n"
        "  provide def hidden_value() -> i32 {\n"
        "    return 0i32;\n"
        "  }\n"
        "};\n"
        "export def only_i32<T>(x: T) with [T == i32] -> i32 {\n"
        "  return 1i32;\n"
        "}\n"
        "export def signed_only<T>(x: T) with [T: constraints::SignedInteger] -> i32 {\n"
        "  return 2i32;\n"
        "}\n"
        "export acts for range::Range<T> with [T: constraints::Comparable] {\n"
        "  def has_same_bounds(self, other: range::Range<T>) -> bool {\n"
        "    return self.start == other.start and self.end == other.end;\n"
        "  }\n"
        "};\n";
    if (!write_text(lib_pr, lib_src)) {
        std::cerr << "failed to write generic library source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto lib_index = temp_root / "lib.exports.json";
    auto [rc_idx, out_idx] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + lib_pr.string() +
        "\" -fsyntax-only --bundle-name lib --bundle-root \"" + temp_root.string() +
        "\" --module-head api --bundle-source \"" + lib_pr.string() +
        "\" --load-export-index \"" + installed_core_index_path.string() +
        "\" --emit-export-index \"" + lib_index.string() + "\"");
    if (rc_idx != 0) {
        std::cerr << "failed to emit external generic export-index\n" << out_idx;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string lib_index_text = read_text(lib_index);
    if (!contains(lib_index_text, "gconstraint=type_eq,T,i32") ||
        !contains(lib_index_text, "gconstraint=proto,T,core::constraints::SignedInteger") ||
        contains(lib_index_text, "gconstraint=proto,T,constraints::SignedInteger") ||
        !contains(lib_index_text, "\"path\":\"api::GenericBox\"")) {
        std::cerr << "generic export-index must carry equality/proto constraint metadata\n" << lib_index_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto ok_pr = temp_root / "ok.pr";
    const std::string ok_src =
        "import api as api;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let a: i32 = api::only_i32(1i32);\n"
        "  let b: i32 = api::signed_only(-1i32);\n"
        "  return a + b;\n"
        "}\n";
    if (!write_text(ok_pr, ok_src)) {
        std::cerr << "failed to write external generic success sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_ok, out_ok] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + ok_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"");
    if (rc_ok != 0) {
        std::cerr << "external generic constraints success sample should typecheck\n" << out_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto proto_target_pr = temp_root / "proto_target_no_import.pr";
    const std::string proto_target_src =
        "def ordered<T>(x: T) with [T: constraints::Comparable] -> bool {\n"
        "  return x >= x;\n"
        "}\n";
    if (!write_text(proto_target_pr, proto_target_src)) {
        std::cerr << "failed to write proto-target import-ergonomics sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_proto_target, out_proto_target] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + proto_target_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() + "\"");
    if (rc_proto_target != 0) {
        std::cerr << "qualified proto target should typecheck without explicit import\n" << out_proto_target;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto ok_import_pr = temp_root / "ok_import.pr";
    const std::string ok_import_src =
        "import constraints as constraints;\n"
        "def ordered<T>(x: T) with [T: constraints::Comparable] -> bool {\n"
        "  return x >= x;\n"
        "}\n"
        "def main() -> i32 {\n"
        "  if (ordered<char>('a')) { return 0i32; }\n"
        "  return 1i32;\n"
        "}\n";
    if (!write_text(ok_import_pr, ok_import_src)) {
        std::cerr << "failed to write explicit-import generic constraint sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_ok_import, out_ok_import] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + ok_import_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() + "\"");
    if (rc_ok_import != 0) {
        std::cerr << "explicit-import generic constraint sample should typecheck\n" << out_ok_import;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto proto_attach_pr = temp_root / "proto_attach_no_import.pr";
    const std::string proto_attach_src =
        "proto Derived: api::BaseProto {\n"
        "};\n"
        "\n"
        "class LocalBox<T>: api::Defaulted<T> {\n"
        "  init() = default;\n"
        "};\n";
    if (!write_text(proto_attach_pr, proto_attach_src)) {
        std::cerr << "failed to write proto-attachment import-ergonomics sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_proto_attach, out_proto_attach] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + proto_attach_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_proto_attach != 0) {
        std::cerr << "qualified proto inheritance/attachment should typecheck without explicit import\n"
                  << out_proto_attach;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto hidden_proto_pr = temp_root / "hidden_proto.pr";
    const std::string hidden_proto_src =
        "def ordered<T>(x: T) with [T: api::HiddenProto] -> bool {\n"
        "  return true;\n"
        "}\n";
    if (!write_text(hidden_proto_pr, hidden_proto_src)) {
        std::cerr << "failed to write hidden-proto visibility sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_hidden_proto, out_hidden_proto] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + hidden_proto_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_hidden_proto == 0 ||
        (!contains(out_hidden_proto, "GenericConstraintProtoNotFound") &&
         !contains(out_hidden_proto, "unknown proto in generic constraint")) ||
        !contains(out_hidden_proto, "note: proto-target import ergonomics only applies to public exported proto targets") ||
        !contains(out_hidden_proto, "help: add an explicit import for the proto, or export the proto through a public path")) {
        std::cerr << "hidden proto must not become source-visible through proto-target import ergonomics\n"
                  << out_hidden_proto;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_type_path_pr = temp_root / "bad_type_path.pr";
    const std::string bad_type_path_src =
        "def main() -> i32 {\n"
        "  let b: api::GenericBox<i32> = api::GenericBox<i32>(1i32);\n"
        "  return b.value_or_zero();\n"
        "}\n";
    if (!write_text(bad_type_path_pr, bad_type_path_src)) {
        std::cerr << "failed to write ordinary type-path import sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_bad_type_path, out_bad_type_path] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_type_path_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_bad_type_path == 0) {
        std::cerr << "ordinary type path must still require an explicit import\n" << out_bad_type_path;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_value_path_pr = temp_root / "bad_value_path.pr";
    const std::string bad_value_path_src =
        "def main() -> i32 {\n"
        "  return cmp::min(1i32, 2i32);\n"
        "}\n";
    if (!write_text(bad_value_path_pr, bad_value_path_src)) {
        std::cerr << "failed to write ordinary value-path import sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_bad_value_path, out_bad_value_path] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_value_path_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() + "\"");
    if (rc_bad_value_path == 0) {
        std::cerr << "ordinary value path must still require an explicit import\n" << out_bad_value_path;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_eq_pr = temp_root / "bad_eq.pr";
    const std::string bad_eq_src =
        "import api as api;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return api::only_i32(1u32);\n"
        "}\n";
    if (!write_text(bad_eq_pr, bad_eq_src)) {
        std::cerr << "failed to write external generic equality negative sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_bad_eq, out_bad_eq] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_eq_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_bad_eq == 0 || !contains(out_bad_eq, "GenericConstraintTypeMismatch")) {
        std::cerr << "external generic equality mismatch must report GenericConstraintTypeMismatch\n" << out_bad_eq;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_proto_pr = temp_root / "bad_proto.pr";
    const std::string bad_proto_src =
        "import api as api;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return api::signed_only(1u32);\n"
        "}\n";
    if (!write_text(bad_proto_pr, bad_proto_src)) {
        std::cerr << "failed to write external generic proto negative sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_bad_proto, out_bad_proto] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_proto_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_bad_proto == 0 || !contains(out_bad_proto, "GenericConstraintUnsatisfied")) {
        std::cerr << "external generic proto mismatch must report GenericConstraintUnsatisfied\n" << out_bad_proto;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto ok_acts_pr = temp_root / "ok_acts.pr";
    const std::string ok_acts_src =
        "import api as api;\n"
        "import range as range;\n"
        "\n"
        "def main() -> bool {\n"
        "  let r1: core::range::Range<i32> = range::range(1i32, 4i32);\n"
        "  let r2: core::range::Range<i32> = range::range(1i32, 4i32);\n"
        "  return r1.has_same_bounds(r2);\n"
        "}\n";
    if (!write_text(ok_acts_pr, ok_acts_src)) {
        std::cerr << "failed to write external generic acts success sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_ok_acts, out_ok_acts] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + ok_acts_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_ok_acts != 0) {
        std::cerr << "external generic acts success sample should typecheck\n" << out_ok_acts;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto ok_class_pr = temp_root / "ok_class.pr";
    const std::string ok_class_src =
        "import api as api;\n"
        "\n"
        "struct LocalNum {\n"
        "  value: i32;\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set b = api::GenericBox<LocalNum>(LocalNum{ value: 1i32 });\n"
        "  let z: i32 = api::GenericBox<LocalNum>::ZERO;\n"
        "  let bonus: i32 = api::GenericBox<LocalNum>::bonus();\n"
        "  let x: i32 = b.value_or_zero();\n"
        "  return z + bonus + x;\n"
        "}\n";
    if (!write_text(ok_class_pr, ok_class_src)) {
        std::cerr << "failed to write external generic class success sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_ok_class, out_ok_class] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + ok_class_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_ok_class != 0) {
        std::cerr << "external generic class success sample should typecheck\n" << out_ok_class;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_acts_pr = temp_root / "bad_acts.pr";
    const std::string bad_acts_src =
        "import api as api;\n"
        "import range as range;\n"
        "\n"
        "struct LocalNum {\n"
        "  value: i32;\n"
        "};\n"
        "\n"
        "def main(x: core::range::Range<LocalNum>) -> bool {\n"
        "  return x.has_same_bounds(x);\n"
        "}\n";
    if (!write_text(bad_acts_pr, bad_acts_src)) {
        std::cerr << "failed to write external generic acts negative sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_bad_acts, out_bad_acts] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_acts_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_bad_acts == 0 || !contains(out_bad_acts, "GenericConstraintUnsatisfied")) {
        std::cerr << "external generic acts proto mismatch must report GenericConstraintUnsatisfied\n"
                  << out_bad_acts;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto runtime_pr = temp_root / "runtime.pr";
    const std::string runtime_src =
        "import api as api;\n"
        "import range as range;\n"
        "\n"
        "struct LocalBox<T>: api::Defaulted<T> {\n"
        "  value: T;\n"
        "};\n"
        "\n"
        "struct LocalNum {\n"
        "  value: i32;\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  let b: LocalBox<i32> = LocalBox<i32>{ value: 3i32 };\n"
        "  let z: i32 = b->ZERO;\n"
        "  let x: i32 = b.value_or_zero();\n"
        "  let r1: core::range::Range<i32> = range::range(1i32, 4i32);\n"
        "  let r2: core::range::Range<i32> = range::range(1i32, 4i32);\n"
        "  let r3: core::range::Range<i32> = range::range(2i32, 5i32);\n"
        "  let ok_same: bool = r1.has_same_bounds(r2);\n"
        "  let ok_diff: bool = r1.has_same_bounds(r3);\n"
        "  {\n"
        "    set c = api::GenericBox<i32>(3i32);\n"
        "    let cz: i32 = api::GenericBox<i32>::ZERO;\n"
        "    let cb: i32 = api::GenericBox<i32>::bonus();\n"
        "    let cx: i32 = c.value_or_zero();\n"
        "    if (cz != 5i32 or cb != 6i32 or cx != 7i32) {\n"
        "      return 0i32;\n"
        "    }\n"
        "  }\n"
        "  {\n"
        "    set d = api::GenericBox<LocalNum>(LocalNum{ value: 9i32 });\n"
        "    let dx: i32 = d.value_or_zero();\n"
        "    if (dx != 7i32) {\n"
        "      return 0i32;\n"
        "    }\n"
        "  }\n"
        "  if (z == 4i32 and x == 7i32 and ok_same and (not ok_diff)) {\n"
        "    return 42i32;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(runtime_pr, runtime_src)) {
        std::cerr << "failed to write external generic proto/acts runtime sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto runtime_exe = temp_root / "runtime";
    auto [rc_runtime_build, out_runtime_build] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + runtime_pr.string() +
        "\" --sysroot \"" + sysroot + "\"" +
        " --target " + target +
        " -o \"" + runtime_exe.string() + "\"" +
        " --load-export-index \"" + installed_core_index_path.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"");
    if (rc_runtime_build != 0) {
        std::cerr << "external generic proto/acts runtime sample should compile/link\n" << out_runtime_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_runtime_run, out_runtime_run] = run_capture("\"" + runtime_exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_runtime_run != 0) {
        std::cerr << "external generic proto/acts runtime sample should execute\n" << out_runtime_run;
        return false;
    }
    if (!contains(out_runtime_run, "drop") ||
        out_runtime_run.find("drop") == out_runtime_run.rfind("drop") ||
        !contains(out_runtime_run, "EXIT:42")) {
        std::cerr << "external generic proto/acts runtime exit mismatch (expected 42)\n"
                  << out_runtime_run;
        return false;
    }
    return true;
}

bool test_external_generic_type_body_closure_v2_work() {
    const std::string bin = PARUS_BUILD_BIN;

    const auto sysroot_target = resolve_installed_sysroot_and_target();
    if (!sysroot_target.has_value()) {
        std::cerr << "installed sysroot manifest not found for external type-body closure test\n";
        return false;
    }
    const std::string sysroot = sysroot_target->first;
    const std::string target = sysroot_target->second;
    const auto installed_core_index_path =
        std::filesystem::path(sysroot) / ".cache/exports/core.exports.json";
    if (!std::filesystem::exists(installed_core_index_path)) {
        std::cerr << "installed core export-index not found for external type-body closure test\n";
        return false;
    }

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-ext-type-body-closure";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto lib_pr = temp_root / "lib.pr";
    const std::string lib_src =
        "import constraints as constraints;\n"
        "import ext as ext;\n"
        "\n"
        "extern \"C\" def puts(s: *const ext::c_char) -> ext::c_int;\n"
        "\n"
        "struct Pair<T> {\n"
        "  lhs: T;\n"
        "  rhs: T;\n"
        "};\n"
        "\n"
        "class PairBox<T> {\n"
        "  lhs: T;\n"
        "  rhs: T;\n"
        "\n"
        "public:\n"
        "  static const BONUS: i32 = 9i32;\n"
        "\n"
        "  init(a: T, b: T) {\n"
        "    self.lhs = a;\n"
        "    self.rhs = b;\n"
        "  }\n"
        "\n"
        "  static def bonus() -> i32 {\n"
        "    return 9i32;\n"
        "  }\n"
        "\n"
        "  def count(self) -> i32 {\n"
        "    return 2i32;\n"
        "  }\n"
        "\n"
        "  deinit() {\n"
        "    puts(\"pair-drop\");\n"
        "  }\n"
        "};\n"
        "\n"
        "enum HiddenStep<T> {\n"
        "  case Empty,\n"
        "  case Seen(value: T),\n"
        "};\n"
        "\n"
        "export enum PublicStep<T> {\n"
        "  case Empty,\n"
        "  case Seen(value: T),\n"
        "};\n"
        "\n"
        "export class Box<T> {\n"
        "  value: T;\n"
        "\n"
        "public:\n"
        "  static const BONUS: i32 = 3i32;\n"
        "\n"
        "  init(v: T) {\n"
        "    self.value = v;\n"
        "  }\n"
        "\n"
        "  def dup(self) -> Pair<T> {\n"
        "    return Pair<T>{ lhs: self.value, rhs: self.value };\n"
        "  }\n"
        "\n"
        "  def helper_score(self) -> i32 {\n"
        "    set p = PairBox<T>(self.value, self.value);\n"
        "    return PairBox<T>::bonus() + PairBox<T>::BONUS + p.count();\n"
        "  }\n"
        "\n"
        "  def to_public(self) -> PublicStep<T> {\n"
        "    return PublicStep<T>::Seen(value: self.value);\n"
        "  }\n"
        "\n"
        "  deinit() {\n"
        "    puts(\"box-drop\");\n"
        "  }\n"
        "};\n"
        "\n"
        "export proto Probe<T> {\n"
        "  provide def seen(v: T) -> bool {\n"
        "    set p = PairBox<T>(v, v);\n"
        "    set s = HiddenStep<T>::Seen(value: v);\n"
        "    switch (s) {\n"
        "    case HiddenStep<T>::Empty: { return false; }\n"
        "    case HiddenStep<T>::Seen(value: x): { return p.count() == 2i32 and x >= x; }\n"
        "    }\n"
        "    return false;\n"
        "  }\n"
        "};\n"
        "\n"
        "export acts for Box<T> with [T: constraints::Comparable] {\n"
        "  def tag(self) -> i32 {\n"
        "    set p = PairBox<T>(self.value, self.value);\n"
        "    set s = HiddenStep<T>::Seen(value: self.value);\n"
        "    switch (s) {\n"
        "    case HiddenStep<T>::Empty: { return 0i32; }\n"
        "    case HiddenStep<T>::Seen(value: x): {\n"
        "      if (x >= x) {\n"
        "        return PairBox<T>::bonus() + PairBox<T>::BONUS + p.count();\n"
        "      }\n"
        "      return 4i32;\n"
        "    }\n"
        "    }\n"
        "    return -1i32;\n"
        "  }\n"
        "};\n";
    if (!write_text(lib_pr, lib_src)) {
        std::cerr << "failed to write external type-body closure library source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto lib_index = temp_root / "lib.exports.json";
    auto [rc_idx, out_idx] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + lib_pr.string() +
        "\" -fsyntax-only --bundle-name lib --bundle-root \"" + temp_root.string() +
        "\" --module-head api --bundle-source \"" + lib_pr.string() +
        "\" --load-export-index \"" + installed_core_index_path.string() +
        "\" --emit-export-index \"" + lib_index.string() + "\"");
    if (rc_idx != 0) {
        std::cerr << "failed to emit external type-body closure export-index\n" << out_idx;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto lib_templates = temp_root / "lib.templates.json";
    const std::string lib_templates_text = read_text(lib_templates);
    if (!contains(lib_templates_text, "\"public_path\":\"Pair\"") ||
        !contains(lib_templates_text, "\"public_path\":\"PairBox\"") ||
        !contains(lib_templates_text, "\"public_path\":\"HiddenStep\"") ||
        !contains(lib_templates_text, "\"public_path\":\"Box\"") ||
        !contains(lib_templates_text, "\"public_path\":\"Probe\"")) {
        std::cerr << "typed sidecar must include helper struct/enum/class closure nodes alongside exported roots\n"
                  << lib_templates_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (count_occurrences(lib_templates_text, "\"public_path\":\"Pair\"") != 1 ||
        count_occurrences(lib_templates_text, "\"public_path\":\"PairBox\"") != 1 ||
        count_occurrences(lib_templates_text, "\"public_path\":\"HiddenStep\"") != 1) {
        std::cerr << "typed sidecar helper closure nodes must be deduped by canonical identity\n"
                  << lib_templates_text;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto runtime_pr = temp_root / "main.pr";
    const std::string runtime_src =
        "import api as api;\n"
        "\n"
        "class LocalProbe: api::Probe<i32> {\n"
        "  init() = default;\n"
        "};\n"
        "\n"
        "def sum_step(s: api::PublicStep<i32>) -> i32 {\n"
        "  switch (s) {\n"
        "  case api::PublicStep<i32>::Empty: { return 0i32; }\n"
        "  case api::PublicStep<i32>::Seen(value: v): { return v; }\n"
        "  }\n"
        "  return -1i32;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  let seen: bool = LocalProbe()->seen(8i32);\n"
        "  {\n"
        "    set b = api::Box<i32>(7i32);\n"
        "    set p = b.dup();\n"
        "    let helper_score: i32 = b.helper_score();\n"
        "    let tag: i32 = b.tag();\n"
        "    let bonus: i32 = api::Box<i32>::BONUS;\n"
        "    let step_sum: i32 = sum_step(b.to_public());\n"
        "    if (seen and p.lhs == 7i32 and p.rhs == 7i32 and helper_score == 20i32 and tag == 20i32 and bonus == 3i32 and step_sum == 7i32) {\n"
        "      return 0i32;\n"
        "    }\n"
        "    return 1i32;\n"
        "  }\n"
        "}\n";
    if (!write_text(runtime_pr, runtime_src)) {
        std::cerr << "failed to write external type-body closure runtime sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto runtime_exe = temp_root / "main";
    auto [rc_runtime_build, out_runtime_build] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + runtime_pr.string() +
        "\" --sysroot \"" + sysroot + "\"" +
        " --target " + target +
        " -o \"" + runtime_exe.string() + "\"" +
        " --load-export-index \"" + installed_core_index_path.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"");
    if (rc_runtime_build != 0) {
        std::cerr << "external type-body closure runtime sample should compile/link\n" << out_runtime_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto runtime_dup_exe = temp_root / "main.dup";
    auto [rc_runtime_dup_build, out_runtime_dup_build] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + runtime_pr.string() +
        "\" --sysroot \"" + sysroot + "\"" +
        " --target " + target +
        " -o \"" + runtime_dup_exe.string() + "\"" +
        " --load-export-index \"" + installed_core_index_path.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"");
    if (rc_runtime_dup_build != 0) {
        std::cerr << "repeated export-index load should dedup imported helper templates\n"
                  << out_runtime_dup_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_runtime_run, out_runtime_run] = run_capture("\"" + runtime_exe.string() + "\"; echo EXIT:$?");
    if (rc_runtime_run != 0 ||
        !contains(out_runtime_run, "pair-drop") ||
        !contains(out_runtime_run, "box-drop") ||
        !contains(out_runtime_run, "EXIT:0")) {
        std::cerr << "external type-body closure runtime sample should execute helper and root class deinit paths\n"
                  << out_runtime_run;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto hidden_ref_pr = temp_root / "hidden_ref.pr";
    const std::string hidden_ref_src =
        "import api as api;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set p = api::PairBox<i32>(1i32, 2i32);\n"
        "  return api::PairBox<i32>::BONUS + p.count();\n"
        "}\n";
    if (!write_text(hidden_ref_pr, hidden_ref_src)) {
        std::cerr << "failed to write hidden helper lexical reference sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_hidden_ref, out_hidden_ref] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + hidden_ref_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + lib_index.string() + "\"");
    if (rc_hidden_ref == 0 ||
        (!contains(out_hidden_ref, "unknown identifier") &&
         !contains(out_hidden_ref, "unknown type") &&
         !contains(out_hidden_ref, "unknown type") &&
         !contains(out_hidden_ref, "SymbolNotExportedFileScope") &&
         !contains(out_hidden_ref, "UnknownType") &&
         !contains(out_hidden_ref, "no callable declaration candidate")) ||
        !contains(out_hidden_ref, "note: closure-private helper declarations are only materialized inside imported generic bodies") ||
        !contains(out_hidden_ref, "help: reference the exported root API instead, or export this helper through a public path")) {
        std::cerr << "closure-private helper type must not become a lexical public import\n"
                  << out_hidden_ref;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto bad_state_pr = temp_root / "lib_bad_state.pr";
    const std::string bad_state_src =
        "nest bad;\n"
        "\n"
        "static mut GLOBAL: i32 = 1i32;\n"
        "\n"
        "class HiddenBox<T> {\n"
        "  stored: T;\n"
        "\n"
        "  init(v: T) {\n"
        "    self.stored = v;\n"
        "  }\n"
        "\n"
        "  def score(self) -> i32 {\n"
        "    return GLOBAL;\n"
        "  }\n"
        "};\n"
        "\n"
        "export class Box<T> {\n"
        "  stored: T;\n"
        "\n"
        "  init(v: T) {\n"
        "    self.stored = v;\n"
        "  }\n"
        "\n"
        "  def score(self) -> i32 {\n"
        "    let h: HiddenBox<T> = HiddenBox<T>(self.stored);\n"
        "    return h.score();\n"
        "  }\n"
        "};\n";
    if (!write_text(bad_state_pr, bad_state_src)) {
        std::cerr << "failed to write mutable-global closure rejection sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto bad_state_index = temp_root / "lib_bad_state.exports.json";
    auto [rc_bad_state, out_bad_state] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + bad_state_pr.string() +
        "\" -fsyntax-only --bundle-name bad --bundle-root \"" + temp_root.string() +
        "\" --module-head bad --bundle-source \"" + bad_state_pr.string() +
        "\" --load-export-index \"" + installed_core_index_path.string() +
        "\" --emit-export-index \"" + bad_state_index.string() + "\"");
    if (rc_bad_state == 0 ||
        !contains(out_bad_state, "TemplateSidecarUnsupportedClosure") ||
        !contains(out_bad_state, "unsupported mutable global dependency closure") ||
        !contains(out_bad_state, "note: dependency chain:") ||
        !contains(out_bad_state, "help: remove the mutable global/static dependency from the exported generic closure")) {
        std::cerr << "mutable global dependency closure must be rejected with a structured diagnostic\n"
                  << out_bad_state;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto broken_index = temp_root / "lib.invalid.exports.json";
    const auto broken_templates = temp_root / "lib.invalid.templates.json";
    if (!write_text(broken_index, read_text(lib_index)) || !write_text(broken_templates, "{")) {
        std::cerr << "failed to write broken sidecar pair for external type-body closure test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    auto [rc_broken, out_broken] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + runtime_pr.string() +
        "\" -fsyntax-only --load-export-index \"" + installed_core_index_path.string() +
        "\" --load-export-index \"" + broken_index.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_broken == 0 ||
        (!contains(out_broken, "unsupported template-sidecar version") &&
         !contains(out_broken, "TemplateSidecarSchema") &&
         !contains(out_broken, "ExportIndexSchema")) ||
        !contains(out_broken, "note:") ||
        !contains(out_broken, "help:")) {
        std::cerr << "broken closure sidecar must report template payload load failure\n" << out_broken;
        return false;
    }

    return true;
}

bool test_bundle_alias_proto_impl_path_resolves() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-proto-alias";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "markers", ec);
    std::filesystem::create_directories(temp_root / "err", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto proto_pr = temp_root / "markers/marker.pr";
    const auto err_pr = temp_root / "err/basic.pr";

    const std::string proto_src =
        "export proto Recoverable {\n"
        "};\n";
    const std::string err_src =
        "import markers as p;\n"
        "\n"
        "export enum CoreErr: p::Recoverable {\n"
        "  case InvalidState,\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(proto_pr, proto_src) || !write_text(err_pr, err_src)) {
        std::cerr << "failed to write proto alias bundle files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + err_pr.string() + "\" -fsyntax-only" +
        " --bundle-name core" +
        " --bundle-root \"" + temp_root.string() + "\"" +
        " --module-head err" +
        " --module-import markers" +
        " --bundle-source \"" + proto_pr.string() + "\"" +
        " --bundle-source \"" + err_pr.string() + "\"";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc != 0) {
        std::cerr << "qualified proto path via import alias should compile\n" << out;
        return false;
    }
    return true;
}

bool test_bundle_relative_import_resolves() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-relative-import";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "foo", ec);
    std::filesystem::create_directories(temp_root / "bar", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto bar_pr = temp_root / "bar/marker.pr";
    const auto foo_pr = temp_root / "foo/main.pr";
    const std::string bar_src =
        "export proto Recoverable {\n"
        "};\n";
    const std::string foo_src =
        "import .bar as b;\n"
        "\n"
        "export enum E: b::Recoverable {\n"
        "  case A,\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(bar_pr, bar_src) || !write_text(foo_pr, foo_src)) {
        std::cerr << "failed to write relative import bundle files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + foo_pr.string() + "\" -fsyntax-only" +
        " --bundle-name app" +
        " --bundle-root \"" + temp_root.string() + "\"" +
        " --module-head foo" +
        " --module-import bar" +
        " --bundle-source \"" + bar_pr.string() + "\"" +
        " --bundle-source \"" + foo_pr.string() + "\"";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc != 0) {
        std::cerr << "relative import .bar should resolve to sibling module\n" << out;
        return false;
    }
    return true;
}

bool test_bundle_parent_relative_import_resolves() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-parent-relative-import";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "foo/bar", ec);
    std::filesystem::create_directories(temp_root / "foo/baz", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto bar_pr = temp_root / "foo/bar/types.pr";
    const auto baz_pr = temp_root / "foo/baz/main.pr";
    const std::string bar_src =
        "export proto Recoverable {\n"
        "};\n";
    const std::string baz_src =
        "import ..foo::bar as b;\n"
        "\n"
        "export enum E: b::Recoverable {\n"
        "  case A,\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(bar_pr, bar_src) || !write_text(baz_pr, baz_src)) {
        std::cerr << "failed to write parent-relative import bundle files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + baz_pr.string() + "\" -fsyntax-only" +
        " --bundle-name app" +
        " --bundle-root \"" + temp_root.string() + "\"" +
        " --module-head foo::baz" +
        " --module-import foo" +
        " --bundle-source \"" + bar_pr.string() + "\"" +
        " --bundle-source \"" + baz_pr.string() + "\"";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc != 0) {
        std::cerr << "relative import ..foo::bar should resolve via parent module\n" << out;
        return false;
    }
    return true;
}

bool test_bundle_grandparent_relative_import_resolves() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-grandparent-relative-import";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "foo/baz/qux", ec);
    std::filesystem::create_directories(temp_root / "bar", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto bar_pr = temp_root / "bar/types.pr";
    const auto qux_pr = temp_root / "foo/baz/qux/main.pr";
    const std::string bar_src =
        "export proto Recoverable {\n"
        "};\n";
    const std::string qux_src =
        "import ...bar as b;\n"
        "\n"
        "export enum E: b::Recoverable {\n"
        "  case A,\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(bar_pr, bar_src) || !write_text(qux_pr, qux_src)) {
        std::cerr << "failed to write grandparent-relative import bundle files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + qux_pr.string() + "\" -fsyntax-only" +
        " --bundle-name app" +
        " --bundle-root \"" + temp_root.string() + "\"" +
        " --module-head foo::baz::qux" +
        " --module-import bar" +
        " --bundle-source \"" + bar_pr.string() + "\"" +
        " --bundle-source \"" + qux_pr.string() + "\"";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc != 0) {
        std::cerr << "relative import ...bar should resolve via grandparent module\n" << out;
        return false;
    }
    return true;
}

bool test_external_generic_owner_enum_runtime() {
    const std::string bin = PARUS_BUILD_BIN;

    const auto sysroot_target = resolve_installed_sysroot_and_target();
    if (!sysroot_target.has_value()) {
        std::cerr << "installed sysroot manifest not found for external generic owner-enum test\n";
        return false;
    }
    const std::string sysroot = sysroot_target->first;
    const std::string target = sysroot_target->second;
    const auto installed_core_index_path =
        std::filesystem::path(sysroot) / ".cache/exports/core.exports.json";
    if (!std::filesystem::exists(installed_core_index_path)) {
        std::cerr << "installed core export-index not found for external generic owner-enum test\n";
        return false;
    }

    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-ext-generic-owner-enum";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto lib_pr = temp_root / "lib.pr";
    const std::string lib_src =
        "export enum Job<T> {\n"
        "  case Empty,\n"
        "  case Ready(worker: ~T),\n"
        "};\n"
        "\n"
        "export def pass<T>(j: Job<T>) -> Job<T> {\n"
        "  return j;\n"
        "}\n";
    if (!write_text(lib_pr, lib_src)) {
        std::cerr << "failed to write external generic owner-enum library source\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto lib_index = temp_root / "lib.exports.json";
    auto [rc_idx, out_idx] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + lib_pr.string() +
        "\" -fsyntax-only --bundle-name ownerlib --bundle-root \"" + temp_root.string() +
        "\" --module-head api --bundle-source \"" + lib_pr.string() +
        "\" --load-export-index \"" + installed_core_index_path.string() +
        "\" --emit-export-index \"" + lib_index.string() + "\"");
    if (rc_idx != 0) {
        std::cerr << "failed to emit external generic owner-enum export-index\n" << out_idx;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto direct_pr = temp_root / "main_direct.pr";
    const std::string direct_src =
        "import api as api;\n"
        "\n"
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "  def run(self) -> i32 { return self.value; }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  let j: api::Job<Worker> = api::Job<Worker>::Ready(worker: make(41i32));\n"
        "  switch (j) {\n"
        "  case api::Job<Worker>::Ready(worker: w): { return w.run(); }\n"
        "  default: { return 1i32; }\n"
        "  }\n"
        "  return 2i32;\n"
        "}\n";
    if (!write_text(direct_pr, direct_src)) {
        std::cerr << "failed to write direct external generic owner-enum runtime sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto direct_exe = temp_root / "main_direct";
    auto [rc_direct_build, out_direct_build] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + direct_pr.string() +
        "\" --sysroot \"" + sysroot + "\"" +
        " --target " + target +
        " -o \"" + direct_exe.string() + "\"" +
        " --load-export-index \"" + installed_core_index_path.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"");
    if (rc_direct_build != 0) {
        std::cerr << "direct external generic owner-enum sample should compile/link\n" << out_direct_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_direct_run, out_direct_run] = run_capture("\"" + direct_exe.string() + "\"; echo EXIT:$?");
    if (rc_direct_run != 0 || !contains(out_direct_run, "EXIT:41")) {
        std::cerr << "direct external generic owner-enum runtime exit mismatch (expected 41)\n"
                  << out_direct_run;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto pass_pr = temp_root / "main.pr";
    const std::string pass_src =
        "import api as api;\n"
        "\n"
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "  def run(self) -> i32 { return self.value; }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  let j: api::Job<Worker> = api::pass<Worker>(api::Job<Worker>::Ready(worker: make(31i32)));\n"
        "  switch (j) {\n"
        "  case api::Job<Worker>::Ready(worker: w): { return w.run(); }\n"
        "  default: { return 1i32; }\n"
        "  }\n"
        "  return 2i32;\n"
        "}\n";
    if (!write_text(pass_pr, pass_src)) {
        std::cerr << "failed to write pass-through external generic owner-enum runtime sample\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto pass_exe = temp_root / "main";
    auto [rc_pass_build, out_pass_build] = run_capture(
        "PARUS_SYSROOT=\"" + sysroot + "\" \"" + bin + "\" tool parusc -- \"" + pass_pr.string() +
        "\" --sysroot \"" + sysroot + "\"" +
        " --target " + target +
        " -o \"" + pass_exe.string() + "\"" +
        " --load-export-index \"" + installed_core_index_path.string() + "\"" +
        " --load-export-index \"" + lib_index.string() + "\"");
    if (rc_pass_build != 0) {
        std::cerr << "pass-through external generic owner-enum sample should compile/link\n" << out_pass_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_pass_run, out_pass_run] = run_capture("\"" + pass_exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_pass_run != 0 || !contains(out_pass_run, "EXIT:31")) {
        std::cerr << "pass-through external generic owner-enum runtime exit mismatch (expected 31)\n"
                  << out_pass_run;
        return false;
    }

    return true;
}

bool test_core_marker_bare_use_special_form() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-core-bare-use";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto core_marker_ok = temp_root / "core_marker_ok.pr";
    const auto core_no_marker = temp_root / "core_no_marker.pr";
    const auto non_core_marker = temp_root / "non_core_marker.pr";

    const std::string marker_src =
        "$![Impl::Core];\n"
        "use c_int;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    const std::string no_marker_src =
        "use c_int;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(core_marker_ok, marker_src) ||
        !write_text(core_no_marker, no_marker_src) ||
        !write_text(non_core_marker, marker_src)) {
        std::cerr << "failed to write core bare-use policy files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto run_tool = [&](const std::filesystem::path& pr,
                        const std::string& bundle_name) -> std::pair<int, std::string> {
        const std::string cmd =
            "\"" + bin + "\" tool parusc -- \"" + pr.string() + "\" -fsyntax-only" +
            " --bundle-name " + bundle_name +
            " --bundle-root \"" + temp_root.string() + "\"" +
            " --module-head ext" +
            " --bundle-source \"" + pr.string() + "\"";
        return run_capture(cmd);
    };

    auto [rc_core_ok, out_core_ok] = run_tool(core_marker_ok, "core");
    if (rc_core_ok != 0) {
        std::cerr << "core marker file should accept bare use form\n" << out_core_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_core_no_marker, out_core_no_marker] = run_tool(core_no_marker, "core");
    if (rc_core_no_marker == 0 || !contains(out_core_no_marker, "literal (use substitution)")) {
        std::cerr << "core file without marker must reject bare use shorthand\n" << out_core_no_marker;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_non_core_marker, out_non_core_marker] = run_tool(non_core_marker, "app");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_non_core_marker == 0 ||
        !contains(out_non_core_marker, "$![Impl::Core]; is allowed only when bundle-name is 'core'")) {
        std::cerr << "non-core bundle must reject Impl::Core marker even with bare use shorthand\n" << out_non_core_marker;
        return false;
    }
    return true;
}

bool test_import_keyword_path_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-import-keyword-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "import proto as p;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write import-keyword rejection file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "import keyword path should fail\n" << out;
        return false;
    }
    if (!contains(out, "identifier (path segment)")) {
        std::cerr << "import keyword rejection must report identifier path segment expectation\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_local_non_variadic() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-local";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "Circle.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_CIRCLE_H\n"
        "#define PARUS_CIRCLE_H\n"
        "int c_add(int a, int b);\n"
        "#endif\n";
    const std::string main_src =
        "import \"Circle.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::c_add(1i32, 2i32);\n"
        "}\n";

    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write c-header import local test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        if (contains(out, "CImportLibClangUnavailable")) {
            return true;
        }
        std::cerr << "local non-variadic c-header import compile failed\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_stdio_variadic_fixed_arg_count_checked() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-stdio";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto stdio_h = temp_root / "stdio.h";
    const std::string header_src =
        "#ifndef PARUS_STDIO_H\n"
        "#define PARUS_STDIO_H\n"
        "int printf(const char* fmt, ...);\n"
        "#endif\n";
    const std::string main_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  stdio::printf();\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(stdio_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write c-header import stdio test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "stdio::printf() without fmt should fail\n" << out;
        return false;
    }
    if (!contains(out, "TypeArgCountMismatch")) {
        std::cerr << "stdio::printf() rejection must report fixed-arg count mismatch\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_stdio_variadic_requires_manual_abi() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-stdio-manual-abi";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto stdio_h = temp_root / "stdio.h";
    const std::string header_src =
        "#ifndef PARUS_STDIO_H\n"
        "#define PARUS_STDIO_H\n"
        "int printf(const char* fmt, ...);\n"
        "#endif\n";
    const std::string main_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  stdio::printf(\"%d\", 5i32);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(stdio_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write manual[abi] cimport test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "C variadic call without manual[abi] must fail\n" << out;
        return false;
    }
    if (!contains(out, "ManualAbiRequired")) {
        std::cerr << "C variadic call without manual[abi] must report ManualAbiRequired\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_stdio_variadic_zero_tail_no_manual_abi() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-stdio-zero-tail";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto stdio_h = temp_root / "stdio.h";
    const std::string header_src =
        "#ifndef PARUS_STDIO_H\n"
        "#define PARUS_STDIO_H\n"
        "int printf(const char* fmt, ...);\n"
        "#endif\n";
    const std::string main_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return stdio::printf(\"hello\\n\");\n"
        "}\n";
    if (!write_text(stdio_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write zero-tail cimport test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "zero-tail C variadic cimport call should compile without manual[abi]\n" << out;
        return false;
    }
    if (contains(out, "ManualAbiRequired")) {
        std::cerr << "zero-tail C variadic cimport call must not require manual[abi]\n" << out;
        return false;
    }
    return true;
}

bool test_iteration_array_loop_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-array-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  let xs: i32[3] = [1i32, 2i32, 3i32];\n"
        "  set mut s = 0i32;\n"
        "  loop(x in xs) {\n"
        "    s = s + x;\n"
        "  }\n"
        "  return s;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write array loop runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for array loop runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "array loop runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "array loop runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:6")) {
        std::cerr << "array loop runtime exit mismatch (expected 6)\n" << out_run;
        return false;
    }
    return true;
}

bool test_iteration_slice_loop_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-slice-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  let arr: i32[4] = [1i32, 2i32, 3i32, 4i32];\n"
        "  let xs: i32[] = arr[1i32..3i32];\n"
        "  set mut s = 0i32;\n"
        "  loop(x in xs) {\n"
        "    s = s + x;\n"
        "  }\n"
        "  return s;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write slice loop runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for slice loop runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "slice loop runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "slice loop runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:5")) {
        std::cerr << "slice loop runtime exit mismatch (expected 5)\n" << out_run;
        return false;
    }
    return true;
}

bool test_iteration_range_loop_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-range-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  set mut s = 0i32;\n"
        "  loop(i in 0i32..:3i32) {\n"
        "    s = s + i;\n"
        "  }\n"
        "  return s;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write range loop runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for range loop runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "range loop runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "range loop runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:6")) {
        std::cerr << "range loop runtime exit mismatch (expected 6)\n" << out_run;
        return false;
    }
    return true;
}

bool test_consume_binding_optional_escape_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-consume-binding-escape-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "static mut LOG: i32? = 17i32;\n"
        "static mut LOG2: i32? = 5i32;\n"
        "static mut FALLBACK: i32 = 9i32;\n"
        "\n"
        "def take_value() -> i32 {\n"
        "  let v: i32 = LOG else {\n"
        "    return 9i32;\n"
        "  };\n"
        "  return v;\n"
        "}\n"
        "\n"
        "def take_handle() -> ~i32 {\n"
        "  let v: i32 = LOG2 else {\n"
        "    return ~FALLBACK;\n"
        "  };\n"
        "  return ~v;\n"
        "}\n"
        "\n"
        "def sink(h: ~i32) -> i32 {\n"
        "  return 30i32;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  return take_value() + take_value() + sink(take_handle());\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write consume-binding escape runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for consume-binding escape runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "consume-binding + escape runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "consume-binding + escape runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:56")) {
        std::cerr << "consume-binding + escape runtime exit mismatch (expected 56)\n" << out_run;
        return false;
    }
    return true;
}

bool test_escape_first_class_places_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-first-class-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "proto Probe {\n"
        "  provide def probe() -> i32 {\n"
        "    return 40i32;\n"
        "  }\n"
        "};\n"
        "\n"
        "class Worker: Probe {\n"
        "  value: i32;\n"
        "\n"
        "  init(v: i32) {\n"
        "    self.value = v;\n"
        "  }\n"
        "\n"
        "  def run(self) -> i32 {\n"
        "    return self.value + 2i32;\n"
        "  }\n"
        "};\n"
        "\n"
        "class Holder {\n"
        "  value: ~Worker;\n"
        "\n"
        "  init(v: ~Worker) {\n"
        "    self.value = v;\n"
        "  }\n"
        "\n"
        "  def run(self) -> i32 {\n"
        "    return self.value.run() + self.value->probe();\n"
        "  }\n"
        "};\n"
        "\n"
        "class Box<T> with [T: Probe] {\n"
        "  value: ~T;\n"
        "\n"
        "  init(v: ~T) {\n"
        "    self.value = v;\n"
        "  }\n"
        "\n"
        "  def probe_value(self) -> i32 {\n"
        "    return self.value->probe();\n"
        "  }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def id_handle(h: ~Worker) -> ~Worker {\n"
        "  return h;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  let h: ~Worker = make(5i32);\n"
        "  set moved = id_handle(h);\n"
        "  let a: i32 = moved.run() + moved->probe();\n"
        "\n"
        "  set holder = Holder(make(7i32));\n"
        "  let b: i32 = holder.run();\n"
        "\n"
        "  let mut slot: (~Worker)? = make(9i32);\n"
        "  let taken: ~Worker = slot else {\n"
        "    return 1i32;\n"
        "  };\n"
        "  let c: i32 = taken.run() + taken->probe();\n"
        "\n"
        "  set box = Box<Worker>(make(11i32));\n"
        "  let d: i32 = box.probe_value();\n"
        "\n"
        "  return a + b + c + d;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write first-class ~ runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for first-class ~ runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "first-class ~ runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "first-class ~ runtime sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:187")) {
        std::cerr << "first-class ~ runtime exit mismatch (expected 187)\n" << out_run;
        return false;
    }
    return true;
}

bool test_iteration_unsupported_iterable_hard_error() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-unsupported-iter";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  let x: i32 = 1i32;\n"
        "  loop(v in x) {\n"
        "    return v;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write unsupported iterable test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "unsupported loop iterable must fail\n" << out;
        return false;
    }
    if (!contains(out, "LoopIterableUnsupported")) {
        std::cerr << "unsupported loop iterable must report LoopIterableUnsupported\n" << out;
        return false;
    }
    return true;
}

bool test_iteration_pure_infer_range_runtime_and_regressions() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-pure-infer-range";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_bad_mismatch = temp_root / "main_bad_mismatch.pr";
    const auto main_bad_float = temp_root / "main_bad_float.pr";
    const auto exe = temp_root / "main";
    const std::string main_ok_src =
        "import \"stdio.h\" as std;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut exclusive = 0i32;\n"
        "  set mut inclusive = 0i32;\n"
        "  loop(i in 1..4) {\n"
        "    exclusive = exclusive + i;\n"
        "  }\n"
        "  loop(i in 1..:4) {\n"
        "    inclusive = inclusive + i;\n"
        "    manual[abi] {\n"
        "      std::printf(\"%d\\n\", i);\n"
        "    }\n"
        "  }\n"
        "  return exclusive + inclusive;\n"
        "}\n";
    const std::string main_bad_mismatch_src =
        "def main() -> i32 {\n"
        "  loop(i in 1i32..:4i64) {\n"
        "    return i;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    const std::string main_bad_float_src =
        "def main() -> i32 {\n"
        "  loop(i in 1.0f64..:4.0f64) {\n"
        "    return 0i32;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_ok, main_ok_src) ||
        !write_text(main_bad_mismatch, main_bad_mismatch_src) ||
        !write_text(main_bad_float, main_bad_float_src)) {
        std::cerr << "failed to write pure infer range test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for pure infer range test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (contains(out_ok, "CImportLibClangUnavailable")) {
        std::filesystem::remove_all(temp_root, ec);
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "pure infer range loop sample should compile/link\n" << out_ok;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    if (rc_run != 0) {
        std::cerr << "pure infer range loop sample should execute\n" << out_run;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_mismatch, out_mismatch] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_bad_mismatch.string() + "\" -fsyntax-only");
    auto [rc_float, out_float] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_bad_float.string() + "\" -fsyntax-only");

    std::filesystem::remove_all(temp_root, ec);

    if (!contains(out_run, "1\n2\n3\n4\n")) {
        std::cerr << "pure infer inclusive range loop should print 1..4 in order\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:16")) {
        std::cerr << "pure infer range loop sample exit mismatch (expected 16)\n" << out_run;
        return false;
    }
    if (rc_mismatch == 0) {
        std::cerr << "typed-mismatch range loop must fail\n" << out_mismatch;
        return false;
    }
    if (!contains(out_mismatch, "LoopRangeBoundTypeMismatch")) {
        std::cerr << "typed-mismatch range loop must report LoopRangeBoundTypeMismatch\n" << out_mismatch;
        return false;
    }
    if (rc_float == 0) {
        std::cerr << "non-integer range loop must fail\n" << out_float;
        return false;
    }
    if (!contains(out_float, "LoopRangeBoundMustBeInteger")) {
        std::cerr << "non-integer range loop must report LoopRangeBoundMustBeInteger\n" << out_float;
        return false;
    }
    return true;
}

bool test_iteration_loop_binder_variadic_typedef_arg_compiles() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-binder-variadic-typedef";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto sizes_h = temp_root / "sizes.h";
    const auto stdio_h = temp_root / "stdio.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string sizes_src =
        "#ifndef PARUS_LOOP_BINDER_VARIADIC_SIZES_H\n"
        "#define PARUS_LOOP_BINDER_VARIADIC_SIZES_H\n"
        "typedef unsigned long parus_size;\n"
        "parus_size size_a(void);\n"
        "parus_size size_b(void);\n"
        "parus_size size_c(void);\n"
        "#endif\n";
    const std::string stdio_src =
        "#ifndef PARUS_LOOP_BINDER_VARIADIC_STDIO_H\n"
        "#define PARUS_LOOP_BINDER_VARIADIC_STDIO_H\n"
        "int printf(const char* fmt, ...);\n"
        "#endif\n";
    const std::string main_src =
        "import \"sizes.h\" as c;\n"
        "import \"stdio.h\" as std;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let xs: c::parus_size[3] = [c::size_a(), c::size_b(), c::size_c()];\n"
        "  loop(x in xs) {\n"
        "    manual[abi] {\n"
        "      std::printf(\"%zu \", x);\n"
        "    }\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(sizes_h, sizes_src) ||
        !write_text(stdio_h, stdio_src) ||
        !write_text(main_pr, main_src)) {
        std::cerr << "failed to write loop binder variadic typedef test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "loop binder value should remain valid as a C variadic typedef argument\n" << out;
        return false;
    }
    return true;
}

bool test_iteration_loop_break_value_context_syntax_only() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-loop-break-context";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "def take(x: i32?) -> i32 {\n"
        "  if (x == null) { return 0i32; }\n"
        "  return 42i32;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set inferred = loop (x in 1..:4) {\n"
        "    if (x == 4) {\n"
        "      break 42;\n"
        "    }\n"
        "  };\n"
        "  let inferred_value: i32 = inferred ?? 0;\n"
        "  let y: i32? = loop (x in 1..:4) {\n"
        "    if (x == 4) {\n"
        "      break 42;\n"
        "    }\n"
        "  };\n"
        "  let t: i32 = y ?? 0;\n"
        "  return inferred_value + t + take(loop {\n"
        "    break 42;\n"
        "  }) - 42i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write loop break context test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        std::cerr << "loop break infer-context syntax-only sample should succeed\n" << out;
        return false;
    }
    return true;
}

bool test_unsuffixed_array_literal_slice_runtime_and_llvm() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-unsuffixed-array-slice";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const auto llvm_out = temp_root / "main.ll";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  let xs: i32[] = [1, 2, 3, 4];\n"
        "  set mut s = 0i32;\n"
        "  loop(x in xs) {\n"
        "    s = s + x;\n"
        "  }\n"
        "  return s;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write unsuffixed array literal slice test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for unsuffixed array literal slice test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    auto [rc_ir, out_ir] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " -Xparus -emit-llvm-ir -o \"" + llvm_out.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "unsuffixed array literal slice sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    if (rc_ir != 0) {
        std::cerr << "unsuffixed array literal slice llvm-ir emission should succeed\n" << out_ir;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    const std::string llvm_ir = read_text(llvm_out);
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "unsuffixed array literal slice sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:10")) {
        std::cerr << "unsuffixed array literal slice exit mismatch (expected 10)\n" << out_run;
        return false;
    }
    if (!contains(llvm_ir, "[4 x i32]")) {
        std::cerr << "unsuffixed array literal slice llvm-ir must materialize [4 x i32]\n" << llvm_ir;
        return false;
    }
    if (contains(llvm_ir, "[4 x i64]")) {
        std::cerr << "unsuffixed array literal slice llvm-ir must not materialize [4 x i64]\n" << llvm_ir;
        return false;
    }
    if (!contains(llvm_ir, "store i32") || !contains(llvm_ir, "load i32")) {
        std::cerr << "unsuffixed array literal slice llvm-ir must use typed i32 stores/loads\n" << llvm_ir;
        return false;
    }
    if (contains(llvm_ir, "store i8")) {
        std::cerr << "unsuffixed array literal slice llvm-ir must not degrade element stores to i8\n" << llvm_ir;
        return false;
    }
    return true;
}

bool test_unsuffixed_named_array_to_slice_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-unsuffixed-array-named-slice";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  let arr: i32[4] = [1, 2, 3, 4];\n"
        "  let xs: i32[] = arr;\n"
        "  set mut s = 0i32;\n"
        "  loop(x in xs) {\n"
        "    s = s + x;\n"
        "  }\n"
        "  return s;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write unsuffixed named array slice test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for unsuffixed named array slice test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "unsuffixed named array slice sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "unsuffixed named array slice sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:10")) {
        std::cerr << "unsuffixed named array slice exit mismatch (expected 10)\n" << out_run;
        return false;
    }
    return true;
}

bool test_unsuffixed_array_literal_call_arg_and_return_contexts() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-unsuffixed-array-call-return";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string main_src =
        "def take(xs: i32[]) -> i32 {\n"
        "  set mut s = 0i32;\n"
        "  loop(x in xs) {\n"
        "    s = s + x;\n"
        "  }\n"
        "  return s;\n"
        "}\n"
        "\n"
        "def mk() -> i32[3] {\n"
        "  return [1, 2, 3];\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  let xs: i32[3] = mk();\n"
        "  return take([1, 2, 3]) + xs[1i32];\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write unsuffixed array call/return test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for unsuffixed array call/return test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "unsuffixed array call/return sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"; echo EXIT:$?");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "unsuffixed array call/return sample should execute\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:8")) {
        std::cerr << "unsuffixed array call/return exit mismatch (expected 8)\n" << out_run;
        return false;
    }
    return true;
}

bool test_unsuffixed_array_literal_float_context_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-unsuffixed-array-float-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "def main() -> i32 {\n"
        "  let xs: f32[] = [1, 2, 3];\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write unsuffixed array float reject test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "unsuffixed integer array literal must not coerce to float array context\n" << out;
        return false;
    }
    if (!contains(out, "IntToFloatNotAllowed")) {
        std::cerr << "unsuffixed array float rejection must report IntToFloatNotAllowed\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_stdio_format_bridge_single_arg() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-stdio-bridge";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_ok_infer = temp_root / "main_ok_infer.pr";
    const auto main_fail = temp_root / "main_fail.pr";
    const auto main_text_fail = temp_root / "main_text_fail.pr";
    const auto llvm_out = temp_root / "printf_call.ll";
    const auto stdio_h = temp_root / "stdio.h";
    const std::string header_src =
        "#ifndef PARUS_STDIO_H\n"
        "#define PARUS_STDIO_H\n"
        "int printf(const char* fmt, ...);\n"
        "#endif\n";
    const std::string ok_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  manual[abi] {\n"
        "    stdio::printf(\"%d\", 5i32);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    const std::string ok_infer_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  manual[abi] {\n"
        "    stdio::printf(\"%d\", 5);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    const std::string fail_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  manual[abi] {\n"
        "    stdio::printf($\"sum={1i32 + 2i32}\");\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    const std::string text_fail_src =
        "import \"stdio.h\" as stdio;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let msg: text = \"hello\";\n"
        "  manual[abi] {\n"
        "    stdio::printf(msg);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(stdio_h, header_src) ||
        !write_text(main_ok, ok_src) ||
        !write_text(main_ok_infer, ok_infer_src) ||
        !write_text(main_fail, fail_src) ||
        !write_text(main_text_fail, text_fail_src)) {
        std::cerr << "failed to write c-header format bridge test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\" -fsyntax-only");
    auto [rc_ok_infer, out_ok_infer] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok_infer.string() + "\" -fsyntax-only");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    auto [rc_text_fail, out_text_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_text_fail.string() + "\" -fsyntax-only");
    auto [rc_ir, out_ir_cmd] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() +
        "\" -Xparus -emit-llvm-ir -o \"" + llvm_out.string() + "\"");
    const std::string llvm_ir = read_text(llvm_out);
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable") ||
        contains(out_ok_infer, "CImportLibClangUnavailable") ||
        contains(out_fail, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "stdio::printf(\"%d\", 5i32) should typecheck\n" << out_ok;
        return false;
    }
    if (rc_ok_infer != 0) {
        std::cerr << "stdio::printf(\"%d\", 5) should typecheck with infer-int promotion\n" << out_ok_infer;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "stdio::printf($\"...\") should be rejected in C ABI call\n" << out_fail;
        return false;
    }
    if (!contains(out_fail, "BareDollarStringRemoved")) {
        std::cerr << "stdio::printf($\"...\") rejection must report BareDollarStringRemoved\n" << out_fail;
        return false;
    }
    if (rc_ir != 0) {
        std::cerr << "stdio::printf llvm-ir emission should succeed\n" << out_ir_cmd;
        return false;
    }
    if (rc_text_fail == 0) {
        std::cerr << "stdio::printf(text) should be rejected at C ABI boundary\n" << out_text_fail;
        return false;
    }
    if (!contains(out_text_fail, "text value is not C ABI-safe; use *const core::ext::c_char")) {
        std::cerr << "stdio::printf(text) rejection must provide explicit C boundary guidance\n" << out_text_fail;
        return false;
    }
    if (!contains(llvm_ir, "call i32 (ptr, ...) @printf(")) {
        std::cerr << "variadic call must be emitted as typed-call form\n" << llvm_ir;
        return false;
    }
    if (contains(llvm_ir, "call i32 @printf(")) {
        std::cerr << "variadic call must not be emitted as untyped direct call\n" << llvm_ir;
        return false;
    }
    return true;
}

bool test_extern_c_variadic_manual_abi_and_null_boundary() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-extern-c-variadic";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_fail = temp_root / "main_fail.pr";
    const auto llvm_out = temp_root / "main_ok.ll";
    const std::string ok_src =
        "extern \"C\" def take_ptr(p: *const i32) -> i32;\n"
        "extern \"C\" def variad(first: *const i8, ...) -> i32;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let p: *const i32 = null;\n"
        "  take_ptr(null);\n"
        "  take_ptr(p);\n"
        "  let z: i32 = variad(null);\n"
        "  manual[abi] {\n"
        "    variad(null, 5);\n"
        "  }\n"
        "  return z;\n"
        "}\n";
    const std::string fail_src =
        "extern \"C\" def variad(first: *const i8, ...) -> i32;\n"
        "\n"
        "def main() -> i32 {\n"
        "  manual[abi] {\n"
        "    variad(null, null);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_ok, ok_src) || !write_text(main_fail, fail_src)) {
        std::cerr << "failed to write extern C variadic test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() +
        "\" -Xparus -emit-llvm-ir -o \"" + llvm_out.string() + "\"");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    const std::string llvm_ir = read_text(llvm_out);
    std::filesystem::remove_all(temp_root, ec);

    if (rc_ok != 0) {
        std::cerr << "extern C variadic declaration with manual[abi] and null->*const boundary should compile\n" << out_ok;
        return false;
    }
    if (contains(out_ok, "ManualAbiRequired")) {
        std::cerr << "zero-tail extern C variadic call must not require manual[abi]\n" << out_ok;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "null literal in variadic tail must fail\n" << out_fail;
        return false;
    }
    if (!contains(out_fail, "CImportVariadicArgTypeUnsupported")) {
        std::cerr << "null literal in variadic tail must report variadic arg type rejection\n" << out_fail;
        return false;
    }
    if (!contains(llvm_ir, "call i32 (ptr, ...) @variad(")) {
        std::cerr << "extern C variadic call must lower as typed variadic call\n" << llvm_ir;
        return false;
    }
    return true;
}

bool test_c_header_import_variadic_function_pointer_zero_tail_calls_without_manual_abi() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-variadic-fnptr-zero-tail";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "F.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_VARIADIC_FPTR_ZERO_TAIL_H\n"
        "#define PARUS_VARIADIC_FPTR_ZERO_TAIL_H\n"
        "typedef int (*PrintfLike)(const char*, ...);\n"
        "extern PrintfLike global_logger;\n"
        "typedef struct LoggerBox {\n"
        "  PrintfLike log;\n"
        "} LoggerBox;\n"
        "PrintfLike get_logger(void);\n"
        "LoggerBox get_box(void);\n"
        "#endif\n";
    const std::string main_src =
        "import \"F.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set f = c::get_logger();\n"
        "  set box = c::get_box();\n"
        "  set a = f(\"one\\n\");\n"
        "  set b = c::global_logger(\"two\\n\");\n"
        "  set d = box.log(\"three\\n\");\n"
        "  return a + b + d;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write zero-tail variadic function pointer cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "zero-tail variadic function pointer calls should compile without manual[abi]\n" << out;
        return false;
    }
    if (contains(out, "ManualAbiRequired")) {
        std::cerr << "zero-tail variadic function pointer calls must not require manual[abi]\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_cstr_runtime_prints_consistent_output() {
    const std::string bin = PARUS_BUILD_BIN;
    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot for cstr runtime print test\n";
        return false;
    }
    const auto& [sysroot, _target] = *sysroot_and_target;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-cstr-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto stdio_h = temp_root / "stdio.h";
    const auto exe = temp_root / "main.out";
    const std::string header_src =
        "#ifndef PARUS_STDIO_H\n"
        "#define PARUS_STDIO_H\n"
        "int printf(const char* fmt, ...);\n"
        "#endif\n";
    const std::string main_src =
        "import \"stdio.h\" as c;\n"
        "import ext as ext;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set x = ext::from_raw_parts(c\"Hello, World!!\", 15usize);\n"
        "  manual[abi] {\n"
        "    c::printf(x);\n"
        "    c::printf(\"\\n\");\n"
        "    c::printf(\"%s\\n\", x);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(stdio_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write cstr runtime print test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_build, out_build] = run_capture(
        "PARUS_NO_CORE=0 \"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\"" +
        " --sysroot \"" + sysroot + "\"" +
        " -o \"" + exe.string() + "\"");
    if (contains(out_build, "CImportLibClangUnavailable")) {
        std::filesystem::remove_all(temp_root, ec);
        return rc_build != 0;
    }
    if (rc_build != 0) {
        std::cerr << "cstr runtime print sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "cstr runtime print sample should execute successfully\n" << out_run;
        return false;
    }

    const std::string expected = "Hello, World!!\nHello, World!!\n";
    if (out_run != expected) {
        std::cerr << "cstr runtime print output mismatch\n"
                  << "expected:\n" << expected
                  << "actual:\n" << out_run;
        return false;
    }
    return true;
}

bool test_c_header_import_include_dir_option() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-include-dir";
    const auto include_dir = temp_root / "include";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(include_dir, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = include_dir / "Math.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_MATH_H\n"
        "#define PARUS_MATH_H\n"
        "int c_add(int a, int b);\n"
        "#endif\n";
    const std::string main_src =
        "import \"Math.h\" as m;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return m::c_add(1i32, 2i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write include-dir cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- -I \"" + include_dir.string() + "\" \"" +
        main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "-I include-dir cimport should compile\n" << out;
        return false;
    }
    return true;
}

bool test_lei_module_bundle_cimport_isystem_option() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-lei-cimport-isystem";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto include_a = temp_root / "include_a";
    const auto include_b = temp_root / "include_b";
    const auto main_pr = temp_root / "main.pr";
    const auto lei = temp_root / "config.lei";
    if (!std::filesystem::create_directories(include_a, ec) || !std::filesystem::create_directories(include_b, ec) || ec) {
        std::cerr << "failed to create include dirs for lei cimport isystem test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string a_h =
        "#ifndef PARUS_CIMPORT_A_H\n"
        "#define PARUS_CIMPORT_A_H\n"
        "#define PARUS_A_VAL 10\n"
        "#endif\n";
    const std::string b_h =
        "#ifndef PARUS_CIMPORT_B_H\n"
        "#define PARUS_CIMPORT_B_H\n"
        "#include <A.h>\n"
        "int c_add(int a, int b);\n"
        "#endif\n";
    const std::string pr =
        "import \"B.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::c_add(1i32, 2i32);\n"
        "}\n";
    const std::string lei_src =
        "plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  cimport = {\n"
        "    isystem: [\"include_a\"],\n"
        "  };\n"
        "  modules = [\n"
        "    module & {\n"
        "      sources = [\"main.pr\"];\n"
        "      imports = [];\n"
        "      cimport = {\n"
        "        isystem: [\"include_b\"],\n"
        "      };\n"
        "    },\n"
        "  ];\n"
        "  deps = [];\n"
        "};\n"
        "\n"
        "plan master = master & {\n"
        "  project = {\n"
        "    name: \"lei-cimport-isystem\",\n"
        "    version: \"0.1.0\",\n"
        "  };\n"
        "  bundles = [app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";
    if (!write_text(include_a / "A.h", a_h) ||
        !write_text(include_b / "B.h", b_h) ||
        !write_text(main_pr, pr) ||
        !write_text(lei, lei_src)) {
        std::cerr << "failed to write lei cimport isystem fixture files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + lei.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "lei module/bundle cimport.isystem should compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_macos_opengl_isystem() {
#if !defined(__APPLE__)
    return true;
#else
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-macos-opengl";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    std::optional<std::filesystem::path> gl_headers{};
    const std::vector<std::filesystem::path> header_candidates = {
        "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/OpenGL.framework/Headers",
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/OpenGL.framework/Headers",
    };
    for (const auto& p : header_candidates) {
        if (std::filesystem::exists(p / "gl3.h")) {
            gl_headers = p;
            break;
        }
    }
    if (!gl_headers.has_value()) {
        std::cerr << "skip: macOS OpenGL SDK headers were not found for cimport isystem test\n";
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    std::optional<std::filesystem::path> sdk_usr_include{};
    const std::vector<std::filesystem::path> sdk_candidates = {
        "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include",
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include",
    };
    for (const auto& p : sdk_candidates) {
        if (std::filesystem::exists(p / "stdint.h")) {
            sdk_usr_include = p;
            break;
        }
    }
    if (!sdk_usr_include.has_value()) {
        std::cerr << "skip: macOS SDK usr/include not found for OpenGL cimport test\n";
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    // SDK OpenGL headers are framework-layout; build an include overlay for -isystem.
    const auto overlay_root = temp_root / "include-overlay";
    std::filesystem::create_directories(overlay_root, ec);
    if (ec) {
        std::cerr << "failed to create include overlay root\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    std::filesystem::create_directory_symlink(*gl_headers, overlay_root / "OpenGL", ec);
    if (ec) {
        std::cerr << "failed to create OpenGL overlay symlink\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "import \"OpenGL/gl3.h\" as gl;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mode = gl::GL_TRIANGLES;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write macOS OpenGL cimport test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- -isystem \"" + overlay_root.string() +
        "\" -isystem \"" + sdk_usr_include->string() + "\" \"" +
        main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "macOS OpenGL header import with -isystem should compile\n" << out;
        return false;
    }
    return true;
#endif
}

bool test_c_header_import_macos_moltenvk_isystem() {
#if !defined(__APPLE__)
    return true;
#else
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-macos-moltenvk";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    std::optional<std::filesystem::path> include_root{};
    const std::vector<std::filesystem::path> candidates = {
        "/opt/homebrew/include",
        "/usr/local/include",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p / "MoltenVK/mvk_vulkan.h")) {
            include_root = p;
            break;
        }
    }
    if (!include_root.has_value()) {
        std::cerr << "skip: MoltenVK headers were not found for cimport isystem test\n";
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    std::optional<std::filesystem::path> sdk_usr_include{};
    const std::vector<std::filesystem::path> sdk_candidates = {
        "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include",
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include",
    };
    for (const auto& p : sdk_candidates) {
        if (std::filesystem::exists(p / "Availability.h")) {
            sdk_usr_include = p;
            break;
        }
    }
    if (!sdk_usr_include.has_value()) {
        std::cerr << "skip: macOS SDK usr/include not found for MoltenVK cimport test\n";
        std::filesystem::remove_all(temp_root, ec);
        return true;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "import \"MoltenVK/mvk_vulkan.h\" as mvk;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set ok = mvk::VK_SUCCESS;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write MoltenVK cimport test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- -isystem \"" + include_root->string() +
        "\" -isystem \"" + sdk_usr_include->string() + "\" \"" +
        main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "MoltenVK header import with -isystem should compile\n" << out;
        return false;
    }
    return true;
#endif
}

bool test_c_header_import_union_manual_get_gate() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-union-get";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "U.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_UNION_U_H\n"
        "#define PARUS_UNION_U_H\n"
        "union U { int a; int b; };\n"
        "union U make_u(void);\n"
        "#endif\n";
    const std::string main_src =
        "import \"U.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set u = c::make_u();\n"
        "  set x = u.a;\n"
        "  return x;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write union get-gate cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "union field read outside manual[get|set] must fail\n" << out;
        return false;
    }
    if (!contains(out, "manual[get] or manual[set]")) {
        std::cerr << "union read gate diagnostic mismatch\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_union_manual_set_gate() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-union-set";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "U.h";
    const auto main_fail = temp_root / "main_fail.pr";
    const auto main_ok = temp_root / "main_ok.pr";
    const std::string header_src =
        "#ifndef PARUS_UNION_U_H\n"
        "#define PARUS_UNION_U_H\n"
        "union U { int a; int b; };\n"
        "union U make_u(void);\n"
        "#endif\n";
    const std::string fail_src =
        "import \"U.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut u = c::make_u();\n"
        "  manual[get] {\n"
        "    u.a = 1i32;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    const std::string ok_src =
        "import \"U.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut u = c::make_u();\n"
        "  manual[set] {\n"
        "    u.a = 1i32;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_fail, fail_src) || !write_text(main_ok, ok_src)) {
        std::cerr << "failed to write union set-gate cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_fail, "CImportLibClangUnavailable") || contains(out_ok, "CImportLibClangUnavailable")) {
        return rc_fail != 0;
    }
    if (rc_fail == 0) {
        std::cerr << "union write with manual[get] must fail\n" << out_fail;
        return false;
    }
    if (!contains(out_fail, "manual[set]")) {
        std::cerr << "union write gate diagnostic mismatch\n" << out_fail;
        return false;
    }
    if (rc_ok != 0) {
        std::cerr << "union write with manual[set] should pass\n" << out_ok;
        return false;
    }
    return true;
}

bool test_c_header_import_struct_borrow_escape_rules() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-struct-cap";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "S.h";
    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_fail = temp_root / "main_fail.pr";
    const std::string header_src =
        "#ifndef PARUS_STRUCT_S_H\n"
        "#define PARUS_STRUCT_S_H\n"
        "struct S { int x; };\n"
        "struct S make_s(void);\n"
        "#endif\n";
    const std::string ok_src =
        "import \"S.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set s = c::make_s();\n"
        "  set r = &s;\n"
        "  set m = ~s;\n"
        "  return 0i32;\n"
        "}\n";
    const std::string fail_src =
        "import \"S.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set s = c::make_s();\n"
        "  set r = &s;\n"
        "  set rr = &r;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) ||
        !write_text(main_ok, ok_src) ||
        !write_text(main_fail, fail_src)) {
        std::cerr << "failed to write struct capability cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\" -fsyntax-only");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable") || contains(out_fail, "CImportLibClangUnavailable")) {
        return rc_ok != 0 || rc_fail != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "borrow/escape on C imported struct value should pass\n" << out_ok;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "re-borrow from borrow value must fail\n" << out_fail;
        return false;
    }
    if (!contains(out_fail, "BorrowOperandMustBeOwnedPlace")) {
        std::cerr << "borrow rule diagnostic mismatch for C imported value path\n" << out_fail;
        return false;
    }
    return true;
}

bool test_c_header_import_enum_constant_usage() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-enum-const";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "E.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_ENUM_E_H\n"
        "#define PARUS_ENUM_E_H\n"
        "enum Color { RED = 3, BLUE = 7 };\n"
        "#endif\n";
    const std::string main_src =
        "import \"E.h\" as c;\n"
        "\n"
        "def main() -> u32 {\n"
        "  return c::RED;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write enum-constant cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "c-import enum constant should compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_global_and_tls_usage() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-global-tls";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "G.h";
    const auto main_pr = temp_root / "main.pr";
    const auto llvm_out = temp_root / "main.ll";
    const std::string header_src =
        "#ifndef PARUS_GLOBAL_G_H\n"
        "#define PARUS_GLOBAL_G_H\n"
        "extern int g_value;\n"
        "extern _Thread_local int g_tls;\n"
        "#endif\n";
    const std::string main_src =
        "import \"G.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  c::g_value = 1i32;\n"
        "  c::g_tls = 2i32;\n"
        "  set x = c::g_value;\n"
        "  set y = c::g_tls;\n"
        "  return x + y;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write cimport global/tls test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    auto [rc_ir, out_ir] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() +
        "\" -Xparus -emit-llvm-ir -o \"" + llvm_out.string() + "\"");
    const std::string llvm_ir = read_text(llvm_out);
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable") || contains(out_ir, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "c-import global/tls use should typecheck\n" << out_ok;
        return false;
    }
    if (rc_ir != 0) {
        std::cerr << "c-import global/tls llvm-ir emission should succeed\n" << out_ir;
        return false;
    }
    if (!contains(llvm_ir, "@g_tls = thread_local external global i32")) {
        std::cerr << "imported C TLS global must lower as thread_local external global\n" << llvm_ir;
        return false;
    }
    return true;
}

bool test_c_header_import_const_global_write_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-const-global";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "G.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_CONST_GLOBAL_G_H\n"
        "#define PARUS_CONST_GLOBAL_G_H\n"
        "extern const int g_ro;\n"
        "#endif\n";
    const std::string main_src =
        "import \"G.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  c::g_ro = 1i32;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write cimport const-global test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "assigning imported const C global must fail\n" << out;
        return false;
    }
    if (!contains(out, "WriteToImmutable")) {
        std::cerr << "const C global write rejection must report immutable-write diagnostic\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_define_undefine_options() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-def-undef";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "Cfg.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_CFG_H\n"
        "#define PARUS_CFG_H\n"
        "#ifdef FEATURE_X\n"
        "#define CCFG_VALUE 77\n"
        "#endif\n"
        "#endif\n";
    const std::string main_src =
        "import \"Cfg.h\" as c;\n"
        "\n"
        "def main() -> i64 {\n"
        "  return c::CCFG_VALUE;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write define/undef cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- -DFEATURE_X \"" + main_pr.string() + "\" -fsyntax-only");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- -DFEATURE_X -UFEATURE_X \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable") || contains(out_fail, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "-D FEATURE_X should make macro import available\n" << out_ok;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "-U FEATURE_X should make macro unavailable\n" << out_fail;
        return false;
    }
    return true;
}

bool test_c_header_import_imacros_option() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-imacros";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto macro_h = temp_root / "Defs.h";
    const auto header_h = temp_root / "Cfg.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string defs_src =
        "#ifndef PARUS_DEFS_H\n"
        "#define PARUS_DEFS_H\n"
        "#define FEATURE_IMACROS 1\n"
        "#endif\n";
    const std::string header_src =
        "#ifndef PARUS_CFG_H\n"
        "#define PARUS_CFG_H\n"
        "#ifdef FEATURE_IMACROS\n"
        "#define CCFG_IMA 9\n"
        "#endif\n"
        "#endif\n";
    const std::string main_src =
        "import \"Cfg.h\" as c;\n"
        "\n"
        "def main() -> i64 {\n"
        "  return c::CCFG_IMA;\n"
        "}\n";
    if (!write_text(macro_h, defs_src) || !write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write imacros cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- -imacros \"" + macro_h.string() + "\" \"" +
        main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "-imacros should affect cimport preprocessing\n" << out_ok;
        return false;
    }
    return true;
}

bool test_c_header_import_forced_include_option() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-include-preprocess";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto force_h = temp_root / "Force.h";
    const auto header_h = temp_root / "Cfg.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string force_src =
        "#ifndef PARUS_FORCE_H\n"
        "#define PARUS_FORCE_H\n"
        "#define FEATURE_INCLUDE 1\n"
        "#endif\n";
    const std::string header_src =
        "#ifndef PARUS_CFG_H\n"
        "#define PARUS_CFG_H\n"
        "#ifdef FEATURE_INCLUDE\n"
        "#define CCFG_INC 123\n"
        "#endif\n"
        "#endif\n";
    const std::string main_src =
        "import \"Cfg.h\" as c;\n"
        "\n"
        "def main() -> i64 {\n"
        "  return c::CCFG_INC;\n"
        "}\n";
    if (!write_text(force_h, force_src) || !write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write forced-include cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "\"" + bin + "\" tool parusc -- -include \"" + force_h.string() + "\" \"" +
        main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "-include should affect cimport preprocessing\n" << out_ok;
        return false;
    }
    return true;
}

bool test_c_header_import_anonymous_typedef_struct_usage() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-anon-typedef-struct";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "Anon.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_ANON_H\n"
        "#define PARUS_ANON_H\n"
        "typedef struct { int x; int y; } Point;\n"
        "Point make_point(void);\n"
        "#endif\n";
    const std::string main_src =
        "import \"Anon.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set p = c::make_point();\n"
        "  return p.x;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write anonymous typedef struct cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "anonymous typedef struct import should compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_transparent_typedef_uint32_assign() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-typedef-transparent-u32";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string main_src =
        "import \"stdint.h\" as std;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let a: std::uint32_t = 10;\n"
        "  let b: std::uint32_t = 10u32;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, main_src)) {
        std::cerr << "failed to write transparent typedef uint32 cimport test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "transparent typedef uint32 assignment should compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_nominal_typedef_record_stays_nominal() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-typedef-nominal-record";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "Nominal.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_NOMINAL_H\n"
        "#define PARUS_NOMINAL_H\n"
        "typedef struct { int x; } Nominal;\n"
        "#endif\n";
    const std::string main_src =
        "import \"Nominal.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let x: c::Nominal = 10i32;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write nominal typedef cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "nominal typedef record assignment must fail\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_pointer_alias_call() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fnptr-alias";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "F.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_F_H\n"
        "#define PARUS_F_H\n"
        "typedef int (*Adder)(int, int);\n"
        "Adder get_adder(void);\n"
        "#endif\n";
    const std::string main_src =
        "import \"F.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set f = c::get_adder();\n"
        "  return f(1i32, 2i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write function pointer alias cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "function pointer typedef alias call should compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_variadic_function_pointer_alias_requires_manual_abi_cache_hit() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-variadic-fnptr-alias";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "F.h";
    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_fail = temp_root / "main_fail.pr";
    const auto llvm_out = temp_root / "main_ok.ll";
    const std::string header_src =
        "#ifndef PARUS_VARIADIC_FPTR_H\n"
        "#define PARUS_VARIADIC_FPTR_H\n"
        "typedef int (*PrintfLike)(const char*, ...);\n"
        "typedef PrintfLike PrintfAlias;\n"
        "PrintfAlias get_logger(void);\n"
        "#endif\n";
    const std::string ok_src =
        "import \"F.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set f = c::get_logger();\n"
        "  set out = 0i32;\n"
        "  manual[abi] {\n"
        "    set out = f(\"%d\", 5i32);\n"
        "  }\n"
        "  return out;\n"
        "}\n";
    const std::string fail_src =
        "import \"F.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set f = c::get_logger();\n"
        "  return f(\"%d\", 5i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) ||
        !write_text(main_ok, ok_src) ||
        !write_text(main_fail, fail_src)) {
        std::cerr << "failed to write variadic function pointer alias cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok_first, out_ok_first] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() +
        "\" -Xparus -emit-llvm-ir -o \"" + llvm_out.string() + "\"");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    auto [rc_ok_second, out_ok_second] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\" -fsyntax-only");
    const std::string llvm_ir = read_text(llvm_out);
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok_first, "CImportLibClangUnavailable") ||
        contains(out_fail, "CImportLibClangUnavailable") ||
        contains(out_ok_second, "CImportLibClangUnavailable")) {
        return rc_ok_first != 0;
    }
    if (rc_ok_first != 0) {
        std::cerr << "variadic function pointer typedef alias call should compile with manual[abi]\n" << out_ok_first;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "variadic function pointer alias call without manual[abi] must fail\n" << out_fail;
        return false;
    }
    if (!contains(out_fail, "ManualAbiRequired")) {
        std::cerr << "variadic function pointer alias call must report ManualAbiRequired\n" << out_fail;
        return false;
    }
    if (rc_ok_second != 0) {
        std::cerr << "cache-hit variadic function pointer alias compile should succeed\n" << out_ok_second;
        return false;
    }
    if (!contains(llvm_ir, "call i32 (ptr, ...) %")) {
        std::cerr << "indirect variadic alias call must lower as typed variadic indirect call\n" << llvm_ir;
        return false;
    }
    return true;
}

bool test_c_header_import_variadic_function_pointer_global_and_field_calls() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-variadic-fnptr-global-field";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "F.h";
    const auto main_global = temp_root / "main_global.pr";
    const auto main_field = temp_root / "main_field.pr";
    const std::string header_src =
        "#ifndef PARUS_VARIADIC_FPTR_GLOBAL_FIELD_H\n"
        "#define PARUS_VARIADIC_FPTR_GLOBAL_FIELD_H\n"
        "typedef int (*PrintfLike)(const char*, ...);\n"
        "extern PrintfLike global_logger;\n"
        "typedef struct LoggerBox {\n"
        "  PrintfLike log;\n"
        "} LoggerBox;\n"
        "LoggerBox get_box(void);\n"
        "#endif\n";
    const std::string global_src =
        "import \"F.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set out = 0i32;\n"
        "  manual[abi] {\n"
        "    set out = c::global_logger(\"%d\", 1i32);\n"
        "  }\n"
        "  return out;\n"
        "}\n";
    const std::string field_src =
        "import \"F.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set box = c::get_box();\n"
        "  set out = 0i32;\n"
        "  manual[abi] {\n"
        "    set out = box.log(\"%d\", 2i32);\n"
        "  }\n"
        "  return out;\n"
        "}\n";
    if (!write_text(header_h, header_src) ||
        !write_text(main_global, global_src) ||
        !write_text(main_field, field_src)) {
        std::cerr << "failed to write variadic global/field function pointer cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_global, out_global] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_global.string() + "\" -fsyntax-only");
    auto [rc_field, out_field] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_field.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_global, "CImportLibClangUnavailable") ||
        contains(out_field, "CImportLibClangUnavailable")) {
        return rc_global != 0;
    }
    if (rc_global != 0) {
        std::cerr << "variadic imported global function pointer call should compile\n" << out_global;
        return false;
    }
    if (rc_field != 0) {
        std::cerr << "variadic imported struct field function pointer call should compile\n" << out_field;
        return false;
    }
    return true;
}

bool test_c_header_import_dropped_global_decl_preserves_supported_imports() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-drop-global";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "Drop.h";
    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_fail = temp_root / "main_fail.pr";
    const std::string header_src =
        "#ifndef PARUS_DROP_GLOBAL_H\n"
        "#define PARUS_DROP_GLOBAL_H\n"
        "int ok_fn(void);\n"
        "extern int bad_global[4];\n"
        "#endif\n";
    const std::string ok_src =
        "import \"Drop.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::ok_fn();\n"
        "}\n";
    const std::string fail_src =
        "import \"Drop.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set x = c::bad_global;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) ||
        !write_text(main_ok, ok_src) ||
        !write_text(main_fail, fail_src)) {
        std::cerr << "failed to write dropped global cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "PARUS_CIMPORT_REPORT=1 \"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\" -fsyntax-only");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable") || contains(out_fail, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "supported decls must remain importable when one global is dropped\n" << out_ok;
        return false;
    }
    if (!contains(out_ok, "dropped global 'bad_global'") || !contains(out_ok, "unsupported_global_type")) {
        std::cerr << "dropped global import must report reason and reason code\n" << out_ok;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "referencing dropped global decl must fail\n" << out_fail;
        return false;
    }
    return true;
}

bool test_c_header_import_dropped_owner_record_preserves_supported_imports() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-drop-record";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "Drop.h";
    const auto main_ok = temp_root / "main_ok.pr";
    const auto main_fail = temp_root / "main_fail.pr";
    const std::string header_src =
        "#ifndef PARUS_DROP_RECORD_H\n"
        "#define PARUS_DROP_RECORD_H\n"
        "typedef struct BadBox {\n"
        "  int data[4];\n"
        "} BadBox;\n"
        "int ok_fn(void);\n"
        "#endif\n";
    const std::string ok_src =
        "import \"Drop.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::ok_fn();\n"
        "}\n";
    const std::string fail_src =
        "import \"Drop.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let x: c::BadBox = 0i32;\n"
        "  return c::ok_fn();\n"
        "}\n";
    if (!write_text(header_h, header_src) ||
        !write_text(main_ok, ok_src) ||
        !write_text(main_fail, fail_src)) {
        std::cerr << "failed to write dropped record cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_ok, out_ok] = run_capture(
        "PARUS_CIMPORT_REPORT=1 \"" + bin + "\" tool parusc -- \"" + main_ok.string() + "\" -fsyntax-only");
    auto [rc_fail, out_fail] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_fail.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out_ok, "CImportLibClangUnavailable") || contains(out_fail, "CImportLibClangUnavailable")) {
        return rc_ok != 0;
    }
    if (rc_ok != 0) {
        std::cerr << "supported decls must remain importable when one record owner is dropped\n" << out_ok;
        return false;
    }
    if (!contains(out_ok, "dropped struct 'BadBox'") || !contains(out_ok, "unsupported_field_type")) {
        std::cerr << "dropped record import must report owner-drop reason and code\n" << out_ok;
        return false;
    }
    if (rc_fail == 0) {
        std::cerr << "materializing dropped owner record type must fail\n" << out_fail;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_not_imported() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_H\n"
        "#define PARUS_M_H\n"
        "#define ADD2(x, y) ((x) + (y))\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::ADD2;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write function-like macro test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "function-like macro should not be imported as constant symbol\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_direct_alias_call() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-direct-alias";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_DIRECT_ALIAS_H\n"
        "#define PARUS_M_DIRECT_ALIAS_H\n"
        "int c_add(int a, int b);\n"
        "#define CADD(a, b) c_add(a, b)\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::CADD(1i32, 2i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write direct-alias function-like macro test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "direct alias function-like macro call should compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_shim_link_success() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-shim-link";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const auto out_exe = temp_root / "main";
    const std::string header_src =
        "#ifndef PARUS_M_SHIM_LINK_H\n"
        "#define PARUS_M_SHIM_LINK_H\n"
        "int abs(int x);\n"
        "#define ABS_WRAP(x) abs((int) x)\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::ABS_WRAP(5i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write shim-link function-like macro test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for cimport shim link test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + out_exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "shim-forward function-like macro link should succeed\n" << out;
        return false;
    }
    if (!contains(out, "linked executable to")) {
        std::cerr << "shim-forward function-like macro link must emit success message\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_skip_warning() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-skip-warning";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_SKIP_WARN_H\n"
        "#define PARUS_M_SKIP_WARN_H\n"
        "#define ADD2(x, y) ((x) + (y))\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write function-like macro skip warning test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "skipped function-like macro should not fail c-import\n" << out;
        return false;
    }
    if (!contains(out, "CImportFnMacroSkipped")) {
        std::cerr << "skipped function-like macro must report warning code CImportFnMacroSkipped\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_ir_only_supported() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-shim-ir-only";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const auto out_ll = temp_root / "main.ll";
    const std::string header_src =
        "#ifndef PARUS_M_SHIM_IR_ONLY_H\n"
        "#define PARUS_M_SHIM_IR_ONLY_H\n"
        "int abs(int x);\n"
        "#define ABS_WRAP(x) abs((int) x)\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::ABS_WRAP(5i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write shim ir-only rejection test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- -Xparus -emit-llvm-ir \"" + main_pr.string() +
        "\" -o \"" + out_ll.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "IR-only emit must support cimport wrapper lowering without C shim\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_chain_promoted() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-chain";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_CHAIN_H\n"
        "#define PARUS_M_CHAIN_H\n"
        "int c_add(int a, int b);\n"
        "#define INNER_ADD(a, b) c_add(a, b)\n"
        "#define OUTER_ADD(a, b) INNER_ADD(a, b)\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::OUTER_ADD(1i32, 2i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write macro-chain cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "function-like macro chain should be promoted and compile\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_object_macro_const_expr_resolved() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-object-macro-const-expr";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_OBJECT_CONST_EXPR_H\n"
        "#define PARUS_M_OBJECT_CONST_EXPR_H\n"
        "#define A 5\n"
        "#define B (A + 3)\n"
        "#define C ((B << 1) | 1)\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let x: i64 = c::C;\n"
        "  let y: i64 = c::B;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write object-like macro const-expr test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "object-like macro const expression should resolve to importable constants\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_chain_cycle_warns() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-cycle";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_CHAIN_CYCLE_H\n"
        "#define PARUS_M_CHAIN_CYCLE_H\n"
        "#define A(x) B(x)\n"
        "#define B(x) A(x)\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "def main() -> i32 { return 0i32; }\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write macro cycle cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "cyclic function-like macro chain must not fail whole c-import\n" << out;
        return false;
    }
    if (!contains(out, "CImportFnMacroSkipped")) {
        std::cerr << "cyclic function-like macro chain should report skip warning\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_function_like_macro_nested_paren_cast_forwarding() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-fn-macro-nested-cast";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "M.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_M_NESTED_CAST_H\n"
        "#define PARUS_M_NESTED_CAST_H\n"
        "int abs(int x);\n"
        "#define ABS_WRAP_NESTED(x) abs(((int)(x)))\n"
        "#endif\n";
    const std::string main_src =
        "import \"M.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return c::ABS_WRAP_NESTED(5i32);\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write nested cast macro test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "nested parenthesized cast-forwarding macro should be promoted\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_bitfield_read_write_no_shim() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-bitfield-shim";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "B.h";
    const auto main_pr = temp_root / "main.pr";
    const auto out_o = temp_root / "main.o";
    const std::string header_src =
        "#ifndef PARUS_B_H\n"
        "#define PARUS_B_H\n"
        "struct Bits {\n"
        "  unsigned a : 3;\n"
        "  unsigned b : 5;\n"
        "};\n"
        "struct Bits make_bits(void);\n"
        "#endif\n";
    const std::string main_src =
        "import \"B.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut s = c::make_bits();\n"
        "  s.a = 5u32;\n"
        "  set x = s.a;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write bitfield shim cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" --emit-object -o \"" + out_o.string() + "\"");

    if (contains(out, "CImportLibClangUnavailable")) {
        std::filesystem::remove_all(temp_root, ec);
        return rc != 0;
    }
    if (rc != 0) {
        std::cerr << "bitfield read/write cimport should compile in object mode\n" << out;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto shim_o = std::filesystem::path(out_o.string() + ".cimport_shim.o");
    if (std::filesystem::exists(shim_o)) {
        std::cerr << "bitfield cimport must not emit shim object sidecar\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    std::filesystem::remove_all(temp_root, ec);
    return true;
}

bool test_c_header_import_bitfield_exotic_layout_hard_error() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-bitfield-hard-error";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "B.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_B_H\n"
        "#define PARUS_B_H\n"
        "struct WeirdBits {\n"
        "  _Bool a : 1;\n"
        "};\n"
        "#endif\n";
    const std::string main_src =
        "import \"B.h\" as c;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write exotic bitfield cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "unsupported exotic bitfield layout must fail during import\n" << out;
        return false;
    }
    if (!contains(out, "unsupported C bitfield layout")) {
        std::cerr << "unsupported exotic bitfield layout must report hard import diagnostic\n" << out;
        return false;
    }
    return true;
}

bool test_c_header_import_flatten_collision_hard_error() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-cimport-flatten-collision";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto header_h = temp_root / "C.h";
    const auto main_pr = temp_root / "main.pr";
    const std::string header_src =
        "#ifndef PARUS_COLLIDE_H\n"
        "#define PARUS_COLLIDE_H\n"
        "struct Collide {\n"
        "  struct { int x; };\n"
        "  struct { int x; };\n"
        "};\n"
        "#endif\n";
    const std::string main_src =
        "import \"C.h\" as c;\n"
        "def main() -> i32 { return 0i32; }\n";
    if (!write_text(header_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write flatten collision cimport test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only");
    std::filesystem::remove_all(temp_root, ec);

    if (contains(out, "CImportLibClangUnavailable")) {
        return rc != 0;
    }
    if (rc == 0) {
        std::cerr << "anonymous flatten collision must fail c-import\n" << out;
        return false;
    }
    if (!contains(out, "anonymous field flatten collision") &&
        !contains(out, "redeclares 'x'")) {
        std::cerr << "flatten collision import must fail with collision-class diagnostic\n" << out;
        return false;
    }
    return true;
}

bool test_actor_rejected_in_no_std_profile() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-actor-no-std";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "actor Counter {\n"
        "  draft {\n"
        "    value: i32;\n"
        "  }\n"
        "\n"
        "  init(seed: i32) {\n"
        "    draft.value = seed;\n"
        "  }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set c = Counter(1i32);\n"
        "  return 0i32;\n"
        "}\n";

    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write actor no-std test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only -fno-std";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "actor no-std compile should fail\n" << out;
        return false;
    }
    if (!contains(out, "ActorNotAvailableInNoStd")) {
        std::cerr << "actor no-std failure did not report ActorNotAvailableInNoStd\n" << out;
        return false;
    }
    return true;
}

bool test_actor_allowed_in_freestanding_profile() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-actor-freestanding";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "actor Counter {\n"
        "  draft {\n"
        "    value: i32;\n"
        "  }\n"
        "\n"
        "  init(seed: i32) {\n"
        "    draft.value = seed;\n"
        "  }\n"
        "\n"
        "  def sub get() -> i32 {\n"
        "    recast;\n"
        "    return draft.value;\n"
        "  }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set c = Counter(1i32);\n"
        "  return c.get();\n"
        "}\n";

    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write actor freestanding test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string cmd =
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\" -fsyntax-only -ffreestanding";
    auto [rc, out] = run_capture(cmd);
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        std::cerr << "actor freestanding syntax-only compile should pass\n" << out;
        return false;
    }
    return true;
}

bool test_hosted_actor_link_uses_clang_driver() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parusc-hosted-actor-link";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string src =
        "actor Counter {\n"
        "  draft {\n"
        "    value: i32;\n"
        "  }\n"
        "\n"
        "  init(seed: i32) {\n"
        "    draft.value = seed;\n"
        "  }\n"
        "\n"
        "  def sub get() -> i32 {\n"
        "    recast;\n"
        "    return draft.value;\n"
        "  }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set c = Counter(1i32);\n"
        "  return c.get();\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write hosted actor link test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for hosted actor link test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        std::cerr << "hosted actor link should succeed\n" << out;
        return false;
    }
    if (contains(out, "undefined symbol: __Znwm") || contains(out, "backend linker failed")) {
        std::cerr << "hosted actor link must not hit raw parus-lld C++ runtime failure\n" << out;
        return false;
    }
    if (!contains(out, "linked executable to")) {
        std::cerr << "hosted actor link must emit success message\n" << out;
        return false;
    }
    return true;
}

bool test_hosted_actor_parus_lld_mode_succeeds() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parusc-hosted-actor-parus-lld";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string src =
        "actor Counter {\n"
        "  draft { value: i32; }\n"
        "  init(seed: i32) {\n"
        "    draft.value = seed;\n"
        "  }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set c = Counter(1i32);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write hosted actor parus-lld policy test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for hosted actor parus-lld test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- -fuse-linker=parus-lld \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);

    if (rc != 0) {
        std::cerr << "hosted actor link with -fuse-linker=parus-lld must succeed\n" << out;
        return false;
    }
    return true;
}

bool test_escape_mem_replace_swap_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-mem-replace-swap";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "main";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "proto Probe {\n"
        "  provide def probe() -> i32 {\n"
        "    return 40i32;\n"
        "  }\n"
        "};\n"
        "\n"
        "class Worker: Probe {\n"
        "  value: i32;\n"
        "\n"
        "  init(v: i32) {\n"
        "    self.value = v;\n"
        "  }\n"
        "\n"
        "  def run(self) -> i32 {\n"
        "    return self.value + 1i32;\n"
        "  }\n"
        "};\n"
        "\n"
        "struct Holder {\n"
        "  value: ~Worker;\n"
        "  spare: ~Worker;\n"
        "};\n"
        "\n"
        "class Box<T> with [T: Probe] {\n"
        "  value: ~T;\n"
        "\n"
        "  init(v: ~T) {\n"
        "    self.value = v;\n"
        "  }\n"
        "\n"
        "  def replace_value(mut self, next: ~T) -> ~T {\n"
        "    return mem::replace(self.value, next);\n"
        "  }\n"
        "\n"
        "  def probe_value(self) -> i32 {\n"
        "    return self.value->probe();\n"
        "  }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set a = Worker(1i32);\n"
        "  set b = Worker(2i32);\n"
        "  set c = Worker(3i32);\n"
        "\n"
        "  set mut left = ~a;\n"
        "  set mut right = ~b;\n"
        "  mem::swap(left, right);\n"
        "\n"
        "  set mut holder = Holder{ value: left, spare: right };\n"
        "  let old: ~Worker = mem::replace(holder.value, ~c);\n"
        "  mem::swap(holder.value, holder.spare);\n"
        "  let ok_base: bool = old.run() == 3i32 and holder.value.run() == 2i32 and holder.spare.run() == 4i32;\n"
        "\n"
        "  set d = Worker(4i32);\n"
        "  set e = Worker(5i32);\n"
        "  set mut box = Box<Worker>(~d);\n"
        "  let old_box: ~Worker = box.replace_value(~e);\n"
        "  let ok_box: bool = old_box.run() == 5i32 and box.probe_value() == 40i32;\n"
        "  if (ok_base and ok_box) { return 0i32; }\n"
        "  return 1i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write escape mem replace/swap runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for escape mem replace/swap test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "escape mem replace/swap runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "escape mem replace/swap runtime exit mismatch (expected 0)\n" << out_run;
        return false;
    }
    return true;
}

bool test_escape_plain_assignment_overwrite_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-plain-assign-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set a = Worker(1i32);\n"
        "  set mut h = ~a;\n"
        "  set b = Worker(2i32);\n"
        "  h = ~b;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write escape plain assignment rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for escape plain assignment rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "plain assignment overwrite on initialized ~T should fail\n";
        return false;
    }
    if (out.find("plain assignment cannot overwrite an initialized `~T` place") == std::string::npos ||
        out.find("core::mem::replace") == std::string::npos) {
        std::cerr << "escape plain assignment rejection should mention mem::replace/swap guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_place_first_mem_rejects_non_escape() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-mem-non-escape-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut x = 1i32;\n"
        "  set mut y = 2i32;\n"
        "  let old: i32 = mem::replace<i32>(x, y);\n"
        "  return old;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write non-escape place-first mem rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for non-escape place-first mem rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "place-first mem::replace on non-escape type should fail\n";
        return false;
    }
    if ((out.find("place-first core::mem::replace is only available for owner-cell places") == std::string::npos &&
         out.find("place-first core::mem::replace is only available for owner-carrying places") == std::string::npos) ||
        out.find("&mut") == std::string::npos) {
        std::cerr << "non-escape place-first mem rejection should explain the `&mut` path\n" << out;
        return false;
    }
    return true;
}

bool test_escape_sized_array_owner_cells_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-sized-array-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "out";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "proto Probe {\n"
        "  provide def probe() -> i32 { return 40i32; }\n"
        "};\n"
        "\n"
        "class Worker: Probe {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "  def run(self) -> i32 { return self.value + 1i32; }\n"
        "};\n"
        "\n"
        "class Pool {\n"
        "  workers: (~Worker)[2];\n"
        "  slots: ((~Worker)?)[2];\n"
        "\n"
        "  init(a: ~Worker, b: ~Worker, c: ~Worker) {\n"
        "    self.workers = [a, b];\n"
        "    let empty_slots: ((~Worker)?)[2] = [null, null];\n"
        "    self.slots = empty_slots;\n"
        "    self.slots[0] ?"
        "?= c;\n"
        "  }\n"
        "\n"
        "  def rotate(mut self) -> i32 {\n"
        "    mem::swap(self.workers[0], self.workers[1]);\n"
        "    return self.workers[0].run() + self.workers[1]->probe();\n"
        "  }\n"
        "\n"
        "  def refill(mut self, next: ~Worker) -> ~Worker {\n"
        "    return mem::replace(self.workers[0], next);\n"
        "  }\n"
        "\n"
        "  def take_slot(mut self) -> i32 {\n"
        "    let taken: ~Worker = self.slots[0] else {\n"
        "      return 1i32;\n"
        "    };\n"
        "    return taken.run();\n"
        "  }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut local = [make(1i32), make(2i32)];\n"
        "  mem::swap(local[0], local[1]);\n"
        "\n"
        "  set mut p = Pool(make(3i32), make(4i32), make(5i32));\n"
        "  let old: ~Worker = p.refill(make(6i32));\n"
        "  let total: i32 = local[0].run() + local[1]->probe() + p.rotate() + p.take_slot() + old.run();\n"
        "  if (total == 98i32) { return 0i32; }\n"
        "  return total;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write escape sized array runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for escape sized array runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "escape sized array runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "escape sized array runtime exit mismatch (expected 0)\n" << out_run;
        return false;
    }
    return true;
}

bool test_escape_sized_array_container_methods_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-array-methods-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "out";
    const std::string src =
        "proto Probe {\n"
        "  provide def probe() -> i32 { return 20i32; }\n"
        "};\n"
        "\n"
        "class Worker: Probe {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "  def run(self) -> i32 { return self.value + 1i32; }\n"
        "};\n"
        "\n"
        "class WorkerGroup {\n"
        "  workers: (~Worker)[2];\n"
        "  slots: ((~Worker)?)[2];\n"
        "\n"
        "  init(a: ~Worker, b: ~Worker, c: ~Worker) {\n"
        "    self.workers = [a, b];\n"
        "    self.slots = [null, null];\n"
        "    let ignored: (~Worker)? = self.slots.put(1usize, c);\n"
        "  }\n"
        "};\n"
        "\n"
        "class Pool {\n"
        "  groups: WorkerGroup[1];\n"
        "  plain: i32[2];\n"
        "\n"
        "  init(a: ~Worker, b: ~Worker, c: ~Worker) {\n"
        "    self.groups = [WorkerGroup(a, b, c)];\n"
        "    self.plain = [10i32, 20i32];\n"
        "  }\n"
        "\n"
        "  def run(mut self) -> i32 {\n"
        "    self.plain.swap(0usize, 1usize);\n"
        "    self.groups[0].workers.swap(0usize, 1usize);\n"
        "    let old: ~Worker = self.groups[0].workers.replace(1usize, make(4i32));\n"
        "    set mut prev = self.groups[0].slots.put(1usize, old);\n"
        "    set mut taken_opt = self.groups[0].slots.take(1usize);\n"
        "    let taken: ~Worker = taken_opt else { return 1i32; };\n"
        "    let prev_worker: ~Worker = prev else { return 2i32; };\n"
        "    return self.plain[0usize] + self.groups[0].workers[0].run() +\n"
        "           self.groups[0].workers[1]->probe() + taken.run() + prev_worker.run();\n"
        "  }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut p = Pool(make(1i32), make(2i32), make(3i32));\n"
        "  let total: i32 = p.run();\n"
        "  if (total == 49i32) { return 0i32; }\n"
        "  return total;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write sized array method runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for sized array method runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "sized array method runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "sized array method runtime exit mismatch (expected 0)\n" << out_run;
        return false;
    }
    return true;
}

bool test_escape_array_method_take_rejected_on_plain_owner_array() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-array-method-take-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "def main() -> i32 {\n"
        "  set mut items = [make(1i32)];\n"
        "  let bad: (~Worker)? = items.take(0usize);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write array take rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for array take rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "plain owner array take should fail\n";
        return false;
    }
    if (out.find("array method `take` is only available on optional owner arrays") == std::string::npos ||
        out.find(".replace(i, value)") == std::string::npos) {
        std::cerr << "plain owner array take rejection should explain optional-owner-array guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_array_method_replace_rejected_on_optional_owner_array() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-array-method-replace-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "def main() -> i32 {\n"
        "  let seed: ((~Worker)?)[1] = [null];\n"
        "  set mut items = seed;\n"
        "  let bad: (~Worker)? = items.replace(0usize, make(1i32));\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write array replace rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for array replace rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "optional owner array replace should fail\n";
        return false;
    }
    if (out.find("array method `replace` is only available on plain owner arrays") == std::string::npos ||
        out.find(".put(i, value)") == std::string::npos) {
        std::cerr << "optional owner array replace rejection should explain put guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_array_method_put_rejected_on_non_owner_array() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-array-method-put-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "def main() -> i32 {\n"
        "  set mut items = [1i32, 2i32];\n"
        "  let bad: i32 = items.put(0usize, make(1i32));\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write array put rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for array put rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "non-owner array put should fail\n";
        return false;
    }
    if (out.find("array method `put` is only available on optional owner arrays") == std::string::npos ||
        out.find("ordinary indexed assignment") == std::string::npos) {
        std::cerr << "non-owner array put rejection should explain non-owner guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_array_method_rejects_immutable_receiver() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-array-method-immutable-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "def main() -> i32 {\n"
        "  let items: i32[2] = [1i32, 2i32];\n"
        "  items.swap(0usize, 1usize);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write immutable receiver rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for immutable receiver rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);
    if (rc == 0) {
        std::cerr << "immutable array receiver should fail\n";
        return false;
    }
    if (out.find("requires a mutable receiver") == std::string::npos ||
        out.find("set mut arr") == std::string::npos) {
        std::cerr << "immutable receiver rejection should explain mut receiver guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_unsized_owner_array_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-unsized-array-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "class Pool {\n"
        "  workers: (~Worker)[];\n"
        "};\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write escape unsized owner array rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for escape unsized owner array rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "unsized owner array should fail\n";
        return false;
    }
    if (out.find("unsized view/container of '~T' is deferred in this round") == std::string::npos ||
        out.find("sized owner arrays") == std::string::npos) {
        std::cerr << "unsized owner array rejection should explain sized-array-only guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_indexed_projection_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-indexed-projection-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut items = [make(1i32)];\n"
        "  return items[0].value;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write escape indexed projection rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for escape indexed projection rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "direct projection on indexed ~T should fail\n";
        return false;
    }
    if (out.find("direct field/index projection on `~T` is deferred in this round") == std::string::npos ||
        out.find("storage-safe aggregate fields") == std::string::npos) {
        std::cerr << "indexed projection rejection should explain deferred direct projection\n" << out;
        return false;
    }
    return true;
}

bool test_escape_projected_owner_cells_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-projected-owner-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "out";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "proto Probe {\n"
        "  provide def probe() -> i32 { return 40i32; }\n"
        "};\n"
        "\n"
        "class Worker: Probe {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "  def run(self) -> i32 { return self.value + 1i32; }\n"
        "};\n"
        "\n"
        "class WorkerSlot {\n"
        "  item: (~Worker)?;\n"
        "  init(v: ~Worker) {\n"
        "    self.item = null;\n"
        "    self.item ?"
        "?= v;\n"
        "  }\n"
        "};\n"
        "\n"
        "class WorkerGroup {\n"
        "  primary: ~Worker;\n"
        "  slots: WorkerSlot[2];\n"
        "\n"
        "  init(a: ~Worker, b: ~Worker, c: ~Worker) {\n"
        "    self.primary = a;\n"
        "    self.slots = [WorkerSlot(b), WorkerSlot(c)];\n"
        "  }\n"
        "};\n"
        "\n"
        "class Pool {\n"
        "  groups: WorkerGroup[2];\n"
        "\n"
        "  init(a: ~Worker, b: ~Worker, c: ~Worker, d: ~Worker, e: ~Worker, f: ~Worker) {\n"
        "    self.groups = [WorkerGroup(a, b, c), WorkerGroup(d, e, f)];\n"
        "  }\n"
        "\n"
        "  def refill(mut self, next: ~Worker) -> ~Worker {\n"
        "    return mem::replace(self.groups[0].primary, next);\n"
        "  }\n"
        "\n"
        "  def rotate(mut self) -> void {\n"
        "    mem::swap(self.groups[0].slots[0].item, self.groups[1].slots[1].item);\n"
        "  }\n"
        "\n"
        "  def take(mut self) -> i32 {\n"
        "    let taken: ~Worker = self.groups[1].slots[1].item else {\n"
        "      return 1i32;\n"
        "    };\n"
        "    return self.groups[0].primary.run() + self.groups[0].primary->probe() + taken.run();\n"
        "  }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut p = Pool(make(1i32), make(2i32), make(3i32), make(4i32), make(5i32), make(6i32));\n"
        "  let old: ~Worker = p.refill(make(7i32));\n"
        "  p.rotate();\n"
        "  let total: i32 = p.take() + old.run();\n"
        "  if (total > 0i32) { return 0i32; }\n"
        "  return 1i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write projected owner-cell runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for projected owner-cell runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "projected owner-cell runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "projected owner-cell runtime exit mismatch (expected 0)\n" << out_run;
        return false;
    }
    return true;
}

bool test_escape_projected_direct_projection_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-projected-direct-projection-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "class WorkerGroup {\n"
        "  primary: ~Worker;\n"
        "  init(v: ~Worker) { self.primary = v; }\n"
        "};\n"
        "\n"
        "class Pool {\n"
        "  groups: WorkerGroup[1];\n"
        "  init(v: ~Worker) { self.groups = [WorkerGroup(v)]; }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set p = Pool(make(1i32));\n"
        "  return p.groups[0].primary.value;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write projected direct projection rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for projected direct projection rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "direct projection on projected ~T should fail\n";
        return false;
    }
    if (out.find("direct field/index projection on `~T` is deferred in this round") == std::string::npos ||
        out.find("storage-safe aggregate fields") == std::string::npos) {
        std::cerr << "projected direct projection rejection should explain deferred projection guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_projected_borrow_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-projected-borrow-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "class WorkerGroup {\n"
        "  primary: ~Worker;\n"
        "  init(v: ~Worker) { self.primary = v; }\n"
        "};\n"
        "\n"
        "class Pool {\n"
        "  groups: WorkerGroup[1];\n"
        "  init(v: ~Worker) { self.groups = [WorkerGroup(v)]; }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut p = Pool(make(1i32));\n"
        "  let r: &~Worker = &p.groups[0].primary;\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write projected borrow rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for projected borrow rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "borrow of projected owner place should fail\n";
        return false;
    }
    if ((out.find("BorrowOperandMustBeOwnedPlace") == std::string::npos &&
         out.find("BorrowOperandMustBePlace") == std::string::npos) ||
        (out.find("move-only owner handle") == std::string::npos &&
         out.find("place expression") == std::string::npos)) {
        std::cerr << "projected borrow rejection should keep projected owner-place borrow forbidden\n" << out;
        return false;
    }
    return true;
}

bool test_escape_deref_raw_ptr_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-deref-raw-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "def main() -> i32 {\n"
        "  manual[get] {\n"
        "    let p: *mut i32 = null;\n"
        "    let h: ~i32 = ~(*p);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write raw deref escape rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for raw deref escape rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "~(*p) should fail\n";
        return false;
    }
    if (out.find("EscapeDerefSourceNotAllowed") == std::string::npos ||
        out.find("not an owner transition source") == std::string::npos) {
        std::cerr << "raw deref escape rejection should explain deref/owner boundary\n" << out;
        return false;
    }
    return true;
}

bool test_escape_deref_borrow_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-deref-borrow-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "def main() -> i32 {\n"
        "  let x: i32 = 1i32;\n"
        "  let b: &i32 = &x;\n"
        "  let h: ~i32 = ~(*b);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write borrow deref escape rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for borrow deref escape rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "~(*bp) should fail\n";
        return false;
    }
    if (out.find("EscapeDerefSourceNotAllowed") == std::string::npos ||
        out.find("deref") == std::string::npos) {
        std::cerr << "borrow deref escape rejection should explain deref/owner boundary\n" << out;
        return false;
    }
    return true;
}

bool test_escape_deref_consume_binding_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-deref-consume-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "def main() -> i32 {\n"
        "  let mut slot: (~Worker)? = null;\n"
        "  let p: &mut ((~Worker)?) = &mut slot;\n"
        "  let h: ~Worker = *p else {\n"
        "    return 1i32;\n"
        "  };\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write deref consume-binding rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for deref consume-binding rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "consume-binding from deref should fail\n";
        return false;
    }
    if (out.find("VarConsumeElseRequiresPlace") == std::string::npos) {
        std::cerr << "deref consume-binding rejection should keep place-only consume-binding rule\n" << out;
        return false;
    }
    return true;
}

bool test_escape_raw_ptr_owner_pointee_read_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-owner-ptr-read-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "def main() -> i32 {\n"
        "  manual[get] {\n"
        "    let p: *mut (~Worker) = null;\n"
        "    let h: ~Worker = *p;\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write owner raw-pointer read rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for owner raw-pointer read rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "raw pointer owner read should fail\n";
        return false;
    }
    if (out.find("RawPointerOwnerPointeeReadNotAllowed") == std::string::npos ||
        out.find("owner-typed pointee") == std::string::npos) {
        std::cerr << "owner raw-pointer read rejection should explain pointer/owner boundary\n" << out;
        return false;
    }
    return true;
}

bool test_escape_raw_ptr_owner_pointee_write_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-owner-ptr-write-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "def main() -> i32 {\n"
        "  manual[set] {\n"
        "    let p: *mut (~Worker) = null;\n"
        "    *p = make(1i32);\n"
        "  }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write owner raw-pointer write rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for owner raw-pointer write rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "raw pointer owner write should fail\n";
        return false;
    }
    if (out.find("RawPointerOwnerPointeeWriteNotAllowed") == std::string::npos ||
        out.find("owner-typed pointee") == std::string::npos) {
        std::cerr << "owner raw-pointer write rejection should explain pointer/owner boundary\n" << out;
        return false;
    }
    return true;
}

bool test_escape_enum_owner_payload_and_take_runtime() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-enum-owner-take-runtime";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const auto exe = temp_root / "out";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "  def run(self) -> i32 { return self.value + 1i32; }\n"
        "};\n"
        "\n"
        "enum Job {\n"
        "  case Empty,\n"
        "  case Ready(worker: ~Worker),\n"
        "  case Done(result: ~Worker),\n"
        "};\n"
        "\n"
        "class WorkerSlot {\n"
        "  item: (~Worker)?;\n"
        "  init(v: ~Worker) {\n"
        "    self.item = v;\n"
        "  }\n"
        "};\n"
        "\n"
        "class Scheduler {\n"
        "  state: Job;\n"
        "  slots: WorkerSlot[2];\n"
        "\n"
        "  init(a: ~Worker, b: ~Worker, c: ~Worker) {\n"
        "    self.state = Job::Ready(worker: a);\n"
        "    self.slots = [WorkerSlot(b), WorkerSlot(c)];\n"
        "  }\n"
        "\n"
        "  def take_worker(mut self) -> (~Worker)? {\n"
        "    let state: Job = mem::replace(self.state, Job::Empty());\n"
        "    switch (state) {\n"
        "    case Job::Ready(worker: w): { return w; }\n"
        "    case Job::Done(result: r): { return r; }\n"
        "    default: { return null; }\n"
        "    }\n"
        "    return null;\n"
        "  }\n"
        "\n"
        "  def take_slot(mut self, i: i32) -> (~Worker)? {\n"
        "    return mem::take(self.slots[i].item);\n"
        "  }\n"
        "};\n"
        "\n"
        "def make(seed: i32) -> ~Worker {\n"
        "  set w = Worker(seed);\n"
        "  return ~w;\n"
        "}\n"
        "\n"
        "def main() -> i32 {\n"
        "  set mut s = Scheduler(make(1i32), make(2i32), make(3i32));\n"
        "  let mut from_state_opt: (~Worker)? = s.take_worker();\n"
        "  let from_state: ~Worker = from_state_opt else { return 1i32; };\n"
        "  let from_slot: ~Worker = mem::take(s.slots[1].item) else { return 2i32; };\n"
        "  let mut from_slot2_opt: (~Worker)? = s.take_slot(0i32);\n"
        "  let from_slot2: ~Worker = from_slot2_opt else { return 3i32; };\n"
        "  let a: i32 = from_state.run();\n"
        "  let b: i32 = from_slot.run();\n"
        "  let c: i32 = from_slot2.run();\n"
        "  if (a != 2i32) { return 11i32; }\n"
        "  if (b != 4i32) { return 12i32; }\n"
        "  if (c != 3i32) { return 13i32; }\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write enum owner payload runtime test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for enum owner payload runtime test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc_build, out_build] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target +
        " -o \"" + exe.string() + "\"");
    if (rc_build != 0) {
        std::cerr << "enum owner payload runtime sample should compile/link\n" << out_build;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    auto [rc_run, out_run] = run_capture("\"" + exe.string() + "\"");
    std::filesystem::remove_all(temp_root, ec);
    if (rc_run != 0) {
        std::cerr << "enum owner payload runtime exit mismatch (expected 0)\n" << out_run;
        return false;
    }
    return true;
}

bool test_escape_enum_owner_payload_direct_place_switch_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-enum-direct-place-switch-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "enum Job {\n"
        "  case Empty,\n"
        "  case Ready(worker: ~Worker),\n"
        "};\n"
        "\n"
        "class Scheduler {\n"
        "  state: Job;\n"
        "\n"
        "  def take_worker(mut self) -> (~Worker)? {\n"
        "    switch (self.state) {\n"
        "    case Job::Ready(worker: w): { return w; }\n"
        "    default: { return null; }\n"
        "    }\n"
        "    return null;\n"
        "  }\n"
        "};\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write direct place enum owner switch rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for direct place enum owner switch rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "direct place enum owner switch should fail\n";
        return false;
    }
    if (out.find("owner payload switch bind does not accept projected-place scrutinee") == std::string::npos ||
        out.find("mem::replace") == std::string::npos) {
        std::cerr << "direct place enum owner switch rejection should mention explicit consume guidance\n" << out;
        return false;
    }
    return true;
}

bool test_escape_enum_owner_payload_borrow_switch_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-enum-borrow-switch-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "enum Job {\n"
        "  case Empty,\n"
        "  case Ready(worker: ~Worker),\n"
        "};\n"
        "\n"
        "def bad(s: &Job) -> (~Worker)? {\n"
        "  switch (s) {\n"
        "  case Job::Ready(worker: w): { return w; }\n"
        "  default: { return null; }\n"
        "  }\n"
        "  return null;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write borrow enum owner switch rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for borrow enum owner switch rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "borrow enum owner switch should fail\n";
        return false;
    }
    if (out.find("owner payload switch bind does not accept borrow scrutinee") == std::string::npos ||
        out.find("first move the enum into a local value, then switch on that local") == std::string::npos) {
        std::cerr << "borrow enum owner switch rejection should explain explicit consume path\n" << out;
        return false;
    }
    return true;
}

bool test_escape_mem_take_rejects_non_owner_optional() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-mem-take-non-owner-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "import mem as mem;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let mut value: i32? = 1i32;\n"
        "  let old: i32? = mem::take(value);\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write non-owner mem::take rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for non-owner mem::take rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "mem::take on non-owner optional should fail\n";
        return false;
    }
    if (out.find("place-first core::mem::take is only available for optional owner-cell places") == std::string::npos ||
        out.find("this round only opens `core::mem::take` for writable projected `(~T)?` places") == std::string::npos) {
        std::cerr << "non-owner mem::take rejection should explain owner-only surface\n" << out;
        return false;
    }
    return true;
}

bool test_escape_named_aggregate_unsized_container_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-escape-named-aggregate-unsized-reject";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto main_pr = temp_root / "main.pr";
    const std::string src =
        "class Worker {\n"
        "  value: i32;\n"
        "  init(v: i32) { self.value = v; }\n"
        "};\n"
        "\n"
        "class WorkerSlot {\n"
        "  item: (~Worker)?;\n"
        "};\n"
        "\n"
        "class BadPool {\n"
        "  slots: WorkerSlot[];\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(main_pr, src)) {
        std::cerr << "failed to write named aggregate unsized rejection test file\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const auto sysroot_and_target = resolve_installed_sysroot_and_target();
    if (!sysroot_and_target) {
        std::cerr << "failed to resolve installed sysroot/target for named aggregate unsized rejection test\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const auto& [sysroot, target] = *sysroot_and_target;

    auto [rc, out] = run_capture(
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\""
        " --sysroot \"" + sysroot + "\""
        " --target " + target);
    std::filesystem::remove_all(temp_root, ec);

    if (rc == 0) {
        std::cerr << "unsized named aggregate owner container should fail\n";
        return false;
    }
    if (out.find("storage-safe named aggregate") == std::string::npos ||
        out.find("WorkerSlot[]") == std::string::npos) {
        std::cerr << "named aggregate unsized rejection should mention storage-safe aggregate guidance\n" << out;
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool ok1 = test_help_and_version();
    const bool ok2 = test_build_and_graph();
    const bool ok3 = test_check_pr();
    const bool ok4 = test_check_lei_project();
    const bool ok5 = test_doctor_json_and_global_json_reject();
    const bool ok6 = test_tool_passthrough();
    const bool ok7 = test_bundle_strict_export_violation();
    const bool ok8 = test_bundle_build_strict_export_violation();
    const bool ok9 = test_bundle_dep_import_not_declared();
    const bool ok10 = test_cross_bundle_non_export_violation();
    const bool ok11 = test_check_capability_diagnostic_surface();
    const bool ok12 = test_cross_bundle_export_runtime_call();
    const bool ok13 = test_same_bundle_multi_module_runtime_call();
    const bool ok14 = test_builtin_acts_policy_core_gate();
    const bool ok15 = test_impl_binding_surface_policy();
    const bool ok16 = test_impl_binding_library_owned_runtime();
    const bool ok17 = test_auto_core_export_index_loaded_for_non_core_bundle();
    const bool ok18 = test_bundle_alias_proto_impl_path_resolves();
    const bool ok19 = test_bundle_relative_import_resolves();
    const bool ok20 = test_bundle_parent_relative_import_resolves();
    const bool ok21 = test_bundle_grandparent_relative_import_resolves();
    const bool ok22 = test_core_marker_bare_use_special_form();
    const bool ok23 = test_import_keyword_path_rejected();
    const bool ok24 = test_c_header_import_local_non_variadic();
    const bool ok25 = test_c_header_import_stdio_variadic_fixed_arg_count_checked();
    const bool ok26 = test_c_header_import_stdio_variadic_requires_manual_abi();
    const bool ok27 = test_c_header_import_stdio_variadic_zero_tail_no_manual_abi();
    const bool ok28 = test_c_header_import_stdio_format_bridge_single_arg();
    const bool ok29 = test_extern_c_variadic_manual_abi_and_null_boundary();
    const bool ok30 = test_c_header_import_cstr_runtime_prints_consistent_output();
    const bool ok31 = test_c_header_import_include_dir_option();
    const bool ok32 = test_lei_module_bundle_cimport_isystem_option();
    const bool ok33 = test_c_header_import_union_manual_get_gate();
    const bool ok34 = test_c_header_import_union_manual_set_gate();
    const bool ok35 = test_c_header_import_struct_borrow_escape_rules();
    const bool ok36 = test_c_header_import_enum_constant_usage();
    const bool ok37 = test_c_header_import_global_and_tls_usage();
    const bool ok38 = test_c_header_import_const_global_write_rejected();
    const bool ok39 = test_c_header_import_define_undefine_options();
    const bool ok40 = test_c_header_import_imacros_option();
    const bool ok41 = test_c_header_import_forced_include_option();
    const bool ok42 = test_c_header_import_anonymous_typedef_struct_usage();
    const bool ok43 = test_c_header_import_transparent_typedef_uint32_assign();
    const bool ok44 = test_c_header_import_nominal_typedef_record_stays_nominal();
    const bool ok45 = test_c_header_import_function_pointer_alias_call();
    const bool ok46 = test_c_header_import_function_like_macro_not_imported();
    const bool ok47 = test_c_header_import_function_like_macro_direct_alias_call();
    const bool ok48 = test_c_header_import_function_like_macro_shim_link_success();
    const bool ok49 = test_c_header_import_function_like_macro_skip_warning();
    const bool ok50 = test_c_header_import_function_like_macro_ir_only_supported();
    const bool ok51 = test_c_header_import_function_like_macro_chain_promoted();
    const bool ok52 = test_c_header_import_object_macro_const_expr_resolved();
    const bool ok53 = test_c_header_import_function_like_macro_chain_cycle_warns();
    const bool ok54 = test_c_header_import_function_like_macro_nested_paren_cast_forwarding();
    const bool ok55 = test_c_header_import_bitfield_read_write_no_shim();
    const bool ok56 = test_c_header_import_bitfield_exotic_layout_hard_error();
    const bool ok57 = test_c_header_import_flatten_collision_hard_error();
    const bool ok58 = test_c_header_import_macos_opengl_isystem();
    const bool ok59 = test_c_header_import_macos_moltenvk_isystem();
    const bool ok60 = test_actor_rejected_in_no_std_profile();
    const bool ok61 = test_actor_allowed_in_freestanding_profile();
    const bool ok62 = test_hosted_actor_link_uses_clang_driver();
    const bool ok63 = test_hosted_actor_parus_lld_mode_succeeds();
    const bool ok64 = test_core_ext_scaffold_and_auto_injection();
    const bool ok65 = test_c_header_import_variadic_function_pointer_alias_requires_manual_abi_cache_hit();
    const bool ok66 = test_c_header_import_variadic_function_pointer_global_and_field_calls();
    const bool ok67 = test_c_header_import_variadic_function_pointer_zero_tail_calls_without_manual_abi();
    const bool ok68 = test_c_header_import_dropped_global_decl_preserves_supported_imports();
    const bool ok69 = test_c_header_import_dropped_owner_record_preserves_supported_imports();
    const bool ok70 = test_iteration_array_loop_runtime();
    const bool ok71 = test_iteration_slice_loop_runtime();
    const bool ok72 = test_iteration_range_loop_runtime();
    const bool ok73 = test_consume_binding_optional_escape_runtime();
    const bool ok74 = test_iteration_unsupported_iterable_hard_error();
    const bool ok75 = test_iteration_pure_infer_range_runtime_and_regressions();
    const bool ok76 = test_iteration_loop_binder_variadic_typedef_arg_compiles();
    const bool ok77 = test_unsuffixed_array_literal_slice_runtime_and_llvm();
    const bool ok78 = test_unsuffixed_named_array_to_slice_runtime();
    const bool ok79 = test_unsuffixed_array_literal_call_arg_and_return_contexts();
    const bool ok80 = test_unsuffixed_array_literal_float_context_rejected();
    const bool ok81 = test_iteration_loop_break_value_context_syntax_only();
    const bool ok82 = test_core_seed_export_index_and_auto_injection();
    const bool ok83 = test_core_seed_runtime_smoke();
    const bool ok84 = test_text_view_cstr_preflight_syntax_only();
    const bool ok85 = test_cstr_private_fields_hidden_but_helpers_work();
    const bool ok86 = test_external_generic_constraints_v2_work();
    const bool ok87 = test_external_generic_type_body_closure_v2_work();
    const bool ok88 = test_external_generic_owner_enum_runtime();
    const bool ok89 = test_escape_first_class_places_runtime();
    const bool ok90 = test_escape_mem_replace_swap_runtime();
    const bool ok91 = test_escape_plain_assignment_overwrite_rejected();
    const bool ok92 = test_escape_place_first_mem_rejects_non_escape();
    const bool ok93 = test_escape_sized_array_owner_cells_runtime();
    const bool ok94 = test_escape_unsized_owner_array_rejected();
    const bool ok95 = test_escape_indexed_projection_rejected();
    const bool ok96 = test_escape_projected_owner_cells_runtime();
    const bool ok97 = test_escape_projected_direct_projection_rejected();
    const bool ok98 = test_escape_projected_borrow_rejected();
    const bool ok99 = test_escape_deref_raw_ptr_rejected();
    const bool ok100 = test_escape_deref_borrow_rejected();
    const bool ok101 = test_escape_deref_consume_binding_rejected();
    const bool ok102 = test_escape_raw_ptr_owner_pointee_read_rejected();
    const bool ok103 = test_escape_raw_ptr_owner_pointee_write_rejected();
    const bool ok104 = test_escape_named_aggregate_unsized_container_rejected();
    const bool ok105 = test_escape_enum_owner_payload_and_take_runtime();
    const bool ok106 = test_escape_enum_owner_payload_direct_place_switch_rejected();
    const bool ok107 = test_escape_enum_owner_payload_borrow_switch_rejected();
    const bool ok108 = test_escape_mem_take_rejects_non_owner_optional();
    const bool ok109 = test_escape_sized_array_container_methods_runtime();
    const bool ok110 = test_escape_array_method_take_rejected_on_plain_owner_array();
    const bool ok111 = test_escape_array_method_replace_rejected_on_optional_owner_array();
    const bool ok112 = test_escape_array_method_put_rejected_on_non_owner_array();
    const bool ok113 = test_escape_array_method_rejects_immutable_receiver();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 || !ok9 || !ok10 || !ok11 ||
        !ok12 || !ok13 || !ok14 || !ok15 || !ok16 || !ok17 || !ok18 || !ok19 || !ok20 || !ok21 || !ok22 || !ok23 ||
        !ok24 || !ok25 || !ok26 || !ok27 || !ok28 || !ok29 || !ok30 || !ok31 || !ok32 || !ok33 || !ok34 || !ok35 ||
        !ok36 || !ok37 || !ok38 || !ok39 || !ok40 || !ok41 || !ok42 || !ok43 || !ok44 || !ok45 || !ok46 || !ok47 ||
        !ok48 || !ok49 || !ok50 || !ok51 || !ok52 || !ok53 || !ok54 || !ok55 || !ok56 || !ok57 || !ok58 || !ok59 ||
        !ok60 || !ok61 || !ok62 || !ok63 || !ok64 || !ok65 || !ok66 || !ok67 || !ok68 || !ok69 || !ok70 || !ok71 ||
        !ok72 || !ok73 || !ok74 || !ok75 || !ok76 || !ok77 || !ok78 || !ok79 || !ok80 || !ok81 || !ok82 ||
        !ok83 || !ok84 || !ok85 || !ok86 || !ok87 || !ok88 || !ok89 || !ok90 || !ok91 || !ok92 || !ok93 || !ok94 ||
        !ok95 || !ok96 || !ok97 || !ok98 || !ok99 || !ok100 || !ok101 || !ok102 || !ok103 || !ok104 || !ok105 ||
        !ok106 || !ok107 || !ok108 || !ok109 || !ok110 || !ok111 || !ok112 || !ok113) {
        return 1;
    }

    std::cout << "parus cli tests passed\n";
    return 0;
}
