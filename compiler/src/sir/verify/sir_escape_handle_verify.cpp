// compiler/src/sir/verify/sir_escape_handle_verify.cpp
#include <gaupel/sir/Verify.hpp>

#include <sstream>
#include <string>
#include <vector>

namespace gaupel::sir {

    namespace {

        /// @brief verify 오류 메시지를 결과 벡터에 추가한다.
        void push_error_(std::vector<VerifyError>& out, const std::string& msg) {
            out.push_back(VerifyError{msg});
        }

        /// @brief EscapeBoundaryKind를 사람이 읽을 수 있는 이름으로 변환한다.
        const char* boundary_name_(EscapeBoundaryKind k) {
            switch (k) {
                case EscapeBoundaryKind::kNone: return "none";
                case EscapeBoundaryKind::kReturn: return "return";
                case EscapeBoundaryKind::kCallArg: return "call_arg";
                case EscapeBoundaryKind::kAbi: return "abi";
                case EscapeBoundaryKind::kFfi: return "ffi";
            }
            return "unknown";
        }

        /// @brief EscapeHandleKind를 사람이 읽을 수 있는 이름으로 변환한다.
        const char* kind_name_(EscapeHandleKind k) {
            switch (k) {
                case EscapeHandleKind::kTrivial: return "trivial";
                case EscapeHandleKind::kStackSlot: return "stack_slot";
                case EscapeHandleKind::kCallerSlot: return "caller_slot";
                case EscapeHandleKind::kHeapBox: return "heap_box";
            }
            return "unknown";
        }

    } // namespace

    /// @brief EscapeHandle 메타 규칙을 검증한다(정적 경계/비물질화 invariant).
    std::vector<VerifyError> verify_escape_handles(const Module& m) {
        std::vector<VerifyError> errs;
        std::vector<uint8_t> escape_has_meta(m.values.size(), 0);

        for (uint32_t i = 0; i < (uint32_t)m.escape_handles.size(); ++i) {
            const auto& h = m.escape_handles[i];

            if (h.escape_value == k_invalid_value || (size_t)h.escape_value >= m.values.size()) {
                std::ostringstream oss;
                oss << "escape-handle #" << i << " has invalid value id " << h.escape_value;
                push_error_(errs, oss.str());
                continue;
            }

            const auto& v = m.values[h.escape_value];
            if (v.kind != ValueKind::kEscape) {
                std::ostringstream oss;
                oss << "escape-handle #" << i << " points to non-escape value #" << h.escape_value;
                push_error_(errs, oss.str());
            } else {
                escape_has_meta[h.escape_value] = 1;
            }

            if (h.materialize_count != 0) {
                std::ostringstream oss;
                oss << "escape-handle #" << i << " materialize_count must be 0 before OIR lowering (got "
                    << h.materialize_count << ")";
                push_error_(errs, oss.str());
            }

            if (!h.from_static && h.boundary == EscapeBoundaryKind::kNone) {
                std::ostringstream oss;
                oss << "escape-handle #" << i
                    << " violates static/boundary rule (non-static origin with boundary=none)";
                push_error_(errs, oss.str());
            }

            if ((h.boundary == EscapeBoundaryKind::kReturn || h.boundary == EscapeBoundaryKind::kCallArg) &&
                h.kind != EscapeHandleKind::kCallerSlot) {
                std::ostringstream oss;
                oss << "escape-handle #" << i
                    << " boundary=" << boundary_name_(h.boundary)
                    << " requires kind=caller_slot (got " << kind_name_(h.kind) << ")";
                push_error_(errs, oss.str());
            }

            if (h.kind == EscapeHandleKind::kHeapBox) {
                std::ostringstream oss;
                oss << "escape-handle #" << i << " uses heap_box kind, which is forbidden in v0";
                push_error_(errs, oss.str());
            }

            if (h.abi_pack_required &&
                !(h.boundary == EscapeBoundaryKind::kAbi || h.boundary == EscapeBoundaryKind::kFfi)) {
                std::ostringstream oss;
                oss << "escape-handle #" << i
                    << " abi_pack_required=true but boundary is " << boundary_name_(h.boundary);
                push_error_(errs, oss.str());
            }

            if (h.ffi_pack_required && h.boundary != EscapeBoundaryKind::kFfi) {
                std::ostringstream oss;
                oss << "escape-handle #" << i
                    << " ffi_pack_required=true but boundary is " << boundary_name_(h.boundary);
                push_error_(errs, oss.str());
            }
        }

        for (uint32_t vid = 0; vid < (uint32_t)m.values.size(); ++vid) {
            if (m.values[vid].kind != ValueKind::kEscape) continue;
            if (escape_has_meta[vid]) continue;
            std::ostringstream oss;
            oss << "escape value #" << vid << " has no EscapeHandle metadata";
            push_error_(errs, oss.str());
        }

        return errs;
    }

} // namespace gaupel::sir

