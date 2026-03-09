#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

namespace {

std::pair<int, std::string> run_capture(const std::string& command) {
    const std::string tmp = "/tmp/parus_cli_capture.txt";
    const std::string full = command + " > " + tmp + " 2>&1";
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
        "  let h: ^&i32 = ^&x;\n"
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
    const auto core_ok = temp_root / "core_ok.pr";
    const auto core_dup = temp_root / "core_dup.pr";

    const std::string base_decl =
        "acts for i32 {\n"
        "  def size(self) -> i32 {\n"
        "    return 4i32;\n"
        "  }\n"
        "};\n";

    const std::string dup_decl =
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

    if (!write_text(non_core, base_decl) ||
        !write_text(core_ok, base_decl) ||
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
    if (rc_non_core == 0 || !contains(out_non_core, "reserved for bundle 'core'")) {
        std::cerr << "non-core builtin acts should fail\n" << out_non_core;
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
    std::filesystem::remove_all(temp_root, ec);
    if (rc_core_dup == 0 || !contains(out_core_dup, "duplicate default acts declaration for type i32")) {
        std::cerr << "core duplicate builtin acts should fail\n" << out_core_dup;
        return false;
    }
    return true;
}

bool test_auto_core_prelude_for_single_pr() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto temp_root = std::filesystem::temp_directory_path(ec) / "parus-cli-auto-core";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root / "sysroot/core/src", ec);
    if (ec) {
        std::cerr << "temp dir create failed\n";
        return false;
    }

    const auto prelude = temp_root / "sysroot/core/src/prelude.pr";
    const auto main_pr = temp_root / "main.pr";
    const auto out_bin = temp_root / "app";

    const std::string prelude_src =
        "acts for i32 {\n"
        "  def size(self) -> i32 {\n"
        "    return 4i32;\n"
        "  }\n"
        "};\n";

    const std::string main_src =
        "def main() -> i32 {\n"
        "  let x: i32 = 123i32;\n"
        "  return x.size();\n"
        "}\n";

    if (!write_text(prelude, prelude_src) || !write_text(main_pr, main_src)) {
        std::cerr << "failed to write auto core test files\n";
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string compile_cmd =
        "\"" + bin + "\" tool parusc -- \"" + main_pr.string() + "\"" +
        " --sysroot \"" + (temp_root / "sysroot").string() + "\"" +
        " -o \"" + out_bin.string() + "\"";
    auto [rc_compile, out_compile] = run_capture(compile_cmd);
    if (rc_compile != 0) {
        std::cerr << "auto core compile failed\n" << out_compile;
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }

    const std::string run_cmd =
        "cd \"" + temp_root.string() + "\" && \"./app\"; echo EXIT:$?";
    auto [rc_run, out_run] = run_capture(run_cmd);
    std::filesystem::remove_all(temp_root, ec);

    if (rc_run != 0) {
        std::cerr << "auto core run command failed\n" << out_run;
        return false;
    }
    if (!contains(out_run, "EXIT:4")) {
        std::cerr << "auto core exit mismatch (expected 4)\n" << out_run;
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
    const bool ok15 = test_auto_core_prelude_for_single_pr();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 || !ok9 || !ok10 || !ok11 ||
        !ok12 || !ok13 || !ok14 || !ok15) {
        return 1;
    }

    std::cout << "parus cli tests passed\n";
    return 0;
}
