// backend/src/wasm/WasmBackend.cpp
#include <parus/backend/wasm/WasmBackend.hpp>

namespace parus::backend::wasm {

    /// @brief WASM 백엔드 종류를 반환한다.
    BackendKind WasmBackend::kind() const {
        return BackendKind::kWasm;
    }

    /// @brief WASM lowering/코드생성은 다음 단계에서 구현한다.
    CompileResult WasmBackend::compile(
        const parus::oir::Module&,
        const parus::ty::TypePool&,
        const CompileOptions&
    ) {
        CompileResult r{};
        r.ok = false;
        r.messages.push_back(CompileMessage{
            true,
            "WASM backend is scaffolded but not implemented yet."
        });
        return r;
    }

} // namespace parus::backend::wasm
