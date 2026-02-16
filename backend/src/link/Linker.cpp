// backend/src/link/Linker.cpp
#include <parus/backend/link/Linker.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace parus::backend::link {

    namespace {

        /// @brief 실행 파일 후보를 찾는다(절대 경로 우선, 없으면 PATH 의존 이름 그대로).
        std::string resolve_tool_candidate_(const std::string& name_or_path) {
            namespace fs = std::filesystem;
            if (name_or_path.empty()) return {};
            if (name_or_path.find('/') != std::string::npos) {
                return fs::exists(name_or_path) ? name_or_path : std::string{};
            }
            return name_or_path;
        }

        /// @brief 시스템 clang++ 도구 후보를 결정한다.
        std::string select_clangxx_() {
            namespace fs = std::filesystem;
            if (fs::exists("/usr/bin/clang++")) return "/usr/bin/clang++";
            return "clang++";
        }

        /// @brief 환경 변수 문자열을 읽는다.
        std::string getenv_string_(const char* key) {
            if (key == nullptr) return {};
            const char* p = std::getenv(key);
            if (p == nullptr) return {};
            return std::string(p);
        }

        /// @brief 문자열을 정수 hash(u64)로 해석한다.
        std::optional<uint64_t> parse_u64_(const std::string& s) {
            if (s.empty()) return std::nullopt;
            try {
                size_t idx = 0;
                const uint64_t v = std::stoull(s, &idx, 0);
                if (idx != s.size()) return std::nullopt;
                return v;
            } catch (...) {
                return std::nullopt;
            }
        }

        /// @brief 경로에서 마지막 파일명을 얻는다.
        std::string basename_(const std::string& path) {
            if (path.empty()) return {};
            namespace fs = std::filesystem;
            return fs::path(path).filename().string();
        }

        /// @brief argv 기반으로 프로세스를 실행하고 종료 코드를 반환한다.
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

        /// @brief object -> executable 링크 argv를 조립한다.
        std::vector<std::string> build_link_argv_(
            const std::string& linker,
            const std::vector<std::string>& objects,
            const std::string& output,
            bool use_lld_via_clang,
            const LinkOptions& opt,
            bool is_parus_lld_mode
        ) {
            std::vector<std::string> argv;
            argv.reserve(objects.size() + 16);
            argv.push_back(linker);

            if (use_lld_via_clang) {
                argv.push_back("-fuse-ld=lld");
            }

            if (is_parus_lld_mode) {
                const std::string sysroot = !opt.sysroot_path.empty()
                    ? opt.sysroot_path
                    : getenv_string_("PARUS_SYSROOT");
                const std::string sdk_root = !opt.apple_sdk_root.empty()
                    ? opt.apple_sdk_root
                    : getenv_string_("PARUS_APPLE_SDK_ROOT");
                uint64_t toolchain_hash = opt.expected_toolchain_hash;
                uint64_t target_hash = opt.expected_target_hash;

                if (toolchain_hash == 0) {
                    const auto parsed = parse_u64_(getenv_string_("PARUS_EXPECTED_TOOLCHAIN_HASH"));
                    if (parsed.has_value()) toolchain_hash = *parsed;
                }
                if (target_hash == 0) {
                    const auto parsed = parse_u64_(getenv_string_("PARUS_EXPECTED_TARGET_HASH"));
                    if (parsed.has_value()) target_hash = *parsed;
                }

                if (!opt.target_triple.empty()) {
                    argv.push_back("--target");
                    argv.push_back(opt.target_triple);
                }
                if (!sysroot.empty()) {
                    argv.push_back("--sysroot");
                    argv.push_back(sysroot);
                }
                if (!sdk_root.empty()) {
                    argv.push_back("--apple-sdk-root");
                    argv.push_back(sdk_root);
                }
                if (toolchain_hash != 0) {
                    argv.push_back("--toolchain-hash");
                    argv.push_back(std::to_string(toolchain_hash));
                }
                if (target_hash != 0) {
                    argv.push_back("--target-hash");
                    argv.push_back(std::to_string(target_hash));
                }
            }

            for (const auto& obj : objects) {
                argv.push_back(obj);
            }

            argv.push_back("-o");
            argv.push_back(output);
            return argv;
        }

        /// @brief 링커 시도 1회를 실행하고 결과 메시지를 만든다.
        std::pair<bool, CompileMessage> try_link_once_(
            const std::string& linker,
            const std::vector<std::string>& objects,
            const std::string& output,
            bool use_lld_via_clang,
            const LinkOptions& opt
        ) {
            if (linker.empty()) {
                return {
                    false,
                    CompileMessage{
                        true,
                        "linker candidate is empty."
                    }
                };
            }

            const bool is_parus_lld_mode =
                !use_lld_via_clang &&
                basename_(linker).find("parus-lld") != std::string::npos;
            const std::vector<std::string> argv = build_link_argv_(
                linker,
                objects,
                output,
                use_lld_via_clang,
                opt,
                is_parus_lld_mode
            );
            const int rc = run_argv_(argv);
            if (rc == 0) {
                return {
                    true,
                    CompileMessage{
                        false,
                        "linked executable with " + linker
                    }
                };
            }

            return {
                false,
                CompileMessage{
                    true,
                    "linker failed (" + linker + ", exit=" + std::to_string(rc) + ")"
                }
            };
        }

    } // namespace

    /// @brief 링크 정책(파루스 내장 우선 + 선택적 폴백)으로 실행 파일을 생성한다.
    LinkResult link_executable(const LinkOptions& opt) {
        namespace fs = std::filesystem;
        LinkResult out{};

        if (opt.object_paths.empty()) {
            out.messages.push_back(CompileMessage{true, "no object files were provided for linking."});
            return out;
        }
        for (const auto& obj : opt.object_paths) {
            if (!fs::exists(obj)) {
                out.messages.push_back(CompileMessage{true, "object file does not exist: " + obj});
                return out;
            }
        }
        if (opt.output_path.empty()) {
            out.messages.push_back(CompileMessage{true, "output path is empty for link step."});
            return out;
        }

        const std::string env_parus_lld = getenv_string_("PARUS_LLD");

        const std::string parus_lld = resolve_tool_candidate_(
            env_parus_lld.empty() ? std::string("parus-lld") : env_parus_lld
        );
        const std::string clangxx = resolve_tool_candidate_(select_clangxx_());

        struct Candidate {
            std::string tool{};
            bool use_lld_via_clang = false;
        };
        std::vector<Candidate> candidates{};

        auto append_auto_policy = [&]() {
            candidates.push_back(Candidate{parus_lld, false});
            if (opt.allow_fallback) {
                candidates.push_back(Candidate{clangxx, true});
                candidates.push_back(Candidate{clangxx, false});
            }
        };

        switch (opt.mode) {
            case LinkerMode::kAuto:
                append_auto_policy();
                break;
            case LinkerMode::kParusLld:
                candidates.push_back(Candidate{parus_lld, false});
                if (opt.allow_fallback) {
                    candidates.push_back(Candidate{clangxx, true});
                    candidates.push_back(Candidate{clangxx, false});
                }
                break;
            case LinkerMode::kSystemLld:
                candidates.push_back(Candidate{clangxx, true});
                if (opt.allow_fallback) {
                    candidates.push_back(Candidate{clangxx, false});
                }
                break;
            case LinkerMode::kSystemClang:
                candidates.push_back(Candidate{clangxx, false});
                break;
        }

        for (const auto& cand : candidates) {
            auto [ok, msg] = try_link_once_(
                cand.tool,
                opt.object_paths,
                opt.output_path,
                cand.use_lld_via_clang,
                opt
            );
            out.messages.push_back(std::move(msg));
            if (!ok) continue;

            out.ok = true;
            out.linker_used = cand.use_lld_via_clang
                ? (cand.tool + " (-fuse-ld=lld)")
                : cand.tool;
            return out;
        }

        out.ok = false;
        out.messages.push_back(CompileMessage{
            true,
            "all linker candidates failed. Consider setting PARUS_LLD or using -fuse-linker to select an explicit linker mode."
        });
        return out;
    }

} // namespace parus::backend::link
