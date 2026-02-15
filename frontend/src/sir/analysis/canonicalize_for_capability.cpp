#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace parus::sir {

    namespace {

        /// @brief EffectClass를 보수적으로 합친다.
        EffectClass join_effect_(EffectClass a, EffectClass b) {
            auto rank = [](EffectClass e) -> int {
                switch (e) {
                    case EffectClass::kPure: return 0;
                    case EffectClass::kMayWrite: return 1;
                    case EffectClass::kUnknown: return 2;
                }
                return 2;
            };
            return (rank(a) >= rank(b)) ? a : b;
        }

        /// @brief ValueKind 기반의 기본 effect를 계산한다.
        EffectClass base_effect_(ValueKind k) {
            switch (k) {
                case ValueKind::kAssign:
                case ValueKind::kPostfixInc:
                case ValueKind::kEscape:
                    return EffectClass::kMayWrite;
                case ValueKind::kCall:
                    return EffectClass::kUnknown;
                default:
                    return EffectClass::kPure;
            }
        }

        /// @brief v가 slice range index(`a..b`, `a..:b`)인지 확인한다.
        bool is_range_index_(const Module& m, const Value& v) {
            if (v.kind != ValueKind::kIndex) return false;
            if (v.b == k_invalid_value || (size_t)v.b >= m.values.size()) return false;
            const auto& idx = m.values[v.b];
            if (idx.kind != ValueKind::kBinary) return false;
            return idx.op == (uint32_t)syntax::TokenKind::kDotDot
                || idx.op == (uint32_t)syntax::TokenKind::kDotDotColon;
        }

        /// @brief 값에서 root symbol(local/global/index/field 경유)을 추출한다.
        SymbolId root_symbol_from_value_(
            const Module& m,
            ValueId vid,
            std::vector<uint8_t>& visiting
        ) {
            if (vid == k_invalid_value || (size_t)vid >= m.values.size()) return k_invalid_symbol;
            if (visiting[vid]) return k_invalid_symbol;
            visiting[vid] = 1;

            const auto& v = m.values[vid];
            switch (v.kind) {
                case ValueKind::kLocal:
                case ValueKind::kGlobal:
                    visiting[vid] = 0;
                    return v.sym;

                case ValueKind::kIndex:
                case ValueKind::kField: {
                    const SymbolId s = root_symbol_from_value_(m, v.a, visiting);
                    visiting[vid] = 0;
                    return s;
                }

                case ValueKind::kBorrow:
                case ValueKind::kEscape: {
                    const SymbolId s = root_symbol_from_value_(m, v.a, visiting);
                    visiting[vid] = 0;
                    return s;
                }

                default:
                    visiting[vid] = 0;
                    return k_invalid_symbol;
            }
        }

        /// @brief 인자 구간을 안전하게 clamp한다.
        void clamp_arg_slice_(const std::vector<Arg>& args, uint32_t& begin, uint32_t& count) {
            if (begin > args.size()) {
                begin = (uint32_t)args.size();
                count = 0;
                return;
            }
            const uint64_t end = (uint64_t)begin + (uint64_t)count;
            if (end > (uint64_t)args.size()) {
                count = (uint32_t)((uint64_t)args.size() - (uint64_t)begin);
            }
        }

        /// @brief call/array value의 arg slice를 canonical form으로 재구성한다.
        void canonicalize_arg_slices_(Module& m, CanonicalizeResult& out) {
            if (m.args.empty()) return;

            const std::vector<Arg> old_args = m.args;
            std::vector<Arg> new_args;
            new_args.reserve(old_args.size());

            for (auto& v : m.values) {
                if (v.kind != ValueKind::kCall && v.kind != ValueKind::kArrayLit) continue;

                uint32_t begin = v.arg_begin;
                uint32_t count = v.arg_count;
                clamp_arg_slice_(old_args, begin, count);

                const uint32_t new_begin = (uint32_t)new_args.size();
                uint32_t new_count = 0;

                for (uint32_t i = 0; i < count; ++i) {
                    const Arg& src = old_args[begin + i];

                    if (v.kind == ValueKind::kCall && src.kind == ArgKind::kNamedGroup) {
                        Arg parent = src;
                        parent.child_begin = 0;
                        parent.child_count = 0;

                        const uint32_t parent_idx = (uint32_t)new_args.size();
                        new_args.push_back(parent);
                        ++new_count;

                        uint32_t child_begin = src.child_begin;
                        uint32_t child_count = src.child_count;
                        clamp_arg_slice_(old_args, child_begin, child_count);

                        const uint32_t packed_child_begin = (uint32_t)new_args.size();
                        uint32_t packed_child_count = 0;

                        for (uint32_t j = 0; j < child_count; ++j) {
                            Arg child = old_args[child_begin + j];

                            // v0 canonical rule:
                            // - named-group 내부에서는 nested named-group을 허용하지 않는다.
                            // - 잘못 들어온 경우 positional로 강등해 후속 패스를 안정화한다.
                            if (child.kind == ArgKind::kNamedGroup) {
                                child.kind = ArgKind::kPositional;
                                child.child_begin = 0;
                                child.child_count = 0;
                            }

                            new_args.push_back(child);
                            ++new_count;
                            ++packed_child_count;
                        }

                        new_args[parent_idx].child_begin = packed_child_begin;
                        new_args[parent_idx].child_count = packed_child_count;
                        continue;
                    }

                    Arg plain = src;
                    plain.child_begin = 0;
                    plain.child_count = 0;
                    if (v.kind == ValueKind::kArrayLit && plain.kind == ArgKind::kNamedGroup) {
                        plain.kind = ArgKind::kPositional;
                    }
                    new_args.push_back(plain);
                    ++new_count;
                }

                if (v.arg_begin != new_begin || v.arg_count != new_count) {
                    ++out.rewritten_calls;
                }
                v.arg_begin = new_begin;
                v.arg_count = new_count;
            }

            m.args = std::move(new_args);
        }

    } // namespace

    /// @brief Cap 분석 전에 borrow/escape/call/index/field 중심으로 값을 정규화한다.
    CanonicalizeResult canonicalize_for_capability(
        Module& m,
        const ty::TypePool& types
    ) {
        (void)types; // 향후 타입 기반 canonicalization 확장을 위해 유지

        CanonicalizeResult out{};
        canonicalize_arg_slices_(m, out);

        std::vector<uint8_t> visiting(m.values.size(), 0);
        for (uint32_t vid = 0; vid < (uint32_t)m.values.size(); ++vid) {
            auto& v = m.values[vid];

            // -----------------------------
            // 1) place canonicalization
            // -----------------------------
            PlaceClass old_place = v.place;
            switch (v.kind) {
                case ValueKind::kLocal:
                case ValueKind::kGlobal:
                    v.place = PlaceClass::kLocal;
                    break;
                case ValueKind::kIndex:
                    v.place = is_range_index_(m, v) ? PlaceClass::kNotPlace : PlaceClass::kIndex;
                    break;
                case ValueKind::kField:
                    v.place = PlaceClass::kField;
                    break;
                default:
                    v.place = PlaceClass::kNotPlace;
                    break;
            }

            // -----------------------------
            // 2) origin symbol canonicalization
            // -----------------------------
            if (v.kind == ValueKind::kBorrow ||
                v.kind == ValueKind::kEscape ||
                v.kind == ValueKind::kIndex ||
                v.kind == ValueKind::kField) {
                const SymbolId root = root_symbol_from_value_(m, vid, visiting);
                if (root != k_invalid_symbol) {
                    if (v.origin_sym != root) {
                        v.origin_sym = root;
                    }
                }
            }

            // -----------------------------
            // 3) effect canonicalization
            // -----------------------------
            EffectClass new_eff = base_effect_(v.kind);
            auto join_child = [&](ValueId cid) {
                if (cid != k_invalid_value && (size_t)cid < m.values.size()) {
                    new_eff = join_effect_(new_eff, m.values[cid].effect);
                }
            };

            switch (v.kind) {
                case ValueKind::kUnary:
                case ValueKind::kBorrow:
                case ValueKind::kEscape:
                case ValueKind::kPostfixInc:
                case ValueKind::kCast:
                    join_child(v.a);
                    break;
                case ValueKind::kBinary:
                case ValueKind::kAssign:
                case ValueKind::kIndex:
                case ValueKind::kField:
                    join_child(v.a);
                    join_child(v.b);
                    break;
                case ValueKind::kIfExpr:
                    join_child(v.a);
                    join_child(v.b);
                    join_child(v.c);
                    break;
                case ValueKind::kCall: {
                    join_child(v.a);
                    const uint64_t end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                    if (end <= (uint64_t)m.args.size()) {
                        for (uint32_t i = 0; i < v.arg_count; ++i) {
                            const auto& a = m.args[v.arg_begin + i];
                            if (a.kind == ArgKind::kNamedGroup) {
                                const uint64_t cend = (uint64_t)a.child_begin + (uint64_t)a.child_count;
                                if (cend <= (uint64_t)m.args.size()) {
                                    for (uint32_t j = 0; j < a.child_count; ++j) {
                                        join_child(m.args[a.child_begin + j].value);
                                    }
                                }
                            } else {
                                join_child(a.value);
                            }
                        }
                    }
                    break;
                }
                case ValueKind::kArrayLit: {
                    const uint64_t end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                    if (end <= (uint64_t)m.args.size()) {
                        for (uint32_t i = 0; i < v.arg_count; ++i) {
                            join_child(m.args[v.arg_begin + i].value);
                        }
                    }
                    break;
                }
                default:
                    break;
            }

            if (v.effect != new_eff || v.place != old_place) {
                ++out.rewritten_values;
            }
            v.effect = new_eff;
        }

        return out;
    }

} // namespace parus::sir

