// frontend/src/oir/oir_passes.cpp
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <queue>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace parus::oir {

    namespace {

        struct DomInfo {
            std::vector<BlockId> blocks;
            std::unordered_map<BlockId, uint32_t> index_of;

            std::vector<std::vector<BlockId>> preds_by_block;
            std::vector<std::vector<BlockId>> succs_by_block;

            // dom[b][d] == 1 이면 d가 b를 지배한다.
            std::vector<std::vector<uint8_t>> dom;
            std::vector<int32_t> idom;
            std::vector<std::vector<uint32_t>> dom_tree;
            std::vector<std::vector<uint32_t>> df;

            uint32_t entry_index = UINT32_MAX;
        };

        struct LoopDesc {
            BlockId header = kInvalidId;
            std::unordered_set<BlockId> blocks;
            std::vector<BlockId> latches;
            BlockId preheader = kInvalidId;
        };

        DomInfo build_dom_info_(const Module& m, const Function& f);
        bool dominates_(const DomInfo& dom, BlockId a, BlockId b);
        std::vector<LoopDesc> collect_loops_(const Module& m, const Function& f, const DomInfo& dom);
        bool is_preheader_block_(const Module& m, BlockId bb, BlockId header);

        /// @brief 메모리 접근이 어떤 별칭 범위를 건드리는지 요약한다.
        enum class AliasFootprint : uint8_t {
            None,
            LocalMemory,
            Unknown,
        };

        /// @brief 메모리 위치 추적 정밀도.
        enum class MemoryLocPrecision : uint8_t {
            Unknown,
            BaseOnly,
            Exact,
        };

        /// @brief OIR value가 가리키는 메모리 위치(기저 slot + 경로)를 요약한다.
        struct MemoryLocDesc {
            ValueId base_slot = kInvalidId;
            std::string path{};
            MemoryLocPrecision precision = MemoryLocPrecision::Unknown;
        };

        /// @brief OIR 명령의 effect/alias 요약 정보.
        struct InstAccessModel {
            bool reads = false;
            bool writes = false;
            bool may_call = false;
            bool may_trap = false;

            AliasFootprint read_fp = AliasFootprint::None;
            AliasFootprint write_fp = AliasFootprint::None;

            ValueId read_slot = kInvalidId;
            ValueId write_slot = kInvalidId;
            MemoryLocDesc read_loc{};
            MemoryLocDesc write_loc{};
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

        /// @brief 메모리 위치가 유효한 base slot을 갖는지 검사한다.
        bool has_memory_base_(const MemoryLocDesc& loc) {
            return loc.base_slot != kInvalidId;
        }

        /// @brief ValueId에서 기저 slot + projection 경로를 추출한다.
        MemoryLocDesc resolve_memory_loc_from_value_(
            const Module& m,
            ValueId v,
            uint32_t depth = 0
        ) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return {};
            if (depth > 32) return {};

            const auto& val = m.values[v];
            if (val.def_b != kInvalidId) {
                // block-param은 별도 메모리 위치 보증이 없다.
                return {};
            }
            if (val.def_a == kInvalidId || (size_t)val.def_a >= m.insts.size()) return {};

            const auto& inst = m.insts[val.def_a];
            if (std::holds_alternative<InstAllocaLocal>(inst.data)) {
                MemoryLocDesc out{};
                out.base_slot = v;
                out.path.clear();
                out.precision = MemoryLocPrecision::Exact;
                return out;
            }

            if (std::holds_alternative<InstField>(inst.data)) {
                const auto& fld = std::get<InstField>(inst.data);
                auto base = resolve_memory_loc_from_value_(m, fld.base, depth + 1);
                if (!has_memory_base_(base)) return {};
                base.path += ".";
                base.path += fld.field;
                return base;
            }

            if (std::holds_alternative<InstIndex>(inst.data)) {
                const auto& idx = std::get<InstIndex>(inst.data);
                auto base = resolve_memory_loc_from_value_(m, idx.base, depth + 1);
                if (!has_memory_base_(base)) return {};
                base.path += "[*]";
                if (base.precision == MemoryLocPrecision::Exact) {
                    base.precision = MemoryLocPrecision::BaseOnly;
                }
                return base;
            }

            if (std::holds_alternative<InstCast>(inst.data)) {
                const auto& c = std::get<InstCast>(inst.data);
                return resolve_memory_loc_from_value_(m, c.src, depth + 1);
            }

            return {};
        }

        /// @brief 두 메모리 위치가 반드시 같은 위치인지 판단한다.
        bool must_alias_memory_loc_(const MemoryLocDesc& a, const MemoryLocDesc& b) {
            if (!has_memory_base_(a) || !has_memory_base_(b)) return false;
            if (a.base_slot != b.base_slot) return false;
            if (a.precision != MemoryLocPrecision::Exact || b.precision != MemoryLocPrecision::Exact) return false;
            return a.path == b.path;
        }

        /// @brief 두 메모리 위치가 충돌할 가능성이 있는지 판단한다.
        bool may_alias_memory_loc_(const MemoryLocDesc& a, const MemoryLocDesc& b) {
            if (!has_memory_base_(a) || !has_memory_base_(b)) return true;
            if (a.base_slot != b.base_slot) return false;
            if (a.path == b.path) return true;
            if (a.precision == MemoryLocPrecision::Unknown || b.precision == MemoryLocPrecision::Unknown) return true;
            if (a.precision == MemoryLocPrecision::BaseOnly || b.precision == MemoryLocPrecision::BaseOnly) return true;
            return false;
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
                                         std::is_same_v<T, InstConstText> ||
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

        /// @brief 명령 단위로 메모리 영향/별칭 범위를 계산한다.
        InstAccessModel build_inst_access_model_(const Module& m, const Inst& inst) {
            InstAccessModel out{};

            // 데이터 형태 기반의 정밀 분류를 우선 적용한다.
            if (std::holds_alternative<InstLoad>(inst.data)) {
                const auto& ld = std::get<InstLoad>(inst.data);
                out.reads = true;
                out.read_loc = resolve_memory_loc_from_value_(m, ld.slot);
                if (has_memory_base_(out.read_loc)) {
                    out.read_fp = AliasFootprint::LocalMemory;
                    out.read_slot = out.read_loc.base_slot;
                } else {
                    out.read_fp = AliasFootprint::Unknown;
                }
            } else if (std::holds_alternative<InstStore>(inst.data)) {
                const auto& st = std::get<InstStore>(inst.data);
                out.writes = true;
                out.write_loc = resolve_memory_loc_from_value_(m, st.slot);
                if (has_memory_base_(out.write_loc)) {
                    out.write_fp = AliasFootprint::LocalMemory;
                    out.write_slot = out.write_loc.base_slot;
                } else {
                    out.write_fp = AliasFootprint::Unknown;
                }
            } else if (std::holds_alternative<InstCall>(inst.data)) {
                out.reads = true;
                out.writes = true;
                out.may_call = true;
                out.read_fp = AliasFootprint::Unknown;
                out.write_fp = AliasFootprint::Unknown;
            } else if (std::holds_alternative<InstIndex>(inst.data)) {
                // 인덱스/필드는 지역 slot 외의 메모리 뷰를 읽는 연산으로 취급한다.
                const auto& ix = std::get<InstIndex>(inst.data);
                out.read_loc = resolve_memory_loc_from_value_(m, ix.base);
                out.reads = true;
                if (has_memory_base_(out.read_loc)) {
                    out.read_fp = AliasFootprint::LocalMemory;
                    out.read_slot = out.read_loc.base_slot;
                } else {
                    out.read_fp = AliasFootprint::Unknown;
                }
            } else if (std::holds_alternative<InstField>(inst.data)) {
                const auto& fld = std::get<InstField>(inst.data);
                out.read_loc = resolve_memory_loc_from_value_(m, fld.base);
                out.reads = true;
                if (has_memory_base_(out.read_loc)) {
                    out.read_fp = AliasFootprint::LocalMemory;
                    out.read_slot = out.read_loc.base_slot;
                } else {
                    out.read_fp = AliasFootprint::Unknown;
                }
            } else if (std::holds_alternative<InstAllocaLocal>(inst.data)) {
                // alloca는 slot 생성 메타 동작으로 보고, 기존 메모리 가시 상태를 클로버하지 않는다.
                out.reads = false;
                out.writes = false;
                out.read_fp = AliasFootprint::None;
                out.write_fp = AliasFootprint::None;
            }

            // 기존 effect 플래그와 합성하여 보수성을 유지한다.
            if (inst.eff == Effect::Call) {
                out.may_call = true;
                out.reads = true;
                out.writes = true;
                out.read_fp = AliasFootprint::Unknown;
                out.write_fp = AliasFootprint::Unknown;
            } else if (inst.eff == Effect::MayWriteMem && out.write_fp == AliasFootprint::None) {
                out.writes = true;
                out.write_fp = AliasFootprint::Unknown;
            } else if (inst.eff == Effect::MayReadMem && out.read_fp == AliasFootprint::None) {
                out.reads = true;
                out.read_fp = AliasFootprint::Unknown;
            } else if (inst.eff == Effect::MayTrap) {
                out.may_trap = true;
            }

            return out;
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
                                         std::is_same_v<T, InstConstText> ||
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

        struct ValueDefLoc {
            bool known = false;
            bool is_block_param = false;
            BlockId bb = kInvalidId;
            uint32_t inst_ord = UINT32_MAX; // block 내 inst 순서(terminator 직전은 inst_count)
        };

        /// @brief inst operand을 순회한다.
        template <typename Fn>
        void for_each_inst_operand_(const Inst& inst, Fn&& fn) {
            std::visit([&](auto&& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, InstUnary>) {
                    fn(x.src);
                } else if constexpr (std::is_same_v<T, InstBinOp>) {
                    fn(x.lhs);
                    fn(x.rhs);
                } else if constexpr (std::is_same_v<T, InstCast>) {
                    fn(x.src);
                } else if constexpr (std::is_same_v<T, InstCall>) {
                    fn(x.callee);
                    for (auto a : x.args) fn(a);
                } else if constexpr (std::is_same_v<T, InstIndex>) {
                    fn(x.base);
                    fn(x.index);
                } else if constexpr (std::is_same_v<T, InstField>) {
                    fn(x.base);
                } else if constexpr (std::is_same_v<T, InstLoad>) {
                    fn(x.slot);
                } else if constexpr (std::is_same_v<T, InstStore>) {
                    fn(x.slot);
                    fn(x.value);
                } else if constexpr (std::is_same_v<T, InstConstInt> ||
                                     std::is_same_v<T, InstConstBool> ||
                                     std::is_same_v<T, InstConstText> ||
                                     std::is_same_v<T, InstConstNull> ||
                                     std::is_same_v<T, InstAllocaLocal>) {
                    // no operand
                }
            }, inst.data);
        }

        /// @brief terminator operand을 순회한다.
        template <typename Fn>
        void for_each_term_operand_(const Terminator& term, Fn&& fn) {
            std::visit([&](auto&& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, TermRet>) {
                    if (t.has_value) fn(t.value);
                } else if constexpr (std::is_same_v<T, TermBr>) {
                    for (auto a : t.args) fn(a);
                } else if constexpr (std::is_same_v<T, TermCondBr>) {
                    fn(t.cond);
                    for (auto a : t.then_args) fn(a);
                    for (auto a : t.else_args) fn(a);
                }
            }, term);
        }

        /// @brief 값이 특정 블록 위치에서 사용 가능한지(지배 + 동일 블록 순서) 검사한다.
        bool value_available_at_(
            const Module& m,
            const DomInfo& dom,
            const std::vector<ValueDefLoc>& defs,
            ValueId v,
            BlockId use_bb,
            uint32_t use_inst_ord
        ) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return false;

            const auto& vv = m.values[v];
            if ((size_t)v >= defs.size()) return false;

            const auto& d = defs[v];
            if (!d.known) {
                // mem2reg에서 만드는 pseudo-undef는 def 미지정 값을 허용한다.
                return vv.def_a == kInvalidId;
            }

            if (d.bb == use_bb) {
                if (d.is_block_param) return true;
                return d.inst_ord < use_inst_ord;
            }
            return dominates_(dom, d.bb, use_bb);
        }

        /// @brief 함수 단위로 SSA 지배 조건(Instruction dominates all uses)을 검사한다.
        bool verify_function_dominance_(const Module& m, const Function& f) {
            const auto dom = build_dom_info_(m, f);
            if (dom.entry_index == UINT32_MAX) return false;

            std::vector<ValueDefLoc> defs(m.values.size());

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                const auto& b = m.blocks[bb];
                for (auto p : b.params) {
                    if (p == kInvalidId || (size_t)p >= defs.size()) continue;
                    defs[p] = ValueDefLoc{
                        .known = true,
                        .is_block_param = true,
                        .bb = bb,
                        .inst_ord = 0
                    };
                }
                for (uint32_t i = 0; i < (uint32_t)b.insts.size(); ++i) {
                    const InstId iid = b.insts[i];
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];
                    if (inst.result == kInvalidId || (size_t)inst.result >= defs.size()) continue;
                    defs[inst.result] = ValueDefLoc{
                        .known = true,
                        .is_block_param = false,
                        .bb = bb,
                        .inst_ord = i + 1
                    };
                }
            }

            // 일반 inst/terminator use 검사
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                const auto& b = m.blocks[bb];

                for (uint32_t i = 0; i < (uint32_t)b.insts.size(); ++i) {
                    const InstId iid = b.insts[i];
                    if ((size_t)iid >= m.insts.size()) continue;
                    bool ok = true;
                    for_each_inst_operand_(m.insts[iid], [&](ValueId v) {
                        if (!value_available_at_(m, dom, defs, v, bb, i + 1)) ok = false;
                    });
                    if (!ok) return false;
                }

                if (!b.has_term) continue;
                const uint32_t term_ord = (uint32_t)b.insts.size() + 1;
                bool ok = true;
                for_each_term_operand_(b.term, [&](ValueId v) {
                    if (!value_available_at_(m, dom, defs, v, bb, term_ord)) ok = false;
                });
                if (!ok) return false;
            }

            // phi(block param) incoming edge use 검사
            for (auto pred : f.blocks) {
                if (pred == kInvalidId || (size_t)pred >= m.blocks.size()) continue;
                const auto& pb = m.blocks[pred];
                if (!pb.has_term) continue;

                auto check_edge_args = [&](BlockId succ, const std::vector<ValueId>& args) -> bool {
                    if (succ == kInvalidId || (size_t)succ >= m.blocks.size()) return true;
                    if (dom.index_of.find(succ) == dom.index_of.end()) return true;
                    const auto& target = m.blocks[succ];
                    const uint32_t n = std::min<uint32_t>((uint32_t)args.size(), (uint32_t)target.params.size());
                    const uint32_t edge_ord = (uint32_t)m.blocks[pred].insts.size() + 1;
                    for (uint32_t i = 0; i < n; ++i) {
                        if (!value_available_at_(m, dom, defs, args[i], pred, edge_ord)) return false;
                    }
                    return true;
                };

                bool ok = true;
                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermBr>) {
                        ok = check_edge_args(t.target, t.args);
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        ok = check_edge_args(t.then_bb, t.then_args) &&
                             check_edge_args(t.else_bb, t.else_args);
                    } else if constexpr (std::is_same_v<T, TermRet>) {
                        ok = true;
                    }
                }, pb.term);
                if (!ok) return false;
            }

            return true;
        }

        /// @brief 함수가 loop canonical form 고정점(preheader 유일성)을 만족하는지 검사한다.
        bool verify_function_loop_fixpoint_(const Module& m, const Function& f) {
            const auto dom = build_dom_info_(m, f);
            if (dom.entry_index == UINT32_MAX) return false;

            const auto loops = collect_loops_(m, f, dom);
            for (const auto& loop : loops) {
                std::vector<BlockId> outside_preds;
                for (auto p : dom.preds_by_block[loop.header]) {
                    if (!loop.blocks.count(p)) outside_preds.push_back(p);
                }
                if (outside_preds.empty()) return false;
                if (outside_preds.size() != 1) return false;
                if (!is_preheader_block_(m, outside_preds[0], loop.header)) return false;
            }
            return true;
        }

        /// @brief OIR 고급 최적화 단계 이후 필수 불변식을 검증한다.
        bool verify_pipeline_invariants_(
            const Module& m,
            bool require_loop_fixpoint
        ) {
            if (!verify(m).empty()) return false;
            for (const auto& f : m.funcs) {
                if (!verify_function_dominance_(m, f)) return false;
                if (require_loop_fixpoint && !verify_function_loop_fixpoint_(m, f)) return false;
            }
            return true;
        }

        /// @brief 패스를 1회 실행하고, 불변식 위반 시 즉시 롤백한다.
        template <typename Fn>
        bool run_guarded_pass_once_(
            Module& m,
            bool require_loop_fixpoint,
            Fn&& fn
        ) {
            Module snapshot = m;
            const bool changed = fn(m);
            if (!changed) return false;
            if (!verify_pipeline_invariants_(m, require_loop_fixpoint)) {
                m = std::move(snapshot);
                return false;
            }
            return true;
        }

        /// @brief 패스를 고정점까지 반복 실행하되, 실패 라운드는 롤백하고 중단한다.
        template <typename Fn>
        bool run_guarded_pass_fixpoint_(
            Module& m,
            bool require_loop_fixpoint,
            uint32_t max_rounds,
            Fn&& fn
        ) {
            bool any_changed = false;
            for (uint32_t round = 0; round < max_rounds; ++round) {
                Module snapshot = m;
                const bool changed = fn(m);
                if (!changed) break;
                if (!verify_pipeline_invariants_(m, require_loop_fixpoint)) {
                    m = std::move(snapshot);
                    break;
                }
                any_changed = true;
            }
            return any_changed;
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

        /// @brief 함수 CFG의 successor 리스트를 만든다.
        std::vector<std::vector<BlockId>> build_succs_(const Module& m, const Function& f) {
            std::vector<std::vector<BlockId>> succs(m.blocks.size());
            const auto owned = build_owned_block_mask_(m, f);

            auto add_succ = [&](BlockId from, BlockId to) {
                if (from == kInvalidId || (size_t)from >= m.blocks.size()) return;
                if (to == kInvalidId || (size_t)to >= m.blocks.size()) return;
                if (!owned[from] || !owned[to]) return;
                succs[from].push_back(to);
            };

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                const auto& b = m.blocks[bb];
                if (!b.has_term) continue;
                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermBr>) {
                        add_succ(bb, t.target);
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        add_succ(bb, t.then_bb);
                        add_succ(bb, t.else_bb);
                    } else if constexpr (std::is_same_v<T, TermRet>) {
                        // no successor
                    }
                }, b.term);
            }
            return succs;
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

        /// @brief 간선의 target을 old -> neu로 치환한다(인자 벡터는 유지).
        bool redirect_edge_target_(Module& m, BlockId pred, BlockId old_target, BlockId neu_target) {
            if ((size_t)pred >= m.blocks.size()) return false;
            auto& b = m.blocks[pred];
            if (!b.has_term) return false;

            bool changed = false;
            std::visit([&](auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, TermBr>) {
                    if (t.target == old_target) {
                        t.target = neu_target;
                        changed = true;
                    }
                } else if constexpr (std::is_same_v<T, TermCondBr>) {
                    if (t.then_bb == old_target) {
                        t.then_bb = neu_target;
                        changed = true;
                    }
                    if (t.else_bb == old_target) {
                        t.else_bb = neu_target;
                        changed = true;
                    }
                } else if constexpr (std::is_same_v<T, TermRet>) {
                    // no edge
                }
            }, b.term);
            return changed;
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

        /// @brief 규격 미정(undef) 값을 생성한다.
        ValueId make_undef_value_(Module& m, TypeId ty) {
            Value v{};
            v.ty = ty;
            v.eff = Effect::Pure;
            v.def_a = kInvalidId;
            v.def_b = kInvalidId;
            return m.add_value(v);
        }

        /// @brief 엣지 전용 블록에 타입 보정 cast 결과 값을 생성한다.
        ValueId make_edge_cast_value_(
            Module& m,
            BlockId edge_block,
            ValueId src,
            TypeId to_ty
        ) {
            Value out_val{};
            out_val.ty = to_ty;
            out_val.eff = Effect::Pure;
            const ValueId out_vid = m.add_value(out_val);

            Inst cast_inst{};
            cast_inst.data = InstCast{
                .kind = CastKind::As,
                .to = to_ty,
                .src = src
            };
            cast_inst.eff = Effect::Pure;
            cast_inst.result = out_vid;
            const InstId cast_iid = m.add_inst(cast_inst);

            m.values[out_vid].def_a = cast_iid;
            m.values[out_vid].def_b = kInvalidId;
            if ((size_t)edge_block < m.blocks.size()) {
                m.blocks[edge_block].insts.push_back(cast_iid);
            }
            return out_vid;
        }

        /// @brief 특정 edge arg 벡터를 target block param 타입으로 맞춘다.
        bool normalize_edge_args_to_target_types_(
            Module& m,
            BlockId edge_block,
            std::vector<ValueId>& edge_args,
            const Block& target_block
        ) {
            bool changed = false;
            const uint32_t n = std::min<uint32_t>(
                static_cast<uint32_t>(edge_args.size()),
                static_cast<uint32_t>(target_block.params.size())
            );

            for (uint32_t i = 0; i < n; ++i) {
                const ValueId arg = edge_args[i];
                const ValueId param = target_block.params[i];
                if (arg == kInvalidId || param == kInvalidId) continue;
                if ((size_t)arg >= m.values.size() || (size_t)param >= m.values.size()) continue;

                const TypeId arg_ty = m.values[arg].ty;
                const TypeId param_ty = m.values[param].ty;
                if (arg_ty == param_ty) continue;

                edge_args[i] = make_edge_cast_value_(m, edge_block, arg, param_ty);
                changed = true;
            }
            return changed;
        }

        /// @brief block-arg(phi 유사) incoming 타입 불일치를 edge-cast로 정규화한다.
        bool normalize_phi_edge_casts_(Module& m, Function& f) {
            bool changed = false;
            const auto owned = build_owned_block_mask_(m, f);

            for (size_t fi = 0; fi < f.blocks.size(); ++fi) {
                const BlockId pred = f.blocks[fi];
                if (pred == kInvalidId || (size_t)pred >= m.blocks.size()) continue;

                auto& pb = m.blocks[pred];
                if (!pb.has_term) continue;

                if (std::holds_alternative<TermBr>(pb.term)) {
                    auto& br = std::get<TermBr>(pb.term);
                    if (br.target == kInvalidId || (size_t)br.target >= m.blocks.size()) continue;
                    if (!owned[br.target]) continue;
                    changed |= normalize_edge_args_to_target_types_(
                        m,
                        pred,
                        br.args,
                        m.blocks[br.target]
                    );
                    continue;
                }

                if (!std::holds_alternative<TermCondBr>(pb.term)) continue;
                auto& cbr = std::get<TermCondBr>(pb.term);

                auto normalize_side = [&](bool then_side) {
                    BlockId& target = then_side ? cbr.then_bb : cbr.else_bb;
                    std::vector<ValueId>& side_args = then_side ? cbr.then_args : cbr.else_args;
                    if (target == kInvalidId || (size_t)target >= m.blocks.size()) return false;
                    if (!owned[target]) return false;

                    const auto& target_block = m.blocks[target];
                    const uint32_t n = std::min<uint32_t>(
                        static_cast<uint32_t>(side_args.size()),
                        static_cast<uint32_t>(target_block.params.size())
                    );

                    bool has_mismatch = false;
                    for (uint32_t i = 0; i < n; ++i) {
                        const ValueId arg = side_args[i];
                        const ValueId param = target_block.params[i];
                        if (arg == kInvalidId || param == kInvalidId) continue;
                        if ((size_t)arg >= m.values.size() || (size_t)param >= m.values.size()) continue;
                        if (m.values[arg].ty != m.values[param].ty) {
                            has_mismatch = true;
                            break;
                        }
                    }
                    if (!has_mismatch) return false;

                    // condbr의 한 분기에만 cast를 넣어야 의미 보존된다.
                    // 다중 successor에서는 edge split으로 전용 블록을 만든다.
                    if (succ_count_(pb.term) > 1) {
                        const BlockId orig_target = target;
                        const BlockId mid = m.add_block(Block{});
                        f.blocks.push_back(mid);

                        TermBr mid_term{};
                        mid_term.target = orig_target;
                        mid_term.args = std::move(side_args);
                        m.blocks[mid].term = std::move(mid_term);
                        m.blocks[mid].has_term = true;

                        target = mid;
                        side_args.clear();
                        m.opt_stats.critical_edges_split += 1;
                        changed = true;

                        auto& edge_term = std::get<TermBr>(m.blocks[mid].term);
                        return normalize_edge_args_to_target_types_(
                            m,
                            mid,
                            edge_term.args,
                            m.blocks[orig_target]
                        );
                    }

                    return normalize_edge_args_to_target_types_(
                        m,
                        pred,
                        side_args,
                        target_block
                    );
                };

                changed |= normalize_side(true);
                changed |= normalize_side(false);
            }

            return changed;
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

        /// @brief Dominator/IDom/DF를 계산한다.
        DomInfo build_dom_info_(const Module& m, const Function& f) {
            DomInfo info{};
            info.blocks = f.blocks;
            info.preds_by_block = build_preds_(m, f);
            info.succs_by_block = build_succs_(m, f);

            for (uint32_t i = 0; i < (uint32_t)info.blocks.size(); ++i) {
                info.index_of[info.blocks[i]] = i;
            }
            auto eit = info.index_of.find(f.entry);
            if (eit == info.index_of.end()) return info;
            info.entry_index = eit->second;

            const uint32_t n = (uint32_t)info.blocks.size();
            info.dom.assign(n, std::vector<uint8_t>(n, 1));
            info.idom.assign(n, -1);
            info.dom_tree.assign(n, {});
            info.df.assign(n, {});

            // 초기화: entry는 자기 자신만 지배.
            for (uint32_t d = 0; d < n; ++d) info.dom[info.entry_index][d] = 0;
            info.dom[info.entry_index][info.entry_index] = 1;

            bool changed = false;
            do {
                changed = false;
                for (uint32_t bi = 0; bi < n; ++bi) {
                    if (bi == info.entry_index) continue;
                    const BlockId bb = info.blocks[bi];
                    const auto& preds = info.preds_by_block[bb];

                    std::vector<uint8_t> ndom(n, 1);
                    bool has_pred = false;
                    for (auto pbb : preds) {
                        auto pit = info.index_of.find(pbb);
                        if (pit == info.index_of.end()) continue;
                        const uint32_t pi = pit->second;
                        if (!has_pred) {
                            ndom = info.dom[pi];
                            has_pred = true;
                        } else {
                            for (uint32_t d = 0; d < n; ++d) {
                                ndom[d] = (uint8_t)(ndom[d] & info.dom[pi][d]);
                            }
                        }
                    }
                    if (!has_pred) {
                        std::fill(ndom.begin(), ndom.end(), 0);
                    }
                    ndom[bi] = 1;

                    if (ndom != info.dom[bi]) {
                        info.dom[bi] = std::move(ndom);
                        changed = true;
                    }
                }
            } while (changed);

            // idom 계산
            for (uint32_t bi = 0; bi < n; ++bi) {
                if (bi == info.entry_index) {
                    info.idom[bi] = -1;
                    continue;
                }
                int32_t idom = -1;
                for (uint32_t d = 0; d < n; ++d) {
                    if (d == bi || !info.dom[bi][d]) continue;
                    bool dominated_by_other = false;
                    for (uint32_t o = 0; o < n; ++o) {
                        if (o == bi || o == d || !info.dom[bi][o]) continue;
                        if (info.dom[d][o]) { // o dominates d
                            dominated_by_other = true;
                            break;
                        }
                    }
                    if (!dominated_by_other) {
                        idom = (int32_t)d;
                        break;
                    }
                }
                info.idom[bi] = idom;
                if (idom >= 0) info.dom_tree[(uint32_t)idom].push_back(bi);
            }

            // DF 계산
            for (uint32_t bi = 0; bi < n; ++bi) {
                const BlockId bb = info.blocks[bi];
                const auto& preds = info.preds_by_block[bb];
                if (preds.size() < 2) continue;

                for (auto pbb : preds) {
                    auto pit = info.index_of.find(pbb);
                    if (pit == info.index_of.end()) continue;
                    int32_t runner = (int32_t)pit->second;
                    while (runner >= 0 && runner != info.idom[bi]) {
                        auto& dfr = info.df[(uint32_t)runner];
                        if (std::find(dfr.begin(), dfr.end(), bi) == dfr.end()) {
                            dfr.push_back(bi);
                        }
                        runner = info.idom[(uint32_t)runner];
                    }
                }
            }

            return info;
        }

        /// @brief a가 b를 지배하는지 검사한다.
        bool dominates_(const DomInfo& dom, BlockId a, BlockId b) {
            auto ia = dom.index_of.find(a);
            auto ib = dom.index_of.find(b);
            if (ia == dom.index_of.end() || ib == dom.index_of.end()) return false;
            return dom.dom[ib->second][ia->second] != 0;
        }

        /// @brief inst -> 소속 block 매핑을 만든다.
        std::unordered_map<InstId, BlockId> build_inst_block_map_(const Module& m, const Function& f) {
            std::unordered_map<InstId, BlockId> out;
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                for (auto iid : m.blocks[bb].insts) {
                    out[iid] = bb;
                }
            }
            return out;
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

                    const uint32_t sc = succ_count_(pb.term);
                    if (sc <= 1) continue;
                    if (!std::holds_alternative<TermCondBr>(pb.term)) continue;

                    auto t = std::get<TermCondBr>(pb.term);
                    bool term_changed = false;

                    auto split_side = [&](bool then_side) {
                        const BlockId succ = then_side ? t.then_bb : t.else_bb;
                        if (succ == kInvalidId || (size_t)succ >= m.blocks.size()) return;
                        if (!owned[succ]) return;
                        if (preds[succ].size() <= 1) return;

                        std::vector<ValueId> edge_args = then_side ? t.then_args : t.else_args;

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

                    if (term_changed) pb.term = std::move(t);
                }

                if (!round_changed) break;
                changed = true;
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
            // def_a/def_b 계약:
            // - inst result: def_a = inst_id, def_b = kInvalidId
            // - block param: def_a = block_id, def_b = param_index
            // block param을 inst로 오해하면 잘못된 상수 폴딩이 발생한다.
            if (vv.def_b != kInvalidId) return false;
            const uint32_t iid = vv.def_a;
            if (iid == kInvalidId || (size_t)iid >= m.insts.size()) return false;
            if (!std::holds_alternative<InstConstInt>(m.insts[iid].data)) return false;
            return parse_int_lit_(std::get<InstConstInt>(m.insts[iid].data).text, out);
        }

        /// @brief 값 ID가 bool 상수인지 조회한다.
        bool as_const_bool_(const Module& m, ValueId v, bool& out) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return false;
            const auto& vv = m.values[v];
            if (vv.def_b != kInvalidId) return false;
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
            if (vv.def_b != kInvalidId) return false;
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
                    } else if constexpr (std::is_same_v<T, InstFuncRef>) {
                        return false;
                    } else if constexpr (std::is_same_v<T, InstGlobalRef>) {
                        return false;
                    } else if constexpr (std::is_same_v<T, InstConstInt> ||
                                         std::is_same_v<T, InstConstBool> ||
                                         std::is_same_v<T, InstConstText> ||
                                         std::is_same_v<T, InstConstNull> ||
                                         std::is_same_v<T, InstAllocaLocal>) {
                        return false;
                    }
                    return false;
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

        /// @brief dominance frontier + rename로 slot 하나를 mem2reg 승격한다.
        bool promote_slot_mem2reg_(
            Module& m,
            Function& f,
            ValueId slot,
            TypeId slot_ty
        ) {
            const auto dom = build_dom_info_(m, f);
            if (dom.entry_index == UINT32_MAX) return false;

            std::unordered_set<uint32_t> def_blocks;
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                for (auto iid : m.blocks[bb].insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];
                    if (!std::holds_alternative<InstStore>(inst.data)) continue;
                    const auto& st = std::get<InstStore>(inst.data);
                    if (st.slot != slot) continue;
                    auto it = dom.index_of.find(bb);
                    if (it != dom.index_of.end()) def_blocks.insert(it->second);
                }
            }

            std::unordered_map<BlockId, ValueId> phi_for_block;
            {
                std::queue<uint32_t> work;
                for (auto di : def_blocks) work.push(di);
                std::vector<uint8_t> has_phi(dom.blocks.size(), 0);

                while (!work.empty()) {
                    const uint32_t x = work.front();
                    work.pop();
                    for (auto y : dom.df[x]) {
                        if (has_phi[y]) continue;
                        has_phi[y] = 1;

                        const BlockId ybb = dom.blocks[y];
                        const ValueId phi = add_block_param_(m, ybb, slot_ty);
                        phi_for_block[ybb] = phi;
                        m.opt_stats.mem2reg_phi_params += 1;

                        if (!def_blocks.count(y)) {
                            work.push(y);
                        }
                    }
                }
            }

            std::unordered_map<ValueId, ValueId> repl;
            std::unordered_set<InstId> remove_set;
            std::vector<ValueId> value_stack;
            const ValueId undef = make_undef_value_(m, slot_ty);

            std::function<void(uint32_t)> rename = [&](uint32_t bi) {
                const BlockId bb = dom.blocks[bi];
                auto& block = m.blocks[bb];
                uint32_t pushed = 0;

                auto pit = phi_for_block.find(bb);
                if (pit != phi_for_block.end()) {
                    value_stack.push_back(pit->second);
                    pushed += 1;
                }

                for (auto iid : block.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];

                    if (inst.result == slot && std::holds_alternative<InstAllocaLocal>(inst.data)) {
                        remove_set.insert(iid);
                        continue;
                    }

                    if (std::holds_alternative<InstStore>(inst.data)) {
                        const auto& st = std::get<InstStore>(inst.data);
                        if (st.slot != slot) continue;
                        value_stack.push_back(resolve_alias_(repl, st.value));
                        pushed += 1;
                        remove_set.insert(iid);
                        continue;
                    }

                    if (std::holds_alternative<InstLoad>(inst.data)) {
                        const auto& ld = std::get<InstLoad>(inst.data);
                        if (ld.slot != slot || inst.result == kInvalidId) continue;
                        repl[inst.result] = value_stack.empty() ? undef : value_stack.back();
                        remove_set.insert(iid);
                        continue;
                    }
                }

                const ValueId outv = value_stack.empty() ? undef : value_stack.back();
                for (auto succ : dom.succs_by_block[bb]) {
                    if (phi_for_block.find(succ) != phi_for_block.end()) {
                        append_edge_arg_(m, bb, succ, outv);
                    }
                }

                for (auto child : dom.dom_tree[bi]) rename(child);

                while (pushed > 0 && !value_stack.empty()) {
                    value_stack.pop_back();
                    pushed -= 1;
                }
            };

            rename(dom.entry_index);

            if (!repl.empty()) rewrite_operands_(m, repl);

            bool changed = !remove_set.empty() || !phi_for_block.empty();
            if (!changed) return false;

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                auto& block = m.blocks[bb];
                std::vector<InstId> kept;
                kept.reserve(block.insts.size());
                for (auto iid : block.insts) {
                    if (!remove_set.count(iid)) kept.push_back(iid);
                }
                if (kept.size() != block.insts.size()) block.insts = std::move(kept);
            }

            m.opt_stats.mem2reg_promoted_slots += 1;
            return true;
        }

        /// @brief dominance 기반 전역 mem2reg를 수행한다.
        [[maybe_unused]] bool global_mem2reg_ssa_(Module& m) {
            bool changed = false;

            for (auto& f : m.funcs) {
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
                        round_changed |= promote_slot_mem2reg_(m, f, slot, slot_ty);
                    }

                    if (!round_changed) break;
                    changed = true;
                }
            }

            return changed;
        }

        /// @brief block-local store->load 전달(mem2reg-lite) 보조 수행.
        /// @details
        /// unknown write/call이 나타나더라도 non-escaping slot은 별칭되지 않으므로
        /// 해당 slot 캐시는 유지한다(정밀 alias/effect 기반 클로버).
        bool local_load_forward_(Module& m) {
            bool changed = false;
            std::unordered_map<ValueId, ValueId> repl;

            for (auto& f : m.funcs) {
                std::unordered_map<ValueId, bool> nonescape_cache;
                auto is_alloca_slot = [&](ValueId slot) -> bool {
                    if (slot == kInvalidId || (size_t)slot >= m.values.size()) return false;
                    const auto& v = m.values[slot];
                    if (v.def_a == kInvalidId || v.def_b != kInvalidId) return false;
                    const InstId iid = (InstId)v.def_a;
                    if ((size_t)iid >= m.insts.size()) return false;
                    return std::holds_alternative<InstAllocaLocal>(m.insts[iid].data);
                };
                auto is_nonescape = [&](ValueId slot) -> bool {
                    auto it = nonescape_cache.find(slot);
                    if (it != nonescape_cache.end()) return it->second;
                    const bool v = is_alloca_slot(slot) && is_non_escaping_slot_(m, f, slot);
                    nonescape_cache[slot] = v;
                    return v;
                };

                struct MemoryCacheEntry {
                    MemoryLocDesc loc{};
                    ValueId value = kInvalidId;
                };
                auto invalidate_aliases = [&](std::vector<MemoryCacheEntry>& cache, const MemoryLocDesc& written) {
                    for (auto it = cache.begin(); it != cache.end();) {
                        if (may_alias_memory_loc_(it->loc, written)) {
                            it = cache.erase(it);
                        } else {
                            ++it;
                        }
                    }
                };
                auto invalidate_base = [&](std::vector<MemoryCacheEntry>& cache, ValueId base_slot) {
                    for (auto it = cache.begin(); it != cache.end();) {
                        if (it->loc.base_slot == base_slot) {
                            it = cache.erase(it);
                        } else {
                            ++it;
                        }
                    }
                };

                for (auto bb : f.blocks) {
                    if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                    auto& b = m.blocks[bb];
                    std::vector<MemoryCacheEntry> mem_value_cache;

                    for (auto iid : b.insts) {
                        if ((size_t)iid >= m.insts.size()) continue;
                        const auto& inst = m.insts[iid];

                        if (std::holds_alternative<InstStore>(inst.data)) {
                            const auto& s = std::get<InstStore>(inst.data);
                            const MemoryLocDesc loc = resolve_memory_loc_from_value_(m, s.slot);
                            if (!has_memory_base_(loc)) {
                                // 미해결 slot store는 메모리 전반을 보수적으로 클로버한다.
                                for (auto it = mem_value_cache.begin(); it != mem_value_cache.end();) {
                                    if (is_nonescape(it->loc.base_slot)) {
                                        ++it;
                                    } else {
                                        it = mem_value_cache.erase(it);
                                    }
                                }
                                continue;
                            }

                            if (loc.precision == MemoryLocPrecision::Exact) {
                                invalidate_aliases(mem_value_cache, loc);
                                mem_value_cache.push_back(MemoryCacheEntry{
                                    .loc = loc,
                                    .value = resolve_alias_(repl, s.value)
                                });
                            } else {
                                invalidate_base(mem_value_cache, loc.base_slot);
                            }
                            continue;
                        }
                        if (std::holds_alternative<InstLoad>(inst.data)) {
                            const auto& l = std::get<InstLoad>(inst.data);
                            if (inst.result != kInvalidId) {
                                const MemoryLocDesc loc = resolve_memory_loc_from_value_(m, l.slot);
                                if (has_memory_base_(loc) && loc.precision == MemoryLocPrecision::Exact) {
                                    for (auto it = mem_value_cache.rbegin(); it != mem_value_cache.rend(); ++it) {
                                        if (!must_alias_memory_loc_(it->loc, loc)) continue;
                                        repl[inst.result] = resolve_alias_(repl, it->value);
                                        changed = true;
                                        break;
                                    }
                                }
                            }
                            continue;
                        }

                        const InstAccessModel fx = build_inst_access_model_(m, inst);

                        if (fx.write_fp == AliasFootprint::LocalMemory && has_memory_base_(fx.write_loc)) {
                            if (fx.write_loc.precision == MemoryLocPrecision::Exact) {
                                invalidate_aliases(mem_value_cache, fx.write_loc);
                            } else {
                                invalidate_base(mem_value_cache, fx.write_loc.base_slot);
                            }
                        }

                        if (fx.write_fp == AliasFootprint::Unknown || fx.may_call || fx.may_trap) {
                            for (auto it = mem_value_cache.begin(); it != mem_value_cache.end();) {
                                if (is_nonescape(it->loc.base_slot)) {
                                    ++it;
                                } else {
                                    it = mem_value_cache.erase(it);
                                }
                            }
                        }
                    }
                }
            }

            if (changed) rewrite_operands_(m, repl);
            return changed;
        }

        /// @brief 루프 preheader 조건인지 검사한다.
        bool is_preheader_block_(const Module& m, BlockId bb, BlockId header) {
            if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) return false;
            const auto& b = m.blocks[bb];
            if (!b.has_term) return false;
            if (!std::holds_alternative<TermBr>(b.term)) return false;
            const auto& br = std::get<TermBr>(b.term);
            if (br.target != header) return false;
            return succ_count_(b.term) == 1;
        }

        /// @brief 현재 CFG에서 natural loop 집합을 수집한다.
        std::vector<LoopDesc> collect_loops_(const Module& m, const Function& f, const DomInfo& dom) {
            std::unordered_map<BlockId, size_t> by_header;
            std::vector<LoopDesc> out;

            for (auto pred : f.blocks) {
                if (pred == kInvalidId || (size_t)pred >= m.blocks.size()) continue;
                for (auto succ : dom.succs_by_block[pred]) {
                    if (!dominates_(dom, succ, pred)) continue; // backedge

                    size_t li = 0;
                    auto it = by_header.find(succ);
                    if (it == by_header.end()) {
                        li = out.size();
                        by_header[succ] = li;
                        LoopDesc l{};
                        l.header = succ;
                        l.blocks.insert(succ);
                        out.push_back(std::move(l));
                    } else {
                        li = it->second;
                    }

                    auto& loop = out[li];
                    if (std::find(loop.latches.begin(), loop.latches.end(), pred) == loop.latches.end()) {
                        loop.latches.push_back(pred);
                    }

                    // natural loop: pred에서 역방향으로 header까지 확장
                    std::vector<BlockId> stack;
                    if (loop.blocks.insert(pred).second) stack.push_back(pred);
                    while (!stack.empty()) {
                        const BlockId x = stack.back();
                        stack.pop_back();
                        for (auto p : dom.preds_by_block[x]) {
                            if (loop.blocks.insert(p).second && p != succ) {
                                stack.push_back(p);
                            }
                        }
                    }
                }
            }

            return out;
        }

        /// @brief loop canonical form(preheader)로 변환한다.
        bool canonicalize_loops_(Module& m, Function& f) {
            bool changed = false;

            for (;;) {
                bool round_changed = false;
                const auto dom = build_dom_info_(m, f);
                if (dom.entry_index == UINT32_MAX) break;
                auto loops = collect_loops_(m, f, dom);
                if (loops.empty()) break;

                for (auto& loop : loops) {
                    std::vector<BlockId> outside_preds;
                    for (auto p : dom.preds_by_block[loop.header]) {
                        if (!loop.blocks.count(p)) outside_preds.push_back(p);
                    }
                    if (outside_preds.empty()) continue;

                    if (outside_preds.size() == 1 &&
                        is_preheader_block_(m, outside_preds[0], loop.header)) {
                        continue; // 이미 canonical
                    }

                    const BlockId pre = m.add_block(Block{});
                    f.blocks.push_back(pre);

                    // header param 타입을 preheader param으로 복제한 뒤 그대로 전달한다.
                    std::vector<ValueId> pre_args;
                    for (auto hparam : m.blocks[loop.header].params) {
                        TypeId ty = kInvalidId;
                        if (hparam != kInvalidId && (size_t)hparam < m.values.size()) {
                            ty = m.values[hparam].ty;
                        }
                        pre_args.push_back(add_block_param_(m, pre, ty));
                    }

                    TermBr t{};
                    t.target = loop.header;
                    t.args = pre_args;
                    m.blocks[pre].term = std::move(t);
                    m.blocks[pre].has_term = true;

                    for (auto p : outside_preds) {
                        (void)redirect_edge_target_(m, p, loop.header, pre);
                    }

                    m.opt_stats.loop_canonicalized += 1;
                    round_changed = true;
                    changed = true;
                }

                if (!round_changed) break;
            }

            return changed;
        }

        /// @brief commutative binop인지 반환한다.
        bool is_commutative_(BinOp op) {
            switch (op) {
                case BinOp::Add:
                case BinOp::Mul:
                case BinOp::Eq:
                case BinOp::Ne:
                    return true;
                default:
                    return false;
            }
        }

        /// @brief pure inst를 위한 GVN 키를 생성한다.
        std::string gvn_key_(
            const Module& m,
            const Inst& inst,
            const std::unordered_map<ValueId, ValueId>& repl
        ) {
            if (inst.result == kInvalidId) return {};
            if (inst.eff != Effect::Pure) return {};
            const TypeId result_ty =
                (static_cast<size_t>(inst.result) < m.values.size()) ? m.values[inst.result].ty : kInvalidId;

            std::ostringstream oss;
            auto rv = [&](ValueId v) { return resolve_alias_(repl, v); };

            return std::visit([&](auto&& x) -> std::string {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, InstConstInt>) {
                    // 타입 정보를 키에 포함해 i32 1 과 i64 1 같은 값이 섞여 CSE 되지 않게 한다.
                    oss << "ci:" << result_ty << ":" << x.text;
                    return oss.str();
                } else if constexpr (std::is_same_v<T, InstConstBool>) {
                    oss << "cb:" << result_ty << ":" << (x.value ? "1" : "0");
                    return oss.str();
                } else if constexpr (std::is_same_v<T, InstConstText>) {
                    oss << "ct:" << result_ty << ":" << x.bytes;
                    return oss.str();
                } else if constexpr (std::is_same_v<T, InstConstNull>) {
                    oss << "cn:" << result_ty;
                    return oss.str();
                } else if constexpr (std::is_same_v<T, InstUnary>) {
                    oss << "u:" << (uint32_t)x.op << ":" << rv(x.src);
                    return oss.str();
                } else if constexpr (std::is_same_v<T, InstBinOp>) {
                    ValueId a = rv(x.lhs);
                    ValueId b = rv(x.rhs);
                    if (is_commutative_(x.op) && b < a) std::swap(a, b);
                    oss << "b:" << (uint32_t)x.op << ":" << a << ":" << b;
                    return oss.str();
                } else if constexpr (std::is_same_v<T, InstCast>) {
                    oss << "c:" << (uint32_t)x.kind << ":" << x.to << ":" << rv(x.src);
                    return oss.str();
                } else {
                    return {};
                }
            }, inst.data);
        }

        /// @brief 함수 단위로 GVN/CSE를 수행한다.
        bool gvn_cse_function_(Module& m, Function& f) {
            const auto dom = build_dom_info_(m, f);
            if (dom.entry_index == UINT32_MAX) return false;

            std::unordered_map<std::string, std::vector<ValueId>> env;
            std::unordered_map<ValueId, ValueId> repl;
            std::unordered_set<InstId> remove_set;

            std::function<void(uint32_t)> dfs = [&](uint32_t bi) {
                const BlockId bb = dom.blocks[bi];
                const auto& block = m.blocks[bb];

                std::vector<std::pair<std::string, ValueId>> pushed;
                pushed.reserve(block.insts.size());

                for (auto iid : block.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];
                    if (inst.result == kInvalidId) continue;

                    const std::string key = gvn_key_(m, inst, repl);
                    if (key.empty()) continue;

                    auto it = env.find(key);
                    if (it != env.end() && !it->second.empty()) {
                        repl[inst.result] = it->second.back();
                        remove_set.insert(iid);
                        m.opt_stats.gvn_cse_eliminated += 1;
                    } else {
                        env[key].push_back(inst.result);
                        pushed.push_back({key, inst.result});
                    }
                }

                for (auto child : dom.dom_tree[bi]) dfs(child);

                for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) {
                    auto env_it = env.find(it->first);
                    if (env_it == env.end() || env_it->second.empty()) continue;
                    if (env_it->second.back() == it->second) {
                        env_it->second.pop_back();
                        if (env_it->second.empty()) env.erase(env_it);
                    }
                }
            };

            dfs(dom.entry_index);

            if (repl.empty() && remove_set.empty()) return false;
            if (!repl.empty()) rewrite_operands_(m, repl);

            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                auto& block = m.blocks[bb];
                std::vector<InstId> kept;
                kept.reserve(block.insts.size());
                for (auto iid : block.insts) {
                    if (!remove_set.count(iid)) kept.push_back(iid);
                }
                if (kept.size() != block.insts.size()) block.insts = std::move(kept);
            }
            return true;
        }

        /// @brief 모듈 전체에 GVN/CSE를 적용한다.
        [[maybe_unused]] bool gvn_cse_(Module& m) {
            bool changed = false;
            for (auto& f : m.funcs) {
                changed |= gvn_cse_function_(m, f);
            }
            return changed;
        }

        /// @brief LICM 후보의 operand 목록을 수집한다.
        std::vector<ValueId> inst_operands_(const Inst& inst) {
            return std::visit([&](auto&& x) -> std::vector<ValueId> {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, InstUnary>) {
                    return {x.src};
                } else if constexpr (std::is_same_v<T, InstBinOp>) {
                    return {x.lhs, x.rhs};
                } else if constexpr (std::is_same_v<T, InstCast>) {
                    return {x.src};
                } else if constexpr (std::is_same_v<T, InstCall>) {
                    std::vector<ValueId> out;
                    out.reserve(x.args.size() + 1);
                    out.push_back(x.callee);
                    out.insert(out.end(), x.args.begin(), x.args.end());
                    return out;
                } else if constexpr (std::is_same_v<T, InstIndex>) {
                    return {x.base, x.index};
                } else if constexpr (std::is_same_v<T, InstField>) {
                    return {x.base};
                } else if constexpr (std::is_same_v<T, InstLoad>) {
                    return {x.slot};
                } else if constexpr (std::is_same_v<T, InstStore>) {
                    return {x.slot, x.value};
                } else {
                    return {};
                }
            }, inst.data);
        }

        /// @brief value 정의가 loop 내부인지 판단한다.
        bool value_defined_in_loop_(
            const Module& m,
            ValueId v,
            const std::unordered_set<BlockId>& loop_blocks,
            const std::unordered_map<InstId, BlockId>& inst_block
        ) {
            if (v == kInvalidId || (size_t)v >= m.values.size()) return false;
            const auto& val = m.values[v];
            if (val.def_a == kInvalidId) return false;

            // block param
            if (val.def_b != kInvalidId) {
                const BlockId b = (BlockId)val.def_a;
                return loop_blocks.count(b) != 0;
            }

            // inst result
            const InstId iid = (InstId)val.def_a;
            auto it = inst_block.find(iid);
            if (it == inst_block.end()) return false;
            return loop_blocks.count(it->second) != 0;
        }

        /// @brief LICM에서 함수 단위 최적화를 수행한다.
        bool licm_function_(Module& m, Function& f) {
            bool changed = false;

            const auto dom = build_dom_info_(m, f);
            if (dom.entry_index == UINT32_MAX) return false;

            auto loops = collect_loops_(m, f, dom);
            if (loops.empty()) return false;

            const auto inst_block = build_inst_block_map_(m, f);
            std::unordered_map<ValueId, bool> noescape_slot_cache;

            for (auto& loop : loops) {
                std::vector<BlockId> outside_preds;
                for (auto p : dom.preds_by_block[loop.header]) {
                    if (!loop.blocks.count(p)) outside_preds.push_back(p);
                }
                if (outside_preds.size() != 1) continue;
                const BlockId preheader = outside_preds[0];
                if (!is_preheader_block_(m, preheader, loop.header)) continue;
                loop.preheader = preheader;

                std::unordered_set<ValueId> mutated_slots;
                for (auto bb : loop.blocks) {
                    if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                    for (auto iid : m.blocks[bb].insts) {
                        if ((size_t)iid >= m.insts.size()) continue;
                        const auto& inst = m.insts[iid];
                        if (!std::holds_alternative<InstStore>(inst.data)) continue;
                        const auto& st = std::get<InstStore>(inst.data);
                        mutated_slots.insert(st.slot);
                    }
                }

                std::unordered_set<ValueId> invariant_values;
                for (uint32_t vid = 0; vid < (uint32_t)m.values.size(); ++vid) {
                    if (!value_defined_in_loop_(m, vid, loop.blocks, inst_block)) {
                        invariant_values.insert(vid);
                    }
                }

                std::unordered_set<InstId> hoist_set;
                std::vector<InstId> hoist_order;

                bool round = false;
                do {
                    round = false;
                    for (auto bb : f.blocks) {
                        if (!loop.blocks.count(bb)) continue;
                        for (auto iid : m.blocks[bb].insts) {
                            if ((size_t)iid >= m.insts.size()) continue;
                            if (hoist_set.count(iid)) continue;
                            const auto& inst = m.insts[iid];
                            if (inst.result == kInvalidId) continue;

                            bool hoistable = false;
                            if (std::holds_alternative<InstLoad>(inst.data)) {
                                const auto& ld = std::get<InstLoad>(inst.data);
                                bool noescape = false;
                                auto it = noescape_slot_cache.find(ld.slot);
                                if (it == noescape_slot_cache.end()) {
                                    noescape = is_non_escaping_slot_(m, f, ld.slot);
                                    noescape_slot_cache[ld.slot] = noescape;
                                } else {
                                    noescape = it->second;
                                }
                                hoistable = noescape && !mutated_slots.count(ld.slot);
                            } else {
                                hoistable = (inst.eff == Effect::Pure);
                            }
                            if (!hoistable) continue;

                            auto ops = inst_operands_(inst);
                            bool operands_invariant = true;
                            for (auto v : ops) {
                                if (!invariant_values.count(v)) {
                                    operands_invariant = false;
                                    break;
                                }
                            }
                            if (!operands_invariant) continue;

                            hoist_set.insert(iid);
                            hoist_order.push_back(iid);
                            invariant_values.insert(inst.result);
                            round = true;
                        }
                    }
                } while (round);

                if (hoist_order.empty()) continue;

                for (auto bb : loop.blocks) {
                    if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                    auto& block = m.blocks[bb];
                    std::vector<InstId> kept;
                    kept.reserve(block.insts.size());
                    for (auto iid : block.insts) {
                        if (!hoist_set.count(iid)) kept.push_back(iid);
                    }
                    if (kept.size() != block.insts.size()) block.insts = std::move(kept);
                }

                auto& pre = m.blocks[preheader];
                pre.insts.insert(pre.insts.end(), hoist_order.begin(), hoist_order.end());

                changed = true;
                m.opt_stats.licm_hoisted += (uint32_t)hoist_order.size();
            }

            return changed;
        }

        /// @brief 모듈 전체에 LICM을 적용한다.
        [[maybe_unused]] bool licm_(Module& m) {
            bool changed = false;
            for (auto& f : m.funcs) {
                changed |= licm_function_(m, f);
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
        // 3) loop canonical form(preheader)
        // 4) block-arg edge-cast 정규화(의미 보존)
        // 5) 상수 폴딩
        // 6) dominance 기반 mem2reg + SSA(block param)
        // 7) GVN/CSE
        // 8) LICM
        // 9) local forwarding 보조
        // 10) escape-handle 특화 정리
        // 11) pure DCE
        // 12) CFG 재정리
        (void)simplify_cfg_(m);
        for (auto& f : m.funcs) {
            (void)split_critical_edges_(m, f);
        }
        for (auto& f : m.funcs) {
            (void)canonicalize_loops_(m, f);
            (void)normalize_phi_edge_casts_(m, f);
        }
        (void)const_fold_(m);
        (void)local_load_forward_(m);

        // NOTE(parus/v0):
        // 고급 패스(mem2reg/GVN/LICM)는 실행 후 즉시 지배/루프 고정점 검증을 수행한다.
        // 검증 실패 라운드는 모듈 스냅샷으로 롤백하여 invalid LLVM-IR 유입을 차단한다.
        const bool require_loop_fixpoint = true;
        const uint32_t max_opt_rounds = 4;
        (void)run_guarded_pass_fixpoint_(
            m,
            require_loop_fixpoint,
            max_opt_rounds,
            [&](Module& mm) { return global_mem2reg_ssa_(mm); }
        );
        (void)run_guarded_pass_fixpoint_(
            m,
            require_loop_fixpoint,
            max_opt_rounds,
            [&](Module& mm) { return gvn_cse_(mm); }
        );
        (void)run_guarded_pass_fixpoint_(
            m,
            require_loop_fixpoint,
            max_opt_rounds,
            [&](Module& mm) { return licm_(mm); }
        );

        // LICM 이후 preheader 형태가 흔들릴 수 있으므로 canonical form을 재검증한다.
        (void)run_guarded_pass_fixpoint_(
            m,
            require_loop_fixpoint,
            max_opt_rounds,
            [&](Module& mm) {
                bool changed = false;
                for (auto& f : mm.funcs) changed |= canonicalize_loops_(mm, f);
                return changed;
            }
        );

        (void)local_load_forward_(m);
        for (auto& f : m.funcs) {
            (void)normalize_phi_edge_casts_(m, f);
        }
        (void)optimize_escape_handles_(m);
        (void)dce_pure_insts_(m);
        (void)simplify_cfg_(m);
    }

} // namespace parus::oir
