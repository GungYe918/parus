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
    const std::string full = "(export PARUS_NO_CORE=1; " + command + ") > " + tmp + " 2>&1";
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
        "  return m::arith::hidden(a: 1i32, b: 2i32);\n"
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
        "cd \"" + temp_root.string() + "\" && \".lei/out/bin/app\"; echo EXIT:$?";
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
        "cd \"" + temp_root.string() + "\" && \".lei/out/bin/app\"; echo EXIT:$?";
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
        "use Equatable;\n"
        "\n"
        "def main() -> i32 {\n"
        "  return 0i32;\n"
        "}\n";
    const std::string no_marker_src =
        "use Equatable;\n"
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
            " --module-head " + bundle_name +
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
        "  stdio::printf($\"sum={1i32 + 2i32}\");\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(stdio_h, header_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write c-header format bridge test files\n";
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
        std::cerr << "stdio::printf($\"...\") format bridge should typecheck\n" << out;
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
        "  set c = Counter(seed: 1i32);\n"
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
        "  set c = Counter(seed: 1i32);\n"
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
        "  set c = Counter(seed: 1i32);\n"
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

bool test_hosted_actor_parus_lld_mode_rejected() {
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
        "  set c = Counter(seed: 1i32);\n"
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

    if (rc == 0) {
        std::cerr << "hosted actor link with -fuse-linker=parus-lld must fail\n" << out;
        return false;
    }
    if (!contains(out, "hosted actor runtime requires a system clang++ driver link")) {
        std::cerr << "hosted actor parus-lld rejection did not report the expected policy error\n" << out;
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
    const bool ok24 = test_c_header_import_stdio_format_bridge_single_arg();
    const bool ok25 = test_c_header_import_include_dir_option();
    const bool ok26 = test_c_header_import_union_manual_get_gate();
    const bool ok27 = test_c_header_import_union_manual_set_gate();
    const bool ok28 = test_actor_rejected_in_no_std_profile();
    const bool ok29 = test_actor_allowed_in_freestanding_profile();
    const bool ok30 = test_hosted_actor_link_uses_clang_driver();
    const bool ok31 = test_hosted_actor_parus_lld_mode_rejected();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 || !ok9 || !ok10 || !ok11 ||
        !ok12 || !ok13 || !ok14 || !ok15 || !ok16 || !ok17 || !ok18 || !ok19 || !ok20 || !ok21 || !ok22 || !ok23 ||
        !ok24 || !ok25 || !ok26 || !ok27 || !ok28 || !ok29 || !ok30 || !ok31) {
        return 1;
    }

    std::cout << "parus cli tests passed\n";
    return 0;
}
