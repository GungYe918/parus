#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdlib>

namespace {

std::string make_frame(std::string_view payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + std::string(payload);
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec{};
    std::filesystem::create_directories(path.parent_path(), ec);
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

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string to_file_uri(const std::filesystem::path& path) {
    const auto abs = std::filesystem::weakly_canonical(path).generic_string();
    if (!abs.empty() && abs.front() == '/') return "file://" + abs;
    return "file:///" + abs;
}

std::string run_lsp_session(const std::vector<std::string>& payloads, int& exit_code) {
    const auto stamp = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto in_path = std::filesystem::temp_directory_path() / ("parusd-lsp-in-" + stamp + ".txt");
    const auto out_path = std::filesystem::temp_directory_path() / ("parusd-lsp-out-" + stamp + ".txt");

    std::string framed;
    for (const auto& p : payloads) {
        framed += make_frame(p);
    }
    if (!write_text(in_path, framed)) {
        exit_code = 1;
        return "failed to write input stream";
    }

    const std::string cmd =
        "PARUSC=\"" + std::string(PARUSC_BUILD_BIN) + "\" "
        "\"" + std::string(PARUSD_BUILD_BIN) + "\" --stdio < \"" + in_path.string()
        + "\" > \"" + out_path.string() + "\" 2>&1";
    exit_code = std::system(cmd.c_str());
    const std::string out = read_text(out_path);

    std::error_code ec{};
    std::filesystem::remove(in_path, ec);
    std::filesystem::remove(out_path, ec);
    return out;
}

bool test_valid_lei_and_semantic_empty() {
    const std::string uri = "file:///tmp/parusd_valid.lei";
    const std::string valid_text =
        "plan master = master & {\\n"
        "  project = {\\n"
        "    name: \\\"ok\\\",\\n"
        "    version: \\\"0.1.0\\\",\\n"
        "  };\\n"
        "  bundles = [];\\n"
        "  tasks = [];\\n"
        "  codegens = [];\\n"
        "};\\n";

    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"languageId\":\"lei\",\"version\":1,\"text\":\"" + valid_text + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/semanticTokens/full\",\"params\":{\"textDocument\":{\"uri\":\""
            + uri + "\"}}}",
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    if (rc != 0) {
        std::cerr << "valid lei session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }

    if (!contains(out, "\"uri\":\"" + uri + "\",\"version\":1,\"diagnostics\":[]")) {
        std::cerr << "expected empty diagnostics for valid lei\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"id\":2") || !contains(out, "\"result\":{\"data\":[]}")) {
        std::cerr << "expected empty semantic token result for lei\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_invalid_lei_reports_diagnostics() {
    const std::string uri = "file:///tmp/parusd_invalid.lei";
    const std::string invalid_text = "@";

    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"languageId\":\"lei\",\"version\":1,\"text\":\"" + invalid_text + "\"}}}",
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    if (rc != 0) {
        std::cerr << "invalid lei session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }

    if (!contains(out, "\"uri\":\"" + uri + "\",\"version\":1,\"diagnostics\":[{")) {
        std::cerr << "expected non-empty diagnostics for invalid lei\n" << out << "\n";
        return false;
    }
    if (!contains(out, "C_UNEXPECTED_TOKEN")) {
        std::cerr << "expected LEI diagnostic code in output\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_parus_regression_valid_pr() {
    const std::string uri = "file:///tmp/parusd_valid.pr";
    const std::string valid_pr =
        "def main() -> i32 {\\n"
        "  return 0i32;\\n"
        "}\\n";

    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + valid_pr + "\"}}}",
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    if (rc != 0) {
        std::cerr << "parus regression session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"uri\":\"" + uri + "\",\"version\":1,\"diagnostics\":[]")) {
        std::cerr << "expected empty diagnostics for valid parus file\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_parus_module_first_bundle_context() {
    const auto stamp = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto root = std::filesystem::temp_directory_path() / ("parusd-module-first-" + stamp);

    const auto config_lei = root / "config.lei";
    const auto math_lei = root / "math" / "math.lei";
    const auto math_add = root / "math" / "api" / "src" / "add.pr";
    const auto app_lei = root / "app" / "app.lei";
    const auto app_main = root / "app" / "src" / "main.pr";

    const std::string config_text =
        "import math from \"./math/math.lei\";\n"
        "import app from \"./app/app.lei\";\n"
        "proto ProjectMeta { name: string; version: string; };\n"
        "plan master = master & {\n"
        "  project = ProjectMeta & {\n"
        "    name = \"lsp-demo\";\n"
        "    version = \"0.1.0\";\n"
        "  };\n"
        "  bundles = [math::math_bundle, app::app_bundle];\n"
        "  tasks = [];\n"
        "  codegens = [];\n"
        "};\n";

    const std::string math_lei_text =
        "export plan math_module = module & {\n"
        "  sources = [\"math/api/src/add.pr\"];\n"
        "  imports = [];\n"
        "};\n"
        "export plan math_bundle = bundle & {\n"
        "  name = \"math\";\n"
        "  kind = \"lib\";\n"
        "  modules = [math_module];\n"
        "  deps = [];\n"
        "};\n";

    const std::string app_lei_text =
        "export plan app_module = module & {\n"
        "  sources = [\"app/src/main.pr\"];\n"
        "  imports = [\"::math::api\"];\n"
        "};\n"
        "export plan app_bundle = bundle & {\n"
        "  name = \"app\";\n"
        "  kind = \"bin\";\n"
        "  modules = [app_module];\n"
        "  deps = [\"math\"];\n"
        "};\n";

    const std::string math_add_text =
        "export def add(a: i32, b: i32) -> i32 {\n"
        "  return a + b;\n"
        "}\n";

    const std::string app_main_text =
        "import ::math::api as m;\n"
        "def main() -> i32 {\n"
        "  return m::add(1i32, 2i32);\n"
        "}\n";

    if (!write_text(config_lei, config_text) ||
        !write_text(math_lei, math_lei_text) ||
        !write_text(math_add, math_add_text) ||
        !write_text(app_lei, app_lei_text) ||
        !write_text(app_main, app_main_text)) {
        std::cerr << "failed to write module-first fixture\n";
        std::error_code ec{};
        std::filesystem::remove_all(root, ec);
        return false;
    }

    const std::string uri = to_file_uri(app_main);
    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + json_escape(uri)
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + json_escape(app_main_text) + "\"}}}",
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    std::error_code ec{};
    std::filesystem::remove_all(root, ec);

    if (rc != 0) {
        std::cerr << "module-first parus session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"uri\":\"" + uri + "\",\"version\":1,\"diagnostics\":[]")) {
        std::cerr << "expected empty diagnostics for module-first cross-bundle import\n" << out << "\n";
        return false;
    }
    if (contains(out, "UndefinedName") || contains(out, "undeclared name")) {
        std::cerr << "unexpected unresolved-name diagnostics in module-first scenario\n" << out << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool ok1 = test_valid_lei_and_semantic_empty();
    const bool ok2 = test_invalid_lei_reports_diagnostics();
    const bool ok3 = test_parus_regression_valid_pr();
    const bool ok4 = test_parus_module_first_bundle_context();

    if (!ok1 || !ok2 || !ok3 || !ok4) return 1;
    std::cout << "parusd lsp tests passed\n";
    return 0;
}
