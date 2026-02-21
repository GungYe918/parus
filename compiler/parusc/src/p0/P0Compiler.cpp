// compiler/parusc/src/p0/P0Compiler.cpp
#include <parusc/p0/P0Compiler.hpp>

#include <parusc/dump/Dump.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/oir/Builder.hpp>
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/sir/MutAnalysis.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/text/SourceManager.hpp>
#include <parus/type/TypeResolve.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/tyck/TypeCheck.hpp>

#if PARUSC_HAS_AOT_BACKEND
#include <parus/backend/aot/AOTBackend.hpp>
#include <parus/backend/link/Linker.hpp>
#endif

#include <filesystem>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace parusc::p0 {

    namespace {

        /// @brief 진단을 컨텍스트 포함 형태로 출력한다.
        int flush_diags_(
            const parus::diag::Bag& bag,
            parus::diag::Language lang,
            const parus::SourceManager& sm,
            uint32_t context_lines,
            cli::DiagFormat format
        ) {
            if (bag.diags().empty()) return 0;

            if (format == cli::DiagFormat::kText) {
                for (const auto& d : bag.diags()) {
                    std::cerr << parus::diag::render_one_context(d, lang, sm, context_lines) << "\n";
                }
                return bag.has_error() ? 1 : 0;
            }

            auto json_escape_ = [](std::string_view s) -> std::string {
                std::string out;
                out.reserve(s.size() + 8);
                for (const char ch : s) {
                    switch (ch) {
                        case '\"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b"; break;
                        case '\f': out += "\\f"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default:
                            if (static_cast<unsigned char>(ch) < 0x20) {
                                std::ostringstream oss;
                                oss << "\\u" << std::hex << std::uppercase
                                    << std::setw(4) << std::setfill('0')
                                    << static_cast<int>(static_cast<unsigned char>(ch));
                                out += oss.str();
                            } else {
                                out.push_back(ch);
                            }
                            break;
                    }
                }
                return out;
            };

            auto severity_name_ = [](parus::diag::Severity sev) -> const char* {
                switch (sev) {
                    case parus::diag::Severity::kWarning: return "warning";
                    case parus::diag::Severity::kFatal: return "fatal";
                    case parus::diag::Severity::kError:
                    default:
                        return "error";
                }
            };

            std::cerr << "[\n";
            bool first = true;
            for (const auto& d : bag.diags()) {
                const auto sp = d.span();
                const uint32_t end_off = (sp.hi >= sp.lo) ? sp.hi : sp.lo;
                const auto begin_lc = sm.line_col(sp.file_id, sp.lo);
                const auto end_lc = sm.line_col(sp.file_id, end_off);

                if (!first) std::cerr << ",\n";
                first = false;

                std::cerr << "  {";
                std::cerr << "\"severity\":\"" << severity_name_(d.severity()) << "\",";
                std::cerr << "\"code\":\"" << json_escape_(parus::diag::code_name(d.code())) << "\",";
                std::cerr << "\"message\":\"" << json_escape_(parus::diag::render_message(d, lang)) << "\",";
                std::cerr << "\"file\":\"" << json_escape_(sm.name(sp.file_id)) << "\",";
                std::cerr << "\"line\":" << begin_lc.line << ",";
                std::cerr << "\"col\":" << begin_lc.col << ",";
                std::cerr << "\"end_line\":" << end_lc.line << ",";
                std::cerr << "\"end_col\":" << end_lc.col << ",";
                std::cerr << "\"args\":[";
                for (size_t i = 0; i < d.args().size(); ++i) {
                    if (i != 0) std::cerr << ",";
                    std::cerr << "\"" << json_escape_(d.args()[i]) << "\"";
                }
                std::cerr << "],";

                const uint32_t start_line0 = (begin_lc.line > 0) ? (begin_lc.line - 1) : 0;
                const uint32_t start_col0 = (begin_lc.col > 0) ? (begin_lc.col - 1) : 0;
                const uint32_t end_line0 = (end_lc.line > 0) ? (end_lc.line - 1) : 0;
                const uint32_t end_col0 = (end_lc.col > 0) ? (end_lc.col - 1) : 0;
                std::cerr << "\"range\":{"
                             "\"start\":{\"line\":" << start_line0 << ",\"character\":" << start_col0 << "},"
                             "\"end\":{\"line\":" << end_line0 << ",\"character\":" << end_col0 << "}"
                          << "}";

                std::cerr << "}";
            }
            std::cerr << "\n]\n";
            return bag.has_error() ? 1 : 0;
        }

        /// @brief SourceManager를 사용해 토큰 스트림을 생성한다.
        std::vector<parus::Token> lex_with_sm_(
            parus::SourceManager& sm,
            uint32_t file_id,
            parus::diag::Bag* bag
        ) {
            parus::Lexer lex(sm.content(file_id), file_id, bag);
            return lex.lex_all();
        }

#if PARUSC_HAS_AOT_BACKEND
        /// @brief 드라이버 실행 경로를 기준으로 PARUS_LLD 환경 변수를 자동 설정한다.
        std::string getenv_string_(const char* key) {
            if (key == nullptr) return {};
            const char* p = std::getenv(key);
            if (p == nullptr) return {};
            return std::string(p);
        }

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

        void seed_parus_toolchain_env_from_driver_(const Invocation& inv) {
            namespace fs = std::filesystem;
            if (inv.driver_executable_path.empty()) {
                return;
            }
            std::error_code ec{};

            fs::path driver_path(inv.driver_executable_path);
            fs::path resolved_driver = fs::weakly_canonical(driver_path, ec);
            if (ec || resolved_driver.empty()) {
                ec.clear();
                resolved_driver = driver_path;
            }
            if (resolved_driver.empty()) return;

            const fs::path driver_bin_dir = resolved_driver.parent_path();
            fs::path toolchain_root = driver_bin_dir;
            if (driver_bin_dir.filename() == "bin") {
                toolchain_root = driver_bin_dir.parent_path();
            }

            if (std::getenv("PARUS_TOOLCHAIN_ROOT") == nullptr) {
#if defined(_WIN32)
                _putenv_s("PARUS_TOOLCHAIN_ROOT", toolchain_root.string().c_str());
#else
                setenv("PARUS_TOOLCHAIN_ROOT", toolchain_root.string().c_str(), 0);
#endif
            }

            if (std::getenv("PARUS_LLD") == nullptr) {
                const fs::path candidate_lld = toolchain_root / "bin" / "parus-lld";
                if (fs::exists(candidate_lld, ec) && !ec) {
#if defined(_WIN32)
                    _putenv_s("PARUS_LLD", candidate_lld.string().c_str());
#else
                    setenv("PARUS_LLD", candidate_lld.string().c_str(), 0);
#endif
                }
            }

            if (std::getenv("PARUS_SYSROOT") == nullptr) {
                const fs::path candidate_sysroot = toolchain_root / "sysroot";
                if (fs::exists(candidate_sysroot, ec) && !ec) {
#if defined(_WIN32)
                    _putenv_s("PARUS_SYSROOT", candidate_sysroot.string().c_str());
#else
                    setenv("PARUS_SYSROOT", candidate_sysroot.string().c_str(), 0);
#endif
                }
            }
        }

        std::optional<uint64_t> expected_hash_from_env_(const char* key) {
            const auto s = getenv_string_(key);
            return parse_u64_(s);
        }

        std::string select_sysroot_(const cli::Options& opt) {
            if (!opt.sysroot_path.empty()) return opt.sysroot_path;
            return getenv_string_("PARUS_SYSROOT");
        }

        std::string select_apple_sdk_root_(const cli::Options& opt) {
            if (!opt.apple_sdk_root.empty()) return opt.apple_sdk_root;
            return getenv_string_("PARUS_APPLE_SDK_ROOT");
        }

        /// @brief 백엔드 메시지를 표준 에러로 출력하고 실패 여부를 반환한다.
        bool print_backend_messages_(const parus::backend::CompileResult& r) {
            bool has_error = false;
            for (const auto& m : r.messages) {
                if (m.is_error) {
                    has_error = true;
                    std::cerr << "error: " << m.text << "\n";
                } else {
                    std::cout << m.text << "\n";
                }
            }
            return has_error;
        }

        /// @brief CLI 링커 모드를 backend 링크 API 모드로 매핑한다.
        parus::backend::link::LinkerMode to_backend_linker_mode_(cli::LinkerMode mode) {
            using In = cli::LinkerMode;
            using Out = parus::backend::link::LinkerMode;
            switch (mode) {
                case In::kParusLld: return Out::kParusLld;
                case In::kSystemLld: return Out::kSystemLld;
                case In::kSystemClang: return Out::kSystemClang;
                case In::kAuto:
                default:
                    return Out::kAuto;
            }
        }

        /// @brief backend 링크 메시지를 표준 입출력으로 출력하고 오류 여부를 반환한다.
        bool print_link_messages_(const parus::backend::link::LinkResult& r) {
            bool has_error = false;
            for (const auto& m : r.messages) {
                if (m.is_error) {
                    if (r.ok) {
                        std::cerr << "note: " << m.text << "\n";
                    } else {
                        has_error = true;
                        std::cerr << "error: " << m.text << "\n";
                    }
                } else {
                    std::cout << m.text << "\n";
                }
            }
            return has_error;
        }
#endif

    } // namespace

    int run(const Invocation& inv) {
        if (inv.options == nullptr) {
            std::cerr << "error: internal compiler invocation has null options.\n";
            return 1;
        }
        const auto& opt = *inv.options;

        parus::SourceManager sm;
        const uint32_t file_id = sm.add(inv.normalized_input_path, inv.source_text);

        parus::diag::Bag bag;
        auto tokens = lex_with_sm_(sm, file_id, &bag);

        if (opt.has_xparus && opt.internal.token_dump) {
            parusc::dump::dump_tokens(tokens);
        }

        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::ParserFeatureFlags parser_flags{};
        parser_flags.macro_with_token = opt.internal.macro_token_experimental;
        parus::Parser parser(tokens, ast, types, &bag, opt.max_errors, parser_flags);
        auto root = parser.parse_program();

        if (opt.has_xparus && opt.internal.ast_dump) {
            parusc::dump::dump_stmt(ast, types, root, 0);
            std::cout << "\nTYPES:\n";
            types.dump(std::cout);
        }

        // 파싱/렉싱 단계에서 오류가 있으면 이후 단계 진단 폭주를 막기 위해
        // name-resolve/tyck/cap으로 진행하지 않는다.
        if (bag.has_error()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        const bool macro_ok = parus::macro::expand_program(ast, types, root, bag, opt.macro_budget);
        if (bag.has_error() || !macro_ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !macro_ok) ? 1 : 0;
        }

        auto type_resolve = parus::type::resolve_program_types(ast, types, root, bag);
        if (bag.has_error() || !type_resolve.ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !type_resolve.ok) ? 1 : 0;
        }

        auto pres = parus::passes::run_on_program(ast, root, bag, opt.pass_opt);
        if (bag.has_error()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        parus::tyck::TyckResult tyck_res;
        {
            parus::tyck::TypeChecker tc(ast, types, bag, &type_resolve);
            tyck_res = tc.check_program(root);
        }
        if (bag.has_error() || !tyck_res.errors.empty()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !tyck_res.errors.empty()) ? 1 : 0;
        }

        const auto ast_cap_res = parus::cap::run_capability_check(
            ast, root, pres.name_resolve, tyck_res, types, bag
        );
        if (bag.has_error() || !ast_cap_res.ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !ast_cap_res.ok) ? 1 : 0;
        }

        if (opt.syntax_only) {
            if (!bag.diags().empty()) {
                (void)flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            }
            return bag.has_error() ? 1 : 0;
        }

        parus::sir::BuildOptions bopt{};
        auto sir_mod = parus::sir::build_sir_module(
            ast, root, pres.sym, pres.name_resolve, tyck_res, types, bopt
        );

        (void)parus::sir::canonicalize_for_capability(sir_mod, types);

        const auto sir_verrs = parus::sir::verify_module(sir_mod);
        if (!sir_verrs.empty()) {
            for (const auto& e : sir_verrs) {
                std::cerr << "error: SIR verify: " << e.msg << "\n";
            }
            return 1;
        }

        (void)parus::sir::analyze_mut(sir_mod, types, bag);
        const auto sir_cap = parus::sir::analyze_capabilities(sir_mod, types, bag);
        if (!sir_cap.ok) {
            std::cerr << "error: SIR capability analysis failed (" << sir_cap.error_count << ")\n";
            return 1;
        }

        const auto sir_handle_verrs = parus::sir::verify_escape_handles(sir_mod);
        if (!sir_handle_verrs.empty()) {
            for (const auto& e : sir_handle_verrs) {
                std::cerr << "error: SIR escape-handle verify: " << e.msg << "\n";
            }
            return 1;
        }

        if (opt.has_xparus && opt.internal.sir_dump) {
            parusc::dump::dump_sir_module(sir_mod, types);
        }

        parus::oir::Builder ob(sir_mod, types);
        auto oir_res = ob.build();
        if (!oir_res.gate_passed) {
            for (const auto& e : oir_res.gate_errors) {
                std::cerr << "error: OIR gate: " << e.msg << "\n";
            }
            return 1;
        }

        parus::oir::run_passes(oir_res.mod);

        const auto oir_verrs = parus::oir::verify(oir_res.mod);
        if (!oir_verrs.empty()) {
            for (const auto& e : oir_verrs) {
                std::cerr << "error: OIR verify: " << e.msg << "\n";
            }
            return 1;
        }

        if (opt.has_xparus && opt.internal.oir_dump) {
            parusc::dump::dump_oir_module(oir_res.mod, types);
        }

#if PARUSC_HAS_AOT_BACKEND
        seed_parus_toolchain_env_from_driver_(inv);

        parus::backend::CompileOptions backend_opt{};
        backend_opt.opt_level = opt.opt_level;
        backend_opt.aot_engine = parus::backend::AOTEngine::kLlvm;
        backend_opt.target_triple = opt.target_triple;

        const bool emit_object = (opt.has_xparus && opt.internal.emit_object);
        const bool emit_llvm_ir = (opt.has_xparus && opt.internal.emit_llvm_ir);
        const bool emit_executable = (!emit_object && !emit_llvm_ir);

        std::string object_for_link = opt.output_path;
        std::string final_exe_output = opt.output_path;
        if (emit_executable) {
            object_for_link = opt.output_path + ".tmp.o";
            backend_opt.output_path = object_for_link;
            backend_opt.emit_object = true;
            backend_opt.emit_llvm_ir = false;
        } else if (emit_object) {
            backend_opt.output_path = opt.output_path;
            backend_opt.emit_object = true;
            backend_opt.emit_llvm_ir = false;
        } else {
            backend_opt.output_path = opt.output_path;
            backend_opt.emit_object = false;
            backend_opt.emit_llvm_ir = true;
        }

        parus::backend::aot::AOTBackend backend;
        auto cr = backend.compile(oir_res.mod, types, backend_opt);
        const bool has_backend_error = print_backend_messages_(cr);
        if (!cr.ok || has_backend_error) return 1;

        if (emit_executable) {
            parus::backend::link::LinkOptions link_opt{};
            link_opt.object_paths = {object_for_link};
            link_opt.output_path = final_exe_output;
            link_opt.target_triple = opt.target_triple;
            link_opt.sysroot_path = select_sysroot_(opt);
            link_opt.apple_sdk_root = select_apple_sdk_root_(opt);
            if (const auto h = expected_hash_from_env_("PARUS_EXPECTED_TOOLCHAIN_HASH"); h.has_value()) {
                link_opt.expected_toolchain_hash = *h;
            }
            if (const auto h = expected_hash_from_env_("PARUS_EXPECTED_TARGET_HASH"); h.has_value()) {
                link_opt.expected_target_hash = *h;
            }
            link_opt.mode = to_backend_linker_mode_(opt.linker_mode);
            link_opt.allow_fallback = opt.allow_link_fallback;

            const auto link_res = parus::backend::link::link_executable(link_opt);
            const bool has_link_error = print_link_messages_(link_res);
            if (!link_res.ok || has_link_error) {
                return 1;
            }
            std::error_code ec;
            std::filesystem::remove(object_for_link, ec);
            std::cout << "linked executable to " << final_exe_output;
            if (!link_res.linker_used.empty()) {
                std::cout << " (via " << link_res.linker_used << ")";
            }
            std::cout << "\n";
        }
        return 0;
#else
        (void)oir_res;
        (void)types;
        std::cerr << "error: parusc was built without AOT backend support.\n";
        return 1;
#endif
    }

} // namespace parusc::p0
