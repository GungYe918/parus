// backend/llvmconfig/lanes/v21/AOTLlvmV21.cpp
#include <parus/backend/aot/AOTLLVMDispatcher.hpp>
#include <parus/backend/aot/LLVMIRLowering.hpp>

#include <fstream>

namespace parus::backend::aot::detail {

    /// @brief LLVM 21 lane 전용 OIR -> LLVM-IR emission 구현.
    CompileResult compile_llvm_lane_v21(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const CompileOptions& opt
    ) {
        CompileResult r{};
        const auto lowered = lower_oir_to_llvm_ir_text(
            oir,
            types,
            LLVMIRLoweringOptions{.llvm_lane_major = 21}
        );
        for (const auto& m : lowered.messages) r.messages.push_back(m);

        if (!lowered.ok) {
            r.ok = false;
            r.messages.push_back(CompileMessage{
                true,
                "LLVM lane v21 lowering failed."
            });
            return r;
        }

        if (opt.emit_object) {
            const std::string out_path = opt.output_path.empty() ? "a.o" : opt.output_path;
            const auto emitted = emit_object_from_llvm_ir_text(
                lowered.llvm_ir,
                out_path,
                LLVMObjectEmissionOptions{
                    .llvm_lane_major = 21,
                    .target_triple = opt.target_triple,
                    .cpu = opt.cpu,
                    .opt_level = opt.opt_level
                }
            );
            for (const auto& m : emitted.messages) r.messages.push_back(m);
            r.ok = emitted.ok;
            return r;
        }

        const std::string out_path = opt.output_path.empty() ? "a.ll" : opt.output_path;
        std::ofstream ofs(out_path, std::ios::out | std::ios::binary);
        if (!ofs) {
            r.ok = false;
            r.messages.push_back(CompileMessage{
                true,
                "failed to open output file: " + out_path
            });
            return r;
        }
        ofs << lowered.llvm_ir;
        ofs.close();

        r.ok = true;
        r.messages.push_back(CompileMessage{
            false,
            "wrote LLVM-IR to " + out_path
        });
        return r;
    }

} // namespace parus::backend::aot::detail
