// backend/include/parus/backend/wasm/WasmBackend.hpp
#pragma once
#include <parus/backend/Backend.hpp>


namespace parus::backend::wasm {

    /// @brief WASM 백엔드 스텁 구현.
    class WasmBackend final : public parus::backend::Backend {
    public:
        /// @brief 백엔드 종류를 반환한다.
        BackendKind kind() const override;

        /// @brief OIR을 WASM 경로로 컴파일한다(현재는 스텁).
        CompileResult compile(
            const parus::oir::Module& oir,
            const parus::ty::TypePool& types,
            const CompileOptions& opt
        ) override;
    };

} // namespace parus::backend::wasm
