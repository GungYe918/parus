// backend/include/parus/backend/aot/LLVMIRLowering.hpp
#pragma once

#include <parus/backend/Backend.hpp>

#include <string>

namespace parus::backend::aot {

    /// @brief OIR -> LLVM-IR 텍스트 lowering 옵션.
    struct LLVMIRLoweringOptions {
        uint32_t llvm_lane_major = 0;
    };

    /// @brief OIR -> LLVM-IR 텍스트 lowering 결과.
    struct LLVMIRLoweringResult {
        bool ok = false;
        std::string llvm_ir{};
        std::vector<CompileMessage> messages{};
    };

    /// @brief OIR 모듈을 LLVM-IR(text)로 낮춘다.
    LLVMIRLoweringResult lower_oir_to_llvm_ir_text(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const LLVMIRLoweringOptions& opt
    );

} // namespace parus::backend::aot
