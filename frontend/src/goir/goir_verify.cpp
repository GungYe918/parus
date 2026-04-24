#include <parus/goir/Verify.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

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

        const RecordLayout* record_layout_(const Module& m, TypeId ty) {
            return m.find_record_layout(ty);
        }

        TypeId place_storage_type_(const Module& m, ValueId id) {
            if (!is_valid_value_(m, id)) return kInvalidType;
            const auto& v = m.values[id];
            return (v.place_elem_type == kInvalidType) ? v.ty : v.place_elem_type;
        }

        void verify_value_ref_(const Module& m,
                               ValueId id,
                               std::string_view where,
                               std::vector<Message>& out) {
            if (!is_valid_value_(m, id)) {
                push_error_(out, std::string(where) + ": invalid value id");
            }
        }

        void verify_block_args_(const Module& m,
                                BlockId target,
                                const std::vector<ValueId>& args,
                                std::string_view where,
                                std::vector<Message>& out) {
            if (!is_valid_block_(m, target)) {
                push_error_(out, std::string(where) + ": invalid target block");
                return;
            }
            const auto& block = m.blocks[target];
            if (block.params.size() != args.size()) {
                push_error_(out, std::string(where) + ": branch arg count does not match block params");
                return;
            }
            for (size_t i = 0; i < args.size(); ++i) {
                verify_value_ref_(m, args[i], std::string(where) + ".arg", out);
                if (!is_valid_value_(m, args[i])) continue;
                const auto actual = m.values[args[i]].ty;
                const auto formal = m.values[block.params[i]].ty;
                if (actual != formal) {
                    push_error_(out, std::string(where) + ": branch arg type does not match block param");
                }
            }
        }

    } // namespace

    std::vector<Message> verify(const Module& module) {
        std::vector<Message> out{};

        for (const auto& layout : module.record_layouts) {
            if (layout.self_type == kInvalidType) {
                push_error_(out, "record layout has invalid self type");
            }
            std::unordered_set<uint32_t> seen_names{};
            for (const auto& field : layout.fields) {
                if (field.name == kInvalidId || static_cast<size_t>(field.name) >= module.strings.size()) {
                    push_error_(out, "record layout has invalid field name string id");
                }
                if (field.type == kInvalidType) {
                    push_error_(out, "record layout has invalid field type");
                }
                if (!seen_names.insert(field.name).second) {
                    push_error_(out, "record layout has duplicate field name");
                }
            }
        }

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
                    continue;
                }
                const auto& value = module.values[param];
                if (value.is_place) {
                    push_error_(out, "block parameter must not be a place value");
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
                        verify_value_ref_(module, data.src, "OpUnary.src", out);
                    } else if constexpr (std::is_same_v<T, OpBinary>) {
                        verify_value_ref_(module, data.lhs, "OpBinary.lhs", out);
                        verify_value_ref_(module, data.rhs, "OpBinary.rhs", out);
                    } else if constexpr (std::is_same_v<T, OpCast>) {
                        verify_value_ref_(module, data.src, "OpCast.src", out);
                        if (data.to == kInvalidType) {
                            push_error_(out, "OpCast has invalid target type");
                        }
                    } else if constexpr (std::is_same_v<T, OpArrayMake>) {
                        if (inst.result == kInvalidId) {
                            push_error_(out, "OpArrayMake must produce a result");
                            return;
                        }
                        if (module.values[inst.result].ty == kInvalidType) {
                            push_error_(out, "OpArrayMake has invalid result type");
                            return;
                        }
                        const auto& t = module.values[inst.result];
                        if (t.layout != LayoutClass::FixedArray) {
                            push_error_(out, "OpArrayMake result must have fixed-array layout");
                        }
                        for (const auto elem : data.elems) {
                            verify_value_ref_(module, elem, "OpArrayMake.elem", out);
                        }
                    } else if constexpr (std::is_same_v<T, OpArrayGet>) {
                        verify_value_ref_(module, data.base, "OpArrayGet.base", out);
                        verify_value_ref_(module, data.index, "OpArrayGet.index", out);
                    } else if constexpr (std::is_same_v<T, OpArrayLen>) {
                        verify_value_ref_(module, data.base, "OpArrayLen.base", out);
                    } else if constexpr (std::is_same_v<T, OpRecordMake>) {
                        if (inst.result == kInvalidId) {
                            push_error_(out, "OpRecordMake must produce a result");
                            return;
                        }
                        const auto* layout = record_layout_(module, module.values[inst.result].ty);
                        if (layout == nullptr) {
                            push_error_(out, "OpRecordMake result type must have a record layout");
                            return;
                        }
                        std::unordered_set<uint32_t> seen_names{};
                        for (const auto& field : data.fields) {
                            if (field.name == kInvalidId || static_cast<size_t>(field.name) >= module.strings.size()) {
                                push_error_(out, "OpRecordMake has invalid field name");
                                continue;
                            }
                            verify_value_ref_(module, field.value, "OpRecordMake.field", out);
                            if (!seen_names.insert(field.name).second) {
                                push_error_(out, "OpRecordMake has duplicate field member");
                            }
                        }
                    } else if constexpr (std::is_same_v<T, OpLocalSlot>) {
                        if (inst.result == kInvalidId) {
                            push_error_(out, "OpLocalSlot must produce a result");
                            return;
                        }
                        const auto& result = module.values[inst.result];
                        if (!result.is_place || result.place_kind != PlaceKind::LocalSlot) {
                            push_error_(out, "OpLocalSlot result must be a local-slot place");
                        }
                    } else if constexpr (std::is_same_v<T, OpFieldPlace>) {
                        verify_value_ref_(module, data.base, "OpFieldPlace.base", out);
                        if (!is_valid_value_(module, data.base)) return;
                        const auto& base = module.values[data.base];
                        if (!base.is_place) {
                            push_error_(out, "OpFieldPlace base must be a place");
                            return;
                        }
                        const auto* layout = record_layout_(module, place_storage_type_(module, data.base));
                        if (layout == nullptr) {
                            push_error_(out, "OpFieldPlace base storage must have a record layout");
                            return;
                        }
                        bool found = false;
                        for (const auto& field : layout->fields) {
                            if (field.name == data.field_name) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            push_error_(out, "OpFieldPlace references an unknown field");
                        }
                    } else if constexpr (std::is_same_v<T, OpIndexPlace>) {
                        verify_value_ref_(module, data.base, "OpIndexPlace.base", out);
                        verify_value_ref_(module, data.index, "OpIndexPlace.index", out);
                        if (!is_valid_value_(module, data.base)) return;
                        const auto& base = module.values[data.base];
                        if (!base.is_place) {
                            push_error_(out, "OpIndexPlace base must be a place");
                        }
                    } else if constexpr (std::is_same_v<T, OpSubView>) {
                        verify_value_ref_(module, data.base, "OpSubView.base", out);
                        verify_value_ref_(module, data.offset, "OpSubView.offset", out);
                        verify_value_ref_(module, data.length, "OpSubView.length", out);
                        if (is_valid_value_(module, data.base) && !module.values[data.base].is_place) {
                            push_error_(out, "OpSubView base must be a place");
                        }
                    } else if constexpr (std::is_same_v<T, OpLoad>) {
                        verify_value_ref_(module, data.place, "OpLoad.place", out);
                        if (is_valid_value_(module, data.place) && !module.values[data.place].is_place) {
                            push_error_(out, "OpLoad operand must be a place");
                        }
                    } else if constexpr (std::is_same_v<T, OpStore>) {
                        verify_value_ref_(module, data.place, "OpStore.place", out);
                        verify_value_ref_(module, data.value, "OpStore.value", out);
                        if (is_valid_value_(module, data.place) && !module.values[data.place].is_place) {
                            push_error_(out, "OpStore place operand must be a place");
                        }
                    } else if constexpr (std::is_same_v<T, OpBorrowView>) {
                        verify_value_ref_(module, data.source_place, "OpBorrowView.source_place", out);
                        if (module.header.stage_kind != StageKind::Open) {
                            push_error_(out, "OpBorrowView is only legal in gOIR-open");
                        }
                    } else if constexpr (std::is_same_v<T, OpEscapeView>) {
                        verify_value_ref_(module, data.source_place, "OpEscapeView.source_place", out);
                        if (module.header.stage_kind != StageKind::Open) {
                            push_error_(out, "OpEscapeView is only legal in gOIR-open");
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
                    verify_block_args_(module, term.target, term.args, "TermBr", out);
                } else if constexpr (std::is_same_v<T, TermCondBr>) {
                    verify_value_ref_(module, term.cond, "TermCondBr.cond", out);
                    verify_block_args_(module, term.then_bb, term.then_args, "TermCondBr.then", out);
                    verify_block_args_(module, term.else_bb, term.else_args, "TermCondBr.else", out);
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
                    push_error_(out, "placed gOIR must not contain ownership-sensitive values in M1");
                }
            }
            for (const auto& inst : module.insts) {
                if (std::holds_alternative<OpBorrowView>(inst.data) ||
                    std::holds_alternative<OpEscapeView>(inst.data) ||
                    std::holds_alternative<OpSemanticInvoke>(inst.data)) {
                    push_error_(out, "placed gOIR still contains open-stage-only operations");
                }
            }
        }

        return out;
    }

} // namespace parus::goir
