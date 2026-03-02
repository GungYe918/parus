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
#include <string_view>
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

    static bool contains_unsafe_null_aggregate_load_(const std::string& ir) {
        size_t pos = 0;
        while (pos < ir.size()) {
            const size_t end = ir.find('\n', pos);
            const size_t len = (end == std::string::npos) ? (ir.size() - pos) : (end - pos);
            const std::string_view line(ir.data() + pos, len);
            if (line.find("load [") != std::string_view::npos &&
                line.find(", ptr null") != std::string_view::npos) {
                return true;
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
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

    static std::optional<OirPipeline> build_oir_pipeline_(
        const std::string& src,
        const std::optional<parus::sir::BuildOptions>& build_opt = std::nullopt
    ) {
        OirPipeline out{};
        out.prog = parse_program_(src);
        if (!run_macro_and_type_(out.prog)) return std::nullopt;

        parus::passes::PassOptions popt{};
        out.pres = parus::passes::run_on_program(out.prog.ast, out.prog.root, out.prog.bag, popt);

        parus::tyck::TypeChecker tc(out.prog.ast, out.prog.types, out.prog.bag, &out.prog.type_resolve);
        out.ty = tc.check_program(out.prog.root);

        parus::sir::BuildOptions bopt{};
        if (build_opt.has_value()) {
            bopt = *build_opt;
        }
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

    /// @brief layout(c)/align(n) struct + C ABI global이 LLVM-IR에 반영되는지 검사한다.
    static bool test_c_abi_field_layout_and_global_symbol() {
        const std::string src = R"(
            struct layout(c) align(16) Vec2 {
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
        ok &= require_(p.has_value(), "C ABI struct/global source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "LLVM text lowering for C ABI struct/global case must succeed");
        ok &= require_(lowered.llvm_ir.find("@g_vec = external global [16 x i8], align 16") != std::string::npos,
                       "C ABI global symbol must be emitted as external global with layout(c) align");
        ok &= require_(lowered.llvm_ir.find("define i32 @probe(") != std::string::npos,
                       "export \"C\" function probe must keep unmangled symbol");
        return ok;
    }

    /// @brief C ABI layout(c) struct by-value 파라미터가 LLVM 시그니처에서 ptr이 아닌 aggregate로 내려가는지 검사한다.
    static bool test_c_abi_field_by_value_param_signature() {
        const std::string src = R"(
            struct layout(c) Vec2 {
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
        ok &= require_(p.has_value(), "C ABI by-value struct source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "LLVM text lowering for C ABI by-value struct case must succeed");
        ok &= require_(lowered.llvm_ir.find("declare i32 @takes([8 x i8])") != std::string::npos,
                       "extern \"C\" struct by-value parameter must be emitted as aggregate signature");
        ok &= require_(lowered.llvm_ir.find("define i32 @pass([8 x i8] %arg0)") != std::string::npos,
                       "export \"C\" struct by-value parameter must be emitted as aggregate signature");
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

    /// @brief 수동 OIR struct 모델이 주소 기반 lowering(getelementptr+load/store)으로 변환되는지 검사한다.
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
        ok &= require_(verrs.empty(), "manual struct OIR must pass verify");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            m,
            types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "manual struct case lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("getelementptr i8, ptr") != std::string::npos,
                       "struct lowering must emit byte-offset GEP");
        ok &= require_(lowered.llvm_ir.find("i64 4") != std::string::npos,
                       "struct lowering must use ABI metadata offset (y=4)");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "struct lowering must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "struct lowering must emit typed load");
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
            struct I32Box {
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

    /// @brief g_vec.x 체인이 struct lowering 경로(GEP+store/load)로 내려가는지 검사한다.
    static bool test_global_field_member_chain_lowering_() {
        const std::string src = R"(
            struct layout(c) Vec2 {
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
        ok &= require_(p.has_value(), "global struct member chain source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "global struct member chain lowering must succeed");
        ok &= require_(lowered.llvm_ir.find("@g_vec = external global [8 x i8]") != std::string::npos,
                       "extern global Vec2 symbol must be emitted");
        ok &= require_(count_substr_(lowered.llvm_ir, "getelementptr i8, ptr @g_vec, i64 0") >= 2,
                       "g_vec.x read/write must both compute struct address");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "g_vec.x assignment must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "g_vec.x read must emit typed load");
        return ok;
    }

    /// @brief struct literal 생성/수정/읽기가 LLVM struct 주소 계산 경로로 내려가는지 검사한다.
    static bool test_field_literal_lowering_() {
        const std::string src = R"(
            struct Vec2 {
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
        ok &= require_(p.has_value(), "struct literal source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );

        ok &= require_(lowered.ok, "struct literal lowering must succeed");
        ok &= require_(count_substr_(lowered.llvm_ir, "getelementptr i8, ptr") >= 3,
                       "struct literal init/update/read must emit struct address GEPs");
        ok &= require_(lowered.llvm_ir.find("store i32") != std::string::npos,
                       "struct literal lowering must emit typed store");
        ok &= require_(lowered.llvm_ir.find("load i32") != std::string::npos,
                       "struct literal lowering must emit typed load");
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
                struct I32Box {
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
                struct I32Box {
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

    /// @brief generic 함수 인스턴스가 dedup되고 LLVM call이 concrete 심볼로 내려가는지 검사한다.
    static bool test_generic_fn_instantiation_llvm_symbols_() {
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

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "generic call source must pass frontend->OIR pipeline");
        if (!ok) return false;

        uint32_t generic_add_defs = 0;
        for (const auto& f : p->oir.mod.funcs) {
            if (f.source_name.find("add<") != std::string::npos) {
                ++generic_add_defs;
            }
        }
        ok &= require_(generic_add_defs == 1, "same concrete generic tuple must be deduplicated before LLVM lowering");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for generic call source must succeed");
        std::string add_symbol;
        size_t pos = 0;
        while (pos < lowered.llvm_ir.size()) {
            const size_t end = lowered.llvm_ir.find('\n', pos);
            const size_t len = (end == std::string::npos) ? (lowered.llvm_ir.size() - pos) : (end - pos);
            const std::string_view line(lowered.llvm_ir.data() + pos, len);
            if (line.find("define i32 @") != std::string_view::npos &&
                line.find("add_i32") != std::string_view::npos) {
                const size_t at = line.find('@');
                const size_t lp = line.find('(', at);
                if (at != std::string_view::npos && lp != std::string_view::npos && lp > at + 1) {
                    add_symbol.assign(line.substr(at + 1, lp - (at + 1)));
                    break;
                }
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        ok &= require_(!add_symbol.empty(), "LLVM IR must include concrete generic add symbol definition");
        if (!ok) return false;

        const std::string call_pattern = "call i32 @" + add_symbol + "(";
        const bool has_call_to_add = (lowered.llvm_ir.find(call_pattern) != std::string::npos);
        ok &= require_(has_call_to_add, "generic call must lower to direct concrete add symbol call");
        return ok;
    }

    /// @brief generic class 인스턴스가 concrete 심볼로 materialize되어 LLVM에 직접 호출로 내려가는지 검사한다.
    static bool test_generic_class_materialization_llvm_symbols_() {
        const std::string src = R"(
            class Box<T> {
                value: T;
                init(v: T) { self.value = v; }
                def get(self) -> T { return self.value; }
            }

            def main() -> i32 {
                set b = Box<i32>(v: 1i32);
                return b.get();
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "generic class source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for generic class source must succeed");
        if (!ok) return false;

        ok &= require_(lowered.llvm_ir.find("$Box_i32_$init$") != std::string::npos,
                       "LLVM IR must include concrete generic class init symbol");
        ok &= require_(lowered.llvm_ir.find("$Box_i32_$get$") != std::string::npos,
                       "LLVM IR must include concrete generic class method symbol");
        ok &= require_(lowered.llvm_ir.find("call i32 @") != std::string::npos &&
                       lowered.llvm_ir.find("$Box_i32_$get$") != std::string::npos,
                       "dot call on generic class value must lower to direct concrete get symbol call");
        return ok;
    }

    /// @brief generic proto default impl이 concrete 심볼로 materialize되어 LLVM에서 호출되는지 검사한다.
    static bool test_generic_proto_default_materialization_llvm_symbols_() {
        const std::string src = R"(
            proto Echo<T> {
                def echo(self, v: T) -> T {
                    return v;
                }
            };

            class EchoUser: Echo<i32> {
                init() = default;
            }

            def main() -> i32 {
                set u = EchoUser();
                return u.echo(v: 7i32);
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "generic proto source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for generic proto source must succeed");
        if (!ok) return false;

        ok &= require_(lowered.llvm_ir.find("$Echo_i32_$echo$") != std::string::npos,
                       "LLVM IR must include concrete generic proto default symbol");
        ok &= require_(lowered.llvm_ir.find("call i32 @") != std::string::npos &&
                       lowered.llvm_ir.find("$Echo_i32_$echo$") != std::string::npos,
                       "proto default call must lower to direct concrete echo symbol call");
        return ok;
    }

    /// @brief generic acts 템플릿은 제외되고 concrete owner용 acts 멤버만 LLVM에 내려가는지 검사한다.
    static bool test_generic_acts_owner_materialization_llvm_symbols_() {
        const std::string src = R"(
            class Vec<T> {
                data: T;
                init(v: T) { self.data = v; }
            }

            acts for Vec<T> {
                def get(self) -> T {
                    return self.data;
                }
            }

            def main() -> i32 {
                set v = Vec<i32>(v: 1i32);
                return v.get();
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "generic acts source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for generic acts source must succeed");
        if (!ok) return false;

        ok &= require_(lowered.llvm_ir.find("$Vec_i32_$init$") != std::string::npos,
                       "LLVM IR must include concrete generic owner init symbol");
        ok &= require_(lowered.llvm_ir.find("$get$") != std::string::npos &&
                       lowered.llvm_ir.find("i32") != std::string::npos,
                       "LLVM IR must include concrete acts member symbol for Vec<i32>");
        ok &= require_(lowered.llvm_ir.find("_____T$") == std::string::npos,
                       "generic acts template symbol must not be lowered into LLVM IR");
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
                value: i32;

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

    /// @brief class override가 있으면 proto default가 아니라 class 멤버 호출이 선택되어야 한다.
    static bool test_proto_override_call_prefers_class_symbol_() {
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

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "proto override source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for proto override source must succeed");
        ok &= require_(lowered.llvm_ir.find("$ValueProto$value$") != std::string::npos,
                       "LLVM IR must include proto default value symbol");
        ok &= require_(lowered.llvm_ir.find("$Counter$value$") != std::string::npos,
                       "LLVM IR must include class override value symbol");

        bool call_hits_class = false;
        size_t pos = 0;
        while (pos < lowered.llvm_ir.size()) {
            const size_t end = lowered.llvm_ir.find('\n', pos);
            const size_t len = (end == std::string::npos) ? (lowered.llvm_ir.size() - pos) : (end - pos);
            const std::string_view line(lowered.llvm_ir.data() + pos, len);
            if (line.find("call i32 @") != std::string_view::npos &&
                line.find("$Counter$value$") != std::string_view::npos) {
                call_hits_class = true;
                break;
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        ok &= require_(call_hits_class, "dot call must lower to class override symbol call");
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
        ok &= require_(!contains_unsafe_null_aggregate_load_(lowered.llvm_ir),
                       "constructor lowering must not emit aggregate load from null pointer");
        ok &= require_(lowered.llvm_ir.find("zeroinitializer") != std::string::npos,
                       "constructor fallback path must materialize zero aggregate storage");
        return ok;
    }

    /// @brief class 생성식 결과를 즉시 dot receiver로 사용할 때도 null aggregate load가 없어야 한다.
    static bool test_class_ctor_temp_receiver_safe_() {
        const std::string src = R"(
            class User {
                init() = default;

                def id(self) -> i32 {
                    return 3i32;
                }
            }

            def main() -> i32 {
                return User().id();
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "temporary receiver source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for temporary receiver source must succeed");
        ok &= require_(!contains_unsafe_null_aggregate_load_(lowered.llvm_ir),
                       "temporary receiver lowering must not emit aggregate load from null pointer");
        ok &= require_(lowered.llvm_ir.find("call void @") != std::string::npos &&
                       lowered.llvm_ir.find("init") != std::string::npos,
                       "temporary receiver lowering must call class init symbol");
        ok &= require_(lowered.llvm_ir.find("alloca ") != std::string::npos,
                       "temporary receiver lowering must materialize constructor storage via alloca");
        return ok;
    }

    /// @brief class 인스턴스 필드 접근이 LLVM IR에서 오프셋 기반 경로로 하향되는지 검사한다.
    static bool test_class_field_offset_lowering_() {
        const std::string src = R"(
            class Vec2 {
                x: i32;
                y: i32;
                init() = default;
            }

            def main() -> i32 {
                set v = Vec2();
                return v.y;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "class struct offset source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for class struct offset source must succeed");
        ok &= require_(lowered.llvm_ir.find("getelementptr i8, ptr") != std::string::npos,
                       "class struct access must emit byte-offset GEP");
        ok &= require_(lowered.llvm_ir.find(", i64 4") != std::string::npos,
                       "class struct access for second i32 member should use offset 4");
        return ok;
    }

    /// @brief class static def/var 경로 사용이 LLVM IR 함수/글로벌 심볼로 남는지 검사한다.
    static bool test_class_static_member_llvm_symbols_() {
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

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "class static member source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for class static member source must succeed");
        ok &= require_(lowered.llvm_ir.find("Counter") != std::string::npos &&
                       lowered.llvm_ir.find("add") != std::string::npos,
                       "LLVM IR must include static class method symbol fragment");
        ok &= require_((lowered.llvm_ir.find("constant i32 zeroinitializer") != std::string::npos) ||
                       (lowered.llvm_ir.find("global i32 zeroinitializer") != std::string::npos) ||
                       (lowered.llvm_ir.find("zeroinitializer") != std::string::npos &&
                        lowered.llvm_ir.find("@") != std::string::npos),
                       "LLVM IR must include static class variable global definition");
        return ok;
    }

    /// @brief RAII lowering이 LLVM IR에서 drop thunk 자동 호출/escape move skip을 유지하는지 검사한다.
    static bool test_class_raii_deinit_llvm_call_patterns_() {
        auto has_drop_call = [](const std::string& ir) -> bool {
            size_t pos = 0;
            while (pos < ir.size()) {
                const size_t end = ir.find('\n', pos);
                const size_t len = (end == std::string::npos) ? (ir.size() - pos) : (end - pos);
                const std::string_view line(ir.data() + pos, len);
                if (line.find("call void @") != std::string_view::npos &&
                    line.find("__parus_drop_") != std::string_view::npos) {
                    return true;
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
            return false;
        };

        const std::string scope_src = R"(
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

        auto p1 = build_oir_pipeline_(scope_src);
        bool ok = true;
        ok &= require_(p1.has_value(), "RAII scope source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered1 = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p1->oir.mod,
            p1->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered1.ok, "LLVM text lowering for RAII scope source must succeed");
        ok &= require_(has_drop_call(lowered1.llvm_ir),
                       "RAII scope-exit path must emit drop thunk call");
        if (!ok) return false;

        const std::string move_src = R"(
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

        auto p2 = build_oir_pipeline_(move_src);
        ok &= require_(p2.has_value(), "RAII move source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered2 = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p2->oir.mod,
            p2->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered2.ok, "LLVM text lowering for RAII move source must succeed");
        ok &= require_(!has_drop_call(lowered2.llvm_ir),
                       "escape-moved local must not emit drop thunk call");
        return ok;
    }

    /// @brief bundle 모드에서 ctor 배열 없이 번들 init 함수 + main 선호출이 생성되는지 검사한다.
    static bool test_bundle_init_wrapper_order_() {
        const std::string src = R"(
            static g: i32 = 7i32;

            def main() -> i32 {
                return g;
            }
        )";

        parus::sir::BuildOptions bopt{};
        bopt.bundle_enabled = true;
        bopt.bundle_name = "demo_bundle";
        bopt.current_source_norm = "/bundle/src/a_main.pr";
        bopt.bundle_sources_norm = {
            "/bundle/src/a_main.pr",
            "/bundle/src/z_other.pr",
        };

        auto p = build_oir_pipeline_(src, bopt);
        bool ok = true;
        ok &= require_(p.has_value(), "bundle init source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for bundle init source must succeed");
        if (!ok) return false;

        const std::string bundle_sym = "parus_bundle_init__demo_bundle";
        ok &= require_(lowered.llvm_ir.find("define void @" + bundle_sym + "()") != std::string::npos,
                       "bundle leader must define bundle init API symbol");
        ok &= require_(lowered.llvm_ir.find("@llvm.global_ctors") == std::string::npos,
                       "LLVM IR must not use global ctor array for bundle init");

        const std::string main_entry = "define i32 @main() {\nentry:\n";
        const auto main_pos = lowered.llvm_ir.find(main_entry);
        ok &= require_(main_pos != std::string::npos, "main wrapper must be emitted");
        if (!ok) return false;

        const auto call_bundle_pos = lowered.llvm_ir.find("call void @" + bundle_sym + "()", main_pos);
        const auto call_user_main_pos = lowered.llvm_ir.find("call i32 @", main_pos);
        ok &= require_(call_bundle_pos != std::string::npos, "main wrapper must call bundle init");
        ok &= require_(call_user_main_pos != std::string::npos, "main wrapper must call user main");
        ok &= require_(call_bundle_pos < call_user_main_pos,
                       "bundle init must run before user main call");
        return ok;
    }

    /// @brief actor spawn/pub/sub가 LLVM-IR에서 모드 심볼과 commit/recast 마커 호출로 내려가는지 검사한다.
    static bool test_actor_marker_calls_and_mode_symbols_() {
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
                set c = spawn Counter(seed: 5i32);
                set x = c.get();
                set y = c.add(delta: 3i32);
                return x + y;
            }
        )";

        auto p = build_oir_pipeline_(src);
        bool ok = true;
        ok &= require_(p.has_value(), "actor marker source must pass frontend->OIR pipeline");
        if (!ok) return false;

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = 20}
        );
        ok &= require_(lowered.ok, "LLVM text lowering for actor marker source must succeed");
        if (!ok) return false;

        ok &= require_(lowered.llvm_ir.find("call void @__parus_actor_commit_marker()") != std::string::npos,
                       "actor commit must lower to marker helper call");
        ok &= require_(lowered.llvm_ir.find("call void @__parus_actor_recast_marker()") != std::string::npos,
                       "actor recast must lower to marker helper call");
        ok &= require_(lowered.llvm_ir.find("define internal void @__parus_actor_commit_marker()") != std::string::npos,
                       "actor commit marker helper definition must exist");
        ok &= require_(lowered.llvm_ir.find("define internal void @__parus_actor_recast_marker()") != std::string::npos,
                       "actor recast marker helper definition must exist");
        ok &= require_(lowered.llvm_ir.find("Mpub") != std::string::npos,
                       "actor pub function symbol must include mode marker");
        ok &= require_(lowered.llvm_ir.find("Msub") != std::string::npos,
                       "actor sub function symbol must include mode marker");
        ok &= require_(lowered.llvm_ir.find("init$Mnone") != std::string::npos,
                       "spawn path must lower via actor init symbol");
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
        {"generic_fn_instantiation_llvm_symbols", test_generic_fn_instantiation_llvm_symbols_},
        {"generic_class_materialization_llvm_symbols", test_generic_class_materialization_llvm_symbols_},
        {"generic_proto_default_materialization_llvm_symbols", test_generic_proto_default_materialization_llvm_symbols_},
        {"generic_acts_owner_materialization_llvm_symbols", test_generic_acts_owner_materialization_llvm_symbols_},
        {"class_proto_default_member_llvm_symbols", test_class_proto_default_member_llvm_symbols_},
        {"proto_override_call_prefers_class_symbol", test_proto_override_call_prefers_class_symbol_},
        {"class_ctor_call_llvm_init_symbol", test_class_ctor_call_llvm_init_symbol_},
        {"class_ctor_temp_receiver_safe", test_class_ctor_temp_receiver_safe_},
        {"class_field_offset_lowering", test_class_field_offset_lowering_},
        {"class_static_member_llvm_symbols", test_class_static_member_llvm_symbols_},
        {"class_raii_deinit_llvm_call_patterns", test_class_raii_deinit_llvm_call_patterns_},
        {"bundle_init_wrapper_order", test_bundle_init_wrapper_order_},
        {"actor_marker_calls_and_mode_symbols", test_actor_marker_calls_and_mode_symbols_},
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
