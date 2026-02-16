#include <parus/backend/aot/LLVMIRLowering.hpp>
#include <parus/backend/link/Linker.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/oir/Builder.hpp>
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/tyck/TypeCheck.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace {

#ifndef PARUS_TEST_LLVM_LANE
#define PARUS_TEST_LLVM_LANE 20
#endif

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

    static std::filesystem::path case_path_(std::string_view name) {
#ifndef PARUS_FFI_CASE_DIR
        return std::filesystem::path(name);
#else
        return std::filesystem::path(PARUS_FFI_CASE_DIR) / std::string(name);
#endif
    }

    static bool read_text_file_(const std::filesystem::path& p, std::string& out) {
        std::ifstream ifs(p, std::ios::in | std::ios::binary);
        if (!ifs) return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return true;
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

    static std::string join_compile_messages_(const std::vector<parus::backend::CompileMessage>& messages) {
        std::string out;
        for (const auto& m : messages) {
            if (!out.empty()) out += " | ";
            out += m.text;
        }
        return out;
    }

    static bool compile_parus_file_to_object_(
        const std::filesystem::path& src_path,
        const std::filesystem::path& obj_path,
        std::string& err
    ) {
        std::string src;
        if (!read_text_file_(src_path, src)) {
            err = "failed to read Parus source: " + src_path.string();
            return false;
        }

        auto p = build_oir_pipeline_(src);
        if (!p.has_value()) {
            err = "frontend->OIR pipeline failed for: " + src_path.string();
            return false;
        }

        const auto lowered = parus::backend::aot::lower_oir_to_llvm_ir_text(
            p->oir.mod,
            p->prog.types,
            parus::backend::aot::LLVMIRLoweringOptions{.llvm_lane_major = PARUS_TEST_LLVM_LANE}
        );
        if (!lowered.ok) {
            err = "OIR->LLVM lowering failed for: " + src_path.string() + " :: " + join_compile_messages_(lowered.messages);
            return false;
        }

        const auto emitted = parus::backend::aot::emit_object_from_llvm_ir_text(
            lowered.llvm_ir,
            obj_path.string(),
            parus::backend::aot::LLVMObjectEmissionOptions{
                .llvm_lane_major = PARUS_TEST_LLVM_LANE,
                .target_triple = "",
                .cpu = "",
                .opt_level = 2
            }
        );
        if (!emitted.ok) {
            err = "LLVM object emission failed for: " + src_path.string() + " :: " + join_compile_messages_(emitted.messages);
            return false;
        }

        if (!std::filesystem::exists(obj_path)) {
            err = "object file does not exist after emission: " + obj_path.string();
            return false;
        }
        return true;
    }

    static std::string shell_quote_(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    static int decode_wait_status_(int raw_status) {
        if (raw_status == -1) return -1;
#if defined(_WIN32)
        return raw_status;
#else
        if (WIFEXITED(raw_status)) return WEXITSTATUS(raw_status);
        if (WIFSIGNALED(raw_status)) return 128 + WTERMSIG(raw_status);
        return raw_status;
#endif
    }

    static int run_command_(const std::string& cmd) {
        return decode_wait_status_(std::system(cmd.c_str()));
    }

    static std::string select_c_compiler_() {
        namespace fs = std::filesystem;
        if (fs::exists("/usr/bin/clang")) return "/usr/bin/clang";
        if (fs::exists("/usr/bin/cc")) return "/usr/bin/cc";
        return "cc";
    }

    static bool compile_c_file_to_object_(
        const std::filesystem::path& c_path,
        const std::filesystem::path& obj_path,
        std::string& err
    ) {
        const std::string cc = select_c_compiler_();
        const std::string cmd =
            shell_quote_(cc) + " -c " +
            shell_quote_(c_path.string()) + " -o " +
            shell_quote_(obj_path.string());

        const int rc = run_command_(cmd);
        if (rc != 0) {
            err = "C compile failed (exit=" + std::to_string(rc) + "): " + c_path.string();
            return false;
        }
        if (!std::filesystem::exists(obj_path)) {
            err = "C object does not exist: " + obj_path.string();
            return false;
        }
        return true;
    }

    static bool link_objects_to_exe_(
        const std::vector<std::filesystem::path>& objects,
        const std::filesystem::path& exe_path,
        std::string& err
    ) {
        parus::backend::link::LinkOptions opt{};
        for (const auto& o : objects) {
            opt.object_paths.push_back(o.string());
        }
        opt.output_path = exe_path.string();
        opt.mode = parus::backend::link::LinkerMode::kSystemClang;
        opt.allow_fallback = false;

        const auto link_res = parus::backend::link::link_executable(opt);
        if (!link_res.ok) {
            err = "link failed: " + join_compile_messages_(link_res.messages);
            return false;
        }
        if (!std::filesystem::exists(exe_path)) {
            err = "linked executable does not exist: " + exe_path.string();
            return false;
        }
        return true;
    }

    static bool run_executable_capture_(
        const std::filesystem::path& exe_path,
        int& exit_code,
        std::string& output
    ) {
        const std::filesystem::path out_path = exe_path.parent_path() / (exe_path.filename().string() + ".out.txt");
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        const std::string cmd =
            shell_quote_(exe_path.string()) + " > " +
            shell_quote_(out_path.string()) + " 2>&1";
        exit_code = run_command_(cmd);

        output.clear();
        (void)read_text_file_(out_path, output);
        return true;
    }

    static std::filesystem::path prepare_work_dir_(std::string_view stem) {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("parus_ffi_" + std::string(stem));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    static bool test_ffi_printf_wrapper_hello_world_() {
        bool ok = true;
        std::string err;
        const auto work = prepare_work_dir_("hello_printf");

        const auto parus_src = case_path_("hello_printf.pr");
        const auto c_src = case_path_("hello_printf_wrapper.c");
        const auto parus_obj = work / "hello_printf.parus.o";
        const auto c_obj = work / "hello_printf.c.o";
        const auto exe = work / "hello_printf.bin";

        ok &= require_(compile_parus_file_to_object_(parus_src, parus_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(compile_c_file_to_object_(c_src, c_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(link_objects_to_exe_({parus_obj, c_obj}, exe, err), err.c_str());
        if (!ok) return false;

        int rc = -1;
        std::string out;
        ok &= require_(run_executable_capture_(exe, rc, out), "failed to execute hello_printf binary");
        ok &= require_(rc == 0, "hello_printf executable must exit with code 0");
        ok &= require_(out == "Hello, World", "hello_printf output must be exactly 'Hello, World'");
        return ok;
    }

    static bool test_ffi_extern_scalar_arithmetic_() {
        bool ok = true;
        std::string err;
        const auto work = prepare_work_dir_("extern_arith");

        const auto parus_src = case_path_("extern_arith.pr");
        const auto c_src = case_path_("extern_arith_wrapper.c");
        const auto parus_obj = work / "extern_arith.parus.o";
        const auto c_obj = work / "extern_arith.c.o";
        const auto exe = work / "extern_arith.bin";

        ok &= require_(compile_parus_file_to_object_(parus_src, parus_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(compile_c_file_to_object_(c_src, c_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(link_objects_to_exe_({parus_obj, c_obj}, exe, err), err.c_str());
        if (!ok) return false;

        int rc = -1;
        std::string out;
        ok &= require_(run_executable_capture_(exe, rc, out), "failed to execute extern_arith binary");
        ok &= require_(rc == 0, "extern_arith executable must exit with code 0");
        return ok;
    }

    static bool test_ffi_export_symbol_callable_from_c_() {
        bool ok = true;
        std::string err;
        const auto work = prepare_work_dir_("export_to_c");

        const auto parus_src = case_path_("export_to_c.pr");
        const auto c_src = case_path_("export_to_c_main.c");
        const auto parus_obj = work / "export_to_c.parus.o";
        const auto c_obj = work / "export_to_c.c.o";
        const auto exe = work / "export_to_c.bin";

        ok &= require_(compile_parus_file_to_object_(parus_src, parus_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(compile_c_file_to_object_(c_src, c_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(link_objects_to_exe_({parus_obj, c_obj}, exe, err), err.c_str());
        if (!ok) return false;

        int rc = -1;
        std::string out;
        ok &= require_(run_executable_capture_(exe, rc, out), "failed to execute export_to_c binary");
        ok &= require_(rc == 0, "export_to_c executable must exit with code 0");
        return ok;
    }

    static bool test_ffi_extern_global_counter_roundtrip_() {
        bool ok = true;
        std::string err;
        const auto work = prepare_work_dir_("extern_global_counter");

        const auto parus_src = case_path_("extern_global_counter.pr");
        const auto c_src = case_path_("extern_global_counter_wrapper.c");
        const auto parus_obj = work / "extern_global_counter.parus.o";
        const auto c_obj = work / "extern_global_counter.c.o";
        const auto exe = work / "extern_global_counter.bin";

        ok &= require_(compile_parus_file_to_object_(parus_src, parus_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(compile_c_file_to_object_(c_src, c_obj, err), err.c_str());
        if (!ok) return false;
        ok &= require_(link_objects_to_exe_({parus_obj, c_obj}, exe, err), err.c_str());
        if (!ok) return false;

        int rc = -1;
        std::string out;
        ok &= require_(run_executable_capture_(exe, rc, out), "failed to execute extern_global_counter binary");
        ok &= require_(rc == 0, "extern_global_counter executable must exit with code 0");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"ffi_printf_wrapper_hello_world", test_ffi_printf_wrapper_hello_world_},
        {"ffi_extern_scalar_arithmetic", test_ffi_extern_scalar_arithmetic_},
        {"ffi_export_symbol_callable_from_c", test_ffi_export_symbol_callable_from_c_},
        {"ffi_extern_global_counter_roundtrip", test_ffi_extern_global_counter_roundtrip_},
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

    std::cout << "ALL FFI TESTS PASSED\n";
    return 0;
}
