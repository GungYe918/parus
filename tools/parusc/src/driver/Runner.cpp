// tools/parusc/src/driver/Runner.cpp
#include "Runner.hpp"

#include "../dump/Dump.hpp"

#include <parus/Version.hpp>
#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/diag/DiagCode.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/os/File.hpp>
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

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace parusc::driver {

    namespace {

        /// @brief 진단을 컨텍스트 포함 형태로 출력하고 종료 코드를 반환한다.
        int flush_diags(
            const parus::diag::Bag& bag,
            parus::diag::Language lang,
            const parus::SourceManager& sm,
            uint32_t context_lines
        ) {
            std::cout << "\nDIAGNOSTICS:\n";
            if (bag.diags().empty()) {
                std::cout << "no error.\n";
                return 0;
            }

            for (const auto& d : bag.diags()) {
                std::cerr << parus::diag::render_one_context(d, lang, sm, context_lines) << "\n";
            }
            return bag.has_error() ? 1 : 0;
        }

        /// @brief SourceManager를 사용해 토큰 스트림을 생성한다.
        std::vector<parus::Token> lex_with_sm(
            parus::SourceManager& sm,
            uint32_t file_id,
            parus::diag::Bag* bag
        ) {
            parus::Lexer lex(sm.content(file_id), file_id, bag);
            return lex.lex_all();
        }

        /// @brief 단일 expr 모드를 실행한다.
        int run_expr(std::string_view src_arg, const cli::Options& opt) {
            parus::SourceManager sm;
            std::string src_owned(src_arg);
            const uint32_t file_id = sm.add("<expr>", src_owned);

            parus::diag::Bag bag;
            auto tokens = lex_with_sm(sm, file_id, &bag);
            dump::dump_tokens(tokens);

            parus::ast::AstArena ast;
            parus::ty::TypePool types;
            parus::Parser p(tokens, ast, types, &bag, opt.max_errors);

            auto root = p.parse_expr();
            parus::passes::run_on_expr(ast, root, bag);

            std::cout << "\nAST:\n";
            dump::dump_expr(ast, root, 0);

            std::cout << "\nTYPES:\n";
            types.dump(std::cout);

            return flush_diags(bag, opt.lang, sm, opt.context_lines);
        }

        /// @brief 단일 stmt 모드를 실행한다.
        int run_stmt(std::string_view src_arg, const cli::Options& opt) {
            parus::SourceManager sm;
            std::string src_owned(src_arg);
            const uint32_t file_id = sm.add("<stmt>", src_owned);

            parus::diag::Bag bag;
            auto tokens = lex_with_sm(sm, file_id, &bag);
            dump::dump_tokens(tokens);

            parus::ast::AstArena ast;
            parus::ty::TypePool types;
            parus::Parser p(tokens, ast, types, &bag, opt.max_errors);

            auto root = p.parse_stmt();
            (void)parus::passes::run_on_stmt_tree(ast, root, bag, opt.pass_opt);

            std::cout << "\nAST(STMT):\n";
            dump::dump_stmt(ast, types, root, 0);

            std::cout << "\nTYPES:\n";
            types.dump(std::cout);

            return flush_diags(bag, opt.lang, sm, opt.context_lines);
        }

        /// @brief 프로그램 모드(AST->TYCK->SIR->(OIR))를 실행한다.
        int run_all(
            std::string_view src_arg,
            std::string_view name,
            const cli::Options& opt
        ) {
            parus::SourceManager sm;
            std::string src_owned(src_arg);
            const uint32_t file_id = sm.add(std::string(name), std::move(src_owned));

            parus::diag::Bag bag;
            auto tokens = lex_with_sm(sm, file_id, &bag);
            dump::dump_tokens(tokens);

            parus::ast::AstArena ast;
            parus::ty::TypePool types;
            parus::Parser p(tokens, ast, types, &bag, opt.max_errors);

            auto root = p.parse_program();

            auto pres = parus::passes::run_on_program(ast, root, bag, opt.pass_opt);

            std::cout << "\nAST(PROGRAM):\n";
            dump::dump_stmt(ast, types, root, 0);

            std::cout << "\nTYPES:\n";
            types.dump(std::cout);

            parus::tyck::TyckResult tyck_res;
            {
                parus::tyck::TypeChecker tc(ast, types, bag);
                tyck_res = tc.check_program(root);

                std::cout << "\nTYCK:\n";
                if (tyck_res.errors.empty()) std::cout << "tyck ok.\n";
                else std::cout << "tyck errors: " << tyck_res.errors.size() << "\n";
            }

            std::cout << "\nCAP:\n";
            const auto cap_res = parus::cap::run_capability_check(
                ast, root, pres.name_resolve, tyck_res, types, bag
            );
            if (cap_res.ok) {
                std::cout << "capability ok.\n";
            } else {
                std::cout << "capability errors: " << cap_res.error_count << "\n";
            }

            parus::sir::Module sir_mod;
            bool sir_verify_ok = true;
            bool sir_cap_ok = true;
            bool sir_handle_verify_ok = true;
            bool oir_gate_ok = true;
            {
                parus::sir::BuildOptions bopt{};
                sir_mod = parus::sir::build_sir_module(
                    ast,
                    root,
                    pres.sym,
                    pres.name_resolve,
                    tyck_res,
                    types,
                    bopt
                );

                const auto canon = parus::sir::canonicalize_for_capability(sir_mod, types);
                std::cout << "\nSIR CANON:\n";
                std::cout << "rewritten values: " << canon.rewritten_values
                          << ", rewritten calls: " << canon.rewritten_calls << "\n";

                const auto sir_verrs = parus::sir::verify_module(sir_mod);
                std::cout << "\nSIR VERIFY:\n";
                if (sir_verrs.empty()) {
                    std::cout << "verify ok.\n";
                } else {
                    sir_verify_ok = false;
                    std::cout << "verify errors: " << sir_verrs.size() << "\n";
                    for (const auto& e : sir_verrs) {
                        std::cout << "  - " << e.msg << "\n";
                    }
                }

                auto mut = parus::sir::analyze_mut(sir_mod, types, bag);
                std::cout << "\nMUT:\n";
                std::cout << "tracked symbols: " << mut.by_symbol.size() << "\n";

                const auto sir_cap = parus::sir::analyze_capabilities(sir_mod, types, bag);
                std::cout << "\nSIR CAP:\n";
                if (sir_cap.ok) {
                    std::cout << "capability ok.\n";
                } else {
                    sir_cap_ok = false;
                    std::cout << "capability errors: " << sir_cap.error_count << "\n";
                }
                std::cout << "escape handles: " << sir_cap.escape_handle_count
                          << ", materialized handles: " << sir_cap.materialized_handle_count
                          << "\n";

                // Capability 분석으로 채워진 EscapeHandle 메타를 반영한 상태를 덤프한다.
                dump::dump_sir_module(sir_mod, types);

                const auto handle_verrs = parus::sir::verify_escape_handles(sir_mod);
                std::cout << "\nSIR HANDLE VERIFY:\n";
                if (handle_verrs.empty()) {
                    std::cout << "verify ok.\n";
                } else {
                    sir_handle_verify_ok = false;
                    std::cout << "verify errors: " << handle_verrs.size() << "\n";
                    for (const auto& e : handle_verrs) {
                        std::cout << "  - " << e.msg << "\n";
                    }
                }
            }

            if (opt.dump_oir && sir_verify_ok && sir_cap_ok && sir_handle_verify_ok) {
                parus::oir::Builder ob(sir_mod, types);
                auto oir_res = ob.build();

                if (!oir_res.gate_passed) {
                    oir_gate_ok = false;
                    std::cout << "\nOIR GATE:\n";
                    std::cout << "gate failed: " << oir_res.gate_errors.size() << "\n";
                    for (const auto& e : oir_res.gate_errors) {
                        std::cout << "  - " << e.msg << "\n";
                    }
                } else {
                    parus::oir::run_passes(oir_res.mod);
                    dump::dump_oir_module(oir_res.mod, types);

                    auto verrs = parus::oir::verify(oir_res.mod);
                    std::cout << "\nOIR VERIFY:\n";
                    if (verrs.empty()) {
                        std::cout << "verify ok.\n";
                    } else {
                        std::cout << "verify errors: " << verrs.size() << "\n";
                        for (auto& e : verrs) std::cout << "  - " << e.msg << "\n";
                    }
                }
            } else if (opt.dump_oir) {
                std::cout << "\nOIR: skipped because SIR verification failed before OIR lowering.\n";
            }

            int diag_rc = flush_diags(bag, opt.lang, sm, opt.context_lines);
            if (!sir_verify_ok || !sir_cap_ok || !sir_handle_verify_ok || !oir_gate_ok) return 1;
            return diag_rc;
        }

        /// @brief 파일을 읽어 프로그램 모드로 실행한다.
        int run_file(const std::string& path, const cli::Options& opt) {
            std::string content;
            std::string err;

            if (!parus::open_file(path, content, err)) {
                std::cerr << "error: " << err << "\n";
                return 1;
            }

            std::string norm = parus::normalize_path(path);
            return run_all(content, norm, opt);
        }

    } // namespace

    int run(const cli::Options& opt) {
        switch (opt.mode) {
            case cli::Mode::kExpr:
                return run_expr(opt.payload, opt);
            case cli::Mode::kStmt:
                return run_stmt(opt.payload, opt);
            case cli::Mode::kAll:
                return run_all(opt.payload, "<all>", opt);
            case cli::Mode::kFile:
                return run_file(opt.payload, opt);
            case cli::Mode::kUsage:
            case cli::Mode::kVersion:
            default:
                return 0;
        }
    }

} // namespace parusc::driver
