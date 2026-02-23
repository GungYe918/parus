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

} // namespace

int main() {
    const bool ok1 = test_valid_lei_and_semantic_empty();
    const bool ok2 = test_invalid_lei_reports_diagnostics();
    const bool ok3 = test_parus_regression_valid_pr();

    if (!ok1 || !ok2 || !ok3) return 1;
    std::cout << "parusd lsp tests passed\n";
    return 0;
}
