#include <parus/goir/Builder.hpp>
#include <parus/goir/Passes.hpp>
#include <parus/goir/Verify.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace parus::goir {

    namespace {

        using parus::sir::StmtKind;
        using parus::sir::ValueKind;
        using parus::syntax::TokenKind;

        bool is_supported_scalar_type_(const parus::ty::TypePool& types, TypeId ty) {
            if (ty == kInvalidType) return false;
            const auto& t = types.get(ty);
            if (t.kind != parus::ty::Kind::kBuiltin) return false;
            using B = parus::ty::Builtin;
            switch (t.builtin) {
                case B::kUnit:
                case B::kBool:
                case B::kChar:
                case B::kI8:
                case B::kI16:
                case B::kI32:
                case B::kI64:
                case B::kU8:
                case B::kU16:
                case B::kU32:
                case B::kU64:
                case B::kISize:
                case B::kUSize:
                case B::kF32:
                case B::kF64:
                    return true;
                default:
                    return false;
            }
        }

        bool is_unit_type_(const parus::ty::TypePool& types, TypeId ty) {
            if (ty == kInvalidType) return false;
            const auto& t = types.get(ty);
            return t.kind == parus::ty::Kind::kBuiltin && t.builtin == parus::ty::Builtin::kUnit;
        }

        bool has_attr_(const parus::sir::Module& sir, const parus::sir::Func& fn, std::string_view name) {
            for (uint32_t i = 0; i < fn.attr_count; ++i) {
                const auto aid = fn.attr_begin + i;
                if (static_cast<size_t>(aid) >= sir.attrs.size()) break;
                if (sir.attrs[aid].name == name) return true;
            }
            return false;
        }

        std::optional<BinOp> map_binop_(TokenKind tk) {
            switch (tk) {
                case TokenKind::kPlus: return BinOp::Add;
                case TokenKind::kMinus: return BinOp::Sub;
                case TokenKind::kStar: return BinOp::Mul;
                case TokenKind::kSlash: return BinOp::Div;
                case TokenKind::kPercent: return BinOp::Rem;
                case TokenKind::kLt: return BinOp::Lt;
                case TokenKind::kLtEq: return BinOp::Le;
                case TokenKind::kGt: return BinOp::Gt;
                case TokenKind::kGtEq: return BinOp::Ge;
                case TokenKind::kEqEq: return BinOp::Eq;
                case TokenKind::kBangEq: return BinOp::Ne;
                case TokenKind::kKwAnd: return BinOp::LogicalAnd;
                case TokenKind::kKwOr: return BinOp::LogicalOr;
                default: return std::nullopt;
            }
        }

        std::optional<UnOp> map_unop_(TokenKind tk) {
            switch (tk) {
                case TokenKind::kPlus: return UnOp::Plus;
                case TokenKind::kMinus: return UnOp::Neg;
                case TokenKind::kKwNot: return UnOp::Not;
                case TokenKind::kBang: return UnOp::BitNot;
                default: return std::nullopt;
            }
        }

        std::optional<CastKind> map_cast_(uint32_t raw) {
            switch (static_cast<parus::ast::CastKind>(raw)) {
                case parus::ast::CastKind::kAs: return CastKind::As;
                case parus::ast::CastKind::kAsOptional: return CastKind::AsQ;
                case parus::ast::CastKind::kAsForce: return CastKind::AsB;
            }
            return std::nullopt;
        }

        class Builder {
        public:
            Builder(const parus::sir::Module& sir, const parus::ty::TypePool& types)
                : sir_(sir), types_(types) {}

            BuildResult build() {
                mod_.header.stage_kind = StageKind::Open;

                discover_supported_funcs_();
                if (!messages_.empty()) return finish_();

                for (size_t i = 0; i < sir_.funcs.size(); ++i) {
                    const auto& fn = sir_.funcs[i];
                    if (!supported_func_[static_cast<uint32_t>(i)]) continue;
                    lower_func_(static_cast<uint32_t>(i), fn);
                }

                run_open_passes(mod_);
                const auto verrs = verify(mod_);
                messages_.insert(messages_.end(), verrs.begin(), verrs.end());
                return finish_();
            }

        private:
            struct FuncState {
                RealizationId realization = kInvalidId;
                BlockId current_block = kInvalidId;
                std::unordered_map<parus::sir::SymbolId, ValueId> locals{};
            };

            const parus::sir::Module& sir_;
            const parus::ty::TypePool& types_;
            Module mod_{};
            std::vector<Message> messages_{};
            std::unordered_map<parus::sir::SymbolId, ComputationId> computation_by_sym_{};
            std::unordered_map<parus::sir::SymbolId, RealizationId> realization_by_sym_{};
            std::unordered_map<uint32_t, SemanticSigId> sig_by_func_index_{};
            std::unordered_map<uint32_t, ComputationId> computation_by_func_index_{};
            std::unordered_map<uint32_t, RealizationId> realization_by_func_index_{};
            std::unordered_map<uint32_t, StringId> name_by_func_index_{};
            std::unordered_map<uint32_t, bool> supported_func_{};

            BuildResult finish_() {
                BuildResult out{};
                out.ok = messages_.empty();
                out.mod = std::move(mod_);
                out.messages = std::move(messages_);
                return out;
            }

            void push_error_(std::string text) {
                messages_.push_back(Message{std::move(text)});
            }

            bool is_supported_signature_(const parus::sir::Func& fn, uint32_t func_index) {
                if (fn.is_extern || fn.is_throwing || fn.abi != parus::sir::FuncAbi::kParus ||
                    fn.is_actor_member || fn.is_actor_init || fn.is_acts_member) {
                    push_error_("gOIR M0 does not support function '" + std::string(fn.name) +
                                "' in the official lane because it is not a pure internal CPU entry.");
                    return false;
                }
                if (!is_supported_scalar_type_(types_, fn.ret) && !is_unit_type_(types_, fn.ret)) {
                    push_error_("gOIR M0 does not support return type of function '" + std::string(fn.name) + "'.");
                    return false;
                }
                for (uint32_t i = 0; i < fn.param_count; ++i) {
                    const auto& param = sir_.params[fn.param_begin + i];
                    if (!is_supported_scalar_type_(types_, param.type)) {
                        push_error_("gOIR M0 does not support parameter type of function '" +
                                    std::string(fn.name) + "'.");
                        return false;
                    }
                }
                (void)func_index;
                return true;
            }

            void discover_supported_funcs_() {
                for (uint32_t i = 0; i < sir_.funcs.size(); ++i) {
                    const auto& fn = sir_.funcs[i];
                    const bool is_pure = fn.is_pure || has_attr_(sir_, fn, "pure");
                    if (!is_pure) {
                        push_error_("gOIR M0 requires pure functions (currently expressed as SIR purity or @pure); unsupported function '" +
                                    std::string(fn.name) + "'.");
                        continue;
                    }
                    if (!is_supported_signature_(fn, i)) continue;

                    const auto name = mod_.add_string(std::string(fn.name));
                    name_by_func_index_[i] = name;

                    SemanticSig sig{};
                    sig.name = name;
                    sig.result_type = fn.ret;
                    sig.is_pure = is_pure;
                    sig.is_throwing = fn.is_throwing;
                    for (uint32_t pi = 0; pi < fn.param_count; ++pi) {
                        sig.param_types.push_back(sir_.params[fn.param_begin + pi].type);
                    }
                    const auto sig_id = mod_.add_semantic_sig(sig);
                    sig_by_func_index_[i] = sig_id;

                    const auto policy_id = mod_.add_placement_policy(GPlacementPolicy{});

                    GComputation comp{};
                    comp.name = name;
                    comp.sig = sig_id;
                    comp.placement_policy = policy_id;
                    const auto comp_id = mod_.add_computation(comp);
                    computation_by_func_index_[i] = comp_id;

                    GRealization real{};
                    real.name = name;
                    real.computation = comp_id;
                    real.family = FamilyKind::Core;
                    real.is_entry = !fn.is_extern;
                    real.is_pure = is_pure;
                    real.is_extern = fn.is_extern;
                    real.fn_type = fn.sig;
                    real.source_func = i;
                    const auto real_id = mod_.add_realization(real);
                    realization_by_func_index_[i] = real_id;

                    mod_.computations[comp_id].realizations.push_back(real_id);
                    computation_by_sym_[fn.sym] = comp_id;
                    realization_by_sym_[fn.sym] = real_id;
                    supported_func_[i] = true;
                }
            }

            ValueId add_block_param_(BlockId bb, TypeId ty, OwnershipInfo ownership = {}) {
                Value value{};
                value.ty = ty;
                value.eff = Effect::Pure;
                value.ownership = ownership;
                value.def_a = bb;
                value.def_b = static_cast<uint32_t>(mod_.blocks[bb].params.size());
                const auto vid = mod_.add_value(value);
                mod_.blocks[bb].params.push_back(vid);
                return vid;
            }

            ValueId emit_inst_(FuncState& state, TypeId result_ty, Effect eff, OpData data,
                               OwnershipInfo ownership = {}) {
                ValueId result = kInvalidId;
                if (result_ty != kInvalidType && !is_unit_type_(types_, result_ty)) {
                    Value value{};
                    value.ty = result_ty;
                    value.eff = eff;
                    value.ownership = ownership;
                    value.def_a = static_cast<uint32_t>(mod_.insts.size());
                    result = mod_.add_value(value);
                }

                Inst inst{};
                inst.data = std::move(data);
                inst.eff = eff;
                inst.result = result;
                const auto iid = mod_.add_inst(inst);
                if (result != kInvalidId) mod_.values[result].def_a = iid;
                mod_.blocks[state.current_block].insts.push_back(iid);
                return result;
            }

            void ensure_block_term_(FuncState& state, Terminator term) {
                if (state.current_block == kInvalidId) return;
                auto& block = mod_.blocks[state.current_block];
                if (block.has_term) return;
                block.term = std::move(term);
                block.has_term = true;
            }

            ValueId lower_value_(FuncState& state, parus::sir::ValueId sid) {
                if (sid == parus::sir::k_invalid_value || static_cast<size_t>(sid) >= sir_.values.size()) {
                    push_error_("gOIR builder saw invalid SIR value id.");
                    return kInvalidId;
                }
                const auto& value = sir_.values[sid];
                switch (value.kind) {
                    case ValueKind::kIntLit:
                        return emit_inst_(state, value.type, Effect::Pure, OpConstInt{std::string(value.text)});
                    case ValueKind::kFloatLit:
                        return emit_inst_(state, value.type, Effect::Pure, OpConstFloat{std::string(value.text)});
                    case ValueKind::kBoolLit:
                        return emit_inst_(state, value.type, Effect::Pure,
                                          OpConstBool{value.text == "true" || value.text == "1"});
                    case ValueKind::kNullLit:
                        return emit_inst_(state, value.type, Effect::Pure, OpConstNull{});
                    case ValueKind::kLocal: {
                        const auto it = state.locals.find(value.sym);
                        if (it == state.locals.end()) {
                            push_error_("gOIR builder could not resolve local symbol in SIR.");
                            return kInvalidId;
                        }
                        return it->second;
                    }
                    case ValueKind::kUnary: {
                        const auto src = lower_value_(state, value.a);
                        const auto op = map_unop_(static_cast<TokenKind>(value.op));
                        if (!op.has_value()) {
                            push_error_("unsupported unary operator in gOIR M0 builder.");
                            return kInvalidId;
                        }
                        return emit_inst_(state, value.type, Effect::Pure, OpUnary{*op, src});
                    }
                    case ValueKind::kBinary: {
                        const auto lhs = lower_value_(state, value.a);
                        const auto rhs = lower_value_(state, value.b);
                        const auto op = map_binop_(static_cast<TokenKind>(value.op));
                        if (!op.has_value()) {
                            push_error_("unsupported binary operator in gOIR M0 builder.");
                            return kInvalidId;
                        }
                        return emit_inst_(state, value.type, Effect::Pure, OpBinary{*op, lhs, rhs});
                    }
                    case ValueKind::kCast: {
                        const auto src = lower_value_(state, value.a);
                        const auto cast = map_cast_(value.op);
                        if (!cast.has_value()) {
                            push_error_("unsupported cast operator in gOIR M0 builder.");
                            return kInvalidId;
                        }
                        return emit_inst_(state, value.type,
                                          (*cast == CastKind::AsB) ? Effect::MayTrap : Effect::Pure,
                                          OpCast{*cast, value.cast_to, src});
                    }
                    case ValueKind::kCall:
                    case ValueKind::kPipeCall: {
                        if (value.core_call_kind != parus::sir::CoreCallKind::kNone) {
                            push_error_("gOIR M0 does not support core runtime helper calls yet.");
                            return kInvalidId;
                        }
                        if (value.call_is_throwing || value.call_is_c_abi) {
                            push_error_("gOIR M0 does not support throwing or C ABI calls.");
                            return kInvalidId;
                        }
                        const auto cit = computation_by_sym_.find(value.callee_sym);
                        if (cit == computation_by_sym_.end()) {
                            push_error_("gOIR M0 only supports direct pure/internal calls.");
                            return kInvalidId;
                        }
                        OpSemanticInvoke invoke{};
                        invoke.computation = cit->second;
                        for (uint32_t i = 0; i < value.arg_count; ++i) {
                            const auto aid = value.arg_begin + i;
                            if (static_cast<size_t>(aid) >= sir_.args.size()) {
                                push_error_("gOIR builder saw invalid SIR arg slice.");
                                return kInvalidId;
                            }
                            const auto& arg = sir_.args[aid];
                            if (arg.is_hole) {
                                push_error_("gOIR M0 does not support hole arguments.");
                                return kInvalidId;
                            }
                            invoke.args.push_back(lower_value_(state, arg.value));
                        }
                        return emit_inst_(state, value.type, Effect::Call, std::move(invoke));
                    }
                    case ValueKind::kBorrow: {
                        OwnershipInfo ownership{};
                        ownership.kind = value.borrow_is_mut ? OwnershipKind::BorrowMut
                                                             : OwnershipKind::BorrowShared;
                        ownership.requires_runtime_lowering = true;
                        return emit_inst_(state, value.type, Effect::Pure, OpConstNull{}, ownership);
                    }
                    case ValueKind::kEscape: {
                        OwnershipInfo ownership{};
                        ownership.kind = OwnershipKind::Escape;
                        ownership.requires_runtime_lowering = true;
                        for (const auto& handle : sir_.escape_handles) {
                            if (handle.escape_value == sid) {
                                ownership.escape_kind = handle.kind;
                                ownership.escape_boundary = handle.boundary;
                                ownership.from_static = handle.from_static;
                                break;
                            }
                        }
                        return emit_inst_(state, value.type, Effect::MayTrap, OpConstNull{}, ownership);
                    }
                    default:
                        push_error_("gOIR M0 builder encountered unsupported SIR value kind.");
                        return kInvalidId;
                }
            }

            void lower_stmt_range_(FuncState& state, uint32_t begin, uint32_t count) {
                for (uint32_t i = 0; i < count; ++i) {
                    if (state.current_block == kInvalidId) return;
                    const auto sid = begin + i;
                    if (static_cast<size_t>(sid) >= sir_.stmts.size()) {
                        push_error_("gOIR builder saw invalid SIR stmt id.");
                        return;
                    }
                    lower_stmt_(state, sir_.stmts[sid]);
                }
            }

            void lower_block_(FuncState& state, parus::sir::BlockId block_id) {
                if (block_id == parus::sir::k_invalid_block || static_cast<size_t>(block_id) >= sir_.blocks.size()) {
                    push_error_("gOIR builder saw invalid SIR block id.");
                    return;
                }
                const auto& block = sir_.blocks[block_id];
                lower_stmt_range_(state, block.stmt_begin, block.stmt_count);
            }

            void lower_if_stmt_(FuncState& state, const parus::sir::Stmt& stmt) {
                const auto cond = lower_value_(state, stmt.expr);
                const auto then_bb = mod_.add_block(Block{});
                const auto else_bb = mod_.add_block(Block{});
                const auto cont_bb = mod_.add_block(Block{});

                auto& real = mod_.realizations[state.realization];
                real.blocks.push_back(then_bb);
                real.blocks.push_back(else_bb);
                real.blocks.push_back(cont_bb);

                ensure_block_term_(state, TermCondBr{
                    .cond = cond,
                    .then_bb = then_bb,
                    .else_bb = else_bb,
                });

                auto then_state = state;
                then_state.current_block = then_bb;
                lower_block_(then_state, stmt.a);
                if (then_state.current_block != kInvalidId &&
                    !mod_.blocks[then_state.current_block].has_term) {
                    ensure_block_term_(then_state, TermBr{.target = cont_bb});
                }

                auto else_state = state;
                else_state.current_block = else_bb;
                if (stmt.b != parus::sir::k_invalid_block) {
                    lower_block_(else_state, stmt.b);
                }
                if (else_state.current_block != kInvalidId &&
                    !mod_.blocks[else_state.current_block].has_term) {
                    ensure_block_term_(else_state, TermBr{.target = cont_bb});
                }

                state.current_block = cont_bb;
            }

            void lower_stmt_(FuncState& state, const parus::sir::Stmt& stmt) {
                switch (stmt.kind) {
                    case StmtKind::kExprStmt:
                        if (stmt.expr != parus::sir::k_invalid_value) {
                            (void)lower_value_(state, stmt.expr);
                        }
                        return;
                    case StmtKind::kVarDecl:
                        if (stmt.is_set || stmt.is_mut || stmt.is_static) {
                            push_error_("gOIR M0 only supports immutable local scalar bindings.");
                            return;
                        }
                        if (stmt.init == parus::sir::k_invalid_value) {
                            push_error_("gOIR M0 requires initialized local bindings.");
                            return;
                        }
                        state.locals[stmt.sym] = lower_value_(state, stmt.init);
                        return;
                    case StmtKind::kIfStmt:
                        lower_if_stmt_(state, stmt);
                        return;
                    case StmtKind::kReturn: {
                        if (stmt.expr == parus::sir::k_invalid_value) {
                            ensure_block_term_(state, TermRet{});
                        } else {
                            const auto value = lower_value_(state, stmt.expr);
                            ensure_block_term_(state, TermRet{
                                .has_value = true,
                                .value = value,
                            });
                        }
                        state.current_block = kInvalidId;
                        return;
                    }
                    default:
                        push_error_("gOIR M0 builder encountered unsupported SIR stmt kind.");
                        return;
                }
            }

            void lower_func_(uint32_t func_index, const parus::sir::Func& fn) {
                FuncState state{};
                state.realization = realization_by_func_index_[func_index];
                state.current_block = mod_.add_block(Block{});

                auto& real = mod_.realizations[state.realization];
                real.entry = state.current_block;
                real.blocks.push_back(state.current_block);

                for (uint32_t i = 0; i < fn.param_count; ++i) {
                    const auto& param = sir_.params[fn.param_begin + i];
                    const auto vid = add_block_param_(state.current_block, param.type);
                    state.locals[param.sym] = vid;
                }

                lower_block_(state, fn.entry);

                if (state.current_block != kInvalidId &&
                    !mod_.blocks[state.current_block].has_term) {
                    if (is_unit_type_(types_, fn.ret)) {
                        ensure_block_term_(state, TermRet{});
                    } else {
                        push_error_("gOIR M0 function '" + std::string(fn.name) + "' falls off the end without return.");
                    }
                }
            }
        };

    } // namespace

    BuildResult build_from_sir(
        const parus::sir::Module& sir,
        const parus::ty::TypePool& types
    ) {
        Builder builder(sir, types);
        return builder.build();
    }

} // namespace parus::goir
