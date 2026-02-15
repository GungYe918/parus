// backend/src/aot/AOTBackend.cpp
#include <parus/backend/aot/AOTBackend.hpp>

#if PARUS_AOT_ENABLE_LLVM
#include <parus/backend/aot/AOTLLVMDispatcher.hpp>
#endif

namespace parus::backend::aot {

    /// @brief AOT 백엔드 종류를 반환한다.
    BackendKind AOTBackend::kind() const {
        return BackendKind::kAot;
    }

    /// @brief AOT 엔진 선택에 따라 컴파일 경로를 분기한다.
    CompileResult AOTBackend::compile(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const CompileOptions& opt
    ) {
#if PARUS_AOT_ENABLE_LLVM
        if (opt.aot_engine == AOTEngine::kLlvm) {
#if PARUS_LLVM_SELECTED_MAJOR == 21
            return detail::compile_llvm_lane_v21(oir, types, opt);
#elif PARUS_LLVM_SELECTED_MAJOR == 20
            return detail::compile_llvm_lane_v20(oir, types, opt);
#else
            CompileResult r{};
            r.ok = false;
            r.messages.push_back(CompileMessage{
                true,
                "Unsupported PARUS_LLVM_SELECTED_MAJOR. Expected 20 or 21."
            });
            return r;
#endif
        }
#else
        (void)oir;
        (void)types;
#endif

        CompileResult r{};
        r.ok = false;
        if (opt.aot_engine == AOTEngine::kNativeCodegen) {
            r.messages.push_back(CompileMessage{
                true,
                "AOT native codegen engine is not implemented yet."
            });
            return r;
        }

#if PARUS_AOT_ENABLE_LLVM
        r.messages.push_back(CompileMessage{
            true,
            "Unknown AOT engine selection."
        });
#else
        r.messages.push_back(CompileMessage{
            true,
            "AOT backend was built without LLVM engine support."
        });
#endif
        return r;
    }

} // namespace parus::backend::aot
