// compiler/include/gaupel/sir/Verify.hpp
#pragma once
#include <gaupel/sir/SIR.hpp>

#include <string>
#include <vector>

namespace gaupel::sir {

    struct VerifyError {
        std::string msg;
    };

    // Structural verifier for SIR module integrity.
    // This does not check language semantics; it checks IR shape/indices invariants.
    std::vector<VerifyError> verify_module(const Module& m);

    /// @brief EscapeHandle 메타 규칙을 검증한다(정적 경계/비물질화 invariant).
    std::vector<VerifyError> verify_escape_handles(const Module& m);

} // namespace gaupel::sir
