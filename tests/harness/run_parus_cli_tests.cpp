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

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs << text;
    return ofs.good();
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
    if (!contains(out, "SirEscapeBoundaryViolation")) {
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
        "  \"version\": 5,\n"
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
    const std::string emit_core_index_cmd =
        "\"" + bin + "\" tool parusc -- \"" + ext_types.string() + "\"" +
        " --bundle-name core" +
        " --bundle-root \"" + core_root.string() + "\"" +
        " --module-head ext" +
        " --bundle-source \"" + ext_types.string() + "\"" +
        " --bundle-source \"" + ext_cstr.string() + "\"" +
        " --bundle-source \"" + ext_errors.string() + "\"" +
        " --emit-export-index \"" + core_index.string() + "\"";
    auto [rc_emit, out_emit] = run_capture(emit_core_index_cmd);
    if (rc_emit != 0) {
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
        "def main() -> i32 {\n"
        "  let x: core::ext::c_int = 7;\n"
        "  let sz: core::ext::c_size = 10;\n"
        "  let ssz: core::ext::c_ssize = 2;\n"
        "  let diff: core::ext::c_ptrdiff = 3;\n"
        "  if (sz == 10 and ssz == 2 and diff == 3) {\n"
        "    return x;\n"
        "  }\n"
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
        "def main() -> i32 {\n"
        "  set x = core::ext::make(\"x\", 1usize);\n"
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
        "extern \"C\" def vprintf(fmt: ptr core::ext::c_char, ap: core::ext::vaList) -> core::ext::c_int;\n"
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
        "def main() -> i32 {\n"
        "  let x: core::ext::vaList;\n"
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
        "  c::puts(c\"hi\");\n"
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
    if (!contains(out_text_fail, "text value is not C ABI-safe; use ptr core::ext::c_char")) {
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
        "extern \"C\" def take_ptr(p: ptr i32) -> i32;\n"
        "extern \"C\" def variad(first: ptr i8, ...) -> i32;\n"
        "\n"
        "def main() -> i32 {\n"
        "  let p: ptr i32 = null;\n"
        "  take_ptr(null);\n"
        "  take_ptr(p);\n"
        "  let z: i32 = variad(null);\n"
        "  manual[abi] {\n"
        "    variad(null, 5);\n"
        "  }\n"
        "  return z;\n"
        "}\n";
    const std::string fail_src =
        "extern \"C\" def variad(first: ptr i8, ...) -> i32;\n"
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
        std::cerr << "extern C variadic declaration with manual[abi] and null->ptr boundary should compile\n" << out_ok;
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
        "\n"
        "def main() -> i32 {\n"
        "  set x = core::ext::from_ptr(\"Hello, World!!\");\n"
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
    const bool ok15 = test_auto_core_export_index_loaded_for_non_core_bundle();
    const bool ok16 = test_bundle_alias_proto_impl_path_resolves();
    const bool ok17 = test_bundle_relative_import_resolves();
    const bool ok18 = test_bundle_parent_relative_import_resolves();
    const bool ok19 = test_bundle_grandparent_relative_import_resolves();
    const bool ok20 = test_core_marker_bare_use_special_form();
    const bool ok21 = test_import_keyword_path_rejected();
    const bool ok22 = test_c_header_import_local_non_variadic();
    const bool ok23 = test_c_header_import_stdio_variadic_fixed_arg_count_checked();
    const bool ok24 = test_c_header_import_stdio_variadic_requires_manual_abi();
    const bool ok25 = test_c_header_import_stdio_variadic_zero_tail_no_manual_abi();
    const bool ok26 = test_c_header_import_stdio_format_bridge_single_arg();
    const bool ok27 = test_extern_c_variadic_manual_abi_and_null_boundary();
    const bool ok28 = test_c_header_import_cstr_runtime_prints_consistent_output();
    const bool ok29 = test_c_header_import_include_dir_option();
    const bool ok30 = test_lei_module_bundle_cimport_isystem_option();
    const bool ok31 = test_c_header_import_union_manual_get_gate();
    const bool ok32 = test_c_header_import_union_manual_set_gate();
    const bool ok33 = test_c_header_import_struct_borrow_escape_rules();
    const bool ok34 = test_c_header_import_enum_constant_usage();
    const bool ok35 = test_c_header_import_global_and_tls_usage();
    const bool ok36 = test_c_header_import_const_global_write_rejected();
    const bool ok37 = test_c_header_import_define_undefine_options();
    const bool ok38 = test_c_header_import_imacros_option();
    const bool ok39 = test_c_header_import_forced_include_option();
    const bool ok40 = test_c_header_import_anonymous_typedef_struct_usage();
    const bool ok41 = test_c_header_import_transparent_typedef_uint32_assign();
    const bool ok42 = test_c_header_import_nominal_typedef_record_stays_nominal();
    const bool ok43 = test_c_header_import_function_pointer_alias_call();
    const bool ok44 = test_c_header_import_function_like_macro_not_imported();
    const bool ok45 = test_c_header_import_function_like_macro_direct_alias_call();
    const bool ok46 = test_c_header_import_function_like_macro_shim_link_success();
    const bool ok47 = test_c_header_import_function_like_macro_skip_warning();
    const bool ok48 = test_c_header_import_function_like_macro_ir_only_supported();
    const bool ok49 = test_c_header_import_function_like_macro_chain_promoted();
    const bool ok50 = test_c_header_import_object_macro_const_expr_resolved();
    const bool ok51 = test_c_header_import_function_like_macro_chain_cycle_warns();
    const bool ok52 = test_c_header_import_function_like_macro_nested_paren_cast_forwarding();
    const bool ok53 = test_c_header_import_bitfield_read_write_no_shim();
    const bool ok54 = test_c_header_import_bitfield_exotic_layout_hard_error();
    const bool ok55 = test_c_header_import_flatten_collision_hard_error();
    const bool ok56 = test_c_header_import_macos_opengl_isystem();
    const bool ok57 = test_c_header_import_macos_moltenvk_isystem();
    const bool ok58 = test_actor_rejected_in_no_std_profile();
    const bool ok59 = test_actor_allowed_in_freestanding_profile();
    const bool ok60 = test_hosted_actor_link_uses_clang_driver();
    const bool ok61 = test_hosted_actor_parus_lld_mode_succeeds();
    const bool ok62 = test_core_ext_scaffold_and_auto_injection();
    const bool ok63 = test_c_header_import_variadic_function_pointer_alias_requires_manual_abi_cache_hit();
    const bool ok64 = test_c_header_import_variadic_function_pointer_global_and_field_calls();
    const bool ok65 = test_c_header_import_variadic_function_pointer_zero_tail_calls_without_manual_abi();
    const bool ok66 = test_c_header_import_dropped_global_decl_preserves_supported_imports();
    const bool ok67 = test_c_header_import_dropped_owner_record_preserves_supported_imports();
    const bool ok68 = test_iteration_array_loop_runtime();
    const bool ok69 = test_iteration_slice_loop_runtime();
    const bool ok70 = test_iteration_range_loop_runtime();
    const bool ok71 = test_iteration_unsupported_iterable_hard_error();
    const bool ok72 = test_iteration_pure_infer_range_runtime_and_regressions();
    const bool ok73 = test_iteration_loop_binder_variadic_typedef_arg_compiles();
    const bool ok74 = test_unsuffixed_array_literal_slice_runtime_and_llvm();
    const bool ok75 = test_unsuffixed_named_array_to_slice_runtime();
    const bool ok76 = test_unsuffixed_array_literal_call_arg_and_return_contexts();
    const bool ok77 = test_unsuffixed_array_literal_float_context_rejected();
    const bool ok78 = test_iteration_loop_break_value_context_syntax_only();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 || !ok9 || !ok10 || !ok11 ||
        !ok12 || !ok13 || !ok14 || !ok15 || !ok16 || !ok17 || !ok18 || !ok19 || !ok20 || !ok21 || !ok22 || !ok23 ||
        !ok24 || !ok25 || !ok26 || !ok27 || !ok28 || !ok29 || !ok30 || !ok31 || !ok32 || !ok33 || !ok34 || !ok35 ||
        !ok36 || !ok37 || !ok38 || !ok39 || !ok40 || !ok41 || !ok42 || !ok43 || !ok44 || !ok45 || !ok46 || !ok47 ||
        !ok48 || !ok49 || !ok50 || !ok51 || !ok52 || !ok53 || !ok54 || !ok55 || !ok56 || !ok57 || !ok58 || !ok59 ||
        !ok60 || !ok61 || !ok62 || !ok63 || !ok64 || !ok65 || !ok66 || !ok67 || !ok68 || !ok69 || !ok70 ||
        !ok71 || !ok72 || !ok73 || !ok74 || !ok75 || !ok76 || !ok77 || !ok78) {
        return 1;
    }

    std::cout << "parus cli tests passed\n";
    return 0;
}
