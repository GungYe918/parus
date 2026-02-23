#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

namespace {

std::pair<int, std::string> run_capture(const std::string& command) {
    const std::string tmp = "/tmp/parus_config_capture.txt";
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

bool test_global_config_roundtrip() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto root = std::filesystem::temp_directory_path(ec) / "parus-config-global";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "xdg", ec);
    if (ec) return false;

    const std::string env = "XDG_CONFIG_HOME=\"" + (root / "xdg").string() + "\" ";

    auto [rc_init, out_init] = run_capture(env + "\"" + bin + "\" config init --global");
    if (rc_init != 0) {
        std::cerr << "global init failed\n" << out_init;
        return false;
    }

    auto [rc_set, out_set] = run_capture(env + "\"" + bin + "\" config set diag.lang ko --global");
    if (rc_set != 0) {
        std::cerr << "global set failed\n" << out_set;
        return false;
    }

    auto [rc_get, out_get] = run_capture(env + "\"" + bin + "\" config get diag.lang --global");
    if (rc_get != 0 || !contains(out_get, "ko")) {
        std::cerr << "global get failed\n" << out_get;
        return false;
    }

    auto [rc_show, out_show] = run_capture(env + "\"" + bin + "\" config show --global --format json");
    if (rc_show != 0 || !contains(out_show, "\"diag.lang\"")) {
        std::cerr << "global show json failed\n" << out_show;
        return false;
    }

    return true;
}

bool test_project_override() {
    const std::string bin = PARUS_BUILD_BIN;
    std::error_code ec{};
    const auto root = std::filesystem::temp_directory_path(ec) / "parus-config-project";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "xdg", ec);
    std::filesystem::create_directories(root / "proj", ec);
    if (ec) return false;

    if (!write_text(root / "proj" / "config.lei", "plan master = master & { project = { name: \"p\", version: \"0.1.0\" }; bundles = []; tasks = []; codegens = []; };")) {
        return false;
    }

    const std::string env = "XDG_CONFIG_HOME=\"" + (root / "xdg").string() + "\" ";
    const std::string in_proj = "cd \"" + (root / "proj").string() + "\" && " + env;

    auto [rc_set_global, out_set_global] = run_capture(env + "\"" + bin + "\" config set diag.lang en --global");
    if (rc_set_global != 0) {
        std::cerr << "set global failed\n" << out_set_global;
        return false;
    }

    auto [rc_set_project, out_set_project] = run_capture(in_proj + "\"" + bin + "\" config set diag.lang ko --project");
    if (rc_set_project != 0) {
        std::cerr << "set project failed\n" << out_set_project;
        return false;
    }

    auto [rc_get_eff, out_get_eff] = run_capture(in_proj + "\"" + bin + "\" config get diag.lang --effective");
    if (rc_get_eff != 0 || !contains(out_get_eff, "ko")) {
        std::cerr << "effective override failed\n" << out_get_eff;
        return false;
    }
    return true;
}

bool test_unknown_key_rejected() {
    const std::string bin = PARUS_BUILD_BIN;
    auto [rc, out] = run_capture("\"" + bin + "\" config set unknown.foo 1 --global");
    if (rc == 0 || !contains(out, "unknown config key")) {
        std::cerr << "unknown key rejection failed\n" << out;
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool ok1 = test_global_config_roundtrip();
    const bool ok2 = test_project_override();
    const bool ok3 = test_unknown_key_rejected();
    if (!ok1 || !ok2 || !ok3) return 1;
    std::cout << "parus config tests passed\n";
    return 0;
}
