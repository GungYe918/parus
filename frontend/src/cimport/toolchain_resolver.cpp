#include <parus/cimport/ToolchainResolver.hpp>

#include <parus/os/File.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parus::cimport {
    namespace {

        std::mutex g_probe_cache_mu{};
        std::unordered_map<std::string, CImportToolchainProbeResult> g_probe_cache{};

        std::string getenv_string_(const char* key) {
            if (key == nullptr || *key == '\0') return {};
            if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
                return std::string(v);
            }
            return {};
        }

        std::string trim_copy_(std::string s) {
            auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
            auto front = std::find_if(s.begin(), s.end(), not_space);
            if (front == s.end()) return {};
            auto back = std::find_if(s.rbegin(), s.rend(), not_space).base();
            return std::string(front, back);
        }

        std::string select_compiler_(std::string_view compiler_hint) {
            if (!compiler_hint.empty()) return std::string(compiler_hint);
            const std::string from_env = getenv_string_("PARUS_CC");
            if (!from_env.empty()) return from_env;
            return "cc";
        }

        std::string shell_quote_(std::string_view s) {
            if (s.empty()) return "''";
            bool need_quote = false;
            for (const char c : s) {
                if (std::isspace(static_cast<unsigned char>(c)) ||
                    c == '\'' || c == '"' || c == '\\' || c == '$') {
                    need_quote = true;
                    break;
                }
            }
            if (!need_quote) return std::string(s);
            std::string out{"'"};
            for (const char c : s) {
                if (c == '\'') out += "'\\''";
                else out.push_back(c);
            }
            out += "'";
            return out;
        }

        std::vector<std::string> parse_include_dirs_from_verbose_output_(std::string_view text) {
            std::vector<std::string> out{};
            bool in_search_block = false;

            size_t pos = 0;
            while (pos <= text.size()) {
                const size_t line_end = text.find('\n', pos);
                std::string line = (line_end == std::string_view::npos)
                    ? std::string(text.substr(pos))
                    : std::string(text.substr(pos, line_end - pos));
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const std::string trimmed = trim_copy_(line);

                if (!in_search_block) {
                    if (trimmed.find("#include <...> search starts here:") != std::string::npos ||
                        trimmed.find("#include \"...\" search starts here:") != std::string::npos) {
                        in_search_block = true;
                    }
                } else {
                    if (trimmed.find("End of search list.") != std::string::npos) {
                        break;
                    }
                    if (!trimmed.empty()) {
                        std::string one = trimmed;
                        constexpr std::string_view kFramework = " (framework directory)";
                        if (one.size() > kFramework.size() &&
                            one.ends_with(std::string(kFramework))) {
                            one.resize(one.size() - kFramework.size());
                            one = trim_copy_(one);
                        }
                        if (!one.empty() && one.front() != '(') {
                            out.push_back(one);
                        }
                    }
                }

                if (line_end == std::string_view::npos) break;
                pos = line_end + 1;
            }

            return out;
        }

        CImportToolchainProbeResult run_probe_(std::string_view compiler_hint) {
            CImportToolchainProbeResult out{};

            const std::string compiler = select_compiler_(compiler_hint);
            const std::filesystem::path tmp_src =
                std::filesystem::temp_directory_path() / "parus_cimport_probe.c";
            {
                std::ofstream ofs(tmp_src, std::ios::binary | std::ios::trunc);
                if (!ofs.is_open()) {
                    out.warning = "failed to create temporary source file for c-import auto-probe";
                    return out;
                }
                ofs << "/* parus c-import probe */\n";
            }

            const std::string command =
                compiler + " -E -x c -v " + shell_quote_(tmp_src.string()) + " -o - 2>&1";
#if defined(_WIN32)
            FILE* pipe = _popen(command.c_str(), "r");
#else
            FILE* pipe = popen(command.c_str(), "r");
#endif
            if (pipe == nullptr) {
                std::error_code ec{};
                std::filesystem::remove(tmp_src, ec);
                out.warning = "failed to run C compiler for c-import auto-probe: " + compiler;
                return out;
            }

            std::string output{};
            char buffer[1024];
            while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
                output.append(buffer);
            }
#if defined(_WIN32)
            (void)_pclose(pipe);
#else
            (void)pclose(pipe);
#endif

            std::error_code ec{};
            std::filesystem::remove(tmp_src, ec);

            const auto parsed = parse_include_dirs_from_verbose_output_(output);
            append_unique_normalized_paths(out.isystem_dirs, parsed);
            if (out.isystem_dirs.empty()) {
                out.warning = "c-import auto-probe did not find system include directories (compiler: " + compiler + ")";
            }
            return out;
        }

    } // namespace

    void append_unique_normalized_paths(std::vector<std::string>& dst,
                                        const std::vector<std::string>& src) {
        std::unordered_set<std::string> seen{};
        std::vector<std::string> out{};
        out.reserve(dst.size() + src.size());
        seen.reserve((dst.size() + src.size()) * 2u + 1u);
        auto append_one = [&](std::string_view raw) {
            if (raw.empty()) return;
            const std::string norm = parus::normalize_path(std::string(raw));
            if (norm.empty()) return;
            if (!seen.insert(norm).second) return;
            out.push_back(norm);
        };
        for (const auto& d : dst) append_one(d);
        for (const auto& s : src) append_one(s);
        dst.swap(out);
    }

    CImportToolchainProbeResult probe_default_c_system_include_dirs(std::string_view compiler_hint) {
        const std::string compiler = select_compiler_(compiler_hint);
        {
            std::lock_guard<std::mutex> lock(g_probe_cache_mu);
            if (auto it = g_probe_cache.find(compiler); it != g_probe_cache.end()) {
                return it->second;
            }
        }

        CImportToolchainProbeResult probed = run_probe_(compiler);

        {
            std::lock_guard<std::mutex> lock(g_probe_cache_mu);
            g_probe_cache[compiler] = probed;
        }
        return probed;
    }

} // namespace parus::cimport
