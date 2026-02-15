// frontend/src/oir/oir_builder.cpp
#include <parus/oir/Builder.hpp>
#include <parus/oir/OIR.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <unordered_map>
#include <vector>
#include <string>


namespace parus::oir {

    namespace {

        // OIR building state per function
        struct FuncBuild {
            Module* out = nullptr;
            const parus::sir::Module* sir = nullptr;
            std::unordered_map<parus::sir::ValueId, ValueId>* escape_value_map = nullptr;

            Function* fn = nullptr;
            BlockId cur_bb = kInvalidId;

            // symbol -> SSA value or slot
            struct Binding {
                bool is_slot = false;
                ValueId v = kInvalidId; // if is_slot: slot value id, else: SSA value id
            };

            std::unordered_map<parus::sir::SymbolId, Binding> env;

            struct LoopContext {
                BlockId break_bb = kInvalidId;
                BlockId continue_bb = kInvalidId;
                bool expects_break_value = false;
                TypeId break_ty = kInvalidId;
            };

            std::vector<LoopContext> loop_stack;

            // scope stack for env restoration
            std::vector<std::vector<std::pair<parus::sir::SymbolId, Binding>>> env_stack;

            void push_scope() { env_stack.emplace_back(); }
            void pop_scope() {
                if (env_stack.empty()) return;
                auto& undo = env_stack.back();
                for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
                    env[it->first] = it->second;
                }
                env_stack.pop_back();
            }

            void bind(parus::sir::SymbolId sym, Binding b) {
                // record previous for undo
                if (!env_stack.empty()) {
                    auto it = env.find(sym);
                    if (it != env.end()) env_stack.back().push_back({sym, it->second});
                    else env_stack.back().push_back({sym, Binding{false, kInvalidId}});
                }
                env[sym] = b;
            }

            // -----------------------
            // OIR creation helpers
            // -----------------------
            ValueId make_value(TypeId ty, Effect eff, uint32_t def_a=kInvalidId, uint32_t def_b=kInvalidId) {
                Value v{};
                v.ty = ty;
                v.eff = eff;
                v.def_a = def_a;
                v.def_b = def_b;
                return out->add_value(v);
            }

            BlockId new_block() {
                Block b{};
                return out->add_block(b);
            }

            ValueId add_block_param(BlockId bb, TypeId ty) {
                // create value as block param
                auto& block = out->blocks[bb];
                uint32_t idx = (uint32_t)block.params.size();
                ValueId vid = make_value(ty, Effect::Pure, /*def_a=*/bb, /*def_b=*/idx);
                block.params.push_back(vid);
                return vid;
            }

            InstId emit_inst(const Inst& inst) {
                InstId iid = out->add_inst(inst);
                out->blocks[cur_bb].insts.push_back(iid);
                return iid;
            }

            ValueId emit_const_int(TypeId ty, std::string text) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstInt{std::move(text)};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_bool(TypeId ty, bool v) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstBool{v};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_null(TypeId ty) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstNull{};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_binop(TypeId ty, Effect eff, BinOp op, ValueId lhs, ValueId rhs) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstBinOp{op, lhs, rhs};
                inst.eff = eff;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_unary(TypeId ty, Effect eff, UnOp op, ValueId src) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstUnary{op, src};
                inst.eff = eff;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_cast(TypeId ty, Effect eff, CastKind kind, TypeId to, ValueId src) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstCast{kind, to, src};
                inst.eff = eff;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_call(TypeId ty, ValueId callee, std::vector<ValueId> args) {
                ValueId r = make_value(ty, Effect::Call);
                Inst inst{};
                inst.data = InstCall{callee, std::move(args)};
                inst.eff = Effect::Call;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_index(TypeId ty, ValueId base, ValueId index) {
                ValueId r = make_value(ty, Effect::MayReadMem);
                Inst inst{};
                inst.data = InstIndex{base, index};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_field(TypeId ty, ValueId base, std::string field) {
                ValueId r = make_value(ty, Effect::MayReadMem);
                Inst inst{};
                inst.data = InstField{base, std::move(field)};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_alloca(TypeId slot_ty) {
                // slot value: use special convention: its ty is slot_ty as-is.
                // backend can treat it as addressable slot.
                ValueId r = make_value(slot_ty, Effect::MayWriteMem);
                Inst inst{};
                inst.data = InstAllocaLocal{slot_ty};
                inst.eff = Effect::MayWriteMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_load(TypeId ty, ValueId slot) {
                ValueId r = make_value(ty, Effect::MayReadMem);
                Inst inst{};
                inst.data = InstLoad{slot};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            void emit_store(ValueId slot, ValueId val) {
                Inst inst{};
                inst.data = InstStore{slot, val};
                inst.eff = Effect::MayWriteMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            void set_term(const Terminator& t) {
                auto& b = out->blocks[cur_bb];
                b.term = t;
                b.has_term = true;
            }

            bool has_term() const {
                return out->blocks[cur_bb].has_term;
            }

            void br(BlockId target, std::vector<ValueId> args = {}) {
                TermBr t{};
                t.target = target;
                t.args = std::move(args);
                set_term(t);
            }

            void condbr(ValueId cond,
                        BlockId then_bb, std::vector<ValueId> then_args,
                        BlockId else_bb, std::vector<ValueId> else_args) {
                TermCondBr t{};
                t.cond = cond;
                t.then_bb = then_bb;
                t.then_args = std::move(then_args);
                t.else_bb = else_bb;
                t.else_args = std::move(else_args);
                set_term(t);
            }

            void ret_void() {
                TermRet t{};
                t.has_value = false;
                t.value = kInvalidId;
                set_term(t);
            }

            void ret(ValueId v) {
                TermRet t{};
                t.has_value = true;
                t.value = v;
                set_term(t);
            }

            // -----------------------
            // SIR -> OIR lowering
            // -----------------------
            ValueId lower_value(parus::sir::ValueId vid);
            void    lower_stmt(uint32_t stmt_index);
            void    lower_block(parus::sir::BlockId bid);
            ValueId lower_block_expr(parus::sir::ValueId block_expr_vid);
            ValueId lower_if_expr(parus::sir::ValueId if_vid);

            // util: resolve local reading as SSA or load(slot)
            ValueId read_local(parus::sir::SymbolId sym, TypeId want_ty) {
                auto it = env.find(sym);
                if (it == env.end()) {
                    // unknown -> produce dummy (should not happen after name-resolve)
                    return emit_const_null(want_ty);
                }
                if (!it->second.is_slot) return it->second.v;
                return emit_load(want_ty, it->second.v);
            }

            // util: ensure a symbol has a slot (for write), possibly demote SSA to slot
            ValueId ensure_slot(parus::sir::SymbolId sym, TypeId slot_ty) {
                auto it = env.find(sym);
                if (it != env.end() && it->second.is_slot) return it->second.v;

                // create a new slot
                ValueId slot = emit_alloca(slot_ty);

                // if previously SSA value existed, initialize slot with it
                if (it != env.end() && !it->second.is_slot && it->second.v != kInvalidId) {
                    emit_store(slot, it->second.v);
                }

                bind(sym, Binding{true, slot});
                return slot;
            }
        };

        static BinOp map_binop(parus::syntax::TokenKind k) {
            using TK = parus::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:              return BinOp::Add;
                case TK::kMinus:             return BinOp::Sub;
                case TK::kStar:              return BinOp::Mul;
                case TK::kSlash:             return BinOp::Div;
                case TK::kPercent:           return BinOp::Rem;
                case TK::kLt:                return BinOp::Lt;
                case TK::kLtEq:              return BinOp::Le;
                case TK::kGt:                return BinOp::Gt;
                case TK::kGtEq:              return BinOp::Ge;
                case TK::kEqEq:              return BinOp::Eq;
                case TK::kBangEq:            return BinOp::Ne;
                case TK::kQuestionQuestion:  return BinOp::NullCoalesce;
                default:                     return BinOp::Add; // v0 fallback
            }
        }

        static UnOp map_unary(parus::syntax::TokenKind k) {
            using TK = parus::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:  return UnOp::Plus;
                case TK::kMinus: return UnOp::Neg;
                case TK::kBang:
                case TK::kKwNot: return UnOp::Not;
                case TK::kCaret: return UnOp::BitNot;
                default:         return UnOp::Plus;
            }
        }

        // -----------------------
        // Lower expressions
        // -----------------------
        ValueId FuncBuild::lower_block_expr(parus::sir::ValueId block_expr_vid) {
            const auto& v = sir->values[block_expr_vid];
            // SIR kBlockExpr: v.a = BlockId, v.b = last expr value (convention in your dumps)
            parus::sir::BlockId bid = (parus::sir::BlockId)v.a;
            parus::sir::ValueId last = (parus::sir::ValueId)v.b;

            // BlockExpr executes statements in that SIR block in current control-flow
            push_scope();
            lower_block(bid);
            ValueId outv = (last != parus::sir::k_invalid_value) ? lower_value(last) : emit_const_null(v.type);
            pop_scope();
            return outv;
        }

        ValueId FuncBuild::lower_if_expr(parus::sir::ValueId if_vid) {
            const auto& v = sir->values[if_vid];
            // SIR kIfExpr: v.a = cond, v.b = then blockexpr/value, v.c = else blockexpr/value
            auto cond_sir = (parus::sir::ValueId)v.a;
            auto then_sir = (parus::sir::ValueId)v.b;
            auto else_sir = (parus::sir::ValueId)v.c;

            ValueId cond = lower_value(cond_sir);

            // create blocks
            BlockId then_bb = new_block();
            BlockId else_bb = new_block();
            BlockId join_bb = new_block();

            // join has one param: result of if expr
            ValueId join_param = add_block_param(join_bb, v.type);

            // terminate current with condbr
            condbr(cond, then_bb, {}, else_bb, {});

            // THEN
            fn->blocks.push_back(then_bb);
            cur_bb = then_bb;
            push_scope();
            ValueId then_val = lower_value(then_sir);
            pop_scope();
            if (!has_term()) br(join_bb, {then_val});

            // ELSE
            fn->blocks.push_back(else_bb);
            cur_bb = else_bb;
            push_scope();
            ValueId else_val = lower_value(else_sir);
            pop_scope();
            if (!has_term()) br(join_bb, {else_val});

            // JOIN
            fn->blocks.push_back(join_bb);
            cur_bb = join_bb;

            // NOTE: In v0 we do not yet verify arg counts strictly here,
            // but verify() will later ensure terminators exist.
            (void)join_param;
            return join_param;
        }

        ValueId FuncBuild::lower_value(parus::sir::ValueId vid) {
            const auto& v = sir->values[vid];

            switch (v.kind) {
            case parus::sir::ValueKind::kIntLit:
                return emit_const_int(v.type, std::string(v.text));

            case parus::sir::ValueKind::kBoolLit:
                return emit_const_bool(v.type, v.text == "true");

            case parus::sir::ValueKind::kNullLit:
                return emit_const_null(v.type);

            case parus::sir::ValueKind::kLocal:
                return read_local(v.sym, v.type);

            case parus::sir::ValueKind::kBorrow:
            case parus::sir::ValueKind::kEscape:
                // v0: borrow/escape는 컴파일타임 capability 토큰이다.
                // OIR에서는 비물질화 원칙을 유지하고 원본 값으로 전달한다.
                {
                    const ValueId lowered = lower_value(v.a);
                    if (v.kind == parus::sir::ValueKind::kEscape && escape_value_map != nullptr) {
                        (*escape_value_map)[vid] = lowered;
                    }
                    return lowered;
                }

            case parus::sir::ValueKind::kUnary: {
                ValueId src = lower_value(v.a);
                auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                UnOp op = map_unary(tk);
                return emit_unary(v.type, Effect::Pure, op, src);
            }

            case parus::sir::ValueKind::kBinary: {
                ValueId lhs = lower_value(v.a);
                ValueId rhs = lower_value(v.b);

                auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                BinOp op = map_binop(tk);

                // v0: 대부분 pure로 둔다. (??/비교도 pure)
                return emit_binop(v.type, Effect::Pure, op, lhs, rhs);
            }

            case parus::sir::ValueKind::kCast: {
                ValueId src = lower_value(v.a);

                // SIR: v.op는 ast::CastKind 저장(이미 dump_sir_module도 그렇게 해석중)
                auto ck_ast = (parus::ast::CastKind)v.op;

                CastKind ck = CastKind::As;
                Effect eff = Effect::Pure;

                switch (ck_ast) {
                    case parus::ast::CastKind::kAs:
                        ck = CastKind::As;  eff = Effect::Pure;   break;
                    case parus::ast::CastKind::kAsOptional:
                        ck = CastKind::AsQ; eff = Effect::Pure;   break;
                    case parus::ast::CastKind::kAsForce:
                        ck = CastKind::AsB; eff = Effect::MayTrap;break;
                }

                return emit_cast(v.type, eff, ck, v.cast_to, src);
            }

            case parus::sir::ValueKind::kCall: {
                ValueId callee = lower_value(v.a);
                std::vector<ValueId> args;
                args.reserve(v.arg_count);

                uint32_t i = 0;
                while (i < v.arg_count) {
                    const uint32_t aid = v.arg_begin + i;
                    if ((size_t)aid >= sir->args.size()) break;
                    const auto& a = sir->args[aid];

                    if (a.kind == parus::sir::ArgKind::kNamedGroup) {
                        for (uint32_t j = 0; j < a.child_count; ++j) {
                            const uint32_t cid = a.child_begin + j;
                            if ((size_t)cid >= sir->args.size()) break;
                            const auto& child = sir->args[cid];
                            if (child.is_hole || child.value == parus::sir::k_invalid_value) continue;
                            args.push_back(lower_value(child.value));
                        }
                        i += 1 + a.child_count;
                        continue;
                    }

                    if (!a.is_hole && a.value != parus::sir::k_invalid_value) {
                        args.push_back(lower_value(a.value));
                    }
                    ++i;
                }

                return emit_call(v.type, callee, std::move(args));
            }

            case parus::sir::ValueKind::kIndex: {
                ValueId base = lower_value(v.a);
                ValueId idx = lower_value(v.b);
                return emit_index(v.type, base, idx);
            }

            case parus::sir::ValueKind::kField: {
                ValueId base = lower_value(v.a);
                return emit_field(v.type, base, std::string(v.text));
            }

            case parus::sir::ValueKind::kAssign: {
                // v.a = place, v.b = rhs
                const auto& place = sir->values[v.a];
                ValueId rhs = lower_value(v.b);

                if (place.kind == parus::sir::ValueKind::kLocal) {
                    // slot 타입은 place_elem_type 우선 (없으면 기존 place.type)
                    TypeId slot_elem_ty =
                        (place.place_elem_type != parus::sir::k_invalid_type)
                            ? (TypeId)place.place_elem_type
                            : (TypeId)place.type;

                    ValueId slot = ensure_slot(place.sym, slot_elem_ty);
                    emit_store(slot, rhs);
                    return rhs; // assign expr result
                }

                // local 외 place(index/field 등)은 generic store로 남긴다.
                // (백엔드에서 place 해석을 확장할 수 있도록 형태를 유지)
                ValueId place_v = lower_value(v.a);
                emit_store(place_v, rhs);
                return rhs;
            }

            case parus::sir::ValueKind::kPostfixInc: {
                const auto& place = sir->values[v.a];
                if (place.kind == parus::sir::ValueKind::kLocal) {
                    TypeId slot_elem_ty =
                        (place.place_elem_type != parus::sir::k_invalid_type)
                            ? (TypeId)place.place_elem_type
                            : (TypeId)place.type;
                    ValueId slot = ensure_slot(place.sym, slot_elem_ty);
                    ValueId oldv = emit_load(v.type, slot);
                    ValueId one = emit_const_int(v.type, "1");
                    ValueId next = emit_binop(v.type, Effect::Pure, BinOp::Add, oldv, one);
                    emit_store(slot, next);
                    return oldv;
                }

                ValueId src = lower_value(v.a);
                ValueId one = emit_const_int(v.type, "1");
                return emit_binop(v.type, Effect::Pure, BinOp::Add, src, one);
            }

            case parus::sir::ValueKind::kBlockExpr:
                return lower_block_expr(vid);

            case parus::sir::ValueKind::kIfExpr:
                return lower_if_expr(vid);

            case parus::sir::ValueKind::kLoopExpr: {
                const BlockId body_bb = new_block();
                const BlockId exit_bb = new_block();

                const bool has_value = (v.type != parus::sir::k_invalid_type);
                const ValueId break_param = has_value ? add_block_param(exit_bb, v.type) : kInvalidId;

                if (!has_term()) br(body_bb, {});

                fn->blocks.push_back(body_bb);
                cur_bb = body_bb;
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = body_bb,
                    .expects_break_value = has_value,
                    .break_ty = (TypeId)v.type
                });
                push_scope();
                lower_block((parus::sir::BlockId)v.b);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(body_bb, {});

                fn->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                if (has_value) return break_param;
                return emit_const_null(v.type);
            }

            default:
                return emit_const_null(v.type);
            }
        }

        // -----------------------
        // Lower statements/blocks
        // -----------------------
        void FuncBuild::lower_stmt(uint32_t stmt_index) {
            const auto& s = sir->stmts[stmt_index];

            switch (s.kind) {
            case parus::sir::StmtKind::kVarDecl: {
                // let / set
                TypeId declared = s.declared_type;
                ValueId init = (s.init != parus::sir::k_invalid_value) ? lower_value(s.init)
                                                                    : emit_const_null(declared);

                // if set or mut => slot
                if (s.is_set || s.is_mut) {
                    ValueId slot = emit_alloca(declared);
                    emit_store(slot, init);
                    bind(s.sym, Binding{true, slot});
                } else {
                    // immutable let => SSA binding
                    bind(s.sym, Binding{false, init});
                }
                return;
            }

            case parus::sir::StmtKind::kExprStmt:
                if (s.expr != parus::sir::k_invalid_value) (void)lower_value(s.expr);
                return;

            case parus::sir::StmtKind::kReturn:
                if (s.expr != parus::sir::k_invalid_value) {
                    ValueId rv = lower_value(s.expr);
                    ret(rv);
                } else {
                    ret_void();
                }
                return;

            case parus::sir::StmtKind::kWhileStmt: {
                // SIR: s.expr = cond, s.a = body block
                BlockId cond_bb = new_block();
                BlockId body_bb = new_block();
                BlockId exit_bb = new_block();

                // jump to cond
                if (!has_term()) br(cond_bb, {});

                // cond block
                fn->blocks.push_back(cond_bb);
                cur_bb = cond_bb;
                ValueId cond = lower_value(s.expr);
                condbr(cond, body_bb, {}, exit_bb, {});

                // body
                fn->blocks.push_back(body_bb);
                cur_bb = body_bb;
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = cond_bb,
                    .expects_break_value = false,
                    .break_ty = kInvalidId
                });
                push_scope();
                lower_block(s.a);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(cond_bb, {});

                // exit
                fn->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kDoScopeStmt: {
                // do { ... } : body를 1회 실행하는 명시 스코프
                push_scope();
                lower_block(s.a);
                pop_scope();
                return;
            }

            case parus::sir::StmtKind::kDoWhileStmt: {
                // do-while: body를 먼저 실행하고 조건을 검사한다.
                BlockId body_bb = new_block();
                BlockId cond_bb = new_block();
                BlockId exit_bb = new_block();

                if (!has_term()) br(body_bb, {});

                // body
                fn->blocks.push_back(body_bb);
                cur_bb = body_bb;
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = cond_bb,
                    .expects_break_value = false,
                    .break_ty = kInvalidId
                });
                push_scope();
                lower_block(s.a);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(cond_bb, {});

                // cond
                fn->blocks.push_back(cond_bb);
                cur_bb = cond_bb;
                ValueId cond = lower_value(s.expr);
                condbr(cond, body_bb, {}, exit_bb, {});

                // exit
                fn->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kIfStmt: {
                // v0: stmt-level if (not expression). SIR: s.expr=cond, s.a=then block, s.b=else block
                BlockId then_bb = new_block();
                BlockId else_bb = new_block();
                BlockId join_bb = new_block();

                ValueId cond = lower_value(s.expr);
                condbr(cond, then_bb, {}, else_bb, {});

                // then
                fn->blocks.push_back(then_bb);
                cur_bb = then_bb;
                push_scope();
                lower_block(s.a);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // else
                fn->blocks.push_back(else_bb);
                cur_bb = else_bb;
                push_scope();
                if (s.b != parus::sir::k_invalid_block) lower_block(s.b);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // join
                fn->blocks.push_back(join_bb);
                cur_bb = join_bb;
                return;
            }

            case parus::sir::StmtKind::kBreak: {
                if (loop_stack.empty()) return;
                const auto& lc = loop_stack.back();

                if (lc.expects_break_value) {
                    ValueId bv = (s.expr != parus::sir::k_invalid_value)
                               ? lower_value(s.expr)
                               : emit_const_null(lc.break_ty);
                    br(lc.break_bb, {bv});
                } else {
                    br(lc.break_bb, {});
                }
                return;
            }

            case parus::sir::StmtKind::kContinue: {
                if (loop_stack.empty()) return;
                br(loop_stack.back().continue_bb, {});
                return;
            }

            default:
                return;
            }
        }

        void FuncBuild::lower_block(parus::sir::BlockId bid) {
            if (bid == parus::sir::k_invalid_block) return;

            const auto& b = sir->blocks[bid];
            for (uint32_t i = 0; i < b.stmt_count; i++) {
                uint32_t si = b.stmt_begin + i;
                if (has_term()) break;
                lower_stmt(si);
            }
        }

        /// @brief SIR escape kind를 OIR 힌트 kind로 변환한다.
        EscapeHandleKind map_escape_kind_(parus::sir::EscapeHandleKind k) {
            using SK = parus::sir::EscapeHandleKind;
            switch (k) {
                case SK::kTrivial:    return EscapeHandleKind::Trivial;
                case SK::kStackSlot:  return EscapeHandleKind::StackSlot;
                case SK::kCallerSlot: return EscapeHandleKind::CallerSlot;
                case SK::kHeapBox:    return EscapeHandleKind::HeapBox;
            }
            return EscapeHandleKind::Trivial;
        }

        /// @brief SIR escape boundary를 OIR 힌트 boundary로 변환한다.
        EscapeBoundaryKind map_escape_boundary_(parus::sir::EscapeBoundaryKind k) {
            using SB = parus::sir::EscapeBoundaryKind;
            switch (k) {
                case SB::kNone:   return EscapeBoundaryKind::None;
                case SB::kReturn: return EscapeBoundaryKind::Return;
                case SB::kCallArg:return EscapeBoundaryKind::CallArg;
                case SB::kAbi:    return EscapeBoundaryKind::Abi;
                case SB::kFfi:    return EscapeBoundaryKind::Ffi;
            }
            return EscapeBoundaryKind::None;
        }

    } // namespace

    // ------------------------------------------------------------
    // Builder::build
    // ------------------------------------------------------------
    BuildResult Builder::build() {
        BuildResult out{};

        // OIR 진입 게이트:
        // - handle 비물질화(materialize_count==0)
        // - static/boundary 규칙
        // - escape 메타 일관성
        // 위 규칙을 만족하지 않으면 OIR lowering 자체를 중단한다.
        out.gate_errors = parus::sir::verify_escape_handles(sir_);
        if (!out.gate_errors.empty()) {
            out.gate_passed = false;
            return out;
        }

        // Build all functions in SIR module.
        // Strategy:
        // - One OIR function per SIR func
        // - Entry OIR block created, then lower SIR entry block into it
        std::unordered_map<parus::sir::ValueId, ValueId> escape_value_map;

        for (const auto& sf : sir_.funcs) {

            // --- create OIR function shell ---
            Function f{};
            f.name   = std::string(sf.name);
            f.ret_ty = (TypeId)sf.ret;

            // --- create entry block ---
            BlockId entry = out.mod.add_block(Block{});
            f.entry = entry;
            f.blocks.push_back(entry);

            // Register function into module (so fb.fn can point to stable storage)
            FuncId fid = out.mod.add_func(f);
            (void)fid;

            FuncBuild fb{};
            fb.out    = &out.mod;
            fb.sir    = &sir_;
            fb.escape_value_map = &escape_value_map;
            fb.fn     = &out.mod.funcs.back();
            fb.cur_bb = entry;

            // 함수 파라미터를 entry block parameter로 시드하고 심볼 바인딩을 연결한다.
            const uint64_t pend = (uint64_t)sf.param_begin + (uint64_t)sf.param_count;
            if (pend <= (uint64_t)sir_.params.size()) {
                for (uint32_t i = 0; i < sf.param_count; ++i) {
                    const auto& sp = sir_.params[sf.param_begin + i];
                    ValueId pv = fb.add_block_param(entry, (TypeId)sp.type);
                    if (sp.sym == parus::sir::k_invalid_symbol) continue;

                    if (sp.is_mut) {
                        ValueId slot = fb.emit_alloca((TypeId)sp.type);
                        fb.emit_store(slot, pv);
                        fb.bind(sp.sym, FuncBuild::Binding{true, slot});
                    } else {
                        fb.bind(sp.sym, FuncBuild::Binding{false, pv});
                    }
                }
            }

            // lower entry block
            fb.push_scope();
            fb.lower_block(sf.entry);
            fb.pop_scope();

            // if no terminator, add default return:
            // - return null for non-void (v0)
            if (!out.mod.blocks[fb.cur_bb].has_term) {
                ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                fb.ret(rv);
            }
        }

        // SIR escape-handle 메타를 OIR 힌트로 연결한다.
        for (const auto& h : sir_.escape_handles) {
            auto it = escape_value_map.find(h.escape_value);
            if (it == escape_value_map.end()) continue;

            EscapeHandleHint hint{};
            hint.value = it->second;
            hint.pointee_type = (TypeId)h.pointee_type;
            hint.kind = map_escape_kind_(h.kind);
            hint.boundary = map_escape_boundary_(h.boundary);
            hint.from_static = h.from_static;
            hint.has_drop = h.has_drop;
            hint.abi_pack_required = h.abi_pack_required;
            hint.ffi_pack_required = h.ffi_pack_required;
            out.mod.add_escape_hint(hint);
        }

        return out;
    }

} // namespace parus::oir
