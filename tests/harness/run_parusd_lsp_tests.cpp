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

std::string run_lsp_session(
    const std::vector<std::string>& payloads,
    int& exit_code,
    std::string_view env_prefix = {}
) {
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

    std::string cmd = "PARUS_NO_CORE=1 ";
    if (!env_prefix.empty()) {
        cmd += std::string(env_prefix) + " ";
    }
    cmd += "PARUSC=\"" + std::string(PARUSC_BUILD_BIN) + "\" "
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

bool test_parus_core_export_index_auto_loaded_for_non_core_bundle() {
    const auto stamp = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto root = std::filesystem::temp_directory_path() / ("parusd-core-index-" + stamp);
    const auto sysroot = root / "sysroot";
    const auto core_index = sysroot / ".cache" / "exports" / "core.exports.json";
    const auto main_pr = root / "main.pr";

    const std::string core_index_text =
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
    const std::string main_text =
        "def main() -> i32 {\n"
        "  let x: i32 = 10i32;\n"
        "  x.size();\n"
        "  return 0i32;\n"
        "}\n";
    if (!write_text(core_index, core_index_text) || !write_text(main_pr, main_text)) {
        std::cerr << "failed to write core export-index fixture\n";
        std::error_code ec{};
        std::filesystem::remove_all(root, ec);
        return false;
    }

    const std::string uri = to_file_uri(main_pr);
    const std::string escaped_main = json_escape(main_text);
    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":41,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + escaped_main + "\"}}}",
        R"({"jsonrpc":"2.0","id":42,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string env_prefix =
        "PARUS_NO_CORE=0 PARUS_SYSROOT=\"" + sysroot.string() + "\"";
    const std::string out = run_lsp_session(payloads, rc, env_prefix);
    int rc_disabled = 0;
    const std::string out_disabled = run_lsp_session(payloads, rc_disabled, "PARUS_NO_CORE=1");
    std::error_code ec{};
    std::filesystem::remove_all(root, ec);
    if (rc != 0) {
        std::cerr << "core export-index LSP session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"uri\":\"" + uri + "\",\"version\":1,\"diagnostics\":[]")) {
        std::cerr << "non-core bundle LSP should auto-load core export-index builtin acts\n" << out << "\n";
        return false;
    }
    if (rc_disabled != 0) {
        std::cerr << "LSP session with PARUS_NO_CORE=1 failed unexpectedly, rc=" << rc_disabled << "\n" << out_disabled << "\n";
        return false;
    }
    if (contains(out_disabled, "\"uri\":\"" + uri + "\",\"version\":1,\"diagnostics\":[]")) {
        std::cerr << "PARUS_NO_CORE=1 should disable implicit core export-index injection\n" << out_disabled << "\n";
        return false;
    }
    return true;
}

bool test_initialize_advertises_completion_and_definition() {
    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":11,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","id":12,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    if (rc != 0) {
        std::cerr << "initialize capability session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"completionProvider\"") || !contains(out, "\"definitionProvider\":true")) {
        std::cerr << "initialize response must advertise completion/definition capabilities\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_completion_keywords_parus_and_lei() {
    const std::string parus_uri = "file:///tmp/parusd_completion.pr";
    const std::string lei_uri = "file:///tmp/parusd_completion.lei";
    const std::string parus_text =
        "def main() -> i32 {\\n"
        "  ret\\n"
        "  return 0i32;\\n"
        "}\\n";
    const std::string lei_text =
        "plan master = master & {\\n"
        "  project = { name: \\\"ok\\\", version: \\\"0.1.0\\\" };\\n"
        "  bundles = [];\\n"
        "  tasks = [];\\n"
        "  codegens = [];\\n"
        "};\\n";

    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":21,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + parus_uri
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + parus_text + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + lei_uri
            + "\",\"languageId\":\"lei\",\"version\":1,\"text\":\"" + lei_text + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\""
            + parus_uri + "\"},\"position\":{\"line\":1,\"character\":0}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\""
            + lei_uri + "\"},\"position\":{\"line\":0,\"character\":0}}}",
        R"({"jsonrpc":"2.0","id":24,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    if (rc != 0) {
        std::cerr << "completion session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }

    if (!contains(out, "\"id\":22") || !contains(out, "\"label\":\"return\"")
        || !contains(out, "\"label\":\"require\"") || !contains(out, "\"label\":\"proto\"")
        || !contains(out, "\"label\":\"actor\"") || !contains(out, "\"label\":\"macro\"")
        || !contains(out, "\"label\":\"const\"")) {
        std::cerr << "parus completion result must include v0 keywords\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"id\":23") || !contains(out, "\"label\":\"plan\"")
        || !contains(out, "\"label\":\"import\"") || !contains(out, "\"label\":\"proto\"")) {
        std::cerr << "lei completion result must include keyword set\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_definition_local_symbol() {
    const std::string uri = "file:///tmp/parusd_definition_local.pr";
    const std::string text =
        "def add(a: i32, b: i32) -> i32 {\\n"
        "  return a + b;\\n"
        "}\\n"
        "def main() -> i32 {\\n"
        "  return add(a: 1i32, b: 2i32);\\n"
        "}\\n";

    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":31,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + text + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\""
            + uri + "\"},\"position\":{\"line\":4,\"character\":10}}}",
        R"({"jsonrpc":"2.0","id":33,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    if (rc != 0) {
        std::cerr << "local definition session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"id\":32") || !contains(out, "\"result\":[{\"uri\":\"" + uri + "\"")
        || !contains(out, "\"line\":0")) {
        std::cerr << "local definition must jump to same-file declaration span\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_definition_cimport_symbol() {
    std::error_code ec{};
    const auto root = std::filesystem::temp_directory_path(ec) / "parusd-definition-cimport";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "failed to create cimport definition temp root\n";
        return false;
    }

    const auto header = root / "Circle.h";
    const auto main_pr = root / "main.pr";
    const std::string header_text =
        "#ifndef CIRCLE_H\n"
        "#define CIRCLE_H\n"
        "int c_add(int a, int b);\n"
        "#endif\n";
    const std::string main_text =
        "import \"Circle.h\" as c;\n"
        "def main() -> i32 {\n"
        "  return c::c_add(1i32, 2i32);\n"
        "}\n";
    if (!write_text(header, header_text) || !write_text(main_pr, main_text)) {
        std::cerr << "failed to write cimport definition fixture\n";
        std::filesystem::remove_all(root, ec);
        return false;
    }

    const std::string uri = to_file_uri(main_pr);
    const std::string header_uri = to_file_uri(header);
    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":101,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + json_escape(uri)
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + json_escape(main_text) + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":102,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\""
            + json_escape(uri) + "\"},\"position\":{\"line\":2,\"character\":12}}}",
        R"({"jsonrpc":"2.0","id":103,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    std::filesystem::remove_all(root, ec);
    if (rc != 0) {
        std::cerr << "cimport definition session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (contains(out, "CImportLibClangUnavailable")) {
        return true;
    }
    if (!contains(out, "\"id\":102") || !contains(out, "\"uri\":\"" + header_uri + "\"")) {
        std::cerr << "cimport definition must jump to C header declaration\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_definition_cimport_promoted_macro_symbol() {
    std::error_code ec{};
    const auto root = std::filesystem::temp_directory_path(ec) / "parusd-definition-cimport-macro";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "failed to create cimport promoted-macro definition temp root\n";
        return false;
    }

    const auto header = root / "Circle.h";
    const auto main_pr = root / "main.pr";
    const std::string header_text =
        "#ifndef CIRCLE_H\n"
        "#define CIRCLE_H\n"
        "int c_add(int a, int b);\n"
        "#define CADD(a, b) c_add(a, b)\n"
        "#endif\n";
    const std::string main_text =
        "import \"Circle.h\" as c;\n"
        "def main() -> i32 {\n"
        "  return c::CADD(1i32, 2i32);\n"
        "}\n";
    if (!write_text(header, header_text) || !write_text(main_pr, main_text)) {
        std::cerr << "failed to write cimport promoted-macro definition fixture\n";
        std::filesystem::remove_all(root, ec);
        return false;
    }

    const std::string uri = to_file_uri(main_pr);
    const std::string header_uri = to_file_uri(header);
    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":111,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + json_escape(uri)
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + json_escape(main_text) + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":112,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\""
            + json_escape(uri) + "\"},\"position\":{\"line\":2,\"character\":12}}}",
        R"({"jsonrpc":"2.0","id":113,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc);
    std::filesystem::remove_all(root, ec);
    if (rc != 0) {
        std::cerr << "cimport promoted-macro definition session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (contains(out, "CImportLibClangUnavailable")) {
        return true;
    }
    if (!contains(out, "\"id\":112") || !contains(out, "\"uri\":\"" + header_uri + "\"")) {
        std::cerr << "cimport promoted-macro definition must jump to C header declaration\n" << out << "\n";
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
    const std::string add_uri = to_file_uri(math_add);
    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + json_escape(uri)
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + json_escape(app_main_text) + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\""
            + json_escape(uri) + "\"},\"position\":{\"line\":2,\"character\":9}}}",
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
    if (!contains(out, "\"id\":2") || !contains(out, "\"uri\":\"" + add_uri + "\"")) {
        std::cerr << "cross-file definition must jump to imported bundle export declaration\n" << out << "\n";
        return false;
    }
    return true;
}

bool test_parus_incremental_newline_falls_back_cleanly() {
    const std::string uri = "file:///tmp/parusd_incremental_actor.pr";
    const std::string text =
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

    std::vector<std::string> payloads{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"languageId\":\"parus\",\"version\":1,\"text\":\"" + json_escape(text) + "\"}}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"version\":2},\"contentChanges\":[{\"range\":{\"start\":{\"line\":11,\"character\":0},\"end\":{\"line\":11,\"character\":0}},\"text\":\"\\n\"}]}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"" + uri
            + "\",\"version\":3},\"contentChanges\":[{\"range\":{\"start\":{\"line\":11,\"character\":0},\"end\":{\"line\":12,\"character\":0}},\"text\":\"\"}]}}",
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":{}})",
    };

    int rc = 0;
    const std::string out = run_lsp_session(payloads, rc, "PARUSD_TRACE_INCREMENTAL=1");
    if (rc != 0) {
        std::cerr << "incremental actor newline lsp session failed, rc=" << rc << "\n" << out << "\n";
        return false;
    }
    if (!contains(out, "\"uri\":\"" + uri + "\",\"version\":2,\"diagnostics\":[]") ||
        !contains(out, "\"uri\":\"" + uri + "\",\"version\":3,\"diagnostics\":[]")) {
        std::cerr << "incremental newline edits must keep diagnostics empty\n" << out << "\n";
        return false;
    }
    if (!contains(out, "parse=fallback-full")) {
        std::cerr << "incremental newline edit must trace fallback-full reparse mode\n" << out << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool ok1 = test_valid_lei_and_semantic_empty();
    const bool ok2 = test_invalid_lei_reports_diagnostics();
    const bool ok3 = test_parus_regression_valid_pr();
    const bool ok4 = test_initialize_advertises_completion_and_definition();
    const bool ok5 = test_completion_keywords_parus_and_lei();
    const bool ok6 = test_definition_local_symbol();
    const bool ok7 = test_definition_cimport_symbol();
    const bool ok8 = test_definition_cimport_promoted_macro_symbol();
    const bool ok9 = test_parus_module_first_bundle_context();
    const bool ok10 = test_parus_core_export_index_auto_loaded_for_non_core_bundle();
    const bool ok11 = test_parus_incremental_newline_falls_back_cleanly();

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 || !ok9 || !ok10 || !ok11) return 1;
    std::cout << "parusd lsp tests passed\n";
    return 0;
}
