// compiler/parusc/src/driver/Driver.cpp
#include <parusc/driver/Driver.hpp>

#include <parusc/p0/P0Compiler.hpp>

#include <parus/os/File.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace parusc::driver {

    namespace {

        /// @brief 입력 파일을 읽고 내부 컴파일러 호출 정보를 구성한다.
        bool prepare_invocation_(
            const cli::Options& opt,
            const char* argv0,
            p0::Invocation& out_inv,
            std::string& out_err
        ) {
            if (opt.inputs.empty()) {
                out_err = "no input file";
                return false;
            }
            const auto& input = opt.inputs.front();

            std::string src;
            std::string io_err;
            if (!parus::open_file(input, src, io_err)) {
                out_err = io_err;
                return false;
            }

            out_inv.input_path = input;
            out_inv.normalized_input_path = parus::normalize_path(input);
            out_inv.source_text = std::move(src);
            out_inv.bundle_root = opt.bundle.bundle_root;
            if (out_inv.bundle_root.empty()) {
                std::error_code ec{};
                const auto input_parent = std::filesystem::weakly_canonical(
                    std::filesystem::path(out_inv.normalized_input_path).parent_path(),
                    ec
                );
                if (!ec) {
                    out_inv.bundle_root = input_parent.string();
                } else {
                    out_inv.bundle_root = std::filesystem::path(out_inv.normalized_input_path).parent_path().string();
                }
            } else {
                out_inv.bundle_root = parus::normalize_path(out_inv.bundle_root);
            }
            out_inv.bundle_sources = opt.bundle.bundle_sources;
            out_inv.bundle_deps = opt.bundle.bundle_deps;
            out_inv.module_head = opt.bundle.module_head;
            out_inv.module_imports = opt.bundle.module_imports;
            out_inv.load_export_index_paths = opt.bundle.load_export_index_paths;
            if (argv0 != nullptr) {
                out_inv.driver_executable_path = parus::normalize_path(argv0);
            }
            out_inv.options = &opt;
            return true;
        }

        std::string getenv_string_(const char* key) {
            if (key == nullptr) return {};
            const char* p = std::getenv(key);
            if (p == nullptr) return {};
            return std::string(p);
        }

        int run_argv_(const std::vector<std::string>& argv) {
            if (argv.empty()) return 1;
#if defined(_WIN32)
            std::vector<const char*> cargs;
            cargs.reserve(argv.size() + 1);
            for (const auto& a : argv) cargs.push_back(a.c_str());
            cargs.push_back(nullptr);
            const int rc = _spawnvp(_P_WAIT, argv[0].c_str(), cargs.data());
            return (rc < 0) ? 1 : rc;
#else
            std::vector<char*> cargs;
            cargs.reserve(argv.size() + 1);
            for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
            cargs.push_back(nullptr);

            pid_t pid = -1;
            const int sp = posix_spawnp(&pid, argv[0].c_str(), nullptr, nullptr, cargs.data(), environ);
            if (sp != 0) return 1;

            int status = 0;
            if (waitpid(pid, &status, 0) < 0) return 1;
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 1;
#endif
        }

        std::string resolve_parusd_path_(const char* argv0) {
            namespace fs = std::filesystem;
            const auto from_env = getenv_string_("PARUSD");
            if (!from_env.empty()) return from_env;

            const auto toolchain_root = getenv_string_("PARUS_TOOLCHAIN_ROOT");
            if (!toolchain_root.empty()) {
                const fs::path candidate = fs::path(toolchain_root) / "bin" / "parusd";
                if (fs::exists(candidate)) return candidate.string();
            }

            if (argv0 != nullptr) {
                std::error_code ec{};
                fs::path driver_path(parus::normalize_path(argv0));
                fs::path resolved = fs::weakly_canonical(driver_path, ec);
                if (ec || resolved.empty()) {
                    ec.clear();
                    resolved = driver_path;
                }
                if (!resolved.empty()) {
                    const fs::path sibling = resolved.parent_path() / "parusd";
                    if (fs::exists(sibling)) return sibling.string();
                }
            }

            return "parusd";
        }

        int run_lsp_(const cli::Options& opt, const char* argv0) {
            if (!opt.lsp_stdio) {
                std::cerr << "error: lsp mode requires --stdio\n";
                return 1;
            }

            const std::string parusd = resolve_parusd_path_(argv0);
            const std::vector<std::string> child_argv{parusd, "--stdio"};
            return run_argv_(child_argv);
        }

    } // namespace

    int run(const cli::Options& opt, const char* argv0) {
        switch (opt.mode) {
            case cli::Mode::kCompile: {
                p0::Invocation inv{};
                std::string err;
                if (!prepare_invocation_(opt, argv0, inv, err)) {
                    std::cerr << "error: " << err << "\n";
                    return 1;
                }
                return p0::run(inv);
            }
            case cli::Mode::kLsp:
                return run_lsp_(opt, argv0);
            case cli::Mode::kUsage:
            case cli::Mode::kVersion:
            default:
                return 0;
        }
    }

} // namespace parusc::driver
