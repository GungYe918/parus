#include <parus/backend/aot/AOTBackend.hpp>
#include <parus/backend/aot/LLVMIRLowering.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/oir/Builder.hpp>
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/tyck/TypeCheck.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

    struct ParsedProgram {
        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::diag::Bag bag;
        parus::ast::StmtId root = parus::ast::k_invalid_stmt;
    };

    struct OirPipeline {
        ParsedProgram prog;
        parus::passes::PassResults pres;
        parus::tyck::TyckResult ty;
        parus::sir::Module sir_mod;
        parus::sir::CapabilityAnalysisResult sir_cap;
        parus::oir::BuildResult oir;
    };

    static bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    static ParsedProgram parse_program_(const std::string& src) {
        ParsedProgram p{};
        parus::Lexer lx(src, /*file_id=*/1, &p.bag);
        const auto tokens = lx.lex_all();

        parus::Parser parser(tokens, p.ast, p.types, &p.bag);
        p.root = parser.parse_program();
        return p;
    }

    static std::optional<OirPipeline> build_oir_pipeline_(const std::string& src) {
        OirPipeline out{};
        out.prog = parse_program_(src);

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

        parus::oir::Builder ob(out.sir_mod, out.prog.types);
        out.oir = ob.build();

        if (out.prog.bag.has_error() || !out.ty.errors.empty() || !out.sir_cap.ok || !out.oir.gate_passed) {
            return std::nullopt;
        }

        parus::oir::run_passes(out.oir.mod);
        if (!parus::oir::verify(out.oir.mod).empty()) {
            return std::nullopt;
        }

        return out;
    }

    /// @brief 소스 기반 index lowering이 실제 메모리 주소 계산(getelementptr)을 생성하는지 검사한다.
    static bool test_source_index_lowering_uses_gep() {
        const std::string src = R"(
            fn main() -> i32 {
                let mut x: i32[3] = [1, 2, 3];
                x[1] = 9;
                return x[1];
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "index lowering source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "LLVM text lowering for index case must succeed");
        ok &= require_(lowered.llvm_ir.find("getelementptr i8") != std::string::npos,
                       "index lowering must emit byte-address GEP");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "index assignment must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "index read must emit typed load");
        return ok;
    }

    /// @brief 수동 OIR field 모델이 주소 기반 lowering(getelementptr+load/store)으로 변환되는지 검사한다.
    static bool test_manual_field_lowering_memory_model() {
        parus::ty::TypePool types;
        parus::oir::Module m;

        const auto tid_i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto tid_named = types.intern_ident("Vec2");

        const parus::oir::BlockId entry = m.add_block(parus::oir::Block{});

        parus::oir::Function f{};
        f.name = "main";
        f.ret_ty = tid_i32;
        f.entry = entry;
        f.blocks.push_back(entry);
        (void)m.add_func(f);

        auto add_value = [&](parus::oir::TypeId ty, parus::oir::Effect eff) {
            parus::oir::Value v{};
            v.ty = ty;
            v.eff = eff;
            return m.add_value(v);
        };

        auto emit_inst = [&](const parus::oir::InstData& data, parus::oir::Effect eff, parus::oir::ValueId result) {
            parus::oir::Inst inst{};
            inst.data = data;
            inst.eff = eff;
            inst.result = result;
            const parus::oir::InstId iid = m.add_inst(inst);
            if (result != parus::oir::kInvalidId) {
                m.values[result].def_a = iid;
                m.values[result].def_b = parus::oir::kInvalidId;
            }
            m.blocks[entry].insts.push_back(iid);
        };

        const auto v_slot = add_value(tid_named, parus::oir::Effect::MayWriteMem);
        emit_inst(parus::oir::InstAllocaLocal{tid_named}, parus::oir::Effect::MayWriteMem, v_slot);

        const auto v_c42 = add_value(tid_i32, parus::oir::Effect::Pure);
        emit_inst(parus::oir::InstConstInt{"42"}, parus::oir::Effect::Pure, v_c42);

        const auto v_field = add_value(tid_i32, parus::oir::Effect::MayReadMem);
        emit_inst(parus::oir::InstField{v_slot, "x"}, parus::oir::Effect::MayReadMem, v_field);

        emit_inst(parus::oir::InstStore{v_field, v_c42}, parus::oir::Effect::MayWriteMem, parus::oir::kInvalidId);

        const auto v_load = add_value(tid_i32, parus::oir::Effect::MayReadMem);
        emit_inst(parus::oir::InstLoad{v_field}, parus::oir::Effect::MayReadMem, v_load);

        parus::oir::TermRet rt{};
        rt.has_value = true;
        rt.value = v_load;
        m.blocks[entry].term = rt;
        m.blocks[entry].has_term = true;

        const auto verrs = parus::oir::verify(m);
        bool ok = true;
        ok &= require_(verrs.empty(), "manual field OIR must pass verify");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            m,
            types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "manual field case lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("getelementptr i8") != std::string::npos,
                       "field lowering must emit byte-offset GEP");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "field lowering must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "field lowering must emit typed load");
        return ok;
    }

    /// @brief LLVM API object emission 경로가 동작하거나(툴체인 존재) 명확한 불가 진단을 내는지 검사한다.
    static bool test_object_emission_api_path() {
        const std::string src = R"(
            fn main() -> i32 {
                return 7i32;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "object emission seed must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "object emission seed lowering must succeed");
        if (!ok) return false;

        const auto obj_path = (std::filesystem::temp_directory_path() / "parus_oir_llvm_test.o").string();
        std::error_code ec_rm;
        std::filesystem::remove(obj_path, ec_rm);

        const auto emitted = parus::backend::aot::emit_object_from_llvm_ir_text(
            lowered.llvm_ir,
            obj_path,
            parus::backend::aot::LLVMObjectEmissionOptions{
                .llvm_lane_major = 20,
                .target_triple = "",
                .cpu = "",
                .opt_level = 2
            }
        );

        if (emitted.ok) {
            ok &= require_(std::filesystem::exists(obj_path), "object emission reported success but output file does not exist");
            return ok;
        }

        bool has_expected_msg = false;
        for (const auto& m : emitted.messages) {
            if (!m.is_error) continue;
            if (m.text.find("toolchain") != std::string::npos ||
                m.text.find("LLVM") != std::string::npos ||
                m.text.find("target") != std::string::npos) {
                has_expected_msg = true;
                break;
            }
        }
        ok &= require_(has_expected_msg,
                       "object emission failure must provide an LLVM/toolchain related diagnostic");
        return ok;
    }

    static bool read_text_file_(const std::filesystem::path& p, std::string& out) {
        std::ifstream ifs(p, std::ios::in | std::ios::binary);
        if (!ifs) return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return true;
    }

    /// @brief `tests/oir_cases`의 케이스를 순회하며 OIR->LLVM lowering 경로를 일괄 검증한다.
    static bool test_oir_case_directory() {
#ifndef PARUS_OIR_CASE_DIR
        std::cerr << "  - PARUS_OIR_CASE_DIR is not defined\n";
        return false;
#else
        const std::filesystem::path case_dir{PARUS_OIR_CASE_DIR};
        bool ok = true;

        if (!std::filesystem::exists(case_dir)) {
            std::cerr << "  - OIR case directory does not exist: " << case_dir << "\n";
            return false;
        }

        std::vector<std::filesystem::path> files;
        for (const auto& ent : std::filesystem::directory_iterator(case_dir)) {
            if (!ent.is_regular_file()) continue;
            if (ent.path().extension() == ".pr") files.push_back(ent.path());
        }
        std::sort(files.begin(), files.end());

        ok &= require_(!files.empty(), "OIR case directory must contain at least one .pr case");
        if (!ok) return false;

        for (const auto& p : files) {
            std::string src;
            if (!read_text_file_(p, src)) {
                std::cerr << "  - failed to read OIR case file: " << p.string() << "\n";
                return false;
            }

            auto built = build_oir_pipeline_(src);
            if (!built.has_value()) {
                std::cerr << "  - frontend->OIR failed for OIR case: " << p.filename().string() << "\n";
                return false;
            }

            const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
                built->oir.mod,
                built->prog.types,
                parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
            );
            if (!lowered.ok) {
                std::cerr << "  - OIR->LLVM lowering failed for case: " << p.filename().string() << "\n";
                return false;
            }

            if (lowered.llvm_ir.find("define ") == std::string::npos) {
                std::cerr << "  - lowered LLVM-IR has no define() for case: " << p.filename().string() << "\n";
                return false;
            }
        }

        return true;
#endif
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"source_index_lowering_uses_gep", test_source_index_lowering_uses_gep},
        {"manual_field_lowering_memory_model", test_manual_field_lowering_memory_model},
        {"object_emission_api_path", test_object_emission_api_path},
        {"oir_case_directory", test_oir_case_directory},
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

    std::cout << "ALL OIR->LLVM TESTS PASSED\n";
    return 0;
}
