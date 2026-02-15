// compiler/src/oir/oir_passes.cpp
#include <parus/oir/Passes.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace parus::oir {

    namespace {

        /// @brief ValueId 치환 테이블을 따라 최종 대표 값을 찾는다.
        ValueId resolve_alias_(const std::unordered_map<ValueId, ValueId>& repl, ValueId v) {
            ValueId cur = v;
            for (uint32_t i = 0; i < 64; ++i) {
                auto it = repl.find(cur);
                if (it == repl.end()) return cur;
                if (it->second == cur) return cur;
                cur = it->second;
            }
            return cur;
        }

        /// @brief inst/terminator의 operand를 순회하며 값 치환을 적용한다.
        void rewrite_operands_(Module& m, const std::unordered_map<ValueId, ValueId>& repl) {
            auto apply = [&](ValueId& v) {
                if (v == kInvalidId) return;
                v = resolve_alias_(repl, v);
            };

            for (auto& inst : m.insts) {
                std::visit([&](auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, InstUnary>) {
                        apply(x.src);
                    } else if constexpr (std::is_same_v<T, InstBinOp>) {
                        apply(x.lhs);
                        apply(x.rhs);
                    } else if constexpr (std::is_same_v<T, InstCast>) {
                        apply(x.src);
                    } else if constexpr (std::is_same_v<T, InstCall>) {
                        apply(x.callee);
                        for (auto& a : x.args) apply(a);
                    } else if constexpr (std::is_same_v<T, InstIndex>) {
                        apply(x.base);
                        apply(x.index);
                    } else if constexpr (std::is_same_v<T, InstField>) {
                        apply(x.base);
                    } else if constexpr (std::is_same_v<T, InstLoad>) {
                        apply(x.slot);
                    } else if constexpr (std::is_same_v<T, InstStore>) {
                        apply(x.slot);
                        apply(x.value);
                    } else if constexpr (std::is_same_v<T, InstConstInt> ||
                                         std::is_same_v<T, InstConstBool> ||
                                         std::is_same_v<T, InstConstNull> ||
                                         std::is_same_v<T, InstAllocaLocal>) {
                        // no operand
                    }
                }, inst.data);
            }

            for (auto& b : m.blocks) {
                if (!b.has_term) continue;
                std::visit([&](auto& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermRet>) {
                        if (t.has_value) apply(t.value);
                    } else if constexpr (std::is_same_v<T, TermBr>) {
                        for (auto& a : t.args) apply(a);
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        apply(t.cond);
                        for (auto& a : t.then_args) apply(a);
                        for (auto& a : t.else_args) apply(a);
                    }
                }, b.term);
            }
        }

        /// @brief 모듈 전체 value use count를 계산한다.
        std::vector<uint32_t> build_use_count_(const Module& m) {
            std::vector<uint32_t> uses(m.values.size(), 0);
            auto add = [&](ValueId v) {
                if (v == kInvalidId || (size_t)v >= uses.size()) return;
                uses[v] += 1;
            };

            for (const auto& inst : m.insts) {
                std::visit([&](auto&& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, InstUnary>) {
                        add(x.src);
                    } else if constexpr (std::is_same_v<T, InstBinOp>) {
                        add(x.lhs);
                        add(x.rhs);
                    } else if constexpr (std::is_same_v<T, InstCast>) {
                        add(x.src);
                    } else if constexpr (std::is_same_v<T, InstCall>) {
                        add(x.callee);
                        for (auto a : x.args) add(a);
                    } else if constexpr (std::is_same_v<T, InstIndex>) {
                        add(x.base);
                        add(x.index);
                    } else if constexpr (std::is_same_v<T, InstField>) {
                        add(x.base);
                    } else if constexpr (std::is_same_v<T, InstLoad>) {
                        add(x.slot);
                    } else if constexpr (std::is_same_v<T, InstStore>) {
                        add(x.slot);
                        add(x.value);
                    } else if constexpr (std::is_same_v<T, InstConstInt> ||
                                         std::is_same_v<T, InstConstBool> ||
                                         std::is_same_v<T, InstConstNull> ||
                                         std::is_same_v<T, InstAllocaLocal>) {
                        // no operand
                    }
                }, inst.data);
            }

            for (const auto& b : m.blocks) {
                if (!b.has_term) continue;
                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermRet>) {
                        if (t.has_value) add(t.value);
                    } else if constexpr (std::is_same_v<T, TermBr>) {
                        for (auto a : t.args) add(a);
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        add(t.cond);
                        for (auto a : t.then_args) add(a);
                        for (auto a : t.else_args) add(a);
                    }
                }, b.term);
            }
            return uses;
        }

        /// @brief condbr의 두 타깃이 동일하면 br로 단순화한다.
        bool simplify_condbr_same_target_(Module& m) {
            bool changed = false;
            for (auto& b : m.blocks) {
                if (!b.has_term) continue;
                if (!std::holds_alternative<TermCondBr>(b.term)) continue;
                const auto& c = std::get<TermCondBr>(b.term);
                if (c.then_bb != c.else_bb) continue;
                if (c.then_args != c.else_args) continue;

                TermBr nb{};
                nb.target = c.then_bb;
                nb.args = c.then_args;
                b.term = std::move(nb);
                changed = true;
            }
            return changed;
        }

        /// @brief 함수의 entry에서 도달 가능한 블록만 남긴다.
        bool remove_unreachable_blocks_(Module& m, Function& f) {
            if (f.entry == kInvalidId || (size_t)f.entry >= m.blocks.size()) return false;

            std::vector<uint8_t> reach(m.blocks.size(), 0);
            std::vector<BlockId> q;
            q.push_back(f.entry);
            reach[f.entry] = 1;

            size_t qi = 0;
            while (qi < q.size()) {
                const BlockId bb = q[qi++];
                if ((size_t)bb >= m.blocks.size()) continue;
                const auto& b = m.blocks[bb];
                if (!b.has_term) continue;

                auto push = [&](BlockId to) {
                    if (to == kInvalidId || (size_t)to >= m.blocks.size()) return;
                    if (reach[to]) return;
                    reach[to] = 1;
                    q.push_back(to);
                };

                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermBr>) {
                        push(t.target);
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        push(t.then_bb);
                        push(t.else_bb);
                    } else if constexpr (std::is_same_v<T, TermRet>) {
                        // no successor
                    }
                }, b.term);
            }

            std::vector<BlockId> kept;
            kept.reserve(f.blocks.size());
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                if (!reach[bb]) continue;
                kept.push_back(bb);
            }
            const bool changed = (kept.size() != f.blocks.size());
            f.blocks = std::move(kept);
            return changed;
        }

        /// @brief CFG 관련 단순화 패스(분기 단순화 + unreachable 제거).
        bool simplify_cfg_(Module& m) {
            bool changed = false;
            changed |= simplify_condbr_same_target_(m);
            for (auto& f : m.funcs) {
                changed |= remove_unreachable_blocks_(m, f);
            }
            return changed;
        }

        /// @brief 문자열 정수 리터럴 파싱(10진수, 접미사/구분자 일부 허용).
        bool parse_int_lit_(const std::string& s, int64_t& out) {
            std::string digits;
            digits.reserve(s.size());
            bool has_sign = false;
            for (size_t i = 0; i < s.size(); ++i) {
                char ch = s[i];
                if ((ch == '+' || ch == '-') && i == 0) {
                    digits.push_back(ch);
                    has_sign = true;
                    continue;
                }
                if (ch >= '0' && ch <= '9') {
                    digits.push_back(ch);
                    continue;
                }
                if (ch == '_') continue;
                break;
            }

            if (digits.empty() || (has_sign && digits.size() == 1)) return false;
            const char* begin = digits.data();
            const char* end = begin + digits.size();
            auto [p, ec] = std::from_chars(begin, end, out, 10);
            return ec == std::errc{} && p == end;
        }

        /// @brief 값 ID가 정수 상수인지 조회한다.
        bool as_const_int_(const Module& m, ValueId v, int64_t& out) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return false;
            const auto& vv = m.values[v];
            const uint32_t iid = vv.def_a;
            if (iid == kInvalidId || (size_t)iid >= m.insts.size()) return false;
            if (!std::holds_alternative<InstConstInt>(m.insts[iid].data)) return false;
            return parse_int_lit_(std::get<InstConstInt>(m.insts[iid].data).text, out);
        }

        /// @brief 값 ID가 bool 상수인지 조회한다.
        bool as_const_bool_(const Module& m, ValueId v, bool& out) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return false;
            const auto& vv = m.values[v];
            const uint32_t iid = vv.def_a;
            if (iid == kInvalidId || (size_t)iid >= m.insts.size()) return false;
            if (!std::holds_alternative<InstConstBool>(m.insts[iid].data)) return false;
            out = std::get<InstConstBool>(m.insts[iid].data).value;
            return true;
        }

        /// @brief 값 ID가 null 상수인지 조회한다.
        bool is_const_null_(const Module& m, ValueId v) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return false;
            const auto& vv = m.values[v];
            const uint32_t iid = vv.def_a;
            if (iid == kInvalidId || (size_t)iid >= m.insts.size()) return false;
            return std::holds_alternative<InstConstNull>(m.insts[iid].data);
        }

        /// @brief 기초 상수 폴딩(Add/Sub/Mul/Div/Rem/비교/논리 단항/NullCoalesce)을 수행한다.
        bool const_fold_(Module& m) {
            bool changed = false;
            for (auto& inst : m.insts) {
                if (inst.result == kInvalidId || (size_t)inst.result >= m.values.size()) continue;

                if (std::holds_alternative<InstUnary>(inst.data)) {
                    auto u = std::get<InstUnary>(inst.data);

                    int64_t iv = 0;
                    bool bv = false;
                    if ((u.op == UnOp::Neg || u.op == UnOp::Plus || u.op == UnOp::BitNot) &&
                        as_const_int_(m, u.src, iv)) {
                        int64_t ov = iv;
                        if (u.op == UnOp::Neg) ov = -iv;
                        if (u.op == UnOp::BitNot) ov = ~iv;
                        inst.data = InstConstInt{std::to_string(ov)};
                        inst.eff = Effect::Pure;
                        changed = true;
                        continue;
                    }
                    if (u.op == UnOp::Not && as_const_bool_(m, u.src, bv)) {
                        inst.data = InstConstBool{!bv};
                        inst.eff = Effect::Pure;
                        changed = true;
                        continue;
                    }
                }

                if (!std::holds_alternative<InstBinOp>(inst.data)) continue;
                auto b = std::get<InstBinOp>(inst.data);

                if (b.op == BinOp::NullCoalesce && is_const_null_(m, b.lhs)) {
                    // lhs가 null 상수면 rhs를 그대로 전달한다.
                    std::unordered_map<ValueId, ValueId> repl;
                    repl[inst.result] = b.rhs;
                    rewrite_operands_(m, repl);
                    changed = true;
                    continue;
                }

                int64_t li = 0, ri = 0;
                if (as_const_int_(m, b.lhs, li) && as_const_int_(m, b.rhs, ri)) {
                    switch (b.op) {
                        case BinOp::Add: inst.data = InstConstInt{std::to_string(li + ri)}; break;
                        case BinOp::Sub: inst.data = InstConstInt{std::to_string(li - ri)}; break;
                        case BinOp::Mul: inst.data = InstConstInt{std::to_string(li * ri)}; break;
                        case BinOp::Div: if (ri != 0) inst.data = InstConstInt{std::to_string(li / ri)}; else continue; break;
                        case BinOp::Rem: if (ri != 0) inst.data = InstConstInt{std::to_string(li % ri)}; else continue; break;
                        case BinOp::Lt:  inst.data = InstConstBool{li <  ri}; break;
                        case BinOp::Le:  inst.data = InstConstBool{li <= ri}; break;
                        case BinOp::Gt:  inst.data = InstConstBool{li >  ri}; break;
                        case BinOp::Ge:  inst.data = InstConstBool{li >= ri}; break;
                        case BinOp::Eq:  inst.data = InstConstBool{li == ri}; break;
                        case BinOp::Ne:  inst.data = InstConstBool{li != ri}; break;
                        case BinOp::NullCoalesce:
                            // int 상수 lhs는 null이 아니므로 lhs를 전달한다.
                            {
                                std::unordered_map<ValueId, ValueId> repl;
                                repl[inst.result] = b.lhs;
                                rewrite_operands_(m, repl);
                                changed = true;
                                continue;
                            }
                    }
                    inst.eff = Effect::Pure;
                    changed = true;
                }
            }
            return changed;
        }

        /// @brief block-local store->load 전달(mem2reg-lite) 수행.
        bool local_load_forward_(Module& m) {
            bool changed = false;
            std::unordered_map<ValueId, ValueId> repl;

            for (auto& b : m.blocks) {
                std::unordered_map<ValueId, ValueId> slot_value;

                for (auto iid : b.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];

                    if (std::holds_alternative<InstStore>(inst.data)) {
                        const auto& s = std::get<InstStore>(inst.data);
                        slot_value[s.slot] = resolve_alias_(repl, s.value);
                        continue;
                    }
                    if (std::holds_alternative<InstLoad>(inst.data)) {
                        const auto& l = std::get<InstLoad>(inst.data);
                        if (inst.result != kInvalidId) {
                            auto it = slot_value.find(l.slot);
                            if (it != slot_value.end()) {
                                repl[inst.result] = resolve_alias_(repl, it->second);
                                changed = true;
                            }
                        }
                        continue;
                    }

                    // call/불명확 write가 나오면 보수적으로 지운다.
                    if (inst.eff == Effect::Call || inst.eff == Effect::MayWriteMem || inst.eff == Effect::MayTrap) {
                        slot_value.clear();
                    }
                }
            }

            if (changed) rewrite_operands_(m, repl);
            return changed;
        }

        /// @brief 결과가 사용되지 않는 pure inst를 제거한다.
        bool dce_pure_insts_(Module& m) {
            bool changed = false;
            for (;;) {
                auto use_count = build_use_count_(m);
                bool round_changed = false;

                for (auto& b : m.blocks) {
                    std::vector<InstId> kept;
                    kept.reserve(b.insts.size());

                    for (auto iid : b.insts) {
                        if ((size_t)iid >= m.insts.size()) continue;
                        const auto& inst = m.insts[iid];

                        const bool has_result = inst.result != kInvalidId;
                        const bool unused = has_result && (size_t)inst.result < use_count.size() && use_count[inst.result] == 0;
                        const bool removable = unused && inst.eff == Effect::Pure;

                        if (removable) {
                            round_changed = true;
                            continue;
                        }
                        kept.push_back(iid);
                    }

                    if (kept.size() != b.insts.size()) b.insts = std::move(kept);
                }

                if (!round_changed) break;
                changed = true;
            }
            return changed;
        }

    } // namespace

    void run_passes(Module& m) {
        // OIR 기본 파이프라인(v0):
        // 1) CFG 단순화
        // 2) 상수 폴딩
        // 3) block-local mem2reg-lite(load forwarding)
        // 4) pure DCE
        // 5) CFG 재정리
        (void)simplify_cfg_(m);
        (void)const_fold_(m);
        (void)local_load_forward_(m);
        (void)dce_pure_insts_(m);
        (void)simplify_cfg_(m);
    }

} // namespace parus::oir
