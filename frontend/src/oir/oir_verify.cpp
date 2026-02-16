// frontend/src/oir/oir_verify.cpp
#include <parus/oir/Verify.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace parus::oir {

    namespace {

        /// @brief verify 오류를 수집 벡터에 추가한다.
        void push_error_(std::vector<VerifyError>& out, const std::string& msg) {
            out.push_back(VerifyError{msg});
        }

        /// @brief 값 ID 범위 유효성을 검사한다.
        bool check_value_id_(
            const Module& m,
            std::vector<VerifyError>& errs,
            uint32_t where_id,
            const char* where_kind,
            ValueId vid
        ) {
            if (vid == kInvalidId || (size_t)vid >= m.values.size()) {
                std::ostringstream oss;
                oss << where_kind << " #" << where_id
                    << " references invalid value id v" << vid;
                push_error_(errs, oss.str());
                return false;
            }
            return true;
        }

        /// @brief 한 함수의 블록 소속 집합을 빠르게 조회하기 위한 테이블을 만든다.
        std::vector<uint8_t> build_owned_blocks_mask_(const Module& m, const Function& f) {
            std::vector<uint8_t> owned(m.blocks.size(), 0);
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                owned[bb] = 1;
            }
            return owned;
        }

        /// @brief terminator의 타깃 블록/인자 무결성을 검사한다.
        void verify_terminator_(
            const Module& m,
            const Function& f,
            uint32_t bbid,
            const std::vector<uint8_t>& owned_blocks,
            const Terminator& term,
            std::vector<VerifyError>& errs
        ) {
            std::visit([&](auto&& t) {
                using T = std::decay_t<decltype(t)>;

                if constexpr (std::is_same_v<T, TermRet>) {
                    if (t.has_value) {
                        (void)check_value_id_(m, errs, bbid, "block(term ret)", t.value);
                    }
                } else if constexpr (std::is_same_v<T, TermBr>) {
                    if (t.target == kInvalidId || (size_t)t.target >= m.blocks.size()) {
                        std::ostringstream oss;
                        oss << "block #" << bbid << " has br with invalid target bb#" << t.target;
                        push_error_(errs, oss.str());
                        return;
                    }
                    if (!owned_blocks[t.target]) {
                        std::ostringstream oss;
                        oss << "block #" << bbid << " branches to foreign block bb#" << t.target
                            << " (outside function " << f.name << ")";
                        push_error_(errs, oss.str());
                    }
                    for (auto v : t.args) {
                        (void)check_value_id_(m, errs, bbid, "block(term br arg)", v);
                    }
                    if ((size_t)t.target < m.blocks.size()) {
                        const auto& target = m.blocks[t.target];
                        if (t.args.size() != target.params.size()) {
                            std::ostringstream oss;
                            oss << "block #" << bbid << " br arg count mismatch: got " << t.args.size()
                                << ", target bb#" << t.target << " expects " << target.params.size();
                            push_error_(errs, oss.str());
                        }
                        const uint32_t n = static_cast<uint32_t>(
                            std::min<size_t>(t.args.size(), target.params.size())
                        );
                        for (uint32_t i = 0; i < n; ++i) {
                            const ValueId arg = t.args[i];
                            const ValueId param = target.params[i];
                            if (arg == kInvalidId || param == kInvalidId) continue;
                            if ((size_t)arg >= m.values.size() || (size_t)param >= m.values.size()) continue;
                            if (m.values[arg].ty != m.values[param].ty) {
                                std::ostringstream oss;
                                oss << "block #" << bbid << " br arg type mismatch at index " << i
                                    << ": arg v" << arg << "(ty=" << m.values[arg].ty << ") != "
                                    << "target param v" << param << "(ty=" << m.values[param].ty << ")";
                                push_error_(errs, oss.str());
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, TermCondBr>) {
                    (void)check_value_id_(m, errs, bbid, "block(term cond)", t.cond);

                    auto check_target = [&](BlockId target, const std::vector<ValueId>& args, const char* tag) {
                        if (target == kInvalidId || (size_t)target >= m.blocks.size()) {
                            std::ostringstream oss;
                            oss << "block #" << bbid << " has condbr " << tag
                                << " with invalid target bb#" << target;
                            push_error_(errs, oss.str());
                            return;
                        }
                        if (!owned_blocks[target]) {
                            std::ostringstream oss;
                            oss << "block #" << bbid << " condbr " << tag
                                << " targets foreign block bb#" << target
                                << " (outside function " << f.name << ")";
                            push_error_(errs, oss.str());
                        }
                        for (auto v : args) {
                            (void)check_value_id_(m, errs, bbid, "block(term condbr arg)", v);
                        }
                        const auto& target_block = m.blocks[target];
                        if (args.size() != target_block.params.size()) {
                            std::ostringstream oss;
                            oss << "block #" << bbid << " condbr " << tag
                                << " arg count mismatch: got " << args.size()
                                << ", target bb#" << target << " expects " << target_block.params.size();
                            push_error_(errs, oss.str());
                        }
                        const uint32_t n = static_cast<uint32_t>(
                            std::min<size_t>(args.size(), target_block.params.size())
                        );
                        for (uint32_t i = 0; i < n; ++i) {
                            const ValueId arg = args[i];
                            const ValueId param = target_block.params[i];
                            if (arg == kInvalidId || param == kInvalidId) continue;
                            if ((size_t)arg >= m.values.size() || (size_t)param >= m.values.size()) continue;
                            if (m.values[arg].ty != m.values[param].ty) {
                                std::ostringstream oss;
                                oss << "block #" << bbid << " condbr " << tag
                                    << " arg type mismatch at index " << i
                                    << ": arg v" << arg << "(ty=" << m.values[arg].ty << ") != "
                                    << "target param v" << param << "(ty=" << m.values[param].ty << ")";
                                push_error_(errs, oss.str());
                            }
                        }
                    };

                    check_target(t.then_bb, t.then_args, "then");
                    check_target(t.else_bb, t.else_args, "else");
                }
            }, term);
        }

        /// @brief instruction operand value id 무결성을 검사한다.
        void verify_inst_operands_(
            const Module& m,
            uint32_t iid,
            const Inst& inst,
            std::vector<VerifyError>& errs
        ) {
            if (inst.result != kInvalidId && (size_t)inst.result >= m.values.size()) {
                std::ostringstream oss;
                oss << "inst #" << iid << " has invalid result id v" << inst.result;
                push_error_(errs, oss.str());
            }

            std::visit([&](auto&& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, InstUnary>) {
                    (void)check_value_id_(m, errs, iid, "inst(unary src)", x.src);
                } else if constexpr (std::is_same_v<T, InstBinOp>) {
                    (void)check_value_id_(m, errs, iid, "inst(bin lhs)", x.lhs);
                    (void)check_value_id_(m, errs, iid, "inst(bin rhs)", x.rhs);
                } else if constexpr (std::is_same_v<T, InstCast>) {
                    (void)check_value_id_(m, errs, iid, "inst(cast src)", x.src);
                } else if constexpr (std::is_same_v<T, InstFuncRef>) {
                    if (x.func == kInvalidId || (size_t)x.func >= m.funcs.size()) {
                        std::ostringstream oss;
                        oss << "inst #" << iid << " has invalid function ref id f" << x.func;
                        push_error_(errs, oss.str());
                    }
                } else if constexpr (std::is_same_v<T, InstGlobalRef>) {
                    if (x.global == kInvalidId || (size_t)x.global >= m.globals.size()) {
                        std::ostringstream oss;
                        oss << "inst #" << iid << " has invalid global ref id g" << x.global;
                        push_error_(errs, oss.str());
                    }
                } else if constexpr (std::is_same_v<T, InstCall>) {
                    if (x.direct_callee == kInvalidId) {
                        (void)check_value_id_(m, errs, iid, "inst(call callee)", x.callee);
                    } else if ((size_t)x.direct_callee >= m.funcs.size()) {
                        std::ostringstream oss;
                        oss << "inst #" << iid << " has invalid direct callee id f" << x.direct_callee;
                        push_error_(errs, oss.str());
                    }
                    for (auto av : x.args) {
                        (void)check_value_id_(m, errs, iid, "inst(call arg)", av);
                    }
                } else if constexpr (std::is_same_v<T, InstIndex>) {
                    (void)check_value_id_(m, errs, iid, "inst(index base)", x.base);
                    (void)check_value_id_(m, errs, iid, "inst(index idx)", x.index);
                } else if constexpr (std::is_same_v<T, InstField>) {
                    (void)check_value_id_(m, errs, iid, "inst(field base)", x.base);
                } else if constexpr (std::is_same_v<T, InstLoad>) {
                    (void)check_value_id_(m, errs, iid, "inst(load slot)", x.slot);
                } else if constexpr (std::is_same_v<T, InstStore>) {
                    (void)check_value_id_(m, errs, iid, "inst(store slot)", x.slot);
                    (void)check_value_id_(m, errs, iid, "inst(store value)", x.value);
                } else if constexpr (std::is_same_v<T, InstConstInt> ||
                                     std::is_same_v<T, InstConstBool> ||
                                     std::is_same_v<T, InstConstText> ||
                                     std::is_same_v<T, InstConstNull> ||
                                     std::is_same_v<T, InstAllocaLocal>) {
                    // no operand
                }
            }, inst.data);
        }

    } // namespace

    std::vector<VerifyError> verify(const Module& m) {
        std::vector<VerifyError> errs;

        // block 소속(owner) 계산: 하나의 블록은 정확히 하나의 함수에 소속되어야 한다.
        std::vector<uint32_t> block_owner(m.blocks.size(), kInvalidId);
        for (uint32_t fi = 0; fi < (uint32_t)m.funcs.size(); ++fi) {
            const auto& f = m.funcs[fi];
            for (auto bb : f.blocks) {
                if (bb == kInvalidId || (size_t)bb >= m.blocks.size()) continue;
                if (block_owner[bb] == kInvalidId) block_owner[bb] = fi;
                else if (block_owner[bb] != fi) {
                    std::ostringstream oss;
                    oss << "block #" << bb << " is owned by multiple functions (#"
                        << block_owner[bb] << ", #" << fi << ")";
                    push_error_(errs, oss.str());
                }
            }
        }

        // 함수 단위 검사
        for (uint32_t fi = 0; fi < (uint32_t)m.funcs.size(); ++fi) {
            const auto& f = m.funcs[fi];
            const auto owned_blocks = build_owned_blocks_mask_(m, f);

            if (f.entry == kInvalidId || (size_t)f.entry >= m.blocks.size()) {
                push_error_(errs, "function has invalid entry: " + f.name);
                continue;
            }
            if (!owned_blocks[f.entry]) {
                std::ostringstream oss;
                oss << "function " << f.name << " entry bb#" << f.entry
                    << " is not present in function block list";
                push_error_(errs, oss.str());
            }

            for (auto bbid : f.blocks) {
                if (bbid == kInvalidId || (size_t)bbid >= m.blocks.size()) {
                    std::ostringstream oss;
                    oss << "function " << f.name << " has invalid block id bb#" << bbid;
                    push_error_(errs, oss.str());
                    continue;
                }

                const auto& b = m.blocks[bbid];
                if (!b.has_term) {
                    std::ostringstream oss;
                    oss << "block has no terminator: #" << bbid;
                    push_error_(errs, oss.str());
                } else {
                    verify_terminator_(m, f, bbid, owned_blocks, b.term, errs);
                }

                for (auto p : b.params) {
                    (void)check_value_id_(m, errs, bbid, "block(param)", p);
                }

                for (auto iid : b.insts) {
                    if ((size_t)iid >= m.insts.size()) {
                        std::ostringstream oss;
                        oss << "block #" << bbid << " references invalid inst id i" << iid;
                        push_error_(errs, oss.str());
                        continue;
                    }
                    verify_inst_operands_(m, iid, m.insts[iid], errs);
                }
            }
        }

        // escape-handle 힌트 무결성 검사
        for (uint32_t i = 0; i < (uint32_t)m.escape_hints.size(); ++i) {
            const auto& h = m.escape_hints[i];
            if (h.value == kInvalidId || (size_t)h.value >= m.values.size()) {
                std::ostringstream oss;
                oss << "escape_hint #" << i << " references invalid value id v" << h.value;
                push_error_(errs, oss.str());
            }
            if (h.kind == EscapeHandleKind::HeapBox) {
                std::ostringstream oss;
                oss << "escape_hint #" << i << " uses heap_box kind, forbidden in v0";
                push_error_(errs, oss.str());
            }
        }

        return errs;
    }

} // namespace parus::oir
