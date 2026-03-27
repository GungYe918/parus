// frontend/src/sir/verify/sir_escape_handle_verify.cpp
#include <parus/sir/Verify.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace parus::sir {

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

        const char* value_form_name_(EscapeValueForm k) {
            switch (k) {
                case EscapeValueForm::kRValue: return "rvalue";
                case EscapeValueForm::kCell: return "cell";
                case EscapeValueForm::kAbiPack: return "abi_pack";
            }
            return "unknown";
        }

        const char* cell_kind_name_(EscapeCellKind k) {
            switch (k) {
                case EscapeCellKind::kNone: return "none";
                case EscapeCellKind::kLocal: return "local";
                case EscapeCellKind::kField: return "field";
                case EscapeCellKind::kStatic: return "static";
                case EscapeCellKind::kOptional: return "optional";
            }
            return "unknown";
        }

        const char* pack_boundary_name_(EscapePackBoundary k) {
            switch (k) {
                case EscapePackBoundary::kNone: return "none";
                case EscapePackBoundary::kCallArg: return "call_arg";
                case EscapePackBoundary::kReturn: return "return";
            }
            return "unknown";
        }

        /// @brief 심볼이 static 저장소인지 확인하기 위해 static 심볼 집합을 만든다.
        std::unordered_set<SymbolId> build_static_symbols_(const Module& m) {
            std::unordered_set<SymbolId> out;
            for (const auto& g : m.globals) {
                if (g.is_static && g.sym != k_invalid_symbol) out.insert(g.sym);
            }
            for (const auto& s : m.stmts) {
                if (s.kind != StmtKind::kVarDecl) continue;
                if (s.is_static && s.sym != k_invalid_symbol) out.insert(s.sym);
            }
            return out;
        }

    } // namespace

    /// @brief EscapeHandle 메타 규칙을 검증한다(cell commit / ABI pack invariant).
    std::vector<VerifyError> verify_escape_handles(const Module& m) {
        std::vector<VerifyError> errs;
        std::vector<uint8_t> escape_has_meta(m.values.size(), 0);
        const auto static_symbols = build_static_symbols_(m);

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

            if (h.from_static) {
                if (h.origin_sym == k_invalid_symbol ||
                    static_symbols.find(h.origin_sym) == static_symbols.end()) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " marked from_static=true but origin symbol is not static";
                    push_error_(errs, oss.str());
                }
            }

            if (h.kind == EscapeHandleKind::kHeapBox) {
                std::ostringstream oss;
                oss << "escape-handle #" << i << " uses heap_box kind, which is forbidden in v0";
                push_error_(errs, oss.str());
            }

            if (h.value_form == EscapeValueForm::kRValue) {
                if (h.cell_kind != EscapeCellKind::kNone || h.pack_boundary != EscapePackBoundary::kNone ||
                    h.cell_commit_count != 0 || h.abi_pack_count != 0) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " form=" << value_form_name_(h.value_form)
                        << " must not record cell/pack accounting";
                    push_error_(errs, oss.str());
                }
                if (h.kind != EscapeHandleKind::kTrivial || h.boundary != EscapeBoundaryKind::kNone ||
                    h.abi_pack_required) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " rvalue form must stay trivial and non-packed";
                    push_error_(errs, oss.str());
                }
            } else if (h.value_form == EscapeValueForm::kCell) {
                if (h.cell_kind == EscapeCellKind::kNone || h.cell_commit_count == 0) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " cell form requires a concrete cell_kind and nonzero cell_commit_count"
                        << " (got cell_kind=" << cell_kind_name_(h.cell_kind) << ")";
                    push_error_(errs, oss.str());
                }
                if (h.pack_boundary != EscapePackBoundary::kNone || h.abi_pack_count != 0 || h.abi_pack_required) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " cell form must not request ABI packing";
                    push_error_(errs, oss.str());
                }
                if (h.boundary != EscapeBoundaryKind::kNone || h.kind != EscapeHandleKind::kStackSlot) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " cell form must use boundary=none and kind=stack_slot (got boundary="
                        << boundary_name_(h.boundary) << ", kind=" << kind_name_(h.kind) << ")";
                    push_error_(errs, oss.str());
                }
            } else if (h.value_form == EscapeValueForm::kAbiPack) {
                if (h.pack_boundary == EscapePackBoundary::kNone || h.abi_pack_count == 0 || !h.abi_pack_required) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " abi_pack form requires pack_boundary, abi_pack_count, and abi_pack_required=true";
                    push_error_(errs, oss.str());
                }
                if (h.cell_kind != EscapeCellKind::kNone || h.cell_commit_count != 0) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " abi_pack form must not record cell commit accounting";
                    push_error_(errs, oss.str());
                }
                const bool boundary_ok =
                    (h.boundary == EscapeBoundaryKind::kReturn && h.pack_boundary == EscapePackBoundary::kReturn) ||
                    (h.boundary == EscapeBoundaryKind::kCallArg && h.pack_boundary == EscapePackBoundary::kCallArg);
                if (!boundary_ok || h.kind != EscapeHandleKind::kCallerSlot) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " abi_pack form requires matching return/call_arg boundary and kind=caller_slot"
                        << " (got boundary=" << boundary_name_(h.boundary)
                        << ", pack_boundary=" << pack_boundary_name_(h.pack_boundary)
                        << ", kind=" << kind_name_(h.kind) << ")";
                    push_error_(errs, oss.str());
                }
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

} // namespace parus::sir
