#include <parus/goir/Verify.hpp>

#include <algorithm>
#include <string>

namespace parus::goir {

    namespace {

        void push_error_(std::vector<Message>& out, std::string text) {
            out.push_back(Message{std::move(text)});
        }

        bool is_valid_value_(const Module& m, ValueId id) {
            return id != kInvalidId && static_cast<size_t>(id) < m.values.size();
        }

        bool is_valid_block_(const Module& m, BlockId id) {
            return id != kInvalidId && static_cast<size_t>(id) < m.blocks.size();
        }

        bool is_valid_computation_(const Module& m, ComputationId id) {
            return id != kInvalidId && static_cast<size_t>(id) < m.computations.size();
        }

        bool is_valid_realization_(const Module& m, RealizationId id) {
            return id != kInvalidId && static_cast<size_t>(id) < m.realizations.size();
        }

        void verify_value_ref_(const Module& m,
                               ValueId id,
                               std::string_view where,
                               std::vector<Message>& out) {
            if (!is_valid_value_(m, id)) {
                push_error_(out, std::string(where) + ": invalid value id");
            }
        }

    } // namespace

    std::vector<Message> verify(const Module& module) {
        std::vector<Message> out{};

        for (size_t i = 0; i < module.semantic_sigs.size(); ++i) {
            const auto& sig = module.semantic_sigs[i];
            if (sig.name == kInvalidId || static_cast<size_t>(sig.name) >= module.strings.size()) {
                push_error_(out, "semantic sig has invalid name string id");
            }
            for (const auto ty : sig.param_types) {
                if (ty == kInvalidType) {
                    push_error_(out, "semantic sig has invalid parameter type");
                }
            }
        }

        for (size_t i = 0; i < module.computations.size(); ++i) {
            const auto& comp = module.computations[i];
            if (comp.sig == kInvalidId || static_cast<size_t>(comp.sig) >= module.semantic_sigs.size()) {
                push_error_(out, "computation has invalid semantic sig");
            }
            if (comp.placement_policy == kInvalidId ||
                static_cast<size_t>(comp.placement_policy) >= module.placement_policies.size()) {
                push_error_(out, "computation has invalid placement policy");
            }
            for (const auto rid : comp.realizations) {
                if (!is_valid_realization_(module, rid)) {
                    push_error_(out, "computation has invalid realization reference");
                }
            }
        }

        for (size_t i = 0; i < module.realizations.size(); ++i) {
            const auto& real = module.realizations[i];
            if (!is_valid_computation_(module, real.computation)) {
                push_error_(out, "realization has invalid computation");
            }
            if (!is_valid_block_(module, real.entry)) {
                push_error_(out, "realization has invalid entry block");
            }
            for (const auto bb : real.blocks) {
                if (!is_valid_block_(module, bb)) {
                    push_error_(out, "realization has invalid block reference");
                }
            }
        }

        for (size_t bi = 0; bi < module.blocks.size(); ++bi) {
            const auto& block = module.blocks[bi];
            for (const auto param : block.params) {
                if (!is_valid_value_(module, param)) {
                    push_error_(out, "block has invalid param value");
                }
            }
            for (const auto iid : block.insts) {
                if (iid == kInvalidId || static_cast<size_t>(iid) >= module.insts.size()) {
                    push_error_(out, "block has invalid inst id");
                    continue;
                }
                const auto& inst = module.insts[iid];
                if (inst.result != kInvalidId && !is_valid_value_(module, inst.result)) {
                    push_error_(out, "inst has invalid result value");
                }

                std::visit([&](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::is_same_v<T, OpUnary>) {
                        verify_value_ref_(module, data.src, "OpUnary", out);
                    } else if constexpr (std::is_same_v<T, OpBinary>) {
                        verify_value_ref_(module, data.lhs, "OpBinary.lhs", out);
                        verify_value_ref_(module, data.rhs, "OpBinary.rhs", out);
                    } else if constexpr (std::is_same_v<T, OpCast>) {
                        verify_value_ref_(module, data.src, "OpCast.src", out);
                        if (data.to == kInvalidType) {
                            push_error_(out, "OpCast has invalid target type");
                        }
                    } else if constexpr (std::is_same_v<T, OpSemanticInvoke>) {
                        if (!is_valid_computation_(module, data.computation)) {
                            push_error_(out, "OpSemanticInvoke has invalid computation");
                        }
                        for (const auto arg : data.args) {
                            verify_value_ref_(module, arg, "OpSemanticInvoke.arg", out);
                        }
                        if (module.header.stage_kind != StageKind::Open) {
                            push_error_(out, "OpSemanticInvoke is only legal in gOIR-open");
                        }
                    } else if constexpr (std::is_same_v<T, OpCallDirect>) {
                        if (!is_valid_realization_(module, data.callee)) {
                            push_error_(out, "OpCallDirect has invalid callee realization");
                        }
                        for (const auto arg : data.args) {
                            verify_value_ref_(module, arg, "OpCallDirect.arg", out);
                        }
                        if (module.header.stage_kind != StageKind::Placed) {
                            push_error_(out, "OpCallDirect is only legal in gOIR-placed");
                        }
                    }
                }, inst.data);
            }

            if (!block.has_term) {
                push_error_(out, "block is missing terminator");
                continue;
            }

            std::visit([&](const auto& term) {
                using T = std::decay_t<decltype(term)>;
                if constexpr (std::is_same_v<T, TermBr>) {
                    if (!is_valid_block_(module, term.target)) {
                        push_error_(out, "TermBr has invalid target");
                    }
                    if (!term.args.empty()) {
                        push_error_(out, "M0 gOIR blocks must not pass branch args");
                    }
                } else if constexpr (std::is_same_v<T, TermCondBr>) {
                    verify_value_ref_(module, term.cond, "TermCondBr.cond", out);
                    if (!is_valid_block_(module, term.then_bb) || !is_valid_block_(module, term.else_bb)) {
                        push_error_(out, "TermCondBr has invalid branch target");
                    }
                    if (!term.then_args.empty() || !term.else_args.empty()) {
                        push_error_(out, "M0 gOIR blocks must not pass branch args");
                    }
                } else if constexpr (std::is_same_v<T, TermRet>) {
                    if (term.has_value) {
                        verify_value_ref_(module, term.value, "TermRet.value", out);
                    }
                }
            }, block.term);
        }

        if (module.header.stage_kind == StageKind::Placed) {
            for (const auto& value : module.values) {
                if (value.ownership.kind != OwnershipKind::Plain) {
                    push_error_(out, "placed gOIR must not contain ownership-sensitive values in M0");
                }
            }
        }

        return out;
    }

} // namespace parus::goir
