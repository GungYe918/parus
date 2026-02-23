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
    const std::string pr = PARUS_MAIN_PR;

    auto [rc, out] = run_capture("\"" + bin + "\" check \"" + pr + "\"");
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
        "  sources = [\"main.pr\"];\n"
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

} // namespace

int main() {
    const bool ok1 = test_help_and_version();
    const bool ok2 = test_build_and_graph();
    const bool ok3 = test_check_pr();
    const bool ok4 = test_check_lei_project();
    const bool ok5 = test_doctor_json_and_global_json_reject();
    const bool ok6 = test_tool_passthrough();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6) {
        return 1;
    }

    std::cout << "parus cli tests passed\n";
    return 0;
}
