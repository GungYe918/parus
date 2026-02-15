// backend/include/parus/backend/Backend.hpp
#pragma once

#include <parus/oir/OIR.hpp>
#include <parus/ty/TypePool.hpp>

#include <cstdint>
#include <string>
#include <vector>


namespace parus::backend {

    /// @brief 백엔드 종류 식별자.
    enum class BackendKind : uint8_t {
        kAot,
        kJit,
        kWasm,
    };

    /// @brief AOT 백엔드 엔진 식별자.
    enum class AOTEngine : uint8_t {
        kLlvm,
        kNativeCodegen,
    };

    /// @brief 백엔드 컴파일 옵션.
    struct CompileOptions {
        uint8_t opt_level = 0;
        std::string target_triple{};
        std::string cpu{};

        std::string output_path{};
        bool emit_llvm_ir = false;
        bool emit_object = false;

        // AOT 전용 선택지
        AOTEngine aot_engine = AOTEngine::kLlvm;
    };

    /// @brief 백엔드 메시지(오류/경고/정보).
    struct CompileMessage {
        bool is_error = false;
        std::string text{};
    };

    /// @brief 백엔드 실행 결과.
    struct CompileResult {
        bool ok = false;
        std::vector<CompileMessage> messages{};
    };

    /// @brief OIR을 실제 타깃 산출물로 변환하는 백엔드 공통 인터페이스.
    class Backend {
    public:
        virtual ~Backend() = default;

        /// @brief 백엔드 종류를 반환한다.
        virtual BackendKind kind() const = 0;

        /// @brief OIR 모듈을 입력 받아 타깃 산출물(.ll/.o 등)을 생성한다.
        virtual CompileResult compile(
            const parus::oir::Module& oir,
            const parus::ty::TypePool& types,
            const CompileOptions& opt
        ) = 0;
    };

} // namespace parus::backend
