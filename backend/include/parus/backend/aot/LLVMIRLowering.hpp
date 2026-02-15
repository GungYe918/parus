// backend/include/parus/backend/aot/LLVMIRLowering.hpp
#pragma once

#include <parus/backend/Backend.hpp>

#include <string>
#include <string_view>
#include <vector>

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

    /// @brief LLVM API 기반 object emission 옵션.
    struct LLVMObjectEmissionOptions {
        uint32_t llvm_lane_major = 0;
        std::string target_triple{};
        std::string cpu{};
        uint8_t opt_level = 0;
    };

    /// @brief LLVM API 기반 object emission 결과.
    struct LLVMObjectEmissionResult {
        bool ok = false;
        std::vector<CompileMessage> messages{};
    };

    /// @brief OIR 모듈을 LLVM-IR(text)로 낮춘다.
    LLVMIRLoweringResult lower_oir_to_llvm_ir_text(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const LLVMIRLoweringOptions& opt
    );

    /// @brief LLVM-IR 텍스트를 LLVM API로 object(.o)로 방출한다.
    LLVMObjectEmissionResult emit_object_from_llvm_ir_text(
        std::string_view llvm_ir_text,
        const std::string& output_path,
        const LLVMObjectEmissionOptions& opt
    );

} // namespace parus::backend::aot
