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
#include <parus/goir/Verify.hpp>
#include <parus/backend/mlir/GOIRMLIRLowering.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef PARUS_TEST_MLIR_LANE
#define PARUS_TEST_MLIR_LANE 22
#endif

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

    static bool require_compile_messages_empty_(const std::vector<parus::backend::CompileMessage>& messages,
                                                const char* label) {
        if (messages.empty()) return true;
        std::cerr << "  - " << label << "\n";
        for (const auto& message : messages) {
            std::cerr << "    * " << message.text << "\n";
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

    static parus::goir::Module make_open_array_get_module_(parus::ty::TypePool& types) {
        parus::goir::Module mod{};
        mod.header.stage_kind = parus::goir::StageKind::Open;

        const auto name = mod.add_string("array_get_main");
        const auto i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto usize_ty = types.builtin(parus::ty::Builtin::kUSize);
        const auto array3 = types.make_array(i32, /*has_size=*/true, /*size=*/3);

        parus::goir::SemanticSig sig{};
        sig.name = name;
        sig.result_type = i32;
        const auto sig_id = mod.add_semantic_sig(sig);

        const auto policy_id = mod.add_placement_policy(parus::goir::GPlacementPolicy{});

        parus::goir::GComputation comp{};
        comp.name = name;
        comp.sig = sig_id;
        comp.placement_policy = policy_id;
        const auto comp_id = mod.add_computation(comp);

        const auto entry = mod.add_block(parus::goir::Block{});

        const auto c0 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"5"},
            parus::goir::LayoutClass::Scalar);
        const auto c1 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"6"},
            parus::goir::LayoutClass::Scalar);
        const auto c2 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"7"},
            parus::goir::LayoutClass::Scalar);

        parus::goir::OpArrayMake make{};
        make.elems = {c0, c1, c2};
        const auto array = add_inst_result_(
            mod, entry, array3, parus::goir::Effect::Pure, std::move(make),
            parus::goir::LayoutClass::FixedArray);

        const auto index = add_inst_result_(
            mod, entry, usize_ty, parus::goir::Effect::Pure, parus::goir::OpConstInt{"1"},
            parus::goir::LayoutClass::Scalar);
        const auto get = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::MayRead,
            parus::goir::OpArrayGet{.base = array, .index = index},
            parus::goir::LayoutClass::Scalar);

        set_term_(mod, entry, parus::goir::TermRet{
            .has_value = true,
            .value = get,
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

    static parus::goir::Module make_open_subview_module_(parus::ty::TypePool& types) {
        parus::goir::Module mod{};
        mod.header.stage_kind = parus::goir::StageKind::Open;

        const auto name = mod.add_string("subview_main");
        const auto array_name = mod.add_string("arr");
        const auto i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto usize_ty = types.builtin(parus::ty::Builtin::kUSize);
        const auto array4 = types.make_array(i32, /*has_size=*/true, /*size=*/4);
        const auto slice = types.make_array(i32, /*has_size=*/false);

        parus::goir::SemanticSig sig{};
        sig.name = name;
        sig.result_type = i32;
        const auto sig_id = mod.add_semantic_sig(sig);

        const auto policy_id = mod.add_placement_policy(parus::goir::GPlacementPolicy{});

        parus::goir::GComputation comp{};
        comp.name = name;
        comp.sig = sig_id;
        comp.placement_policy = policy_id;
        const auto comp_id = mod.add_computation(comp);

        const auto entry = mod.add_block(parus::goir::Block{});

        const auto c10 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"10"},
            parus::goir::LayoutClass::Scalar);
        const auto c20 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"20"},
            parus::goir::LayoutClass::Scalar);
        const auto c30 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"30"},
            parus::goir::LayoutClass::Scalar);
        const auto c40 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure, parus::goir::OpConstInt{"40"},
            parus::goir::LayoutClass::Scalar);

        parus::goir::OpArrayMake make{};
        make.elems = {c10, c20, c30, c40};
        const auto array = add_inst_result_(
            mod, entry, array4, parus::goir::Effect::Pure, std::move(make),
            parus::goir::LayoutClass::FixedArray);

        const auto slot = add_inst_result_(
            mod, entry, array4, parus::goir::Effect::Pure,
            parus::goir::OpLocalSlot{.debug_name = array_name},
            parus::goir::LayoutClass::FixedArray,
            true,
            array4,
            parus::goir::PlaceKind::LocalSlot,
            true
        );
        (void)add_inst_result_(
            mod, entry, parus::goir::kInvalidType, parus::goir::Effect::MayWrite,
            parus::goir::OpStore{.place = slot, .value = array}
        );

        const auto offset = add_inst_result_(
            mod, entry, usize_ty, parus::goir::Effect::Pure, parus::goir::OpConstInt{"1"},
            parus::goir::LayoutClass::Scalar);
        const auto length = add_inst_result_(
            mod, entry, usize_ty, parus::goir::Effect::Pure, parus::goir::OpConstInt{"2"},
            parus::goir::LayoutClass::Scalar);
        const auto zero = add_inst_result_(
            mod, entry, usize_ty, parus::goir::Effect::Pure, parus::goir::OpConstInt{"0"},
            parus::goir::LayoutClass::Scalar);

        const auto sub = add_inst_result_(
            mod, entry, slice, parus::goir::Effect::Pure,
            parus::goir::OpSubView{.base = slot, .offset = offset, .length = length},
            parus::goir::LayoutClass::SliceView,
            true,
            slice,
            parus::goir::PlaceKind::SubView,
            true
        );

        const auto first_place = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure,
            parus::goir::OpIndexPlace{.base = sub, .index = zero},
            parus::goir::LayoutClass::Scalar,
            true,
            i32,
            parus::goir::PlaceKind::IndexPath,
            true
        );
        const auto first = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::MayRead,
            parus::goir::OpLoad{.place = first_place},
            parus::goir::LayoutClass::Scalar
        );
        const auto len = add_inst_result_(
            mod, entry, usize_ty, parus::goir::Effect::Pure,
            parus::goir::OpArrayLen{.base = sub},
            parus::goir::LayoutClass::Scalar
        );
        const auto len_i32 = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure,
            parus::goir::OpCast{.kind = parus::goir::CastKind::As, .to = i32, .src = len},
            parus::goir::LayoutClass::Scalar
        );
        const auto sum = add_inst_result_(
            mod, entry, i32, parus::goir::Effect::Pure,
            parus::goir::OpBinary{.op = parus::goir::BinOp::Add, .lhs = first, .rhs = len_i32},
            parus::goir::LayoutClass::Scalar
        );

        set_term_(mod, entry, parus::goir::TermRet{
            .has_value = true,
            .value = sum,
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

    static parus::goir::Module make_open_ownership_marker_module_(parus::ty::TypePool& types) {
        parus::goir::Module mod{};
        mod.header.stage_kind = parus::goir::StageKind::Open;

        const auto name = mod.add_string("ownership_main");
        const auto seed_name = mod.add_string("seed");
        const auto i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto borrow_i32 = types.make_borrow(i32, /*is_mut=*/false);

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
        (void)add_inst_result_(
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

    static bool build_placed_goir_from_source_(const std::string& src,
                                               parus::goir::Module& out_mod,
                                               parus::ty::TypePool& out_types) {
        auto p = build_sir_pipeline_(src);
        bool ok = true;
        ok &= require_(!p.prog.bag.has_error(), "MLIR source seed must not emit diagnostics");
        ok &= require_(p.ty.errors.empty(), "MLIR source seed must not emit tyck errors");
        ok &= require_(p.sir_cap.ok, "MLIR source seed must pass SIR capability");
        if (!ok) return false;

        auto open = parus::goir::build_from_sir(p.sir_mod, p.prog.types);
        ok &= require_(open.ok, "gOIR open-stage build must succeed before MLIR lowering");
        ok &= require_goir_messages_empty_(open.messages, "open-stage build emitted diagnostics before MLIR lowering");
        ok &= require_(parus::goir::verify(open.mod).empty(), "open-stage verify must pass before MLIR lowering");
        if (!ok) return false;

        auto placed = parus::goir::place_module(open.mod);
        ok &= require_(placed.ok, "gOIR placement must succeed before MLIR lowering");
        ok &= require_goir_messages_empty_(placed.messages, "placement emitted diagnostics before MLIR lowering");
        ok &= require_(parus::goir::verify(placed.mod).empty(), "placed-stage verify must pass before MLIR lowering");
        if (!ok) return false;

        out_mod = std::move(placed.mod);
        out_types = std::move(p.prog.types);
        return true;
    }

    static bool lower_placed_to_mlir_and_llvm_(const parus::goir::Module& placed,
                                               const parus::ty::TypePool& types,
                                               parus::backend::mlir::GOIRLoweringResult& mlir,
                                               parus::backend::mlir::GOIRLLVMIRResult& llvm_ir) {
        mlir = parus::backend::mlir::lower_goir_to_mlir_text(placed, types);
        bool ok = true;
        ok &= require_(mlir.ok, "gOIR placed module must lower to MLIR text");
        ok &= require_compile_messages_empty_(mlir.messages, "MLIR lowering emitted diagnostics");
        if (!ok) return false;

        llvm_ir = parus::backend::mlir::lower_goir_to_llvm_ir_text(
            placed,
            types,
            parus::backend::mlir::GOIRLoweringOptions{.llvm_lane_major = PARUS_TEST_MLIR_LANE}
        );
        ok &= require_(llvm_ir.ok, "gOIR placed module must lower to LLVM IR text");
        ok &= require_compile_messages_empty_(llvm_ir.messages, "LLVM IR lowering emitted diagnostics");
        return ok;
    }

    static bool emit_object_from_placed_goir_(const parus::goir::Module& placed,
                                              const parus::ty::TypePool& types,
                                              const std::string& stem,
                                              parus::backend::mlir::GOIRObjectEmissionResult& out) {
        const auto out_path = (std::filesystem::temp_directory_path() / ("parus_goir_mlir_" + stem + ".o")).string();
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        out = parus::backend::mlir::emit_object_from_goir_via_mlir(
            placed,
            types,
            parus::backend::mlir::GOIRLoweringOptions{.llvm_lane_major = PARUS_TEST_MLIR_LANE},
            out_path,
            "",
            "",
            2
        );

        bool ok = true;
        ok &= require_(out.ok, "gOIR placed module must emit an object through the MLIR lane");
        if (!out.ok) {
            ok &= require_compile_messages_empty_(out.messages, "gOIR object emission emitted diagnostics");
        }
        ok &= require_(std::filesystem::exists(out_path), "MLIR object emission reported success but the object file is missing");
        return ok;
    }

    static bool test_mlir_smoke_ok_() {
        std::string error{};
        const bool ok = parus::backend::mlir::run_mlir_smoke(&error);
        if (!ok) std::cerr << "  - " << error << "\n";
        return ok;
    }

    static bool test_source_memory_record_slice_to_mlir_and_llvm_ir_ok_() {
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

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("func.func @main") != std::string::npos,
                       "MLIR text must contain main function");
        ok &= require_(mlir.mlir_text.find("cf.cond_br") != std::string::npos,
                       "MLIR text must contain CFG branch lowering");
        ok &= require_(mlir.mlir_text.find("memref.alloca") != std::string::npos,
                       "MLIR text must contain memref.alloca for mutable local storage");
        ok &= require_(mlir.mlir_text.find("memref.load") != std::string::npos,
                       "MLIR text must contain memref.load for place reads");
        ok &= require_(mlir.mlir_text.find("memref.store") != std::string::npos,
                       "MLIR text must contain memref.store for mutable writes");
        ok &= require_(mlir.mlir_text.find("llvm.alloca") != std::string::npos,
                       "MLIR text must contain llvm.alloca for plain-record local storage");
        ok &= require_(mlir.mlir_text.find("llvm.getelementptr") != std::string::npos,
                       "MLIR text must contain llvm.getelementptr for field places");
        ok &= require_(mlir.mlir_text.find("func.call @add") != std::string::npos,
                       "MLIR text must contain direct pure/internal call lowering");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("define i32 @main") != std::string::npos,
                       "LLVM IR must contain main definition");
        ok &= require_(llvm_ir.llvm_ir.find("call i32 @add") != std::string::npos,
                       "LLVM IR must contain direct add call");
        ok &= require_(llvm_ir.llvm_ir.find("alloca") != std::string::npos,
                       "LLVM IR must contain stack allocation for the first memory/aggregate slice");
        return ok;
    }

    static bool test_source_text_value_to_mlir_and_llvm_ir_ok_() {
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

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("llvm.mlir.global internal constant @__parus_text_") != std::string::npos,
                       "MLIR text must materialize text literals as LLVM globals");
        ok &= require_(mlir.mlir_text.find("func.call @pick") != std::string::npos,
                       "MLIR text must support direct calls with text-view values");
        ok &= require_(mlir.mlir_text.find("!llvm.struct<(!llvm.ptr, i64)>") != std::string::npos,
                       "MLIR text must use the canonical text-view LLVM aggregate");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("@__parus_text_") != std::string::npos,
                       "LLVM IR must contain lowered text globals");
        ok &= require_(llvm_ir.llvm_ir.find("define { ptr, i64 } @main") != std::string::npos ||
                       llvm_ir.llvm_ir.find("define {ptr, i64} @main") != std::string::npos,
                       "LLVM IR must expose text-view functions as struct returns");
        return ok;
    }

    static bool test_source_optional_coalesce_to_mlir_and_llvm_ir_ok_() {
        const std::string src = R"(
            @pure def takes_opt(x: i32?) -> i32 {
                return x ?? 99i32;
            }

            @pure def ret_opt() -> i32? {
                return 9i32;
            }

            @pure def main() -> i32 {
                let a: i32? = 5;
                let mut b: i32? = null;
                b = 7;
                let c: i32 = takes_opt(3);
                let d: i32? = ret_opt();
                let e: i32 = d ?? 0i32;
                return (a ?? 0i32) + (b ?? 0i32) + c + e;
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("llvm.alloca") != std::string::npos,
                       "MLIR text must use llvm.alloca for optional local storage");
        ok &= require_(mlir.mlir_text.find("llvm.load") != std::string::npos,
                       "MLIR text must load optional values from LLVM-backed storage");
        ok &= require_(mlir.mlir_text.find("llvm.store") != std::string::npos,
                       "MLIR text must store optional values through LLVM-backed storage");
        ok &= require_(mlir.mlir_text.find("llvm.extractvalue") != std::string::npos,
                       "MLIR text must extract optional tag/payload fields");
        ok &= require_(mlir.mlir_text.find("llvm.insertvalue") != std::string::npos,
                       "MLIR text must construct optional values as LLVM aggregates");
        ok &= require_(mlir.mlir_text.find("cf.cond_br") != std::string::npos,
                       "MLIR text must lower null-coalescing through CFG");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("extractvalue") != std::string::npos,
                       "LLVM IR must preserve optional tag/payload extraction");
        return ok;
    }

    static bool test_source_range_subview_to_mlir_and_llvm_ir_ok_() {
        const std::string src = R"(
            @pure def main() -> i32 {
                let arr: i32[4] = [10i32, 20i32, 30i32, 40i32];
                let mid: i32[] = arr[1i32..3i32];
                return mid[0i32] + mid[1i32];
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("memref.subview") != std::string::npos,
                       "MLIR text must lower source range slicing to memref.subview");
        ok &= require_(mlir.mlir_text.find("memref.load") != std::string::npos,
                       "MLIR text must read from the lowered subview");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("define i32 @main") != std::string::npos,
                       "LLVM IR must contain the source-range subview definition");
        return ok;
    }

    static bool test_source_tag_enum_switch_to_mlir_and_llvm_ir_ok_() {
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
                let e: E = choose(flag);
                switch (e) {
                case E::A: { return 11i32; }
                case E::B: { return 22i32; }
                }
                return 0i32;
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("cf.switch") != std::string::npos,
                       "MLIR text must lower tag-only enum switch through cf.switch");
        ok &= require_(mlir.mlir_text.find("func.call @choose") != std::string::npos,
                       "MLIR text must lower enum-producing direct calls");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("switch i32") != std::string::npos,
                       "LLVM IR must lower tag-only enum switch to an integer switch");
        return ok;
    }

    static bool test_manual_block_arg_cfg_to_mlir_and_llvm_ir_ok_() {
        parus::ty::TypePool types{};
        const auto open = make_open_loop_block_arg_module_(types);
        const auto placed = parus::goir::place_module(open);
        bool ok = true;
        ok &= require_(placed.ok, "placement must succeed for manual block-arg CFG module");
        ok &= require_goir_messages_empty_(placed.messages, "manual block-arg placement emitted diagnostics");
        if (!ok) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed.mod, types, mlir, llvm_ir)) return false;

        ok &= require_(mlir.mlir_text.find("^bb") != std::string::npos,
                       "MLIR text must contain explicit non-entry block labels");
        ok &= require_(mlir.mlir_text.find("cf.br ^bb") != std::string::npos,
                       "MLIR text must contain branch arguments on cf.br");
        ok &= require_(mlir.mlir_text.find("cf.cond_br") != std::string::npos,
                       "MLIR text must contain conditional branch lowering for loop CFG");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("phi i32") != std::string::npos,
                       "LLVM IR must materialize a phi for block-argument loop state");
        return ok;
    }

    static bool test_manual_array_make_get_to_mlir_and_llvm_ir_ok_() {
        parus::ty::TypePool types{};
        const auto open = make_open_array_get_module_(types);
        const auto placed = parus::goir::place_module(open);
        bool ok = true;
        ok &= require_(placed.ok, "placement must succeed for manual array.make/array.get module");
        ok &= require_goir_messages_empty_(placed.messages, "array.make/array.get placement emitted diagnostics");
        if (!ok) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed.mod, types, mlir, llvm_ir)) return false;

        ok &= require_(mlir.mlir_text.find("memref.alloca") != std::string::npos,
                       "array.get lowering must materialize a temporary memref for array.make");
        ok &= require_(mlir.mlir_text.find("memref.load") != std::string::npos,
                       "array.get lowering must read back through memref.load");
        ok &= require_(llvm_ir.llvm_ir.find("define i32 @array_get_main") != std::string::npos,
                       "LLVM IR must contain array_get_main definition");
        return ok;
    }

    static bool test_manual_subview_len_to_mlir_and_llvm_ir_ok_() {
        parus::ty::TypePool types{};
        const auto open = make_open_subview_module_(types);
        const auto placed = parus::goir::place_module(open);
        bool ok = true;
        ok &= require_(placed.ok, "placement must succeed for manual subview/len module");
        ok &= require_goir_messages_empty_(placed.messages, "subview placement emitted diagnostics");
        if (!ok) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed.mod, types, mlir, llvm_ir)) return false;

        ok &= require_(mlir.mlir_text.find("memref.subview") != std::string::npos,
                       "MLIR text must contain memref.subview for slice-view lowering");
        ok &= require_(mlir.mlir_text.find("memref.dim") != std::string::npos,
                       "MLIR text must contain memref.dim for slice len lowering");
        ok &= require_(mlir.mlir_text.find("memref.load") != std::string::npos,
                       "MLIR text must contain memref.load on the subview");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("define i32 @subview_main") != std::string::npos,
                       "LLVM IR must contain subview_main definition");
        return ok;
    }

    static bool test_source_host_abi_global_and_export_to_mlir_and_llvm_ir_ok_() {
        const std::string src = R"(
            struct layout(c) align(16) Vec2 {
                x: i32;
                y: i32;
            };

            extern "C" static mut g_vec: Vec2;

            export "C" def probe() -> i32 {
                g_vec.x = 7i32;
                return g_vec.x;
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("llvm.mlir.global external @g_vec() : !llvm.array<16 x i8>") != std::string::npos,
                       "MLIR text must expose layout(c) extern globals as byte aggregates");
        ok &= require_(mlir.mlir_text.find("llvm.mlir.addressof @g_vec") != std::string::npos,
                       "MLIR text must materialize extern global addresses");
        ok &= require_(mlir.mlir_text.find("llvm.getelementptr") != std::string::npos,
                       "MLIR text must lower global field chains through llvm.getelementptr");
        ok &= require_(mlir.mlir_text.find("func.func @probe") != std::string::npos,
                       "MLIR text must keep export C definitions as explicit symbols");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("@g_vec = external global [16 x i8], align 16") != std::string::npos,
                       "LLVM IR must emit the extern layout(c) global with byte storage and ABI alignment");
        ok &= require_(llvm_ir.llvm_ir.find("define i32 @probe(") != std::string::npos,
                       "LLVM IR must keep the unmangled export symbol");
        ok &= require_(llvm_ir.llvm_ir.find("store i32") != std::string::npos,
                       "LLVM IR must emit typed stores for global field writes");
        ok &= require_(llvm_ir.llvm_ir.find("load i32") != std::string::npos,
                       "LLVM IR must emit typed loads for global field reads");
        return ok;
    }

    static bool test_source_c_abi_by_value_record_call_to_mlir_and_llvm_ir_ok_() {
        const std::string src = R"(
            struct layout(c) Vec2 {
                x: i32;
                y: i32;
            };

            extern "C" def takes(v: Vec2) -> i32;

            export "C" def pass(v: Vec2) -> i32 {
                return takes(v);
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("llvm.func @takes(!llvm.array<8 x i8>) -> i32") != std::string::npos,
                       "MLIR text must declare extern C by-value records as byte aggregates");
        ok &= require_(mlir.mlir_text.find("func.func @pass(") != std::string::npos &&
                       mlir.mlir_text.find("!llvm.array<8 x i8>") != std::string::npos,
                       "MLIR text must keep export C by-value parameters in aggregate form");
        ok &= require_(mlir.mlir_text.find("llvm.call @takes(") != std::string::npos &&
                       mlir.mlir_text.find("(!llvm.array<8 x i8>) -> i32") != std::string::npos,
                       "MLIR text must lower extern C calls through llvm.call with aggregate args");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("declare i32 @takes([8 x i8])") != std::string::npos,
                       "LLVM IR must expose extern C by-value records as aggregate parameters");
        ok &= require_(llvm_ir.llvm_ir.find("define i32 @pass([8 x i8]") != std::string::npos,
                       "LLVM IR must preserve aggregate export signatures");
        ok &= require_(llvm_ir.llvm_ir.find("call i32 @takes([8 x i8]") != std::string::npos,
                       "LLVM IR must pass C ABI records by value, not by pointer");
        return ok;
    }

    static bool test_source_c_abi_callconv_metadata_to_llvm_ir_ok_() {
        const std::string src = R"(
            extern "C" def c_fn(x: i32) -> i32;

            def main() -> i32 {
                return c_fn(7i32);
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        for (auto& real : placed.realizations) {
            const auto name = std::string(
                real.link_name != parus::goir::kInvalidId
                    ? placed.string(real.link_name)
                    : placed.string(real.name)
            );
            if (name == "c_fn") {
                real.c_callconv = parus::sir::CCallConv::kSysV;
            }
        }

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(llvm_ir.llvm_ir.find("declare x86_64_sysvcc i32 @c_fn(i32)") != std::string::npos,
                       "LLVM IR must preserve patched host callconv metadata on extern declarations");
        ok &= require_(llvm_ir.llvm_ir.find("call x86_64_sysvcc i32 @c_fn(i32") != std::string::npos,
                       "LLVM IR must preserve patched host callconv metadata on call sites");
        return ok;
    }

    static bool test_source_static_globals_object_ready_via_mlir_ok_() {
        const std::string src = R"(
            static const G_CONST: i32 = 7i32;
            static G_MUT: i32 = 9i32;

            def main() -> i32 {
                return G_CONST + G_MUT;
            }
        )";

        parus::goir::Module placed{};
        parus::ty::TypePool types{};
        if (!build_placed_goir_from_source_(src, placed, types)) return false;

        parus::backend::mlir::GOIRLoweringResult mlir{};
        parus::backend::mlir::GOIRLLVMIRResult llvm_ir{};
        if (!lower_placed_to_mlir_and_llvm_(placed, types, mlir, llvm_ir)) return false;

        bool ok = true;
        ok &= require_(mlir.mlir_text.find("llvm.mlir.global internal constant @G_CONST() : i32") != std::string::npos,
                       "MLIR text must emit static const globals as LLVM globals");
        ok &= require_(mlir.mlir_text.find("llvm.mlir.global internal @G_MUT() : i32") != std::string::npos,
                       "MLIR text must emit static mutable globals as zero-init LLVM globals");
        ok &= require_(mlir.mlir_text.find("func.func @__parus_goir_module_init()") != std::string::npos,
                       "MLIR text must emit the synthetic static-global init function");
        if (!ok) return false;

        ok &= require_(llvm_ir.llvm_ir.find("constant i32 7") != std::string::npos,
                       "LLVM IR must emit static const globals as constants");
        ok &= require_(llvm_ir.llvm_ir.find("define void @__parus_goir_module_init()") != std::string::npos,
                       "LLVM IR must emit the synthetic module-init function symbol");
        ok &= require_(llvm_ir.llvm_ir.find("store i32 9, ptr @G_MUT") != std::string::npos,
                       "LLVM IR must initialize static mutable globals through explicit stores");
        if (!ok) return false;

        parus::backend::mlir::GOIRObjectEmissionResult obj{};
        ok &= emit_object_from_placed_goir_(placed, types, "host_abi_object_ready", obj);
        return ok;
    }

    static bool test_ownership_sensitive_failure_stays_out_of_mlir_lane_() {
        parus::ty::TypePool types{};
        const auto open = make_open_ownership_marker_module_(types);

        const auto placed = parus::goir::place_module(open);
        bool ok = true;
        ok &= require_(!placed.ok, "ownership-sensitive gOIR must fail at placement before MLIR lowering");
        ok &= require_(!placed.messages.empty(), "ownership-sensitive placement failure must produce a diagnostic");
        if (!placed.messages.empty()) {
            ok &= require_(placed.messages.front().msg.find("ownership-sensitive") != std::string::npos,
                           "ownership-sensitive placement failure must explain the missing runtime lowering");
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
        {"mlir_smoke", test_mlir_smoke_ok_},
        {"source_memory_record_slice_to_mlir_and_llvm_ir", test_source_memory_record_slice_to_mlir_and_llvm_ir_ok_},
        {"source_text_value_to_mlir_and_llvm_ir", test_source_text_value_to_mlir_and_llvm_ir_ok_},
        {"source_optional_coalesce_to_mlir_and_llvm_ir", test_source_optional_coalesce_to_mlir_and_llvm_ir_ok_},
        {"source_range_subview_to_mlir_and_llvm_ir", test_source_range_subview_to_mlir_and_llvm_ir_ok_},
        {"source_tag_enum_switch_to_mlir_and_llvm_ir", test_source_tag_enum_switch_to_mlir_and_llvm_ir_ok_},
        {"manual_block_arg_cfg_to_mlir_and_llvm_ir", test_manual_block_arg_cfg_to_mlir_and_llvm_ir_ok_},
        {"manual_array_make_get_to_mlir_and_llvm_ir", test_manual_array_make_get_to_mlir_and_llvm_ir_ok_},
        {"manual_subview_len_to_mlir_and_llvm_ir", test_manual_subview_len_to_mlir_and_llvm_ir_ok_},
        {"source_host_abi_global_and_export_to_mlir_and_llvm_ir", test_source_host_abi_global_and_export_to_mlir_and_llvm_ir_ok_},
        {"source_c_abi_by_value_record_call_to_mlir_and_llvm_ir", test_source_c_abi_by_value_record_call_to_mlir_and_llvm_ir_ok_},
        {"source_c_abi_callconv_metadata_to_llvm_ir", test_source_c_abi_callconv_metadata_to_llvm_ir_ok_},
        {"source_static_globals_object_ready_via_mlir", test_source_static_globals_object_ready_via_mlir_ok_},
        {"ownership_sensitive_failure_stays_out_of_mlir_lane", test_ownership_sensitive_failure_stays_out_of_mlir_lane_},
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
