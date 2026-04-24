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

    static bool require_goir_messages_empty_(const std::vector<parus::goir::Message>& messages,
                                             const char* label) {
        if (messages.empty()) return true;
        std::cerr << "  - " << label << "\n";
        for (const auto& message : messages) {
            std::cerr << "    * " << message.msg << "\n";
        }
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

    static parus::goir::ValueId add_block_param_(parus::goir::Module& mod,
                                                 parus::goir::BlockId block,
                                                 parus::goir::TypeId ty,
                                                 parus::goir::LayoutClass layout = parus::goir::LayoutClass::Unknown) {
        parus::goir::Value value{};
        value.ty = ty;
        value.place_elem_type = ty;
        value.eff = parus::goir::Effect::Pure;
        value.layout = (layout == parus::goir::LayoutClass::Unknown) ? parus::goir::LayoutClass::Scalar : layout;
        value.def_a = block;
        value.def_b = static_cast<uint32_t>(mod.blocks[block].params.size());
        const auto id = mod.add_value(value);
        mod.blocks[block].params.push_back(id);
        return id;
    }

    static parus::goir::ValueId add_inst_result_(parus::goir::Module& mod,
                                                 parus::goir::BlockId block,
                                                 parus::goir::TypeId result_ty,
                                                 parus::goir::Effect eff,
                                                 parus::goir::OpData data,
                                                 parus::goir::LayoutClass layout = parus::goir::LayoutClass::Unknown,
                                                 bool is_place = false,
                                                 parus::goir::TypeId place_elem_type = parus::goir::kInvalidType,
                                                 parus::goir::PlaceKind place_kind = parus::goir::PlaceKind::None,
                                                 bool is_mutable = false,
                                                 parus::goir::OwnershipInfo ownership = {}) {
        parus::goir::ValueId result = parus::goir::kInvalidId;
        if (result_ty != parus::goir::kInvalidType) {
            parus::goir::Value value{};
            value.ty = result_ty;
            value.place_elem_type = (place_elem_type == parus::goir::kInvalidType) ? result_ty : place_elem_type;
            value.eff = eff;
            value.ownership = ownership;
            value.layout = layout;
            value.place_kind = place_kind;
            value.is_place = is_place;
            value.is_mutable = is_mutable;
            value.def_a = static_cast<uint32_t>(mod.insts.size());
            result = mod.add_value(value);
        }

        parus::goir::Inst inst{};
        inst.data = std::move(data);
        inst.eff = eff;
        inst.result = result;
        const auto iid = mod.add_inst(inst);
        if (result != parus::goir::kInvalidId) {
            mod.values[result].def_a = iid;
        }
        mod.blocks[block].insts.push_back(iid);
        return result;
    }

    static void set_term_(parus::goir::Module& mod,
                          parus::goir::BlockId block,
                          parus::goir::Terminator term) {
        mod.blocks[block].term = std::move(term);
        mod.blocks[block].has_term = true;
    }

    static parus::goir::Module make_open_loop_block_arg_module_(parus::ty::TypePool& types) {
        parus::goir::Module mod{};
        mod.header.stage_kind = parus::goir::StageKind::Open;

        const auto name = mod.add_string("loop_phi_main");
        const auto i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto bool_ty = types.builtin(parus::ty::Builtin::kBool);

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

        const auto entry = mod.add_block(parus::goir::Block{});
        const auto loop = mod.add_block(parus::goir::Block{});
        const auto body = mod.add_block(parus::goir::Block{});
        const auto exit = mod.add_block(parus::goir::Block{});

        const auto input = add_block_param_(mod, entry, i32, parus::goir::LayoutClass::Scalar);
        const auto iter = add_block_param_(mod, loop, i32, parus::goir::LayoutClass::Scalar);
        const auto body_cur = add_block_param_(mod, body, i32, parus::goir::LayoutClass::Scalar);
        const auto out = add_block_param_(mod, exit, i32, parus::goir::LayoutClass::Scalar);

        const auto three = add_inst_result_(
            mod, loop, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"3"},
            parus::goir::LayoutClass::Scalar);
        const auto cond = add_inst_result_(
            mod, loop, bool_ty, parus::goir::Effect::Pure,
            parus::goir::OpBinary{.op = parus::goir::BinOp::Lt, .lhs = iter, .rhs = three},
            parus::goir::LayoutClass::Scalar);

        const auto one = add_inst_result_(
            mod, body, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"1"},
            parus::goir::LayoutClass::Scalar);
        const auto next = add_inst_result_(
            mod, body, i32, parus::goir::Effect::Pure,
            parus::goir::OpBinary{.op = parus::goir::BinOp::Add, .lhs = body_cur, .rhs = one},
            parus::goir::LayoutClass::Scalar);

        set_term_(mod, entry, parus::goir::TermBr{
            .target = loop,
            .args = {input},
        });
        set_term_(mod, loop, parus::goir::TermCondBr{
            .cond = cond,
            .then_bb = body,
            .then_args = {iter},
            .else_bb = exit,
            .else_args = {iter},
        });
        set_term_(mod, body, parus::goir::TermBr{
            .target = loop,
            .args = {next},
        });
        set_term_(mod, exit, parus::goir::TermRet{
            .has_value = true,
            .value = out,
        });

        parus::goir::GRealization real{};
        real.name = name;
        real.computation = comp_id;
        real.family = parus::goir::FamilyKind::Core;
        real.is_entry = true;
        real.entry = entry;
        real.blocks = {entry, loop, body, exit};
        const auto real_id = mod.add_realization(real);

        mod.computations[comp_id].realizations.push_back(real_id);
        return mod;
    }

    static parus::goir::Module make_open_ownership_marker_module_(parus::ty::TypePool& types) {
        parus::goir::Module mod{};
        mod.header.stage_kind = parus::goir::StageKind::Open;

        const auto name = mod.add_string("ownership_main");
        const auto seed_name = mod.add_string("seed");
        const auto i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto borrow_i32 = types.make_borrow(i32, /*is_mut=*/false);
        const auto escape_i32 = types.make_escape(i32);

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

        const auto entry = mod.add_block(parus::goir::Block{});
        const auto param = add_block_param_(mod, entry, i32, parus::goir::LayoutClass::Scalar);

        const auto slot = add_inst_result_(
            mod,
            entry,
            i32,
            parus::goir::Effect::Pure,
            parus::goir::OpLocalSlot{.debug_name = seed_name},
            parus::goir::LayoutClass::Scalar,
            true,
            i32,
            parus::goir::PlaceKind::LocalSlot,
            true
        );
        (void)add_inst_result_(
            mod,
            entry,
            parus::goir::kInvalidType,
            parus::goir::Effect::MayWrite,
            parus::goir::OpStore{.place = slot, .value = param}
        );

        parus::goir::OwnershipInfo borrow_ownership{};
        borrow_ownership.kind = parus::goir::OwnershipKind::BorrowShared;
        borrow_ownership.requires_runtime_lowering = true;
        const auto borrow = add_inst_result_(
            mod,
            entry,
            borrow_i32,
            parus::goir::Effect::MayRead,
            parus::goir::OpBorrowView{.source_place = slot},
            parus::goir::LayoutClass::Unknown,
            false,
            borrow_i32,
            parus::goir::PlaceKind::None,
            false,
            borrow_ownership
        );

        parus::goir::OwnershipInfo escape_ownership{};
        escape_ownership.kind = parus::goir::OwnershipKind::Escape;
        escape_ownership.requires_runtime_lowering = true;
        escape_ownership.escape_kind = parus::sir::EscapeHandleKind::kCallerSlot;
        escape_ownership.escape_boundary = parus::sir::EscapeBoundaryKind::kReturn;
        const auto escaped = add_inst_result_(
            mod,
            entry,
            escape_i32,
            parus::goir::Effect::MayTrap,
            parus::goir::OpEscapeView{.source_place = slot},
            parus::goir::LayoutClass::Unknown,
            false,
            escape_i32,
            parus::goir::PlaceKind::None,
            false,
            escape_ownership
        );

        (void)borrow;
        (void)escaped;

        set_term_(mod, entry, parus::goir::TermRet{
            .has_value = true,
            .value = param,
        });

        parus::goir::GRealization real{};
        real.name = name;
        real.computation = comp_id;
        real.family = parus::goir::FamilyKind::Core;
        real.is_entry = true;
        real.entry = entry;
        real.blocks = {entry};
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
        ok &= require_(!p.prog.bag.has_error(), "gOIR scalar seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "gOIR scalar seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "gOIR scalar seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must succeed for supported scalar subset");
        ok &= require_goir_messages_empty_(open.messages, "scalar open-stage build emitted diagnostics");
        ok &= require_(parus::goir::verify(open.mod).empty(), "gOIR open-stage verify must pass");
        if (!ok) return false;

        const auto open_text = parus::goir::to_string(open.mod, &p.prog.types);
        ok &= require_(open_text.find("stage=open") != std::string::npos, "open dump must show open stage");
        ok &= require_(open_text.find("semantic.invoke @add") != std::string::npos,
                       "open dump must keep semantic.invoke for direct call");

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "gOIR placement must succeed for supported scalar subset");
        ok &= require_goir_messages_empty_(placed.messages, "scalar placement emitted diagnostics");
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

    static bool test_goir_memory_and_record_builder_slice_ok_() {
        const std::string src = R"(
            struct Pair {
                lhs: i32;
                rhs: i32;
            };

            @pure def add(a: i32, b: i32) -> i32 {
                return a + b;
            }

            @pure def main(flag: bool, seed: i32) -> i32 {
                set mut n = seed;
                set mut arr = [1i32, 2i32, 3i32];
                set mut pair = Pair{ lhs: arr[0usize], rhs: arr[1usize] };
                while (n < 3i32) {
                    arr[1usize] = arr[1usize] + n;
                    n = n + 1i32;
                }
                pair.rhs = add(pair.rhs, arr[1usize]);
                if (flag) {
                    return pair.lhs;
                }
                return pair.rhs + n;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "gOIR memory/record seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "gOIR memory/record seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "gOIR memory/record seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must succeed for mutable local/array/record subset");
        ok &= require_goir_messages_empty_(open.messages, "memory/record open-stage build emitted diagnostics");
        ok &= require_(parus::goir::verify(open.mod).empty(), "memory/record open-stage verify must pass");
        if (!ok) return false;

        const auto open_text = parus::goir::to_string(open.mod, &p.prog.types);
        ok &= require_(open_text.find("local.slot") != std::string::npos,
                       "open dump must contain local.slot for mutable/aggregate locals");
        ok &= require_(open_text.find("record.make {") != std::string::npos,
                       "open dump must contain record.make for plain internal record literals");
        ok &= require_(open_text.find("index.place") != std::string::npos,
                       "open dump must contain index.place for array element places");
        ok &= require_(open_text.find("field.place") != std::string::npos,
                       "open dump must contain field.place for record field places");
        ok &= require_(open_text.find("store %") != std::string::npos,
                       "open dump must contain store for mutable writes");
        ok &= require_(open_text.find("load %") != std::string::npos,
                       "open dump must contain load for place reads");
        if (!ok) return false;

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "placement must succeed for the first memory/aggregate CPU slice");
        ok &= require_goir_messages_empty_(placed.messages, "memory/record placement emitted diagnostics");
        ok &= require_(parus::goir::verify(placed.mod).empty(), "memory/record placed verify must pass");
        if (!ok) return false;

        const auto placed_text = parus::goir::to_string(placed.mod, &p.prog.types);
        ok &= require_(placed_text.find("call.direct @add") != std::string::npos,
                       "placed dump must keep direct pure/internal call lowering");
        ok &= require_(placed_text.find("family=cpu") != std::string::npos,
                       "placed dump must freeze the realization family to cpu");
        return ok;
    }

    static bool test_goir_open_and_placed_verify_block_args_and_backedges_() {
        parus::ty::TypePool types{};
        const auto mod = make_open_loop_block_arg_module_(types);

        bool ok = true;
        const auto open_errs = parus::goir::verify(mod);
        ok &= require_(open_errs.empty(), "open verifier must accept non-entry block params and branch args");
        if (!ok) return false;

        const auto open_text = parus::goir::to_string(mod, &types);
        ok &= require_(open_text.find("block ^bb") != std::string::npos,
                       "open dump must show explicit block labels");
        ok &= require_(open_text.find("term condbr") != std::string::npos,
                       "open dump must show condbr over block-param CFG");
        ok &= require_(open_text.find("term br ^bb") != std::string::npos,
                       "open dump must show backedge branch");
        if (!ok) return false;

        const auto placed = parus::goir::place_module(mod);
        ok &= require_(placed.ok, "placement must accept block-param CFG in the supported scalar subset");
        ok &= require_goir_messages_empty_(placed.messages, "block-param placement emitted diagnostics");
        ok &= require_(parus::goir::verify(placed.mod).empty(), "placed verifier must accept block-param CFG");
        return ok;
    }

    static bool test_goir_open_accepts_explicit_borrow_escape_markers_() {
        parus::ty::TypePool types{};
        const auto mod = make_open_ownership_marker_module_(types);

        bool ok = true;
        const auto errs = parus::goir::verify(mod);
        ok &= require_(errs.empty(), "open-stage verifier must accept explicit borrow/escape markers");
        ok &= require_(mod.values.size() >= 4, "ownership marker test module must contain marker values");
        if (!ok) return false;

        const auto dump = parus::goir::to_string(mod, &types);
        ok &= require_(dump.find("borrow.view") != std::string::npos,
                       "open dump must contain borrow.view");
        ok &= require_(dump.find("escape.view") != std::string::npos,
                       "open dump must contain escape.view");
        return ok;
    }

    static bool test_goir_placement_rejects_explicit_ownership_markers_() {
        parus::ty::TypePool types{};
        const auto mod = make_open_ownership_marker_module_(types);

        const auto placed = parus::goir::place_module(mod);
        bool ok = true;
        ok &= require_(!placed.ok, "placement must reject ownership-sensitive borrow/escape markers in M1");
        ok &= require_(!placed.messages.empty(), "ownership rejection must produce a diagnostic");
        if (!placed.messages.empty()) {
            ok &= require_(placed.messages.front().msg.find("ownership-sensitive") != std::string::npos,
                           "ownership rejection must explain that runtime ownership lowering is missing");
        }
        return ok;
    }

    static bool test_goir_text_value_slice_ok_() {
        const std::string src = R"(
            @pure def pick(flag: bool, a: text, b: text) -> text {
                if (flag) {
                    return a;
                }
                return b;
            }

            @pure def main(flag: bool) -> text {
                let hello: text = "hello";
                let world: text = "world";
                return pick(flag, hello, world);
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "gOIR text seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "gOIR text seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "gOIR text seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must support text-view values");
        ok &= require_goir_messages_empty_(open.messages, "text open-stage build emitted diagnostics");
        ok &= require_(parus::goir::verify(open.mod).empty(), "text open-stage verify must pass");
        if (!ok) return false;

        const auto open_text = parus::goir::to_string(open.mod, &p.prog.types);
        ok &= require_(open_text.find("text.lit") != std::string::npos,
                       "open dump must contain text.lit for semantic text constants");
        ok &= require_(open_text.find("semantic.invoke @pick") != std::string::npos,
                       "open dump must keep semantic.invoke for text-value calls");
        if (!ok) return false;

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "gOIR placement must support text-view values");
        ok &= require_goir_messages_empty_(placed.messages, "text placement emitted diagnostics");
        ok &= require_(parus::goir::verify(placed.mod).empty(), "text placed verify must pass");
        if (!ok) return false;

        const auto placed_text = parus::goir::to_string(placed.mod, &p.prog.types);
        ok &= require_(placed_text.find("call.direct @pick") != std::string::npos,
                       "placed dump must lower text-value direct calls");
        return ok;
    }

    static bool test_goir_optional_subview_enum_switch_slice_ok_() {
        const std::string src = R"(
            enum E {
                case A,
                case B,
            };

            @pure def choose(flag: bool) -> E {
                if (flag) { return E::A(); }
                return E::B();
            }

            @pure def main(flag: bool) -> i32 {
                let mut maybe: i32? = null;
                if (flag) {
                    maybe = 7i32;
                }
                let arr: i32[4] = [10i32, 20i32, 30i32, 40i32];
                let mid: i32[] = arr[1i32..3i32];
                let e: E = choose(flag);
                switch (e) {
                case E::A: { return (maybe ?? 5i32) + mid[0i32]; }
                default: {}
                }
                return (maybe ?? 2i32) + mid[1i32];
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "gOIR optional/subview/enum seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "gOIR optional/subview/enum seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "gOIR optional/subview/enum seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must support optionals, source subview, and tag-enum switch");
        ok &= require_goir_messages_empty_(open.messages, "optional/subview/enum open-stage build emitted diagnostics");
        ok &= require_(parus::goir::verify(open.mod).empty(), "optional/subview/enum open-stage verify must pass");
        if (!ok) return false;

        const auto open_text = parus::goir::to_string(open.mod, &p.prog.types);
        ok &= require_(open_text.find("optional.none") != std::string::npos,
                       "open dump must contain optional.none for null-lowered optionals");
        ok &= require_(open_text.find("optional.some") != std::string::npos,
                       "open dump must contain optional.some for lifted values");
        ok &= require_(open_text.find("optional.is_present") != std::string::npos,
                       "open dump must contain optional.is_present for null-sensitive CFG");
        ok &= require_(open_text.find("optional.get") != std::string::npos,
                       "open dump must contain optional.get for present-path payload extraction");
        ok &= require_(open_text.find("subview") != std::string::npos,
                       "open dump must contain subview for source range lowering");
        ok &= require_(open_text.find("enum.tag") != std::string::npos,
                       "open dump must contain enum.tag for tag-only enum constructors");
        ok &= require_(open_text.find("term switch") != std::string::npos,
                       "open dump must preserve switch as a first-class terminator");
        if (!ok) return false;

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "placement must support optionals, source subview, and tag-enum switch");
        ok &= require_goir_messages_empty_(placed.messages, "optional/subview/enum placement emitted diagnostics");
        ok &= require_(parus::goir::verify(placed.mod).empty(), "optional/subview/enum placed verify must pass");
        if (!ok) return false;

        const auto placed_text = parus::goir::to_string(placed.mod, &p.prog.types);
        ok &= require_(placed_text.find("call.direct @choose") != std::string::npos,
                       "placed dump must contain direct call after enum-producing semantic invoke placement");
        ok &= require_(placed_text.find("term switch") != std::string::npos,
                       "placed dump must keep switch in the CPU-legal subset");
        return ok;
    }

    static bool test_goir_builder_rejects_string_switch_patterns_() {
        const std::string src = R"(
            @pure def main() -> i32 {
                let msg: text = "hello";
                switch (msg) {
                case "hello": { return 1i32; }
                default: { return 0i32; }
                }
                return 0i32;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "string-switch rejection seed must parse/type-check cleanly");
        ok &= require_(p.ty.errors.empty(), "string-switch rejection seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "string-switch rejection seed must pass SIR capability");
        if (!ok) return false;

        const auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(!open.ok, "gOIR official lane must reject string switch patterns in this round");
        ok &= require_(!open.messages.empty(), "string-switch rejection must produce a diagnostic");
        if (!open.messages.empty()) {
            bool found_expected = false;
            for (const auto& message : open.messages) {
                if (message.msg.find("string switch patterns") != std::string::npos) {
                    found_expected = true;
                    break;
                }
            }
            ok &= require_(found_expected,
                           "string-switch rejection must explain that string patterns are outside the current official lane");
        }
        return ok;
    }

    static bool test_goir_builder_rejects_payload_enum_switch_bindings_() {
        const std::string src = R"(
            enum Job {
                case Empty,
                case Ready(worker: i32),
            };

            @pure def choose(flag: bool) -> Job {
                if (flag) { return Job::Ready(worker: 7i32); }
                return Job::Empty();
            }

            @pure def main(flag: bool) -> i32 {
                let job: Job = choose(flag);
                switch (job) {
                case Job::Ready(worker: w): { return w; }
                default: { return 0i32; }
                }
                return 0i32;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "payload-enum rejection seed must parse/type-check cleanly");
        ok &= require_(p.ty.errors.empty(), "payload-enum rejection seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "payload-enum rejection seed must pass SIR capability");
        if (!ok) return false;

        const auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(!open.ok, "gOIR official lane must reject payload enum switch bindings in this round");
        ok &= require_(!open.messages.empty(), "payload-enum rejection must produce a diagnostic");
        if (!open.messages.empty()) {
            bool found_expected = false;
            for (const auto& message : open.messages) {
                if (message.msg.find("payload enum") != std::string::npos) {
                    found_expected = true;
                    break;
                }
            }
            ok &= require_(found_expected,
                           "payload-enum rejection must explain that payload enum switch bindings are not supported yet");
        }
        return ok;
    }

    static bool test_goir_builder_rejects_extern_reference_lane_escape_() {
        const std::string src = R"(
            extern "C" def c_add(a: i32, b: i32) -> i32;

            @pure def main() -> i32 {
                return 0i32;
            }
        )";

        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "extern rejection seed must parse/type-check cleanly");
        ok &= require_(p.ty.errors.empty(), "extern rejection seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "extern rejection seed must pass SIR capability");
        if (!ok) return false;

        const auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(!open.ok, "gOIR official lane must reject extern functions instead of treating OIR as a fallback");
        ok &= require_(!open.messages.empty(), "extern rejection must produce a diagnostic");
        if (!open.messages.empty()) {
            bool found_expected = false;
            for (const auto& message : open.messages) {
                if (message.msg.find("pure internal CPU entry") != std::string::npos ||
                    message.msg.find("requires pure functions") != std::string::npos) {
                    found_expected = true;
                    break;
                }
            }
            ok &= require_(found_expected,
                           "extern rejection must explain that the official gOIR lane rejects non-pure/non-internal functions");
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
        {"goir_memory_and_record_builder_slice", test_goir_memory_and_record_builder_slice_ok_},
        {"goir_open_and_placed_verify_block_args_and_backedges", test_goir_open_and_placed_verify_block_args_and_backedges_},
        {"goir_open_accepts_explicit_borrow_escape_markers", test_goir_open_accepts_explicit_borrow_escape_markers_},
        {"goir_placement_rejects_explicit_ownership_markers", test_goir_placement_rejects_explicit_ownership_markers_},
        {"goir_text_value_slice", test_goir_text_value_slice_ok_},
        {"goir_optional_subview_enum_switch_slice", test_goir_optional_subview_enum_switch_slice_ok_},
        {"goir_builder_rejects_string_switch_patterns", test_goir_builder_rejects_string_switch_patterns_},
        {"goir_builder_rejects_payload_enum_switch_bindings", test_goir_builder_rejects_payload_enum_switch_bindings_},
        {"goir_builder_rejects_extern_reference_lane_escape", test_goir_builder_rejects_extern_reference_lane_escape_},
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
