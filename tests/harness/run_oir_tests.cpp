#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/tyck/TypeCheck.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/oir/Builder.hpp>
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

    struct ParsedProgram {
        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::diag::Bag bag;
        parus::ast::StmtId root = parus::ast::k_invalid_stmt;
    };

    static ParsedProgram parse_program(const std::string& src) {
        ParsedProgram p{};
        parus::Lexer lx(src, /*file_id=*/1, &p.bag);
        const auto tokens = lx.lex_all();

        parus::Parser parser(tokens, p.ast, p.types, &p.bag);
        p.root = parser.parse_program();
        return p;
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
        out.prog = parse_program(src);

        parus::passes::PassOptions popt{};
        out.pres = parus::passes::run_on_program(out.prog.ast, out.prog.root, out.prog.bag, popt);

        parus::tyck::TypeChecker tc(out.prog.ast, out.prog.types, out.prog.bag);
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

    /// @brief OIR lowering에서 Call 노드가 실제 InstCall로 생성되는지 검사한다.
    static bool test_oir_call_lowering_ok() {
        const std::string src = R"(
            fn add(a: i32, b: i32) -> i32 {
                return a + b;
            }
            fn main() -> i32 {
                return add(a: 1i32, b: 2i32);
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "call lowering seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "call lowering seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "call lowering seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for normal call source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass after lowering call source");

        bool has_call = false;
        for (const auto& inst : oir.mod.insts) {
            if (std::holds_alternative<parus::oir::InstCall>(inst.data)) {
                has_call = true;
                break;
            }
        }
        ok &= require_(has_call, "OIR must contain at least one InstCall for function call");
        return ok;
    }

    /// @brief OIR pass가 상수 폴딩과 dead pure inst 제거를 수행하는지 검사한다.
    static bool test_oir_const_fold_and_dce() {
        parus::oir::Module m;

        parus::oir::BlockId entry = m.add_block(parus::oir::Block{});

        parus::oir::Function f{};
        f.name = "main";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks.push_back(entry);
        (void)m.add_func(f);

        auto add_result_inst = [&](parus::oir::TypeId ty, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            parus::oir::ValueId vid = m.add_value(v);

            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = vid;
            parus::oir::InstId iid = m.add_inst(inst);

            m.values[vid].def_a = iid;
            m.blocks[entry].insts.push_back(iid);
            return vid;
        };

        parus::oir::ValueId c2 = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"2"});
        parus::oir::ValueId c3 = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"3"});
        parus::oir::ValueId dead_add = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstBinOp{parus::oir::BinOp::Add, c2, c3});
        (void)dead_add;

        parus::oir::ValueId live_add = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstBinOp{parus::oir::BinOp::Add, c2, c3});

        parus::oir::TermRet rt{};
        rt.has_value = true;
        rt.value = live_add;
        m.blocks[entry].term = rt;
        m.blocks[entry].has_term = true;

        const size_t before_inst_count = m.blocks[entry].insts.size();
        parus::oir::run_passes(m);
        const size_t after_inst_count = m.blocks[entry].insts.size();

        bool ok = true;
        ok &= require_(after_inst_count < before_inst_count, "OIR DCE must remove at least one dead pure inst");

        const auto live_iid = m.values[live_add].def_a;
        ok &= require_(live_iid != parus::oir::kInvalidId, "live_add must keep valid def inst");
        if (ok) {
            ok &= require_(std::holds_alternative<parus::oir::InstConstInt>(m.insts[live_iid].data),
                "const fold must replace live add with ConstInt");
            if (std::holds_alternative<parus::oir::InstConstInt>(m.insts[live_iid].data)) {
                const auto& ci = std::get<parus::oir::InstConstInt>(m.insts[live_iid].data);
                ok &= require_(ci.text == "5", "const fold(Add 2,3) must become ConstInt 5");
            }
        }

        const auto verrs = parus::oir::verify(m);
        ok &= require_(verrs.empty(), "OIR verify must pass after const-fold + dce");
        return ok;
    }

    /// @brief OIR verify가 branch 인자/블록 파라미터 개수 불일치를 잡는지 검사한다.
    static bool test_oir_verify_branch_param_mismatch() {
        parus::oir::Module m;

        parus::oir::BlockId entry = m.add_block(parus::oir::Block{});
        parus::oir::BlockId bb1 = m.add_block(parus::oir::Block{});

        parus::oir::Value param{};
        param.ty = 1;
        param.eff = parus::oir::Effect::Pure;
        param.def_a = bb1;
        param.def_b = 0;
        parus::oir::ValueId p0 = m.add_value(param);
        m.blocks[bb1].params.push_back(p0);

        parus::oir::Function f{};
        f.name = "f";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks.push_back(entry);
        f.blocks.push_back(bb1);
        (void)m.add_func(f);

        parus::oir::TermBr br{};
        br.target = bb1;
        // 의도적으로 args 비움: bb1은 param 1개 필요
        m.blocks[entry].term = br;
        m.blocks[entry].has_term = true;

        parus::oir::TermRet rt{};
        rt.has_value = false;
        m.blocks[bb1].term = rt;
        m.blocks[bb1].has_term = true;

        const auto verrs = parus::oir::verify(m);
        bool ok = true;
        ok &= require_(!verrs.empty(), "OIR verify must detect branch arg/param mismatch");
        return ok;
    }

    /// @brief OIR 진입 게이트가 invalid escape handle을 차단하는지 검사한다.
    static bool test_oir_gate_rejects_invalid_escape_handle() {
        const std::string src = R"(
            static let G: i32 = 7i32;
            fn sink(h: &&i32) -> i32 {
                return 0i32;
            }
            fn main() -> i32 {
                return sink(h: &&G);
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "OIR gate seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "OIR gate seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "OIR gate seed must pass SIR capability");
        ok &= require_(!p.sir_mod.escape_handles.empty(), "OIR gate seed must create escape handle metadata");
        if (!ok) return false;

        p.sir_mod.escape_handles[0].materialize_count = 1;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();

        ok &= require_(!oir.gate_passed, "OIR gate must fail when materialize_count is non-zero");
        ok &= require_(!oir.gate_errors.empty(), "OIR gate must emit at least one gate error");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"oir_call_lowering_ok", test_oir_call_lowering_ok},
        {"oir_const_fold_and_dce", test_oir_const_fold_and_dce},
        {"oir_verify_branch_param_mismatch", test_oir_verify_branch_param_mismatch},
        {"oir_gate_rejects_invalid_escape_handle", test_oir_gate_rejects_invalid_escape_handle},
    };

    int failed = 0;
    for (const auto& tc : cases) {
        std::cout << "[TEST] " << tc.name << "\n";
        const bool ok = tc.fn();
        if (!ok) {
            ++failed;
            std::cout << "  -> FAIL\n";
        } else {
            std::cout << "  -> PASS\n";
        }
    }

    if (failed != 0) {
        std::cout << "FAILED: " << failed << " test(s)\n";
        return 1;
    }

    std::cout << "ALL OIR TESTS PASSED\n";
    return 0;
}
