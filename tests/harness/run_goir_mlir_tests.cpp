#include <parus/lex/Lexer.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/tyck/TypeCheck.hpp>
#include <parus/type/TypeResolve.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/goir/Builder.hpp>
#include <parus/goir/Placement.hpp>
#include <parus/backend/mlir/GOIRMLIRLowering.hpp>

#include <iostream>
#include <string>

namespace {

    struct ParsedProgram {
        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::diag::Bag bag;
        parus::ast::StmtId root = parus::ast::k_invalid_stmt;
        parus::type::TypeResolveResult type_resolve{};
        bool macro_type_ready = false;
        bool macro_type_ok = false;
    };

    static ParsedProgram parse_program_(const std::string& src) {
        ParsedProgram p{};
        parus::Lexer lx(src, /*file_id=*/1, &p.bag);
        const auto tokens = lx.lex_all();

        parus::Parser parser(tokens, p.ast, p.types, &p.bag);
        p.root = parser.parse_program();
        return p;
    }

    static bool run_macro_and_type_(ParsedProgram& p) {
        if (p.macro_type_ready) return p.macro_type_ok;
        p.macro_type_ready = true;

        const bool macro_ok = parus::macro::expand_program(p.ast, p.types, p.root, p.bag);
        if (p.bag.has_error() || !macro_ok) {
            p.macro_type_ok = false;
            return false;
        }
        p.type_resolve = parus::type::resolve_program_types(p.ast, p.types, p.root, p.bag);
        p.macro_type_ok = (!p.bag.has_error() && p.type_resolve.ok);
        return p.macro_type_ok;
    }

    static bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    struct SirPipeline {
        ParsedProgram prog;
        parus::passes::PassResults pres;
        parus::tyck::TyckResult ty;
        parus::sir::Module sir_mod;
        parus::sir::CapabilityAnalysisResult sir_cap;
    };

    static SirPipeline build_sir_pipeline_(const std::string& src) {
        SirPipeline out{};
        out.prog = parse_program_(src);
        if (!run_macro_and_type_(out.prog)) return out;

        parus::passes::PassOptions popt{};
        out.pres = parus::passes::run_on_program(out.prog.ast, out.prog.root, out.prog.bag, popt);

        parus::tyck::TypeChecker tc(
            out.prog.ast,
            out.prog.types,
            out.prog.bag,
            &out.prog.type_resolve,
            &out.pres.generic_prep
        );
        out.ty = tc.check_program(out.prog.root);

        parus::sir::BuildOptions bopt{};
        out.sir_mod = parus::sir::build_sir_module(
            out.prog.ast,
            out.prog.root,
            out.pres.sym,
            out.pres.name_resolve,
            out.ty,
            out.prog.types,
            bopt
        );

        (void)parus::sir::canonicalize_for_capability(out.sir_mod, out.prog.types);
        out.sir_cap = parus::sir::analyze_capabilities(out.sir_mod, out.prog.types, out.prog.bag);
        return out;
    }

    static bool build_placed_goir_(parus::goir::Module& out_mod, parus::ty::TypePool& out_types) {
        const std::string src = R"(
            @pure def add(a: i32, b: i32) -> i32 {
                return a + b;
            }

            @pure def main(flag: bool, x: i32, y: i32) -> i32 {
                if (flag) {
                    return add(x, y);
                }
                let z: i32 = x - y;
                return z;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "MLIR seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "MLIR seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "MLIR seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must succeed before MLIR lowering");
        if (!ok) return false;

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "gOIR placement must succeed before MLIR lowering");
        if (!ok) return false;

        out_mod = std::move(placed.mod);
        out_types = std::move(p.prog.types);
        return true;
    }

    static bool test_mlir_smoke_ok_() {
        std::string error{};
        const bool ok = parus::backend::mlir::run_mlir_smoke(&error);
        if (!ok) std::cerr << "  - " << error << "\n";
        return ok;
    }

    static bool test_goir_to_mlir_and_llvm_ir_ok_() {
        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_(placed, types)) return false;

        auto mlir = parus::backend::mlir::lower_goir_to_mlir_text(placed, types);
        bool ok = true;
        ok &= require_(mlir.ok, "gOIR placed module must lower to MLIR text");
        ok &= require_(mlir.messages.empty(), "MLIR lowering must not emit diagnostics");
        ok &= require_(mlir.mlir_text.find("func.func @main") != std::string::npos,
                       "MLIR text must contain main function");
        ok &= require_(mlir.mlir_text.find("cf.cond_br") != std::string::npos,
                       "MLIR text must contain control-flow branch lowering");
        ok &= require_(mlir.mlir_text.find("func.call @add") != std::string::npos,
                       "MLIR text must contain direct call lowering");
        if (!ok) return false;

        auto llvm_ir = parus::backend::mlir::lower_goir_to_llvm_ir_text(
            placed,
            types,
            parus::backend::mlir::GOIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(llvm_ir.ok, "gOIR placed module must lower to LLVM IR text");
        ok &= require_(llvm_ir.messages.empty(), "LLVM IR lowering must not emit diagnostics");
        ok &= require_(llvm_ir.llvm_ir.find("define i32 @add") != std::string::npos,
                       "LLVM IR must contain add definition");
        ok &= require_(llvm_ir.llvm_ir.find("define i32 @main") != std::string::npos,
                       "LLVM IR must contain main definition");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"mlir_smoke", test_mlir_smoke_ok_},
        {"goir_to_mlir_and_llvm_ir", test_goir_to_mlir_and_llvm_ir_ok_},
    };

    int failed = 0;
    for (const auto& c : cases) {
        std::cout << "[TEST] " << c.name << "\n";
        if (!c.fn()) {
            ++failed;
            std::cout << "  -> FAIL\n";
        } else {
            std::cout << "  -> PASS\n";
        }
    }

    if (failed != 0) {
        std::cout << "\nFAILED " << failed << " test(s)\n";
        return 1;
    }
    std::cout << "\nALL TESTS PASSED\n";
    return 0;
}
