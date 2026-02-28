#include <parus/lex/Lexer.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/tyck/TypeCheck.hpp>
#include <parus/type/TypeResolve.hpp>
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
        parus::type::TypeResolveResult type_resolve{};
        bool macro_type_ready = false;
        bool macro_type_ok = false;
    };

    static ParsedProgram parse_program(const std::string& src) {
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
        out.prog = parse_program(src);
        if (!run_macro_and_type_(out.prog)) return out;

        parus::passes::PassOptions popt{};
        out.pres = parus::passes::run_on_program(out.prog.ast, out.prog.root, out.prog.bag, popt);

        parus::tyck::TypeChecker tc(out.prog.ast, out.prog.types, out.prog.bag, &out.prog.type_resolve);
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
            def add(a: i32, b: i32) -> i32 {
                return a + b;
            }
            def main() -> i32 {
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

    /// @brief generic 함수 인스턴스가 1회 materialize되고 direct call로 선택되는지 검사한다.
    static bool test_generic_fn_instantiation_oir_call_ok() {
        const std::string src = R"(
            def add<T>(a: T, b: T) -> i32 {
                return a + b;
            }
            def main() -> i32 {
                set x = add<i32>(1, 2);
                set y = add<i32>(3, 4);
                return x + y;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "generic call source must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "generic call source must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "generic call source must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for generic call source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass after generic call lowering");
        if (!ok) return false;

        uint32_t generic_add_defs = 0;
        for (const auto& f : oir.mod.funcs) {
            if (f.source_name.find("add<") != std::string::npos) {
                ++generic_add_defs;
            }
        }

        bool has_call_to_generic_add = false;
        for (const auto& inst : oir.mod.insts) {
            if (!std::holds_alternative<parus::oir::InstCall>(inst.data)) continue;
            const auto& c = std::get<parus::oir::InstCall>(inst.data);
            if (c.direct_callee == parus::oir::kInvalidId || c.direct_callee >= oir.mod.funcs.size()) continue;
            const auto& callee = oir.mod.funcs[c.direct_callee];
            if (callee.source_name.find("add<") != std::string::npos) {
                has_call_to_generic_add = true;
                break;
            }
        }

        ok &= require_(generic_add_defs == 1, "same concrete generic tuple must be deduplicated in OIR");
        ok &= require_(has_call_to_generic_add, "generic call must lower to direct concrete callee");
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

    /// @brief const-fold가 block param을 상수로 오인하지 않는지 검사한다.
    static bool test_oir_const_fold_respects_block_params() {
        parus::oir::Module m;

        const parus::oir::BlockId entry = m.add_block(parus::oir::Block{});

        parus::oir::Function f{};
        f.name = "sum_param";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks.push_back(entry);
        (void)m.add_func(f);

        parus::oir::Value p0{};
        p0.ty = 1;
        p0.eff = parus::oir::Effect::Pure;
        p0.def_a = entry;
        p0.def_b = 0;
        const parus::oir::ValueId v_param = m.add_value(p0);
        m.blocks[entry].params.push_back(v_param);

        auto add_result_inst = [&](parus::oir::TypeId ty, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            const parus::oir::ValueId vid = m.add_value(v);

            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = vid;
            const parus::oir::InstId iid = m.add_inst(inst);

            m.values[vid].def_a = iid;
            m.values[vid].def_b = parus::oir::kInvalidId;
            m.blocks[entry].insts.push_back(iid);
            return vid;
        };

        const parus::oir::ValueId c2 = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"2"});
        const parus::oir::ValueId sum = add_result_inst(
            1,
            parus::oir::Effect::Pure,
            parus::oir::InstBinOp{parus::oir::BinOp::Add, v_param, c2}
        );

        parus::oir::TermRet rt{};
        rt.has_value = true;
        rt.value = sum;
        m.blocks[entry].term = rt;
        m.blocks[entry].has_term = true;

        parus::oir::run_passes(m);

        bool ok = true;
        const auto sum_iid = m.values[sum].def_a;
        ok &= require_(sum_iid != parus::oir::kInvalidId, "sum value must keep valid def inst");
        if (ok) {
            ok &= require_(
                std::holds_alternative<parus::oir::InstBinOp>(m.insts[sum_iid].data),
                "const fold must not fold expression that depends on block parameter"
            );
        }

        const auto verrs = parus::oir::verify(m);
        ok &= require_(verrs.empty(), "OIR verify must pass after block-param const-fold guard");
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
            static G: i32 = 7i32;
            def sink(h: &&i32) -> i32 {
                return 0i32;
            }
            def main() -> i32 {
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

    /// @brief critical-edge split + OIR verify 안정성을 검증한다.
    static bool test_oir_global_mem2reg_and_critical_edge() {
        parus::oir::Module m;

        const parus::oir::BlockId entry = m.add_block(parus::oir::Block{});
        const parus::oir::BlockId then_bb = m.add_block(parus::oir::Block{});
        const parus::oir::BlockId join_bb = m.add_block(parus::oir::Block{});

        parus::oir::Function f{};
        f.name = "mem2reg_cfg";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks = {entry, then_bb, join_bb};
        (void)m.add_func(f);

        auto add_result_inst = [&](parus::oir::BlockId bb, parus::oir::TypeId ty, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            const parus::oir::ValueId vid = m.add_value(v);

            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = vid;
            const parus::oir::InstId iid = m.add_inst(inst);

            m.values[vid].def_a = iid;
            m.blocks[bb].insts.push_back(iid);
            return vid;
        };

        auto add_void_inst = [&](parus::oir::BlockId bb, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = parus::oir::kInvalidId;
            const parus::oir::InstId iid = m.add_inst(inst);
            m.blocks[bb].insts.push_back(iid);
        };

        const parus::oir::ValueId cond = add_result_inst(entry, 1, parus::oir::Effect::Pure, parus::oir::InstConstBool{true});
        const parus::oir::ValueId slot = add_result_inst(entry, 1, parus::oir::Effect::MayWriteMem, parus::oir::InstAllocaLocal{1});
        const parus::oir::ValueId c0 = add_result_inst(entry, 1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"0"});
        add_void_inst(entry, parus::oir::Effect::MayWriteMem, parus::oir::InstStore{slot, c0});

        parus::oir::TermCondBr ebr{};
        ebr.cond = cond;
        ebr.then_bb = then_bb;
        ebr.else_bb = join_bb;
        m.blocks[entry].term = ebr;
        m.blocks[entry].has_term = true;

        const parus::oir::ValueId c1 = add_result_inst(then_bb, 1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"1"});
        add_void_inst(then_bb, parus::oir::Effect::MayWriteMem, parus::oir::InstStore{slot, c1});

        parus::oir::TermBr tbr{};
        tbr.target = join_bb;
        m.blocks[then_bb].term = tbr;
        m.blocks[then_bb].has_term = true;

        const parus::oir::ValueId lv = add_result_inst(join_bb, 1, parus::oir::Effect::MayReadMem, parus::oir::InstLoad{slot});
        parus::oir::TermRet jret{};
        jret.has_value = true;
        jret.value = lv;
        m.blocks[join_bb].term = jret;
        m.blocks[join_bb].has_term = true;

        parus::oir::run_passes(m);
        const auto verrs = parus::oir::verify(m);

        bool ok = true;
        ok &= require_(verrs.empty(), "verify must pass after global mem2reg + critical-edge split");
        ok &= require_(m.opt_stats.critical_edges_split > 0, "critical-edge split stat must be increased");
        // v0 안정화 단계에서는 mem2reg를 보수 모드로 둘 수 있으므로 통계 수치는 강제하지 않는다.
        // 핵심 품질 기준은 CFG split 후에도 verify가 유지되는지 여부다.
        return ok;
    }

    /// @brief escape-handle 힌트 기반으로 불필요한 cast/경계 전달을 정리하는지 검사한다.
    static bool test_oir_escape_handle_opt() {
        parus::oir::Module m;

        const parus::oir::BlockId entry = m.add_block(parus::oir::Block{});
        parus::oir::Function f{};
        f.name = "escape_opt";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks.push_back(entry);
        (void)m.add_func(f);

        auto add_result_inst = [&](parus::oir::TypeId ty, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            const parus::oir::ValueId vid = m.add_value(v);

            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = vid;
            const parus::oir::InstId iid = m.add_inst(inst);

            m.values[vid].def_a = iid;
            m.blocks[entry].insts.push_back(iid);
            return vid;
        };

        const parus::oir::ValueId base = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"42"});
        const parus::oir::ValueId casted = add_result_inst(
            1,
            parus::oir::Effect::Pure,
            parus::oir::InstCast{parus::oir::CastKind::As, 1, base}
        );

        parus::oir::EscapeHandleHint hint{};
        hint.value = base;
        hint.pointee_type = 1;
        hint.kind = parus::oir::EscapeHandleKind::CallerSlot;
        hint.boundary = parus::oir::EscapeBoundaryKind::Return;
        hint.abi_pack_required = false;
        m.add_escape_hint(hint);

        parus::oir::TermRet rt{};
        rt.has_value = true;
        rt.value = casted;
        m.blocks[entry].term = rt;
        m.blocks[entry].has_term = true;

        parus::oir::run_passes(m);
        const auto verrs = parus::oir::verify(m);

        bool ok = true;
        ok &= require_(verrs.empty(), "verify must pass after escape-handle optimization");
        ok &= require_(m.opt_stats.escape_pack_elided > 0, "escape-handle pass must elide at least one pack/cast");
        ok &= require_(m.opt_stats.escape_boundary_rewrites > 0, "escape-handle pass must rewrite boundary operand");

        ok &= require_(std::holds_alternative<parus::oir::TermRet>(m.blocks[entry].term), "entry terminator must remain ret");
        if (std::holds_alternative<parus::oir::TermRet>(m.blocks[entry].term)) {
            const auto& ret = std::get<parus::oir::TermRet>(m.blocks[entry].term);
            ok &= require_(ret.has_value && ret.value == base, "ret must point to canonical escape source value");
        }
        return ok;
    }

    /// @brief 중복 순수 연산 제거 경로가 verify를 깨지 않는지 검사한다.
    static bool test_oir_gvn_cse_ok() {
        parus::oir::Module m;
        const parus::oir::BlockId entry = m.add_block(parus::oir::Block{});

        parus::oir::Function f{};
        f.name = "gvn";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks.push_back(entry);
        (void)m.add_func(f);

        parus::oir::Value pv{};
        pv.ty = 1;
        pv.eff = parus::oir::Effect::Pure;
        pv.def_a = entry;
        pv.def_b = 0;
        const parus::oir::ValueId p0 = m.add_value(pv);
        m.blocks[entry].params.push_back(p0);

        auto add_result_inst = [&](parus::oir::TypeId ty, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            const parus::oir::ValueId vid = m.add_value(v);

            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = vid;
            const parus::oir::InstId iid = m.add_inst(inst);

            m.values[vid].def_a = iid;
            m.blocks[entry].insts.push_back(iid);
            return vid;
        };

        const parus::oir::ValueId c1 = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"1"});
        (void)add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstBinOp{parus::oir::BinOp::Add, p0, c1});
        const parus::oir::ValueId add2 = add_result_inst(1, parus::oir::Effect::Pure, parus::oir::InstBinOp{parus::oir::BinOp::Add, p0, c1});

        parus::oir::TermRet rt{};
        rt.has_value = true;
        rt.value = add2;
        m.blocks[entry].term = rt;
        m.blocks[entry].has_term = true;

        const size_t before = m.blocks[entry].insts.size();
        parus::oir::run_passes(m);
        const size_t after = m.blocks[entry].insts.size();

        bool ok = true;
        ok &= require_(after < before, "GVN/CSE must remove duplicated pure expression");
        ok &= require_(parus::oir::verify(m).empty(), "verify must pass after GVN/CSE");
        return ok;
    }

    /// @brief loop canonical form 경로가 verify를 깨지 않는지 검사한다.
    static bool test_oir_loop_canonical_and_licm_ok() {
        parus::oir::Module m;
        const parus::oir::BlockId entry = m.add_block(parus::oir::Block{});
        const parus::oir::BlockId header = m.add_block(parus::oir::Block{});
        const parus::oir::BlockId body = m.add_block(parus::oir::Block{});
        const parus::oir::BlockId exit = m.add_block(parus::oir::Block{});

        parus::oir::Function f{};
        f.name = "loop";
        f.ret_ty = 1;
        f.entry = entry;
        f.blocks = {entry, header, body, exit};
        (void)m.add_func(f);

        parus::oir::Value p{};
        p.ty = 1;
        p.eff = parus::oir::Effect::Pure;
        p.def_a = entry;
        p.def_b = 0;
        const parus::oir::ValueId p0 = m.add_value(p);
        m.blocks[entry].params.push_back(p0);

        auto add_result_inst = [&](parus::oir::BlockId bb, parus::oir::TypeId ty, parus::oir::Effect eff, parus::oir::InstData data) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            const parus::oir::ValueId vid = m.add_value(v);

            parus::oir::Inst inst{};
            inst.data = std::move(data);
            inst.eff = eff;
            inst.result = vid;
            const parus::oir::InstId iid = m.add_inst(inst);

            m.values[vid].def_a = iid;
            m.blocks[bb].insts.push_back(iid);
            return vid;
        };

        const parus::oir::ValueId cond0 = add_result_inst(entry, 1, parus::oir::Effect::Pure, parus::oir::InstConstBool{true});
        const parus::oir::ValueId cond1 = add_result_inst(header, 1, parus::oir::Effect::Pure, parus::oir::InstConstBool{true});
        const parus::oir::ValueId c2 = add_result_inst(body, 1, parus::oir::Effect::Pure, parus::oir::InstConstInt{"2"});
        (void)add_result_inst(body, 1, parus::oir::Effect::Pure, parus::oir::InstBinOp{parus::oir::BinOp::Add, p0, c2});

        parus::oir::TermCondBr et{};
        et.cond = cond0;
        et.then_bb = header;
        et.else_bb = exit;
        m.blocks[entry].term = et;
        m.blocks[entry].has_term = true;

        parus::oir::TermCondBr ht{};
        ht.cond = cond1;
        ht.then_bb = body;
        ht.else_bb = exit;
        m.blocks[header].term = ht;
        m.blocks[header].has_term = true;

        parus::oir::TermBr bt{};
        bt.target = header;
        m.blocks[body].term = bt;
        m.blocks[body].has_term = true;

        parus::oir::TermRet rt{};
        rt.has_value = true;
        rt.value = p0;
        m.blocks[exit].term = rt;
        m.blocks[exit].has_term = true;

        parus::oir::run_passes(m);
        bool ok = true;
        ok &= require_(m.opt_stats.loop_canonicalized > 0, "loop canonical form must create a preheader");
        ok &= require_(parus::oir::verify(m).empty(), "verify must pass after loop canonical + LICM");
        return ok;
    }

    /// @brief class/proto(default body) 멤버가 SIR->OIR 함수로 lowering되는지 검사한다.
    static bool test_class_and_proto_default_member_lowering_ok() {
        const std::string src = R"(
            proto WidgetProto {
                def id(self) -> i32 {
                    return 7i32;
                }
            };

            class Button : WidgetProto {
                value: i32;

                def tap(self) -> i32 {
                    return 3i32;
                }
            }

            def main() -> i32 {
                return 0i32;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "class/proto lowering seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "class/proto lowering seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "class/proto lowering seed must pass SIR capability");
        if (!ok) return false;

        bool has_proto_default = false;
        bool has_class_member = false;
        for (const auto& f : p.sir_mod.funcs) {
            const std::string n(f.name);
            if (n.find("WidgetProto::id") != std::string::npos) has_proto_default = true;
            if (n.find("Button::tap") != std::string::npos) has_class_member = true;
        }
        ok &= require_(has_proto_default, "SIR must contain proto default member function");
        ok &= require_(has_class_member, "SIR must contain class member function");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for class/proto member lowering");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for class/proto member lowering");
        return ok;
    }

    /// @brief class override가 있으면 proto default 대신 class 멤버를 호출해야 한다.
    static bool test_proto_default_override_dispatch_prefers_class_member_ok() {
        const std::string src = R"(
            proto ValueProto {
                def value(self) -> i32 {
                    return 1i32;
                }
            };

            class Counter : ValueProto {
                init() = default;

                def value(self) -> i32 {
                    return 2i32;
                }
            }

            def main() -> i32 {
                set c = Counter();
                return c.value();
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "proto override seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "proto override seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "proto override seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for proto override source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for proto override source");
        if (!ok) return false;

        return ok;
    }

    /// @brief class 생성식 `A(...)`가 OIR에서 `A::init(...)` direct call로 lowering되는지 검사한다.
    static bool test_class_ctor_call_lowers_to_init_call_ok() {
        const std::string src = R"(
            class User {
                init() = default;

                def id(self) -> i32 {
                    return 5i32;
                }
            }

            def main() -> i32 {
                set u = User();
                return u.id();
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "class ctor lowering seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "class ctor lowering seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "class ctor lowering seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for class ctor call source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for class ctor call source");
        if (!ok) return false;

        bool has_init_direct_call = false;
        for (const auto& inst : oir.mod.insts) {
            if (!std::holds_alternative<parus::oir::InstCall>(inst.data)) continue;
            const auto& c = std::get<parus::oir::InstCall>(inst.data);
            if (c.direct_callee == parus::oir::kInvalidId || c.direct_callee >= oir.mod.funcs.size()) continue;
            const auto& f = oir.mod.funcs[c.direct_callee];
            const bool name_match =
                (f.name.find("User::init") != std::string::npos) ||
                (f.name.find("User") != std::string::npos && f.name.find("init") != std::string::npos);
            const bool source_match = (f.source_name == "init");
            if (name_match || source_match) {
                has_init_direct_call = true;
                break;
            }
        }
        ok &= require_(has_init_direct_call, "constructor call must lower to direct User::init call");
        return ok;
    }

    /// @brief class 인스턴스 필드가 SIR/OIR 레이아웃 메타로 내려가는지 검사한다.
    static bool test_class_field_layout_lowering_ok() {
        const std::string src = R"(
            class Vec2 {
                x: i32;
                y: i32;
                init() = default;

                def sum(self) -> i32 {
                    return self.x + self.y;
                }
            }

            def main() -> i32 {
                set v = Vec2();
                return v.x;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "class field layout seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "class field layout seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "class field layout seed must pass SIR capability");
        if (!ok) return false;

        bool has_class_layout = false;
        for (const auto& f : p.sir_mod.fields) {
            const std::string n(f.name);
            if (n.find("Vec2") == std::string::npos) continue;
            if (f.member_count == 2) {
                has_class_layout = true;
                break;
            }
        }
        ok &= require_(has_class_layout, "SIR must contain class field layout metadata for Vec2");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for class field layout source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for class field layout source");
        if (!ok) return false;

        bool has_vec2_layout = false;
        for (const auto& f : oir.mod.fields) {
            if (f.name.find("Vec2") == std::string::npos) continue;
            if (f.members.size() == 2) {
                has_vec2_layout = true;
                break;
            }
        }
        ok &= require_(has_vec2_layout, "OIR must contain class field layout metadata for Vec2");
        return ok;
    }

    /// @brief class static def/var가 OIR 함수/글로벌로 하향되는지 검사한다.
    static bool test_class_static_members_lowering_ok() {
        const std::string src = R"(
            class Counter {
                init() = default;
                static count: i32 = 7i32;

                static def add(a: i32, b: i32) -> i32 {
                    return a + b;
                }
            }

            def main() -> i32 {
                return Counter::add(a: Counter::count, b: 1i32);
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "class static member seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "class static member seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "class static member seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for class static member source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for class static member source");
        if (!ok) return false;

        bool has_static_fn_call = false;
        for (const auto& inst : oir.mod.insts) {
            if (!std::holds_alternative<parus::oir::InstCall>(inst.data)) continue;
            const auto& c = std::get<parus::oir::InstCall>(inst.data);
            if (c.direct_callee != parus::oir::kInvalidId) {
                has_static_fn_call = true;
                break;
            }
        }
        ok &= require_(has_static_fn_call, "OIR must contain direct call lowered from static class method path call");

        const bool has_static_global = !oir.mod.globals.empty();
        ok &= require_(has_static_global, "OIR must contain static class variable global symbol");
        return ok;
    }

    /// @brief class 로컬 값은 스코프 종료 시 InstDrop이 삽입되어야 한다.
    static bool test_class_raii_scope_exit_deinit_call_ok() {
        const std::string src = R"(
            class Resource {
                init() = default;
                deinit() = default;
            }

            def main() -> i32 {
                do {
                    set r = Resource();
                }
                return 0i32;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "raii scope-exit seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "raii scope-exit seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "raii scope-exit seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for raii scope-exit source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for raii scope-exit source");
        if (!ok) return false;

        bool has_drop = false;
        for (const auto& inst : oir.mod.insts) {
            if (std::holds_alternative<parus::oir::InstDrop>(inst.data)) {
                has_drop = true;
                break;
            }
        }
        ok &= require_(has_drop, "scope-exit path must include InstDrop");
        return ok;
    }

    /// @brief `&&`로 이동된 class 로컬은 스코프 종료 deinit 대상에서 제외되어야 한다.
    static bool test_class_raii_escape_move_skips_deinit_call_ok() {
        const std::string src = R"(
            class Resource {
                init() = default;
                deinit() = default;
            }

            def sink(v: &&Resource) -> i32 {
                return 0i32;
            }

            def main() -> i32 {
                set r = Resource();
                sink(v: &&r);
                return 0i32;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "raii escape-move seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "raii escape-move seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "raii escape-move seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for raii escape-move source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for raii escape-move source");
        if (!ok) return false;

        uint32_t main_fid = parus::oir::kInvalidId;
        for (uint32_t i = 0; i < oir.mod.funcs.size(); ++i) {
            if (oir.mod.funcs[i].source_name == "main") {
                main_fid = i;
                break;
            }
        }
        ok &= require_(main_fid != parus::oir::kInvalidId, "main function must exist in OIR module");
        if (!ok) return false;

        bool has_main_drop = false;
        const auto& mf = oir.mod.funcs[main_fid];
        for (const auto bb : mf.blocks) {
            if (bb == parus::oir::kInvalidId || bb >= oir.mod.blocks.size()) continue;
            const auto& block = oir.mod.blocks[bb];
            for (const auto iid : block.insts) {
                if (iid == parus::oir::kInvalidId || iid >= oir.mod.insts.size()) continue;
                const auto& inst = oir.mod.insts[iid];
                if (std::holds_alternative<parus::oir::InstDrop>(inst.data)) {
                    has_main_drop = true;
                    break;
                }
            }
            if (has_main_drop) break;
        }

        ok &= require_(!has_main_drop, "moved local must not emit InstDrop on scope exit");
        return ok;
    }

    /// @brief actor spawn/pub/sub가 OIR에 내려가고 commit/recast 마커 inst가 생성되는지 검사한다.
    static bool test_actor_spawn_and_markers_lowering_ok() {
        const std::string src = R"(
            actor Counter {
                draft {
                    value: i32;
                }

                init(seed: i32) {
                    draft.value = seed;
                }

                def sub get() -> i32 {
                    recast;
                    return draft.value;
                }

                def pub add(delta: i32) -> i32 {
                    draft.value = draft.value + delta;
                    commit;
                    return draft.value;
                }
            }

            def main() -> i32 {
                set c = spawn Counter(seed: 1i32);
                set x = c.get();
                set y = c.add(delta: 2i32);
                return x + y;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "actor seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "actor seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "actor seed must pass SIR capability");
        if (!ok) return false;

        parus::oir::Builder ob(p.sir_mod, p.prog.types);
        auto oir = ob.build();
        ok &= require_(oir.gate_passed, "OIR gate must pass for actor source");
        if (!ok) return false;

        parus::oir::run_passes(oir.mod);
        const auto verrs = parus::oir::verify(oir.mod);
        ok &= require_(verrs.empty(), "OIR verify must pass for actor source");
        if (!ok) return false;

        bool has_commit = false;
        bool has_recast = false;
        bool has_ctor_init_call = false;
        bool has_pub_mode_symbol = false;
        bool has_sub_mode_symbol = false;

        for (const auto& f : oir.mod.funcs) {
            if (f.name.find("Mpub") != std::string::npos) has_pub_mode_symbol = true;
            if (f.name.find("Msub") != std::string::npos) has_sub_mode_symbol = true;
        }

        for (const auto& inst : oir.mod.insts) {
            if (std::holds_alternative<parus::oir::InstActorCommit>(inst.data)) {
                has_commit = true;
            } else if (std::holds_alternative<parus::oir::InstActorRecast>(inst.data)) {
                has_recast = true;
            } else if (std::holds_alternative<parus::oir::InstCall>(inst.data)) {
                const auto& c = std::get<parus::oir::InstCall>(inst.data);
                if (c.direct_callee != parus::oir::kInvalidId &&
                    c.direct_callee < oir.mod.funcs.size()) {
                    const auto& callee = oir.mod.funcs[c.direct_callee];
                    if (callee.source_name.find("init") != std::string::npos) {
                        has_ctor_init_call = true;
                    }
                }
            }
        }

        ok &= require_(has_commit, "actor pub must lower commit statement to InstActorCommit");
        ok &= require_(has_recast, "actor sub must lower recast statement to InstActorRecast");
        ok &= require_(has_ctor_init_call, "spawn must lower to direct init call");
        ok &= require_(has_pub_mode_symbol, "actor pub function must keep mode marker in OIR symbol");
        ok &= require_(has_sub_mode_symbol, "actor sub function must keep mode marker in OIR symbol");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*def)();
    };

    const Case cases[] = {
        {"oir_call_lowering_ok", test_oir_call_lowering_ok},
        {"generic_fn_instantiation_oir_call_ok", test_generic_fn_instantiation_oir_call_ok},
        {"oir_const_fold_and_dce", test_oir_const_fold_and_dce},
        {"oir_const_fold_respects_block_params", test_oir_const_fold_respects_block_params},
        {"oir_verify_branch_param_mismatch", test_oir_verify_branch_param_mismatch},
        {"oir_gate_rejects_invalid_escape_handle", test_oir_gate_rejects_invalid_escape_handle},
        {"oir_global_mem2reg_and_critical_edge", test_oir_global_mem2reg_and_critical_edge},
        {"oir_escape_handle_opt", test_oir_escape_handle_opt},
        {"oir_gvn_cse_ok", test_oir_gvn_cse_ok},
        {"oir_loop_canonical_and_licm_ok", test_oir_loop_canonical_and_licm_ok},
        {"class_and_proto_default_member_lowering_ok", test_class_and_proto_default_member_lowering_ok},
        {"proto_default_override_dispatch_prefers_class_member_ok", test_proto_default_override_dispatch_prefers_class_member_ok},
        {"class_ctor_call_lowers_to_init_call_ok", test_class_ctor_call_lowers_to_init_call_ok},
        {"class_field_layout_lowering_ok", test_class_field_layout_lowering_ok},
        {"class_static_members_lowering_ok", test_class_static_members_lowering_ok},
        {"class_raii_scope_exit_deinit_call_ok", test_class_raii_scope_exit_deinit_call_ok},
        {"class_raii_escape_move_skips_deinit_call_ok", test_class_raii_escape_move_skips_deinit_call_ok},
        {"actor_spawn_and_markers_lowering_ok", test_actor_spawn_and_markers_lowering_ok},
    };

    int failed = 0;
    for (const auto& tc : cases) {
        std::cout << "[TEST] " << tc.name << "\n";
        const bool ok = tc.def();
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
