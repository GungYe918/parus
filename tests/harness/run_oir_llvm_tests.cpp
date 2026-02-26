#include <parus/backend/aot/AOTBackend.hpp>
#include <parus/backend/aot/LLVMIRLowering.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/oir/Builder.hpp>
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/type/TypeResolve.hpp>
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
        parus::type::TypeResolveResult type_resolve{};
        bool macro_type_ready = false;
        bool macro_type_ok = false;
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

    static std::optional<OirPipeline> build_oir_pipeline_(const std::string& src) {
        OirPipeline out{};
        out.prog = parse_program_(src);
        if (!run_macro_and_type_(out.prog)) return std::nullopt;

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
            def main() -> i32 {
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

    /// @brief layout(c)/align(n) field + C ABI global이 LLVM-IR에 반영되는지 검사한다.
    static bool test_c_abi_field_layout_and_global_symbol() {
        const std::string src = R"(
            field layout(c) align(16) Vec2 {
                x: f32;
                y: f32;
            }

            extern "C" static mut g_vec: Vec2;

            export "C" def probe() -> i32 {
                return 0i32;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "C ABI field/global source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "LLVM text lowering for C ABI field/global case must succeed");
        ok &= require_(lowered.llvm_ir.find("@g_vec = external global [16 x i8], align 16") != std::string::npos,
                       "C ABI global symbol must be emitted as external global with layout(c) align");
        ok &= require_(lowered.llvm_ir.find("define i32 @probe(") != std::string::npos,
                       "export \"C\" function probe must keep unmangled symbol");
        return ok;
    }

    /// @brief C ABI layout(c) field by-value 파라미터가 LLVM 시그니처에서 ptr이 아닌 aggregate로 내려가는지 검사한다.
    static bool test_c_abi_field_by_value_param_signature() {
        const std::string src = R"(
            field layout(c) Vec2 {
                x: i32;
                y: i32;
            }

            extern "C" def takes(v: Vec2) -> i32;

            export "C" def pass(v: Vec2) -> i32 {
                return takes(v: v);
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "C ABI by-value field source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "LLVM text lowering for C ABI by-value field case must succeed");
        ok &= require_(lowered.llvm_ir.find("declare i32 @takes([8 x i8])") != std::string::npos,
                       "extern \"C\" field by-value parameter must be emitted as aggregate signature");
        ok &= require_(lowered.llvm_ir.find("define i32 @pass([8 x i8] %arg0)") != std::string::npos,
                       "export \"C\" field by-value parameter must be emitted as aggregate signature");
        ok &= require_(lowered.llvm_ir.find("call i32 @takes([8 x i8]") != std::string::npos,
                       "C ABI by-value call must pass aggregate argument, not ptr");
        return ok;
    }

    /// @brief text/문자열 리터럴이 OIR->LLVM에서 rodata 상수 + `{ptr,len}` ABI로 내려가는지 검사한다.
    static bool test_text_literal_rodata_and_c_abi_span_signature() {
        const std::string src = R"(
            extern "C" def sink(msg: text) -> i32;

            def main() -> i32 {
                sink(msg: "A\nB");
                sink(msg: R"""A\nB""");
                sink(msg: F"""A{1}B""");
                return 0i32;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "text literal source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        size_t text_const_count = 0;
        {
            std::string_view hay = lowered.llvm_ir;
            constexpr std::string_view needle = "@.parus_text.";
            size_t pos = 0;
            while (true) {
                const size_t found = hay.find(needle, pos);
                if (found == std::string_view::npos) break;
                ++text_const_count;
                pos = found + needle.size();
            }
        }

        ok &= require_(lowered.ok, "LLVM text lowering for text literal case must succeed");
        ok &= require_(lowered.llvm_ir.find("declare i32 @sink({ ptr, i64 })") != std::string::npos,
                       "extern \"C\" text parameter must be emitted as `{ptr,i64}` aggregate");
        ok &= require_(text_const_count >= 3,
                       "three string literals must be emitted as rodata constants");
        ok &= require_(lowered.llvm_ir.find("A\\0AB\\00") != std::string::npos,
                       "escaped normal string must contain decoded newline byte (0x0A)");
        ok &= require_(lowered.llvm_ir.find("A\\5CnB\\00") != std::string::npos,
                       "raw string must preserve backslash+n byte sequence");
        ok &= require_(lowered.llvm_ir.find("A1B\\00") != std::string::npos,
                       "format triple string must be folded at compile time");
        ok &= require_(lowered.llvm_ir.find("malloc") == std::string::npos,
                       "text literal lowering must not introduce heap allocation calls");
        return ok;
    }

    /// @brief float/char 리터럴이 OIR/LLVM 경로에서 ConstNull로 대체되지 않고 정상 lowering되는지 검사한다.
    static bool test_float_char_literal_lowering_() {
        const std::string src = R"(
            def sinkf(v: f64) -> i32 {
                if (v > 0.5f64) {
                    return 1i32;
                }
                return 0i32;
            }

            def sinkc(v: char) -> i32 {
                if (v == 'A') {
                    return 1i32;
                }
                return 0i32;
            }

            def main() -> i32 {
                return sinkf(v: 1.25f64) + sinkc(v: 'A');
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "float/char literal source must pass frontend->OIR pipeline");
        if (!ok) return false;

        bool saw_float = false;
        bool saw_char = false;
        bool saw_null = false;
        for (const auto& inst : p->oir.mod.insts) {
            if (std::holds_alternative<parus::oir::InstConstFloat>(inst.data)) saw_float = true;
            if (std::holds_alternative<parus::oir::InstConstChar>(inst.data)) saw_char = true;
            if (std::holds_alternative<parus::oir::InstConstNull>(inst.data)) saw_null = true;
        }
        ok &= require_(saw_float, "OIR must contain InstConstFloat for float literal");
        ok &= require_(saw_char, "OIR must contain InstConstChar for char literal");

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "LLVM text lowering for float/char literal case must succeed");
        ok &= require_(lowered.llvm_ir.find("fcmp") != std::string::npos,
                       "LLVM IR must include float comparison path");
        ok &= require_(lowered.llvm_ir.find("icmp eq i32") != std::string::npos,
                       "LLVM IR must include char comparison path");
        ok &= require_(!(saw_null && !saw_float && !saw_char),
                       "float/char literals must not silently degrade into ConstNull");
        return ok;
    }

    /// @brief 수동 OIR field 모델이 주소 기반 lowering(getelementptr+load/store)으로 변환되는지 검사한다.
    static bool test_manual_field_lowering_memory_model() {
        parus::ty::TypePool types;
        parus::oir::Module m;

        const auto tid_i32 = types.builtin(parus::ty::Builtin::kI32);
        const auto tid_named = types.intern_ident("Vec2");

        parus::oir::FieldLayoutDecl vec2_layout{};
        vec2_layout.name = "Vec2";
        vec2_layout.self_type = tid_named;
        vec2_layout.layout = parus::oir::FieldLayout::C;
        vec2_layout.align = 16;
        vec2_layout.size = 16;
        vec2_layout.members.push_back(parus::oir::FieldMemberLayout{"x", tid_i32, 0});
        vec2_layout.members.push_back(parus::oir::FieldMemberLayout{"y", tid_i32, 4});
        (void)m.add_field(vec2_layout);

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
        emit_inst(parus::oir::InstField{v_slot, "y"}, parus::oir::Effect::MayReadMem, v_field);

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
        ok &= require_(lowered.llvm_ir.find("getelementptr i8, ptr") != std::string::npos,
                       "field lowering must emit byte-offset GEP");
        ok &= require_(lowered.llvm_ir.find("i64 4") != std::string::npos,
                       "field lowering must use ABI metadata offset (y=4)");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "field lowering must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "field lowering must emit typed load");
        return ok;
    }

    /// @brief LLVM API object emission 경로가 동작하거나(툴체인 존재) 명확한 불가 진단을 내는지 검사한다.
    static bool test_object_emission_api_path() {
        const std::string src = R"(
            def main() -> i32 {
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

    static size_t count_substr_(std::string_view haystack, std::string_view needle) {
        if (needle.empty()) return 0;
        size_t n = 0;
        size_t pos = 0;
        while (true) {
            const size_t found = haystack.find(needle, pos);
            if (found == std::string_view::npos) break;
            ++n;
            pos = found + needle.size();
        }
        return n;
    }

    /// @brief LLVM-IR 텍스트를 실제 오브젝트(.o)로 만들어 코드 생성 경로를 검증한다.
    static bool emit_object_for_test_case_(const std::string& llvm_ir, const std::string& stem) {
        const auto out_path = (std::filesystem::temp_directory_path() / ("parus_oir_llvm_" + stem + ".o")).string();
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        const auto emitted = parus::backend::aot::emit_object_from_llvm_ir_text(
            llvm_ir,
            out_path,
            parus::backend::aot::LLVMObjectEmissionOptions{
                .llvm_lane_major = 20,
                .target_triple = "",
                .cpu = "",
                .opt_level = 2
            }
        );

        bool ok = true;
        ok &= require_(emitted.ok, "LLVM object emission must succeed for test case");
        if (!emitted.ok) {
            for (const auto& m : emitted.messages) {
                if (m.is_error) {
                    std::cerr << "    object emission error: " << m.text << "\n";
                }
            }
            return false;
        }
        ok &= require_(std::filesystem::exists(out_path), "object emission reported success but output file does not exist");
        return ok;
    }

    /// @brief 함수 오버로딩 + 연산자 오버로딩(acts for) 경로가 올바른 LLVM 호출로 내려가는지 검사한다.
    static bool test_overload_and_operator_lowering_patterns_() {
        const std::string src = R"(
            field I32Box {
                v: i32;
            };

            acts for I32Box {
                operator(+)(self move, rhs: I32Box) -> i32 {
                    return self.v + rhs.v;
                }
            };

            def add(a: i32, b: i32) -> i32 {
                return a + b;
            }

            def add(a: i64, b: i64) -> i64 {
                return a + b;
            }

            def main() -> i32 {
                let x: i32 = add(a: 1i32, b: 2i32);
                let y: i64 = add(a: 3i64, b: 4i64);
                let z: i32 = I32Box { v: 10i32 } + I32Box { v: 20i32 };
                return x + z;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "overload/operator case must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "overload/operator LLVM text lowering must succeed");
        if (!ok) return false;

        ok &= require_(lowered.llvm_ir.find("define i32 @p$main$_$add$Mnone$Rnone$S") != std::string::npos,
                       "i32 overload definition must exist in LLVM-IR");
        ok &= require_(lowered.llvm_ir.find("define i64 @p$main$_$add$Mnone$Rnone$S") != std::string::npos,
                       "i64 overload definition must exist in LLVM-IR");
        ok &= require_(lowered.llvm_ir.find("call i32 @p$main$_$add$Mnone$Rnone$S") != std::string::npos,
                       "i32 overload call must be direct in LLVM-IR");
        ok &= require_(lowered.llvm_ir.find("call i64 @p$main$_$add$Mnone$Rnone$S") != std::string::npos,
                       "i64 overload call must be direct in LLVM-IR");
        const bool has_operator_def =
            lowered.llvm_ir.find("define i32 @p$main$__acts_") != std::string::npos ||
            lowered.llvm_ir.find("define i32 @p$main$_$__op_") != std::string::npos;
        const bool has_operator_call =
            lowered.llvm_ir.find("call i32 @p$main$__acts_") != std::string::npos ||
            lowered.llvm_ir.find("call i32 @p$main$_$__op_") != std::string::npos;
        ok &= require_(has_operator_def, "operator overload function must be present in LLVM-IR");
        ok &= require_(has_operator_call, "operator overload must lower to direct call");
        ok &= require_(lowered.llvm_ir.find("@parus_oir_call_stub") == std::string::npos,
                       "direct overload lowering should not require call stub");
        ok &= require_(lowered.llvm_ir.find("add i64") != std::string::npos,
                       "non-overloaded i64 arithmetic path should remain hot binop in LLVM-IR");
        if (!ok) return false;

        return emit_object_for_test_case_(lowered.llvm_ir, "overload_operator_patterns");
    }

    /// @brief nest 경로 함수가 namespace 포함 맹글 심볼로 내려가고 direct call되는지 검사한다.
    static bool test_nest_path_mangling_and_direct_call_() {
        const std::string src = R"(
            nest engine {
                nest math {
                    def add(a: i32, b: i32) -> i32 {
                        return a + b;
                    }
                }
            }

            def main() -> i32 {
                return engine::math::add(a: 1i32, b: 2i32);
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "nest path source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "nest path LLVM text lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("define i32 @p$main$engine__math$add$Mnone$Rnone$S") != std::string::npos,
                       "nested function must include namespace path in mangled symbol");
        ok &= require_(lowered.llvm_ir.find("call i32 @p$main$engine__math$add$Mnone$Rnone$S") != std::string::npos,
                       "nested function call must be direct to namespace mangled symbol");
        return ok;
    }

    /// @brief import alias가 nest 경로 호출로 정상 해소되는지 검사한다.
    static bool test_import_alias_path_resolution_to_llvm_() {
        const std::string src = R"(
            import engine::math as m;

            nest engine {
                nest math {
                    def add(a: i32, b: i32) -> i32 {
                        return a + b;
                    }
                }
            }

            def main() -> i32 {
                return m::add(a: 3i32, b: 4i32);
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "import alias source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "import alias LLVM text lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("call i32 @p$main$engine__math$add$Mnone$Rnone$S") != std::string::npos,
                       "import alias call must resolve to namespace-qualified target");
        return ok;
    }

    /// @brief switch 문이 LLVM condbr 체인으로 lowering되는지 검사한다.
    static bool test_switch_stmt_lowering_cfg_() {
        const std::string src = R"(
            def pick(x: i32) -> i32 {
                switch (x) {
                    case 1: { return 11i32; }
                    case 2: { return 22i32; }
                    default: { return 33i32; }
                }
                return 0i32;
            }

            def main() -> i32 {
                return pick(x: 2i32);
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "switch source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "switch LLVM text lowering must succeed");
        ok &= require_(count_substr_(lowered.llvm_ir, "br i1 ") >= 2,
                       "switch lowering must emit multiple conditional branches");
        ok &= require_(lowered.llvm_ir.find("add i32 0, 11") != std::string::npos,
                       "switch case(1) constant path must be present");
        ok &= require_(lowered.llvm_ir.find("add i32 0, 22") != std::string::npos,
                       "switch case(2) constant path must be present");
        ok &= require_(lowered.llvm_ir.find("add i32 0, 33") != std::string::npos,
                       "switch default constant path must be present");
        return ok;
    }

    /// @brief g_vec.x 체인이 field lowering 경로(GEP+store/load)로 내려가는지 검사한다.
    static bool test_global_field_member_chain_lowering_() {
        const std::string src = R"(
            field layout(c) Vec2 {
                x: i32;
                y: i32;
            }

            extern "C" static mut g_vec: Vec2;

            def main() -> i32 {
                g_vec.x = 7i32;
                return g_vec.x;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "global field member chain source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "global field member chain lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("@g_vec = external global [8 x i8]") != std::string::npos,
                       "extern global Vec2 symbol must be emitted");
        ok &= require_(count_substr_(lowered.llvm_ir, "getelementptr i8, ptr @g_vec, i64 0") >= 2,
                       "g_vec.x read/write must both compute field address");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "g_vec.x assignment must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "g_vec.x read must emit typed load");
        return ok;
    }

    /// @brief field literal 생성/수정/읽기가 LLVM field 주소 계산 경로로 내려가는지 검사한다.
    static bool test_field_literal_lowering_() {
        const std::string src = R"(
            field Vec2 {
                x: i32;
                y: i32;
            }

            def main() -> i32 {
                let mut v: Vec2 = Vec2{ x: 1i32, y: 2i32 };
                v.y = 9i32;
                return v.y;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "field literal source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "field literal lowering must succeed");
        ok &= require_(count_substr_(lowered.llvm_ir, "getelementptr i8, ptr") >= 3,
                       "field literal init/update/read must emit field address GEPs");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "field literal lowering must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "field literal lowering must emit typed load");
        return ok;
    }

    /// @brief nullable 경계 승격(T->T?)과 `??` lowering이 optional payload/tag 경로로 내려가는지 검사한다.
    static bool test_nullable_lift_and_coalesce_lowering_() {
        const std::string src = R"(
            def takes_opt(x: i32?) -> i32 {
                return x ?? 99i32;
            }

            def ret_opt() -> i32? {
                return 9i32;
            }

            def main() -> i32 {
                let a: i32? = 5;
                let mut b: i32? = null;
                b = 7;
                let c: i32 = takes_opt(x: 3);
                let d: i32? = ret_opt();
                let e: i32 = d ?? 0i32;
                return (a ?? 0i32) + (b ?? 0i32) + c + e;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "nullable source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "nullable LLVM text lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("define { i1, i32 } @p$main$_$ret_opt") != std::string::npos,
                       "nullable return function must keep optional aggregate signature");
        ok &= require_(lowered.llvm_ir.find("extractvalue { i1, i32 }") != std::string::npos,
                       "nullable `??` lowering must read optional tag/payload");
        ok &= require_(lowered.llvm_ir.find("select i1 ") != std::string::npos,
                       "nullable `??` lowering must emit select on optional tag");
        ok &= require_(lowered.llvm_ir.find("store i1 true") != std::string::npos,
                       "nullable lift(T->T?) must materialize Some(tag=true)");
        ok &= require_(lowered.llvm_ir.find("inttoptr i32") == std::string::npos,
                       "nullable lift must not degrade to int->ptr reinterpret cast");
        ok &= require_(lowered.llvm_ir.find("ptrtoint ptr") == std::string::npos,
                       "nullable coalesce must not degrade to ptr->int reinterpret cast");
        if (!ok) return false;

        return emit_object_for_test_case_(lowered.llvm_ir, "nullable_lift_coalesce_patterns");
    }

    /// @brief 오버로딩/연산자 오버로딩 소스를 다수 순회하며 LLVM-IR + 오브젝트 생성을 함께 검증한다.
    static bool test_overload_object_emission_matrix_() {
        const std::vector<std::string> sources = {
            R"(
                def sum(a: i32, b: i32) -> i32 { return a + b; }
                def sum(a: i64, b: i64) -> i64 { return a + b; }
                def main() -> i32 {
                    let x: i32 = sum(a: 1i32, b: 2i32);
                    let y: i64 = sum(a: 3i64, b: 4i64);
                    return x;
                }
            )",
            R"(
                field I32Box {
                    v: i32;
                };
                acts for I32Box {
                    operator(+)(self move, rhs: I32Box) -> i32 { return self.v + rhs.v; }
                };
                def main() -> i32 {
                    let a: I32Box = I32Box { v: 1i32 };
                    let b: I32Box = I32Box { v: 2i32 };
                    let c: i32 = a + b;
                    return c;
                }
            )",
            R"(
                field I32Box {
                    v: i32;
                };
                acts for I32Box {
                    operator(+)(self move, rhs: I32Box) -> i32 { return self.v + rhs.v; }
                };
                def mix(a: i32, b: i32) -> i32 { return a + b; }
                def mix(a: i64, b: i64) -> i64 { return a + b; }
                def main() -> i32 {
                    let p: i32 = mix(a: 7i32, b: 8i32);
                    let q: i64 = mix(a: 9i64, b: 10i64);
                    let r: i32 = I32Box { v: p } + I32Box { v: 1i32 };
                    return r;
                }
            )"
        };

        bool ok = true;
        for (size_t i = 0; i < sources.size(); ++i) {
            auto built = build_oir_pipeline_(sources[i]);
            ok &= require_(built.has_value(), "matrix source must pass frontend->OIR pipeline");
            if (!built.has_value()) return false;

            const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
                built->oir.mod,
                built->prog.types,
                parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
            );
            ok &= require_(lowered.ok, "matrix source lowering must succeed");
            if (!lowered.ok) return false;

            ok &= require_(lowered.llvm_ir.find("define ") != std::string::npos,
                           "matrix source LLVM-IR must contain define()");
            if (!ok) return false;

            const std::string stem = "matrix_" + std::to_string(i + 1);
            ok &= emit_object_for_test_case_(lowered.llvm_ir, stem);
            if (!ok) return false;
        }
        return ok;
    }

    /// @brief class/proto(default body) 멤버가 LLVM IR 함수로 출력되는지 검사한다.
    static bool test_class_proto_default_member_llvm_symbols_() {
        const std::string src = R"(
            proto WidgetProto {
                def id(self) -> i32 {
                    return 11i32;
                }
            };

            class Button : WidgetProto {
                let value: i32;

                def tap(self) -> i32 {
                    return 3i32;
                }
            }

            def main() -> i32 {
                return 0i32;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "class/proto default member source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for class/proto member source must succeed");
        ok &= require_(lowered.llvm_ir.find("$WidgetProto$id$") != std::string::npos,
                       "LLVM IR must include proto default member symbol fragment");
        ok &= require_(lowered.llvm_ir.find("$Button$tap$") != std::string::npos,
                       "LLVM IR must include class member symbol fragment");
        return ok;
    }

    /// @brief class 생성식 `A(...)`가 LLVM IR에서 `A::init` 호출로 내려가는지 검사한다.
    static bool test_class_ctor_call_llvm_init_symbol_() {
        const std::string src = R"(
            class User {
                init() = default;

                def id(self) -> i32 {
                    return 1i32;
                }
            }

            def main() -> i32 {
                set u = User();
                return u.id();
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "class ctor source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for class ctor source must succeed");
        const bool has_init_symbol =
            lowered.llvm_ir.find("$User$init$") != std::string::npos ||
            (lowered.llvm_ir.find("User") != std::string::npos &&
             lowered.llvm_ir.find("init") != std::string::npos);
        ok &= require_(has_init_symbol, "LLVM IR must include class init symbol fragment");
        ok &= require_(lowered.llvm_ir.find("call void @") != std::string::npos &&
                       lowered.llvm_ir.find("init") != std::string::npos,
                       "constructor expression must emit call to class init symbol");
        return ok;
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
        bool (*def)();
    };

    const Case cases[] = {
        {"source_index_lowering_uses_gep", test_source_index_lowering_uses_gep},
        {"c_abi_field_layout_and_global_symbol", test_c_abi_field_layout_and_global_symbol},
        {"c_abi_field_by_value_param_signature", test_c_abi_field_by_value_param_signature},
        {"text_literal_rodata_and_c_abi_span_signature", test_text_literal_rodata_and_c_abi_span_signature},
        {"float_char_literal_lowering", test_float_char_literal_lowering_},
        {"manual_field_lowering_memory_model", test_manual_field_lowering_memory_model},
        {"object_emission_api_path", test_object_emission_api_path},
        {"overload_and_operator_lowering_patterns", test_overload_and_operator_lowering_patterns_},
        {"nest_path_mangling_and_direct_call", test_nest_path_mangling_and_direct_call_},
        {"import_alias_path_resolution_to_llvm", test_import_alias_path_resolution_to_llvm_},
        {"switch_stmt_lowering_cfg", test_switch_stmt_lowering_cfg_},
        {"global_field_member_chain_lowering", test_global_field_member_chain_lowering_},
        {"field_literal_lowering", test_field_literal_lowering_},
        {"nullable_lift_and_coalesce_lowering", test_nullable_lift_and_coalesce_lowering_},
        {"overload_object_emission_matrix", test_overload_object_emission_matrix_},
        {"class_proto_default_member_llvm_symbols", test_class_proto_default_member_llvm_symbols_},
        {"class_ctor_call_llvm_init_symbol", test_class_ctor_call_llvm_init_symbol_},
        {"oir_case_directory", test_oir_case_directory},
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

    std::cout << "ALL OIR->LLVM TESTS PASSED\n";
    return 0;
}
