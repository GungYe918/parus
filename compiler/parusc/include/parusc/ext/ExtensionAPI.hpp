// compiler/parusc/include/parusc/ext/ExtensionAPI.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace parusc::ext {

    /// @brief 확장 진단 심각도.
    enum class DiagnosticSeverity : uint8_t {
        kInfo,
        kWarning,
        kError,
    };

    /// @brief 확장 진단 정보(POD).
    struct Diagnostic {
        DiagnosticSeverity severity = DiagnosticSeverity::kInfo;
        std::string code{};
        std::string message{};
        std::string file{};
        uint32_t line = 0;
        uint32_t column = 0;
    };

    /// @brief 빌트인 함수 시그니처 정보(POD).
    struct BuiltinSignature {
        std::string name{};
        std::vector<std::string> param_type_names{};
        std::string return_type_name{};
        bool variadic = false;
    };

    /// @brief Lint 룰 실행 컨텍스트 인터페이스.
    class LintContext {
    public:
        virtual ~LintContext() = default;

        /// @brief 소스 파일 경로를 반환한다.
        virtual std::string_view file_path() const = 0;

        /// @brief 진단을 추가한다.
        virtual void emit(const Diagnostic& d) = 0;
    };

    /// @brief 빌트인 함수 등록 인터페이스.
    class BuiltinRegistrar {
    public:
        virtual ~BuiltinRegistrar() = default;

        /// @brief 빌트인 시그니처를 등록한다.
        virtual void register_builtin(const BuiltinSignature& sig) = 0;
    };

    /// @brief Lint 확장 포인트 인터페이스.
    class LintRule {
    public:
        virtual ~LintRule() = default;

        /// @brief 룰 고유 식별자를 반환한다.
        virtual std::string_view id() const = 0;

        /// @brief 룰 실행을 수행한다.
        virtual void run(LintContext& ctx) = 0;
    };

    /// @brief parusc 확장 모듈 루트 인터페이스.
    class Extension {
    public:
        virtual ~Extension() = default;

        /// @brief 확장 이름을 반환한다.
        virtual std::string_view name() const = 0;

        /// @brief 빌트인 함수 등록 단계에서 호출된다.
        virtual void register_builtins(BuiltinRegistrar& registrar) = 0;

        /// @brief 확장이 제공하는 lint rule 목록을 반환한다.
        virtual std::vector<std::unique_ptr<LintRule>> create_lint_rules() = 0;
    };

} // namespace parusc::ext
