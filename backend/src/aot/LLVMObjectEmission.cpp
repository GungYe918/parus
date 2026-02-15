// backend/src/aot/LLVMObjectEmission.cpp
#include <parus/backend/aot/LLVMIRLowering.hpp>

#include <optional>
#include <string>

#ifndef PARUS_LLVM_TOOLCHAIN_FOUND
#define PARUS_LLVM_TOOLCHAIN_FOUND 0
#endif

#if PARUS_LLVM_TOOLCHAIN_FOUND
#include <llvm/AsmParser/Parser.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#if __has_include(<llvm/TargetParser/Host.h>)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#endif

namespace parus::backend::aot {

    namespace {

#if PARUS_LLVM_TOOLCHAIN_FOUND
        /// @brief O 레벨 숫자를 LLVM CodeGen 레벨로 변환한다.
        llvm::CodeGenOptLevel to_codegen_opt_level_(uint8_t opt_level) {
            switch (opt_level) {
                case 0: return llvm::CodeGenOptLevel::None;
                case 1: return llvm::CodeGenOptLevel::Less;
                case 2: return llvm::CodeGenOptLevel::Default;
                case 3: return llvm::CodeGenOptLevel::Aggressive;
                default: return llvm::CodeGenOptLevel::Default;
            }
        }

        /// @brief LLVM target 서브시스템을 1회 초기화한다.
        void init_llvm_targets_once_() {
            static bool inited = false;
            if (inited) return;
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmParsers();
            llvm::InitializeAllAsmPrinters();
            inited = true;
        }

        /// @brief LLVM 파서/코드젠 오류를 문자열로 렌더링한다.
        std::string render_diag_(const llvm::SMDiagnostic& diag) {
            std::string s;
            llvm::raw_string_ostream os(s);
            diag.print("parus", os);
            os.flush();
            return s;
        }
#endif

    } // namespace

    LLVMObjectEmissionResult emit_object_from_llvm_ir_text(
        std::string_view llvm_ir_text,
        const std::string& output_path,
        const LLVMObjectEmissionOptions& opt
    ) {
        LLVMObjectEmissionResult out{};

#if !PARUS_LLVM_TOOLCHAIN_FOUND
        (void)llvm_ir_text;
        (void)output_path;
        (void)opt;
        out.ok = false;
        out.messages.push_back(CompileMessage{
            true,
            "LLVM toolchain is not available in this build. Object emission requires direct LLVM static linkage."
        });
        return out;
#else
        init_llvm_targets_once_();

        llvm::LLVMContext context;
        llvm::SMDiagnostic smdiag;
        auto mem = llvm::MemoryBuffer::getMemBufferCopy(std::string(llvm_ir_text), "parus.oir.ll");
        auto module = llvm::parseAssembly(*mem, smdiag, context);
        if (!module) {
            out.ok = false;
            out.messages.push_back(CompileMessage{
                true,
                "failed to parse lowered LLVM-IR: " + render_diag_(smdiag)
            });
            return out;
        }

        const std::string triple =
            opt.target_triple.empty() ? llvm::sys::getDefaultTargetTriple() : opt.target_triple;
        llvm::Triple triple_obj(triple);
#if LLVM_VERSION_MAJOR >= 21
        module->setTargetTriple(std::move(triple_obj));
#else
        module->setTargetTriple(triple);
#endif

        std::string target_err;
#if LLVM_VERSION_MAJOR >= 21
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple_obj, target_err);
#else
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, target_err);
#endif
        if (target == nullptr) {
            out.ok = false;
            out.messages.push_back(CompileMessage{
                true,
                "failed to lookup LLVM target for triple '" + triple + "': " + target_err
            });
            return out;
        }

        llvm::TargetOptions target_opt{};
        const std::string cpu = opt.cpu.empty() ? "generic" : opt.cpu;
        const auto cg_level = to_codegen_opt_level_(opt.opt_level);
        std::optional<llvm::Reloc::Model> reloc_model{};
        std::optional<llvm::CodeModel::Model> code_model{};
#if LLVM_VERSION_MAJOR >= 21
        auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
            triple_obj,
            cpu,
            "",
            target_opt,
            reloc_model,
            code_model,
            cg_level
        ));
#else
        auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
            triple,
            cpu,
            "",
            target_opt,
            reloc_model,
            code_model,
            cg_level
        ));
#endif
        if (!tm) {
            out.ok = false;
            out.messages.push_back(CompileMessage{
                true,
                "failed to create LLVM TargetMachine for triple '" + triple + "'."
            });
            return out;
        }

        module->setDataLayout(tm->createDataLayout());

        std::error_code ec;
        llvm::raw_fd_ostream obj_out(output_path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            out.ok = false;
            out.messages.push_back(CompileMessage{
                true,
                "failed to open output object path '" + output_path + "': " + ec.message()
            });
            return out;
        }

        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, obj_out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            out.ok = false;
            out.messages.push_back(CompileMessage{
                true,
                "LLVM target machine does not support object emission for triple '" + triple + "'."
            });
            return out;
        }

        pm.run(*module);
        obj_out.flush();

        out.ok = true;
        out.messages.push_back(CompileMessage{
            false,
            "wrote object file to " + output_path
        });
        return out;
#endif
    }

} // namespace parus::backend::aot
