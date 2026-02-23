#include <parus_tool/doctor/Doctor.hpp>

#include <parus_tool/proc/Process.hpp>
#include <parus_tool/toolchain/Resolver.hpp>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace parus_tool::doctor {

namespace {

constexpr const char* kAnsiReset = "\033[0m";
constexpr const char* kAnsiGreen = "\033[32m";
constexpr const char* kAnsiRed = "\033[31m";
constexpr const char* kAnsiOrange = "\033[38;5;208m";
constexpr const char* kAnsiCyan = "\033[36m";

bool color_enabled(std::string_view mode) {
    if (mode == "never") return false;
    if (mode == "always") return true;
    if (std::getenv("NO_COLOR") != nullptr) return false;
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

const char* level_color(Level l) {
    switch (l) {
        case Level::kPass: return kAnsiGreen;
        case Level::kWarn: return kAnsiOrange;
        case Level::kFail: return kAnsiRed;
    }
    return "";
}

std::string level_text(Level l) {
    switch (l) {
        case Level::kPass: return "PASS";
        case Level::kWarn: return "WARN";
        case Level::kFail: return "FAIL";
    }
    return "UNKNOWN";
}

std::string level_symbol(Level l) {
    switch (l) {
        case Level::kPass: return "✓";
        case Level::kWarn: return "!";
        case Level::kFail: return "✗";
    }
    return "?";
}

std::string level_rank(Level l) {
    switch (l) {
        case Level::kPass: return "pass";
        case Level::kWarn: return "warn";
        case Level::kFail: return "fail";
    }
    return "fail";
}

std::string category_name(Category c) {
    switch (c) {
        case Category::kTools: return "Tools";
        case Category::kEnvironment: return "Environment";
        case Category::kFilesystem: return "Filesystem";
        case Category::kSmoke: return "Smoke";
    }
    return "Unknown";
}

std::string category_key(Category c) {
    switch (c) {
        case Category::kTools: return "tools";
        case Category::kEnvironment: return "environment";
        case Category::kFilesystem: return "filesystem";
        case Category::kSmoke: return "smoke";
    }
    return "unknown";
}

std::string paint(std::string_view text, const char* color, bool enabled) {
    if (!enabled) return std::string(text);
    return std::string(color) + std::string(text) + kAnsiReset;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void add_item(std::vector<Item>& items,
              std::string id,
              Category cat,
              Level level,
              std::string message,
              std::vector<std::string> hints = {}) {
    items.push_back(Item{
        .id = std::move(id),
        .category = cat,
        .level = level,
        .message = std::move(message),
        .hints = std::move(hints),
    });
}

int run_probe(const std::vector<std::string>& argv) {
    std::string out;
    int rc = 1;
    if (proc::run_argv_capture_stdout(argv, out, rc)) {
        return rc;
    }
    return proc::run_argv(argv);
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs << text;
    return ofs.good();
}

std::vector<std::string> default_hints_for(std::string_view id, std::string_view path_hint = {}) {
    if (id == "tool.lei.version") {
        return {
            "Reinstall toolchain with ./install.sh so 'lei' is placed in the active toolchain bin directory.",
            "Verify launcher path: ~/.local/bin/lei --version",
            "If custom root is used, set it explicitly: parus config set toolchain.root \"<path>\" --global",
        };
    }
    if (id == "tool.parusc.version") {
        return {
            "Run ./install.sh to refresh parusc in the active toolchain.",
            "Verify launcher path: ~/.local/bin/parusc --version",
        };
    }
    if (id == "tool.parusd.version") {
        return {
            "Run ./install.sh to refresh parusd in the active toolchain.",
            "Verify launcher path: ~/.local/bin/parusd --version",
        };
    }
    if (id == "tool.parus-lld.help") {
        return {
            "Run ./install.sh to refresh parus-lld in the active toolchain.",
            "Verify launcher path: ~/.local/bin/parus-lld --help",
        };
    }
    if (id == "env.toolchain_root") {
        return {
            "Set toolchain root globally: parus config set toolchain.root \"<path>\" --global",
            "Or export PARUS_TOOLCHAIN_ROOT before running parus.",
        };
    }
    if (id == "env.parus_sysroot") {
        return {
            "Set sysroot path: parus config set toolchain.parus_sysroot \"<path>\" --global",
            "Or export PARUS_SYSROOT before running parus.",
        };
    }
    if (id == "fs.cache_writable") {
        return {
            "Ensure the working directory is writable.",
            "Or run from a writable project root so .lei-cache can be created.",
        };
    }
    if (id == "smoke.lei_check") {
        return {
            "Reproduce manually: lei --check \"" + std::string(path_hint) + "\"",
            "If tool lookup fails, run parus doctor -v to inspect resolved tool paths.",
        };
    }
    if (id == "smoke.parus_syntax") {
        return {
            "Reproduce manually: parusc -fsyntax-only \"" + std::string(path_hint) + "\"",
            "If sysroot/toolchain is stale, rerun ./install.sh.",
        };
    }
    return {};
}

Level category_level(const std::vector<Item>& items, Category category) {
    Level level = Level::kPass;
    for (const auto& it : items) {
        if (it.category != category) continue;
        if (it.level == Level::kFail) return Level::kFail;
        if (it.level == Level::kWarn) level = Level::kWarn;
    }
    return level;
}

} // namespace

int Report::exit_code(bool strict) const {
    bool has_fail = false;
    bool has_warn = false;
    for (const auto& it : items) {
        if (it.level == Level::kFail) has_fail = true;
        if (it.level == Level::kWarn) has_warn = true;
    }
    if (has_fail) return 2;
    if (has_warn && strict) return 1;
    return 0;
}

std::string Report::render_text(bool verbose, bool show_hints, const std::string& color_mode) const {
    const bool color = color_enabled(color_mode);
    const Category cats[] = {
        Category::kTools,
        Category::kEnvironment,
        Category::kFilesystem,
        Category::kSmoke,
    };

    std::ostringstream oss;
    oss << "Doctor summary (run parus doctor -v for details):\n";
    for (const auto cat : cats) {
        const auto lvl = category_level(items, cat);
        oss << "[" << paint(level_symbol(lvl), level_color(lvl), color) << "] " << category_name(cat) << "\n";
    }

    std::vector<const Item*> issues{};
    for (const auto& it : items) {
        if (it.level != Level::kPass) issues.push_back(&it);
    }

    if (!issues.empty()) {
        oss << "\nIssues:\n";
        for (const auto* it : issues) {
            oss << "[" << paint(level_symbol(it->level), level_color(it->level), color) << "] "
                << it->id << ": " << it->message << "\n";
            if (show_hints && !it->hints.empty()) {
                oss << "    " << paint("Recommended fix:", kAnsiCyan, color) << "\n";
                for (const auto& hint : it->hints) {
                    oss << "      - " << hint << "\n";
                }
            }
        }
    }

    if (verbose) {
        oss << "\nDetails:\n";
        for (const auto& it : items) {
            oss << "[ " << paint(level_text(it.level), level_color(it.level), color) << " ] "
                << it.id << ": " << it.message << "\n";
        }
    }

    int warn_count = 0;
    int fail_count = 0;
    for (const auto& it : items) {
        if (it.level == Level::kWarn) ++warn_count;
        if (it.level == Level::kFail) ++fail_count;
    }
    if (warn_count == 0 && fail_count == 0) {
        oss << "\nNo issues found.\n";
    } else {
        oss << "\nDoctor found issues in " << (warn_count + fail_count)
            << " item(s): " << fail_count << " fail, " << warn_count << " warn.\n";
    }
    return oss.str();
}

std::string Report::render_json() const {
    const Category cats[] = {
        Category::kTools,
        Category::kEnvironment,
        Category::kFilesystem,
        Category::kSmoke,
    };

    int pass_count = 0;
    int warn_count = 0;
    int fail_count = 0;
    for (const auto& it : items) {
        if (it.level == Level::kPass) ++pass_count;
        if (it.level == Level::kWarn) ++warn_count;
        if (it.level == Level::kFail) ++fail_count;
    }

    std::ostringstream oss;
    oss << "{";
    oss << "\"summary\":{"
        << "\"pass\":" << pass_count << ","
        << "\"warn\":" << warn_count << ","
        << "\"fail\":" << fail_count
        << "},";

    oss << "\"categories\":[";
    for (size_t i = 0; i < std::size(cats); ++i) {
        if (i != 0) oss << ",";
        const auto c = cats[i];
        const auto lvl = category_level(items, c);
        oss << "{"
            << "\"key\":\"" << category_key(c) << "\","
            << "\"name\":\"" << category_name(c) << "\","
            << "\"status\":\"" << level_rank(lvl) << "\""
            << "}";
    }
    oss << "],";

    oss << "\"issues\":[";
    bool first_issue = true;
    for (const auto& it : items) {
        if (it.level == Level::kPass) continue;
        if (!first_issue) oss << ",";
        first_issue = false;
        oss << "{"
            << "\"id\":\"" << json_escape(it.id) << "\","
            << "\"category\":\"" << category_key(it.category) << "\","
            << "\"level\":\"" << level_rank(it.level) << "\","
            << "\"message\":\"" << json_escape(it.message) << "\","
            << "\"hints\":[";
        for (size_t hi = 0; hi < it.hints.size(); ++hi) {
            if (hi != 0) oss << ",";
            oss << "\"" << json_escape(it.hints[hi]) << "\"";
        }
        oss << "]"
            << "}";
    }
    oss << "],";

    oss << "\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (i != 0) oss << ",";
        oss << "{"
            << "\"id\":\"" << json_escape(it.id) << "\","
            << "\"category\":\"" << category_key(it.category) << "\","
            << "\"level\":\"" << level_rank(it.level) << "\","
            << "\"message\":\"" << json_escape(it.message) << "\","
            << "\"hints\":[";
        for (size_t hi = 0; hi < it.hints.size(); ++hi) {
            if (hi != 0) oss << ",";
            oss << "\"" << json_escape(it.hints[hi]) << "\"";
        }
        oss << "]"
            << "}";
    }
    oss << "]";

    oss << "}";
    return oss.str();
}

Report run(const cli::DoctorOptions& doctor_opt,
           const std::string& toolchain_root,
           const char* argv0) {
    Report rep{};

    toolchain::ResolveOptions ro{};
    ro.toolchain_root = toolchain_root;
    ro.argv0 = argv0;

    const auto parusc = toolchain::resolve_tool("parusc", ro);
    const auto parusd = toolchain::resolve_tool("parusd", ro);
    const auto parus_lld = toolchain::resolve_tool("parus-lld", ro);
    const auto lei = toolchain::resolve_tool("lei", ro);

    auto probe = [&](std::string id, Category cat, const std::vector<std::string>& argv, bool fail_on_error) {
        const int rc = run_probe(argv);
        if (rc == 0) {
            add_item(rep.items, std::move(id), cat, Level::kPass, "ok");
            return;
        }
        const Level level = fail_on_error ? Level::kFail : Level::kWarn;
        auto hints = default_hints_for(id);
        add_item(rep.items,
                 std::move(id),
                 cat,
                 level,
                 "exit=" + std::to_string(rc),
                 std::move(hints));
    };

    probe("tool.parusc.version", Category::kTools, {parusc, "--version"}, true);
    probe("tool.parusd.version", Category::kTools, {parusd, "--version"}, true);
    probe("tool.parus-lld.help", Category::kTools, {parus_lld, "--help"}, true);
    probe("tool.lei.version", Category::kTools, {lei, "--version"}, true);

    if (!toolchain_root.empty()) {
        std::error_code ec{};
        if (std::filesystem::exists(toolchain_root, ec) && std::filesystem::is_directory(toolchain_root, ec)) {
            add_item(rep.items, "env.toolchain_root", Category::kEnvironment, Level::kPass, toolchain_root);
        } else {
            add_item(rep.items,
                     "env.toolchain_root",
                     Category::kEnvironment,
                     Level::kFail,
                     "path not found: " + toolchain_root,
                     default_hints_for("env.toolchain_root"));
        }
    } else {
        add_item(rep.items,
                 "env.toolchain_root",
                 Category::kEnvironment,
                 Level::kWarn,
                 "not set; using resolver fallback",
                 default_hints_for("env.toolchain_root"));
    }

    const char* sysroot = std::getenv("PARUS_SYSROOT");
    if (sysroot != nullptr && *sysroot != '\0') {
        std::error_code ec{};
        if (std::filesystem::exists(sysroot, ec) && std::filesystem::is_directory(sysroot, ec)) {
            add_item(rep.items, "env.parus_sysroot", Category::kEnvironment, Level::kPass, sysroot);
        } else {
            add_item(rep.items,
                     "env.parus_sysroot",
                     Category::kEnvironment,
                     Level::kFail,
                     std::string("path not found: ") + sysroot,
                     default_hints_for("env.parus_sysroot"));
        }
    } else {
        add_item(rep.items,
                 "env.parus_sysroot",
                 Category::kEnvironment,
                 Level::kWarn,
                 "not set",
                 default_hints_for("env.parus_sysroot"));
    }

    {
        std::error_code ec{};
        const auto cache_dir = std::filesystem::path(".lei-cache");
        std::filesystem::create_directories(cache_dir, ec);
        const auto probe_file = cache_dir / "doctor_write_probe.tmp";
        if (!ec && write_text(probe_file, "probe")) {
            std::filesystem::remove(probe_file, ec);
            add_item(rep.items, "fs.cache_writable", Category::kFilesystem, Level::kPass, cache_dir.string());
        } else {
            add_item(rep.items,
                     "fs.cache_writable",
                     Category::kFilesystem,
                     Level::kFail,
                     "failed to write .lei-cache",
                     default_hints_for("fs.cache_writable"));
        }
    }

    if (!doctor_opt.quick) {
#if defined(_WIN32)
        const int pid = static_cast<int>(_getpid());
#else
        const int pid = static_cast<int>(getpid());
#endif
        std::error_code ec{};
        const auto temp_root = std::filesystem::temp_directory_path(ec) / ("parus-doctor-" + std::to_string(pid));
        std::filesystem::create_directories(temp_root, ec);
        if (ec) {
            add_item(rep.items,
                     "smoke.temp_dir",
                     Category::kSmoke,
                     Level::kFail,
                     "failed to create temporary workspace");
            return rep;
        }

        const auto lei_file = temp_root / "config.lei";
        const auto pr_file = temp_root / "main.pr";

        const std::string lei_src =
            "plan master = master & {\n"
            "  project = {\n"
            "    name: \"doctor\",\n"
            "    version: \"0.0.0\",\n"
            "  };\n"
            "  bundles = [];\n"
            "  tasks = [];\n"
            "  codegens = [];\n"
            "};\n";
        const std::string pr_src =
            "def main() -> i32 {\n"
            "  return 0i32;\n"
            "}\n";

        if (!write_text(lei_file, lei_src) || !write_text(pr_file, pr_src)) {
            add_item(rep.items,
                     "smoke.write_inputs",
                     Category::kSmoke,
                     Level::kFail,
                     "failed to write temporary smoke test files");
            return rep;
        }

        const int rc_lei = run_probe({lei, "--check", lei_file.string()});
        if (rc_lei == 0) {
            add_item(rep.items, "smoke.lei_check", Category::kSmoke, Level::kPass, "ok");
        } else {
            add_item(rep.items,
                     "smoke.lei_check",
                     Category::kSmoke,
                     Level::kFail,
                     "exit=" + std::to_string(rc_lei),
                     default_hints_for("smoke.lei_check", lei_file.string()));
        }

        const int rc_pr = run_probe({parusc, "-fsyntax-only", pr_file.string()});
        if (rc_pr == 0) {
            add_item(rep.items, "smoke.parus_syntax", Category::kSmoke, Level::kPass, "ok");
        } else {
            add_item(rep.items,
                     "smoke.parus_syntax",
                     Category::kSmoke,
                     Level::kFail,
                     "exit=" + std::to_string(rc_pr),
                     default_hints_for("smoke.parus_syntax", pr_file.string()));
        }

        std::filesystem::remove_all(temp_root, ec);
    }

    return rep;
}

} // namespace parus_tool::doctor
