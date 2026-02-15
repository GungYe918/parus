// backend/include/parus/backend/aot/AOTLLVMDispatcher.hpp
#pragma once

#include <parus/backend/aot/AOTBackend.hpp>


namespace parus::backend::aot::detail {

    /// @brief LLVM 20 lane AOT 컴파일 엔트리.
    CompileResult compile_llvm_lane_v20(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const CompileOptions& opt
    );

    /// @brief LLVM 21 lane AOT 컴파일 엔트리.
    CompileResult compile_llvm_lane_v21(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const CompileOptions& opt
    );

} // namespace parus::backend::aot::detail
