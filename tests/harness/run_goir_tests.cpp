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
#include <parus/goir/Print.hpp>
#include <parus/goir/Verify.hpp>

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

    static parus::goir::Module make_open_borrow_module_(parus::ty::TypePool& types) {
        parus::goir::Module mod{};
        mod.header.stage_kind = parus::goir::StageKind::Open;

        const auto name = mod.add_string("borrow_main");
        const auto i32 = types.builtin(parus::ty::Builtin::kI32);

        parus::goir::SemanticSig sig{};
        sig.name = name;
        sig.param_types.push_back(i32);
        sig.result_type = i32;
        const auto sig_id = mod.add_semantic_sig(sig);

        const auto policy_id = mod.add_placement_policy(parus::goir::GPlacementPolicy{});

        parus::goir::GComputation comp{};
        comp.name = name;
        comp.sig = sig_id;
        comp.placement_policy = policy_id;
        const auto comp_id = mod.add_computation(comp);

        const auto block_id = mod.add_block(parus::goir::Block{});

        parus::goir::Value value{};
        value.ty = i32;
        value.eff = parus::goir::Effect::Pure;
        value.ownership.kind = parus::goir::OwnershipKind::BorrowShared;
        value.ownership.requires_runtime_lowering = true;
        value.def_a = block_id;
        value.def_b = 0;
        const auto param_id = mod.add_value(value);
        mod.blocks[block_id].params.push_back(param_id);

        mod.blocks[block_id].term = parus::goir::TermRet{
            .has_value = true,
            .value = param_id,
        };
        mod.blocks[block_id].has_term = true;

        parus::goir::GRealization real{};
        real.name = name;
        real.computation = comp_id;
        real.family = parus::goir::FamilyKind::Core;
        real.is_entry = true;
        real.entry = block_id;
        real.blocks.push_back(block_id);
        const auto real_id = mod.add_realization(real);

        mod.computations[comp_id].realizations.push_back(real_id);
        return mod;
    }

    static bool test_goir_open_and_placement_vertical_slice_ok_() {
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
        ok &= require_(!p.prog.bag.has_error(), "gOIR seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "gOIR seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "gOIR seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must succeed for supported scalar subset");
        ok &= require_(parus::goir::verify(open.mod).empty(), "gOIR open-stage verify must pass");
        if (!ok) return false;

        const auto open_text = parus::goir::to_string(open.mod, &p.prog.types);
        ok &= require_(open_text.find("stage=open") != std::string::npos, "open dump must show open stage");
        ok &= require_(open_text.find("semantic.invoke @add") != std::string::npos,
                       "open dump must keep semantic.invoke for direct call");

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "gOIR placement must succeed for supported scalar subset");
        ok &= require_(parus::goir::verify(placed.mod).empty(), "gOIR placed-stage verify must pass");
        if (!ok) return false;

        const auto placed_text = parus::goir::to_string(placed.mod, &p.prog.types);
        ok &= require_(placed_text.find("stage=placed") != std::string::npos, "placed dump must show placed stage");
        ok &= require_(placed_text.find("call.direct @add") != std::string::npos,
                       "placed dump must contain direct call after placement");
        ok &= require_(placed_text.find("family=cpu") != std::string::npos,
                       "placement must specialize core realizations to cpu");
        return ok;
    }

    static bool test_goir_open_accepts_ownership_metadata_() {
        parus::ty::TypePool types{};
        const auto mod = make_open_borrow_module_(types);

        bool ok = true;
        const auto errs = parus::goir::verify(mod);
        ok &= require_(errs.empty(), "open-stage verifier must accept ownership metadata");

        ok &= require_(!mod.values.empty(), "manual ownership test module must contain a value");
        if (!mod.values.empty()) {
            ok &= require_(mod.values.front().ownership.kind == parus::goir::OwnershipKind::BorrowShared,
                           "open-stage module must retain borrow ownership metadata");
        }
        return ok;
    }

    static bool test_goir_placement_rejects_ownership_sensitive_values_() {
        parus::ty::TypePool types{};
        const auto mod = make_open_borrow_module_(types);

        const auto placed = parus::goir::place_module(mod);
        bool ok = true;
        ok &= require_(!placed.ok, "placement must reject ownership-sensitive values in M0");
        ok &= require_(!placed.messages.empty(), "ownership rejection must produce a diagnostic");
        if (!placed.messages.empty()) {
            ok &= require_(placed.messages.front().msg.find("ownership-sensitive") != std::string::npos,
                           "ownership rejection must explain that runtime lowering is missing");
        }
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"goir_open_and_placement_vertical_slice", test_goir_open_and_placement_vertical_slice_ok_},
        {"goir_open_accepts_ownership_metadata", test_goir_open_accepts_ownership_metadata_},
        {"goir_placement_rejects_ownership_sensitive_values", test_goir_placement_rejects_ownership_sensitive_values_},
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
