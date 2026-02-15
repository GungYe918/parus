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

        /// @brief place-like value에서 root symbol을 추적한다(local/index/field/global).
        std::optional<SymbolId> root_symbol_(const Module& m, ValueId vid) {
            if (vid == k_invalid_value || (size_t)vid >= m.values.size()) return std::nullopt;
            const auto& v = m.values[vid];
            if (v.origin_sym != k_invalid_symbol) return v.origin_sym;
            switch (v.kind) {
                case ValueKind::kLocal:
                case ValueKind::kGlobal:
                    return (v.sym == k_invalid_symbol) ? std::nullopt : std::optional<SymbolId>{v.sym};
                case ValueKind::kIndex:
                case ValueKind::kField:
                    return root_symbol_(m, v.a);
                default:
                    return std::nullopt;
            }
        }

        /// @brief 대입 lhs가 static place인지 판정한다.
        bool is_static_place_(const Module& m, ValueId lhs, const std::unordered_set<SymbolId>& static_symbols) {
            const auto root = root_symbol_(m, lhs);
            if (!root) return false;
            return static_symbols.find(*root) != static_symbols.end();
        }

    } // namespace

    /// @brief EscapeHandle 메타 규칙을 검증한다(정적 경계/비물질화 invariant).
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

            if (h.from_static) {
                if (h.origin_sym == k_invalid_symbol ||
                    static_symbols.find(h.origin_sym) == static_symbols.end()) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " marked from_static=true but origin symbol is not static";
                    push_error_(errs, oss.str());
                }
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

            if (h.boundary == EscapeBoundaryKind::kNone) {
                if (h.kind != EscapeHandleKind::kTrivial) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " boundary=none must keep trivial non-materialized kind (got "
                        << kind_name_(h.kind) << ")";
                    push_error_(errs, oss.str());
                }
                if (h.abi_pack_required || h.ffi_pack_required) {
                    std::ostringstream oss;
                    oss << "escape-handle #" << i
                        << " boundary=none cannot request ABI/FFI packing";
                    push_error_(errs, oss.str());
                }
            }
        }

        // non-static 바인딩으로 escape 토큰을 물질화하는 경로를 금지한다.
        for (uint32_t sid = 0; sid < (uint32_t)m.stmts.size(); ++sid) {
            const auto& s = m.stmts[sid];
            if (s.kind != StmtKind::kVarDecl) continue;
            if (s.init == k_invalid_value || (size_t)s.init >= m.values.size()) continue;
            if (m.values[s.init].kind != ValueKind::kEscape) continue;
            if (s.is_static) continue;

            std::ostringstream oss;
            oss << "stmt #" << sid
                << " materializes escape handle into non-static variable declaration";
            push_error_(errs, oss.str());
        }

        for (uint32_t vid = 0; vid < (uint32_t)m.values.size(); ++vid) {
            const auto& v = m.values[vid];
            if (v.kind != ValueKind::kAssign) continue;
            if (v.b == k_invalid_value || (size_t)v.b >= m.values.size()) continue;
            if (m.values[v.b].kind != ValueKind::kEscape) continue;
            if (is_static_place_(m, v.a, static_symbols)) continue;

            std::ostringstream oss;
            oss << "value #" << vid
                << " materializes escape handle into non-static assignment target";
            push_error_(errs, oss.str());
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
