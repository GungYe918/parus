// backend/include/parus/backend/aot/AOTBackend.hpp
#pragma once
#include <parus/backend/Backend.hpp>


namespace parus::backend::aot {

    /// @brief AOT 백엔드 구현.
    class AOTBackend final : public parus::backend::Backend {
    public:
        /// @brief 백엔드 종류를 반환한다.
        BackendKind kind() const override;

        /// @brief OIR을 AOT 경로로 컴파일한다.
        CompileResult compile(
            const parus::oir::Module& oir,
            const parus::ty::TypePool& types,
            const CompileOptions& opt
        ) override;
    };

} // namespace parus::backend::aot
