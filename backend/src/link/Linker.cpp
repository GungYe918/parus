// backend/src/link/Linker.cpp
#include <parus/backend/link/Linker.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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

        /// @brief 명령을 실행하고 종료 코드를 반환한다.
        int run_command_(const std::string& cmd) {
            return std::system(cmd.c_str());
        }

        /// @brief object -> executable 링크 명령을 조립한다.
        std::string build_link_command_(
            const std::string& linker,
            const std::vector<std::string>& objects,
            const std::string& output,
            bool use_lld_via_clang
        ) {
            std::string cmd;
            cmd.reserve(256);

            cmd += shell_quote_(linker);
            if (use_lld_via_clang) {
                cmd += " -fuse-ld=lld";
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
            bool use_lld_via_clang
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

            const std::string cmd = build_link_command_(linker, objects, output, use_lld_via_clang);
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

        const std::string env_parus_lld = []() -> std::string {
            const char* p = std::getenv("PARUS_LLD");
            if (p == nullptr) return {};
            return std::string(p);
        }();

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
                cand.use_lld_via_clang
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
