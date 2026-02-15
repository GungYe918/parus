// backend/llvmconfig/lanes/v21/AOTLlvmV21.cpp
#include <parus/backend/aot/AOTLLVMDispatcher.hpp>

namespace parus::backend::aot::detail {

    /// @brief LLVM 21 lane 전용 AOT 스텁 구현.
    CompileResult compile_llvm_lane_v21(
        const parus::oir::Module&,
        const parus::ty::TypePool&,
        const CompileOptions&
    ) {
        CompileResult r{};
        r.ok = false;
        r.messages.push_back(CompileMessage{
            true,
            "AOT LLVM lane v21 is selected, but lowering is not implemented yet."
        });
        return r;
    }

} // namespace parus::backend::aot::detail
