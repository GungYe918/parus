#pragma once

#include <parus/backend/Backend.hpp>
#include <parus/goir/GOIR.hpp>
#include <parus/ty/TypePool.hpp>

#include <string>
#include <vector>

namespace parus::backend::mlir {

    struct GOIRLoweringOptions {
        uint32_t llvm_lane_major = 0;
    };

    struct GOIRLoweringResult {
        bool ok = false;
        std::string mlir_text{};
        std::vector<CompileMessage> messages{};
    };

    struct GOIRLLVMIRResult {
        bool ok = false;
        std::string mlir_text{};
        std::string llvm_ir{};
        std::vector<CompileMessage> messages{};
    };

    struct GOIRObjectEmissionResult {
        bool ok = false;
        std::string mlir_text{};
        std::string llvm_ir{};
        std::vector<CompileMessage> messages{};
    };

    GOIRLoweringResult lower_goir_to_mlir_text(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types
    );

    GOIRLLVMIRResult lower_goir_to_llvm_ir_text(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types,
        const GOIRLoweringOptions& options
    );

    GOIRObjectEmissionResult emit_object_from_goir_via_mlir(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types,
        const GOIRLoweringOptions& lowering_options,
        const std::string& output_path,
        const std::string& target_triple,
        const std::string& cpu,
        uint8_t opt_level
    );

    bool run_mlir_smoke(std::string* error_text = nullptr);

} // namespace parus::backend::mlir
