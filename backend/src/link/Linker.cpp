// backend/src/link/Linker.cpp
#include <parus/backend/link/Linker.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace parus::backend::link {

    namespace {

        /// @brief 쉘 인자 1개를 안전한 single-quote 형식으로 이스케이프한다.
        std::string shell_quote_(const std::string& s) {
            std::string out;
            out.reserve(s.size() + 8);
            out.push_back('\'');
            for (char c : s) {
                if (c == '\'') out += "'\\''";
                else out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }

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

        /// @brief 명령을 실행하고 종료 코드를 반환한다.
        int decode_wait_status_(int raw_status) {
            if (raw_status == -1) return -1;
#if defined(_WIN32)
            return raw_status;
#else
            if (WIFEXITED(raw_status)) return WEXITSTATUS(raw_status);
            if (WIFSIGNALED(raw_status)) return 128 + WTERMSIG(raw_status);
            return raw_status;
#endif
        }

        /// @brief 명령을 실행하고 표준화된 종료 코드를 반환한다.
        int run_command_(const std::string& cmd) {
            return decode_wait_status_(std::system(cmd.c_str()));
        }

        /// @brief object -> executable 링크 명령을 조립한다.
        std::string build_link_command_(
            const std::string& linker,
            const std::vector<std::string>& objects,
            const std::string& output,
            bool use_lld_via_clang,
            const LinkOptions& opt,
            bool is_parus_lld_mode
        ) {
            std::string cmd;
            cmd.reserve(512);

            cmd += shell_quote_(linker);
            if (use_lld_via_clang) {
                cmd += " -fuse-ld=lld";
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
                    cmd += " --target ";
                    cmd += shell_quote_(opt.target_triple);
                }
                if (!sysroot.empty()) {
                    cmd += " --sysroot ";
                    cmd += shell_quote_(sysroot);
                }
                if (!sdk_root.empty()) {
                    cmd += " --apple-sdk-root ";
                    cmd += shell_quote_(sdk_root);
                }
                if (toolchain_hash != 0) {
                    cmd += " --toolchain-hash ";
                    cmd += shell_quote_(std::to_string(toolchain_hash));
                }
                if (target_hash != 0) {
                    cmd += " --target-hash ";
                    cmd += shell_quote_(std::to_string(target_hash));
                }
            }

            for (const auto& obj : objects) {
                cmd += " ";
                cmd += shell_quote_(obj);
            }

            cmd += " -o ";
            cmd += shell_quote_(output);
            return cmd;
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
            const std::string cmd = build_link_command_(
                linker,
                objects,
                output,
                use_lld_via_clang,
                opt,
                is_parus_lld_mode
            );
            const int rc = run_command_(cmd);
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
