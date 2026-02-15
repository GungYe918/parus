// compiler/src/oir/oir_passes.cpp
#include <parus/oir/Passes.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parus::oir {

    namespace {

        struct FlowValue {
            bool known = false;
            ValueId value = kInvalidId;
        };

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
        void rewrite_operands_(
            Module& m,
            const std::unordered_map<ValueId, ValueId>& repl,
            uint32_t* rewrite_counter = nullptr
        ) {
            auto apply = [&](ValueId& v) {
                if (v == kInvalidId) return;
                const ValueId nv = resolve_alias_(repl, v);
                if (nv != v && rewrite_counter != nullptr) {
                    *rewrite_counter += 1;
                }
                v = nv;
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

        /// @brief 함수 소속 블록 마스크를 생성한다.
        std::vector<uint8_t> build_owned_block_mask_(const Module& m, const Function& f) {
            std::vector<uint8_t> owned(m.blocks.size(), 0);
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                owned[bb] = 1;
            }
            return owned;
        }

        /// @brief 함수 CFG의 predecessor 리스트를 만든다.
        std::vector<std::vector<BlockId>> build_preds_(const Module& m, const Function& f) {
            std::vector<std::vector<BlockId>> preds(m.blocks.size());
            const auto owned = build_owned_block_mask_(m, f);

            auto add_pred = [&](BlockId from, BlockId to) {
                if (to == kInvalidId || (size_t)to >= m.blocks.size()) return;
                if (!owned[to]) return;
                preds[to].push_back(from);
            };

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                const auto& b = m.blocks[bb];
                if (!b.has_term) continue;
                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermBr>) {
                        add_pred(bb, t.target);
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        add_pred(bb, t.then_bb);
                        add_pred(bb, t.else_bb);
                    } else if constexpr (std::is_same_v<T, TermRet>) {
                        // no successor
                    }
                }, b.term);
            }
            return preds;
        }

        /// @brief terminator의 successor 개수를 구한다(중복 타깃은 1로 취급).
        uint32_t succ_count_(const Terminator& term) {
            if (std::holds_alternative<TermBr>(term)) {
                return 1;
            }
            if (std::holds_alternative<TermCondBr>(term)) {
                const auto& t = std::get<TermCondBr>(term);
                return (t.then_bb == t.else_bb) ? 1u : 2u;
            }
            return 0;
        }

        /// @brief 특정 간선(pred -> target)에 block-arg를 추가한다.
        void append_edge_arg_(Module& m, BlockId pred, BlockId target, ValueId arg) {
            if ((size_t)pred >= m.blocks.size()) return;
            auto& b = m.blocks[pred];
            if (!b.has_term) return;

            std::visit([&](auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, TermBr>) {
                    if (t.target == target) t.args.push_back(arg);
                } else if constexpr (std::is_same_v<T, TermCondBr>) {
                    if (t.then_bb == target) t.then_args.push_back(arg);
                    if (t.else_bb == target) t.else_args.push_back(arg);
                } else if constexpr (std::is_same_v<T, TermRet>) {
                    // no edge
                }
            }, b.term);
        }

        /// @brief block parameter(SSA phi 유사값)를 추가한다.
        ValueId add_block_param_(Module& m, BlockId bb, TypeId ty) {
            auto& block = m.blocks[bb];
            const uint32_t idx = (uint32_t)block.params.size();
            Value v{};
            v.ty = ty;
            v.eff = Effect::Pure;
            v.def_a = bb;
            v.def_b = idx;
            const ValueId vid = m.add_value(v);
            block.params.push_back(vid);
            return vid;
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

        /// @brief critical-edge를 split해서 이후 SSA/mem2reg 패스의 단순성을 확보한다.
        bool split_critical_edges_(Module& m, Function& f) {
            bool changed = false;

            // 간선 split 시 CFG가 변하므로 고정점까지 반복한다.
            for (;;) {
                bool round_changed = false;
                const auto owned = build_owned_block_mask_(m, f);
                const auto preds = build_preds_(m, f);

                for (auto pred : f.blocks) {
                    if (pred == kInvalidId || (size_t)pred >= m.blocks.size()) continue;
                    auto& pb = m.blocks[pred];
                    if (!pb.has_term) continue;

                    const uint32_t succ_count = succ_count_(pb.term);
                    if (succ_count <= 1) continue;
                    if (!std::holds_alternative<TermCondBr>(pb.term)) continue;

                    auto t = std::get<TermCondBr>(pb.term);
                    bool term_changed = false;

                    auto split_side = [&](bool then_side) {
                        const BlockId succ = then_side ? t.then_bb : t.else_bb;
                        if (succ == kInvalidId || (size_t)succ >= m.blocks.size()) return;
                        if (!owned[succ]) return;
                        if (preds[succ].size() <= 1) return;

                        std::vector<ValueId> edge_args =
                            then_side ? t.then_args : t.else_args;

                        const BlockId mid = m.add_block(Block{});
                        f.blocks.push_back(mid);

                        TermBr mid_term{};
                        mid_term.target = succ;
                        mid_term.args = std::move(edge_args);
                        m.blocks[mid].term = std::move(mid_term);
                        m.blocks[mid].has_term = true;

                        if (then_side) {
                            t.then_bb = mid;
                            t.then_args.clear();
                        } else {
                            t.else_bb = mid;
                            t.else_args.clear();
                        }

                        m.opt_stats.critical_edges_split += 1;
                        term_changed = true;
                        round_changed = true;
                    };

                    split_side(true);
                    split_side(false);

                    if (term_changed) {
                        pb.term = std::move(t);
                    }
                }

                if (!round_changed) break;
                changed = true;
            }

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
                        case BinOp::NullCoalesce: {
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

                    if (inst.eff == Effect::Call || inst.eff == Effect::MayWriteMem || inst.eff == Effect::MayTrap) {
                        slot_value.clear();
                    }
                }
            }

            if (changed) rewrite_operands_(m, repl);
            return changed;
        }

        /// @brief FlowValue 비교.
        bool flow_equal_(const FlowValue& a, const FlowValue& b) {
            if (a.known != b.known) return false;
            if (!a.known) return true;
            return a.value == b.value;
        }

        /// @brief slot 데이터플로우의 meet 연산.
        FlowValue meet_preds_(
            BlockId bb,
            const std::vector<std::vector<BlockId>>& preds,
            const std::vector<FlowValue>& out_state
        ) {
            if ((size_t)bb >= preds.size()) return FlowValue{};
            const auto& ps = preds[bb];
            if (ps.empty()) return FlowValue{};

            FlowValue cur = out_state[ps[0]];
            if (!cur.known) return FlowValue{};
            for (size_t i = 1; i < ps.size(); ++i) {
                const auto& ov = out_state[ps[i]];
                if (!ov.known || ov.value != cur.value) {
                    return FlowValue{};
                }
            }
            return cur;
        }

        /// @brief slot이 load/store slot 위치에서만 사용되는지 검사한다.
        bool is_non_escaping_slot_(const Module& m, const Function& f, ValueId slot) {
            auto used_as_escape = [&](const Inst& inst) -> bool {
                return std::visit([&](auto&& x) -> bool {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, InstLoad>) {
                        return false;
                    } else if constexpr (std::is_same_v<T, InstStore>) {
                        return x.value == slot;
                    } else if constexpr (std::is_same_v<T, InstUnary>) {
                        return x.src == slot;
                    } else if constexpr (std::is_same_v<T, InstBinOp>) {
                        return x.lhs == slot || x.rhs == slot;
                    } else if constexpr (std::is_same_v<T, InstCast>) {
                        return x.src == slot;
                    } else if constexpr (std::is_same_v<T, InstCall>) {
                        if (x.callee == slot) return true;
                        for (auto a : x.args) if (a == slot) return true;
                        return false;
                    } else if constexpr (std::is_same_v<T, InstIndex>) {
                        return x.base == slot || x.index == slot;
                    } else if constexpr (std::is_same_v<T, InstField>) {
                        return x.base == slot;
                    } else if constexpr (std::is_same_v<T, InstConstInt> ||
                                         std::is_same_v<T, InstConstBool> ||
                                         std::is_same_v<T, InstConstNull> ||
                                         std::is_same_v<T, InstAllocaLocal>) {
                        return false;
                    }
                }, inst.data);
            };

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                const auto& block = m.blocks[bb];
                for (auto iid : block.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];

                    if (std::holds_alternative<InstLoad>(inst.data)) {
                        const auto& l = std::get<InstLoad>(inst.data);
                        if (l.slot == slot) continue;
                    } else if (std::holds_alternative<InstStore>(inst.data)) {
                        const auto& s = std::get<InstStore>(inst.data);
                        if (s.slot == slot) {
                            if (s.value == slot) return false;
                            continue;
                        }
                    }

                    if (used_as_escape(inst)) return false;
                }

                if (!block.has_term) continue;
                bool escape_in_term = std::visit([&](auto&& t) -> bool {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermRet>) {
                        return t.has_value && t.value == slot;
                    } else if constexpr (std::is_same_v<T, TermBr>) {
                        for (auto a : t.args) if (a == slot) return true;
                        return false;
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        if (t.cond == slot) return true;
                        for (auto a : t.then_args) if (a == slot) return true;
                        for (auto a : t.else_args) if (a == slot) return true;
                        return false;
                    }
                }, block.term);
                if (escape_in_term) return false;
            }

            return true;
        }

        /// @brief slot 데이터플로우(in/out)를 고정점까지 계산한다.
        void compute_slot_flow_(
            const Module& m,
            const Function& f,
            ValueId slot,
            const std::vector<std::vector<BlockId>>& preds,
            const std::unordered_map<BlockId, ValueId>& phi_for_block,
            std::vector<FlowValue>& in_state,
            std::vector<FlowValue>& out_state
        ) {
            in_state.assign(m.blocks.size(), FlowValue{});
            out_state.assign(m.blocks.size(), FlowValue{});

            bool changed = false;
            do {
                changed = false;
                for (auto bb : f.blocks) {
                    if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                    const auto& block = m.blocks[bb];

                    FlowValue in = FlowValue{};
                    auto phi_it = phi_for_block.find(bb);
                    if (phi_it != phi_for_block.end()) {
                        in.known = true;
                        in.value = phi_it->second;
                    } else {
                        in = meet_preds_(bb, preds, out_state);
                    }

                    FlowValue cur = in;
                    for (auto iid : block.insts) {
                        if ((size_t)iid >= m.insts.size()) continue;
                        const auto& inst = m.insts[iid];
                        if (!std::holds_alternative<InstStore>(inst.data)) continue;
                        const auto& st = std::get<InstStore>(inst.data);
                        if (st.slot != slot) continue;
                        if (st.value == kInvalidId) {
                            cur = FlowValue{};
                        } else {
                            cur.known = true;
                            cur.value = st.value;
                        }
                    }

                    if (!flow_equal_(in_state[bb], in)) {
                        in_state[bb] = in;
                        changed = true;
                    }
                    if (!flow_equal_(out_state[bb], cur)) {
                        out_state[bb] = cur;
                        changed = true;
                    }
                }
            } while (changed);
        }

        /// @brief 서로 다른 predecessor 값이 합류하는 블록에 phi(block param)를 삽입한다.
        bool insert_slot_phi_(
            Module& m,
            Function& f,
            TypeId slot_ty,
            const std::vector<std::vector<BlockId>>& preds,
            const std::vector<FlowValue>& out_state,
            std::unordered_map<BlockId, ValueId>& phi_for_block
        ) {
            bool inserted = false;
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                if (phi_for_block.find(bb) != phi_for_block.end()) continue;
                if ((size_t)bb >= preds.size()) continue;

                const auto& ps = preds[bb];
                if (ps.size() < 2) continue;

                bool all_known = true;
                std::unordered_set<ValueId> uniq;
                for (auto p : ps) {
                    if ((size_t)p >= out_state.size() || !out_state[p].known) {
                        all_known = false;
                        break;
                    }
                    uniq.insert(out_state[p].value);
                }
                if (!all_known) continue;
                if (uniq.size() < 2) continue;

                const ValueId phi = add_block_param_(m, bb, slot_ty);
                phi_for_block[bb] = phi;
                m.opt_stats.mem2reg_phi_params += 1;

                for (auto p : ps) {
                    append_edge_arg_(m, p, bb, out_state[p].value);
                }
                inserted = true;
            }
            return inserted;
        }

        /// @brief slot 하나를 전역 mem2reg + SSA(block param)로 승격한다.
        bool promote_slot_global_(
            Module& m,
            Function& f,
            ValueId slot,
            TypeId slot_ty
        ) {
            const auto preds = build_preds_(m, f);
            std::unordered_map<BlockId, ValueId> phi_for_block;

            std::vector<FlowValue> in_state;
            std::vector<FlowValue> out_state;

            // phi 삽입이 멈출 때까지 반복
            for (;;) {
                compute_slot_flow_(m, f, slot, preds, phi_for_block, in_state, out_state);
                if (!insert_slot_phi_(m, f, slot_ty, preds, out_state, phi_for_block)) break;
            }
            compute_slot_flow_(m, f, slot, preds, phi_for_block, in_state, out_state);

            // 모든 load가 known 경로에서만 읽히는지 확인
            bool promotable = true;
            std::unordered_map<ValueId, ValueId> repl;

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                const auto& block = m.blocks[bb];
                FlowValue cur = ((size_t)bb < in_state.size()) ? in_state[bb] : FlowValue{};

                for (auto iid : block.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];
                    if (std::holds_alternative<InstStore>(inst.data)) {
                        const auto& st = std::get<InstStore>(inst.data);
                        if (st.slot == slot) {
                            if (st.value == kInvalidId) cur = FlowValue{};
                            else {
                                cur.known = true;
                                cur.value = st.value;
                            }
                        }
                        continue;
                    }
                    if (std::holds_alternative<InstLoad>(inst.data)) {
                        const auto& ld = std::get<InstLoad>(inst.data);
                        if (ld.slot != slot) continue;
                        if (!cur.known || inst.result == kInvalidId) {
                            promotable = false;
                            break;
                        }
                        repl[inst.result] = cur.value;
                    }
                }
                if (!promotable) break;
            }

            if (!promotable) return false;

            // operand 치환 후 slot 관련 inst 제거
            if (!repl.empty()) rewrite_operands_(m, repl);

            bool changed = false;
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                auto& block = m.blocks[bb];
                std::vector<InstId> kept;
                kept.reserve(block.insts.size());

                for (auto iid : block.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];
                    bool remove = false;

                    if (inst.result == slot && std::holds_alternative<InstAllocaLocal>(inst.data)) {
                        remove = true;
                    } else if (std::holds_alternative<InstLoad>(inst.data)) {
                        const auto& ld = std::get<InstLoad>(inst.data);
                        remove = (ld.slot == slot);
                    } else if (std::holds_alternative<InstStore>(inst.data)) {
                        const auto& st = std::get<InstStore>(inst.data);
                        remove = (st.slot == slot);
                    }

                    if (remove) {
                        changed = true;
                        continue;
                    }
                    kept.push_back(iid);
                }

                if (kept.size() != block.insts.size()) {
                    block.insts = std::move(kept);
                }
            }

            if (changed) {
                m.opt_stats.mem2reg_promoted_slots += 1;
            }
            return changed;
        }

        /// @brief 함수 전체에서 승격 가능한 alloca slot을 찾아 전역 mem2reg를 수행한다.
        bool global_mem2reg_ssa_(Module& m) {
            bool changed = false;

            for (auto& f : m.funcs) {
                // 반복 승격: 한 슬롯 승격이 다른 슬롯 승격을 가능하게 만들 수 있다.
                for (;;) {
                    bool round_changed = false;
                    std::vector<std::pair<ValueId, TypeId>> candidates;

                    for (auto bb : f.blocks) {
                        if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                        const auto& block = m.blocks[bb];
                        for (auto iid : block.insts) {
                            if ((size_t)iid >= m.insts.size()) continue;
                            const auto& inst = m.insts[iid];
                            if (!std::holds_alternative<InstAllocaLocal>(inst.data)) continue;
                            if (inst.result == kInvalidId) continue;
                            candidates.push_back({inst.result, std::get<InstAllocaLocal>(inst.data).slot_ty});
                        }
                    }

                    for (const auto& [slot, slot_ty] : candidates) {
                        if (!is_non_escaping_slot_(m, f, slot)) continue;
                        round_changed |= promote_slot_global_(m, f, slot, slot_ty);
                    }

                    if (!round_changed) break;
                    changed = true;
                }
            }

            return changed;
        }

        /// @brief escape-handle 경계에서 불필요한 캐스트/패킹을 제거한다.
        bool optimize_escape_handles_(Module& m) {
            if (m.escape_hints.empty()) return false;

            bool changed = false;
            std::unordered_set<ValueId> escape_values;
            escape_values.reserve(m.escape_hints.size() * 2 + 1);
            for (const auto& h : m.escape_hints) {
                escape_values.insert(h.value);
            }

            std::unordered_map<ValueId, ValueId> repl;
            for (auto& inst : m.insts) {
                if (!std::holds_alternative<InstCast>(inst.data)) continue;
                if (inst.result == kInvalidId || (size_t)inst.result >= m.values.size()) continue;

                const auto& c = std::get<InstCast>(inst.data);
                if (c.kind != CastKind::As) continue;
                if (escape_values.find(c.src) == escape_values.end()) continue;
                if ((size_t)c.src >= m.values.size()) continue;

                // 동일 타입 강제 cast는 escape 비물질화 경로에서 제거한다.
                if (m.values[c.src].ty != m.values[inst.result].ty) continue;

                repl[inst.result] = c.src;
                changed = true;
                m.opt_stats.escape_pack_elided += 1;
            }

            if (!repl.empty()) {
                uint32_t rewrites = 0;
                rewrite_operands_(m, repl, &rewrites);
                m.opt_stats.escape_boundary_rewrites += rewrites;

                for (auto& h : m.escape_hints) {
                    h.value = resolve_alias_(repl, h.value);
                }
            }

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
        // OIR 강화 파이프라인(v0):
        // 1) CFG 단순화
        // 2) critical-edge split
        // 3) 상수 폴딩
        // 4) 전역 mem2reg + SSA(block param)
        // 5) block-local forwarding 보조
        // 6) escape-handle 특화 정리
        // 7) pure DCE
        // 8) CFG 재정리
        (void)simplify_cfg_(m);
        for (auto& f : m.funcs) {
            (void)split_critical_edges_(m, f);
        }
        (void)const_fold_(m);
        (void)global_mem2reg_ssa_(m);
        (void)local_load_forward_(m);
        (void)optimize_escape_handles_(m);
        (void)dce_pure_insts_(m);
        (void)simplify_cfg_(m);
    }

} // namespace parus::oir
