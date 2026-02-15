// backend/src/jit/JITBackend.cpp
#include <parus/backend/jit/JITBackend.hpp>

namespace parus::backend::jit {

    /// @brief JIT 백엔드 종류를 반환한다.
    BackendKind JITBackend::kind() const {
        return BackendKind::kJit;
    }

    /// @brief JIT lowering/실행 파이프라인은 다음 단계에서 구현한다.
    CompileResult JITBackend::compile(
        const parus::oir::Module&,
        const parus::ty::TypePool&,
        const CompileOptions&
    ) {
        CompileResult r{};
        r.ok = false;
        r.messages.push_back(CompileMessage{
            true,
            "JIT backend is scaffolded but not implemented yet."
        });
        return r;
    }

} // namespace parus::backend::jit
