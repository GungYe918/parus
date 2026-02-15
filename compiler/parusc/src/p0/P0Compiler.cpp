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
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/sir/MutAnalysis.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/text/SourceManager.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/tyck/TypeCheck.hpp>

#if PARUSC_HAS_AOT_BACKEND
#include <parus/backend/aot/AOTBackend.hpp>
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace parusc::p0 {

    namespace {

        /// @brief 진단을 컨텍스트 포함 형태로 출력한다.
        int flush_diags_(
            const parus::diag::Bag& bag,
            parus::diag::Language lang,
            const parus::SourceManager& sm,
            uint32_t context_lines
        ) {
            if (bag.diags().empty()) return 0;
            for (const auto& d : bag.diags()) {
                std::cerr << parus::diag::render_one_context(d, lang, sm, context_lines) << "\n";
            }
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

        /// @brief 쉘 인자를 단일 인용부호 형태로 이스케이프한다.
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

        /// @brief 시스템 링커(clang++ driver)를 통해 단일 object를 실행 파일로 링크한다.
        bool link_single_object_to_exe_(
            const std::string& object_path,
            const std::string& exe_path,
            std::string& out_err
        ) {
            namespace fs = std::filesystem;
            if (!fs::exists(object_path)) {
                out_err = "object file not found for linking: " + object_path;
                return false;
            }

            std::string linker = "/usr/bin/clang++";
            if (!fs::exists(linker)) linker = "clang++";

            const std::string cmd =
                shell_quote_(linker) + " " +
                shell_quote_(object_path) + " -o " +
                shell_quote_(exe_path);

            const int rc = std::system(cmd.c_str());
            if (rc != 0) {
                out_err = "linker invocation failed (exit=" + std::to_string(rc) + ")";
                return false;
            }
            return true;
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
        parus::Parser parser(tokens, ast, types, &bag, opt.max_errors);
        auto root = parser.parse_program();

        auto pres = parus::passes::run_on_program(ast, root, bag, opt.pass_opt);

        parus::tyck::TyckResult tyck_res;
        {
            parus::tyck::TypeChecker tc(ast, types, bag);
            tyck_res = tc.check_program(root);
        }

        const auto ast_cap_res = parus::cap::run_capability_check(
            ast, root, pres.name_resolve, tyck_res, types, bag
        );

        if (opt.has_xparus && opt.internal.ast_dump) {
            parusc::dump::dump_stmt(ast, types, root, 0);
            std::cout << "\nTYPES:\n";
            types.dump(std::cout);
        }

        // 프론트엔드 진단이 있으면 백엔드로 내려가지 않는다.
        int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines);
        if (diag_rc != 0 || !tyck_res.errors.empty() || !ast_cap_res.ok) return 1;

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
        parus::backend::CompileOptions backend_opt{};
        backend_opt.opt_level = opt.opt_level;
        backend_opt.aot_engine = parus::backend::AOTEngine::kLlvm;

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
            std::string link_err;
            if (!link_single_object_to_exe_(object_for_link, final_exe_output, link_err)) {
                std::cerr << "error: " << link_err << "\n";
                return 1;
            }
            std::error_code ec;
            std::filesystem::remove(object_for_link, ec);
            std::cout << "linked executable to " << final_exe_output << "\n";
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
