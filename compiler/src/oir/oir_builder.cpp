// compiler/src/oir/oir_builder.cpp
#include <gaupel/oir/Builder.hpp>
#include <gaupel/oir/OIR.hpp>

#include <gaupel/ast/Nodes.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <unordered_map>
#include <vector>
#include <string>


namespace gaupel::oir {

    namespace {

        // OIR building state per function
        struct FuncBuild {
            Module* out = nullptr;
            const gaupel::sir::Module* sir = nullptr;

            Function* fn = nullptr;
            BlockId cur_bb = kInvalidId;

            // symbol -> SSA value or slot
            struct Binding {
                bool is_slot = false;
                ValueId v = kInvalidId; // if is_slot: slot value id, else: SSA value id
            };

            std::unordered_map<gaupel::sir::SymbolId, Binding> env;

            // scope stack for env restoration
            std::vector<std::vector<std::pair<gaupel::sir::SymbolId, Binding>>> env_stack;

            void push_scope() { env_stack.emplace_back(); }
            void pop_scope() {
                if (env_stack.empty()) return;
                auto& undo = env_stack.back();
                for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
                    env[it->first] = it->second;
                }
                env_stack.pop_back();
            }

            void bind(gaupel::sir::SymbolId sym, Binding b) {
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

            ValueId emit_cast(TypeId ty, Effect eff, CastKind kind, TypeId to, ValueId src) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstCast{kind, to, src};
                inst.eff = eff;
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
            ValueId lower_value(gaupel::sir::ValueId vid);
            void    lower_stmt(uint32_t stmt_index);
            void    lower_block(gaupel::sir::BlockId bid);
            ValueId lower_block_expr(gaupel::sir::ValueId block_expr_vid);
            ValueId lower_if_expr(gaupel::sir::ValueId if_vid);

            // util: resolve local reading as SSA or load(slot)
            ValueId read_local(gaupel::sir::SymbolId sym, TypeId want_ty) {
                auto it = env.find(sym);
                if (it == env.end()) {
                    // unknown -> produce dummy (should not happen after name-resolve)
                    return emit_const_null(want_ty);
                }
                if (!it->second.is_slot) return it->second.v;
                return emit_load(want_ty, it->second.v);
            }

            // util: ensure a symbol has a slot (for write), possibly demote SSA to slot
            ValueId ensure_slot(gaupel::sir::SymbolId sym, TypeId slot_ty) {
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

        static BinOp map_binop(gaupel::syntax::TokenKind k) {
            using TK = gaupel::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:              return BinOp::Add;
                case TK::kLt:                return BinOp::Lt;
                case TK::kQuestionQuestion:  return BinOp::NullCoalesce;
                default:                     return BinOp::Add; // v0 fallback
            }
        }

        // -----------------------
        // Lower expressions
        // -----------------------
        ValueId FuncBuild::lower_block_expr(gaupel::sir::ValueId block_expr_vid) {
            const auto& v = sir->values[block_expr_vid];
            // SIR kBlockExpr: v.a = BlockId, v.b = last expr value (convention in your dumps)
            gaupel::sir::BlockId bid = (gaupel::sir::BlockId)v.a;
            gaupel::sir::ValueId last = (gaupel::sir::ValueId)v.b;

            // BlockExpr executes statements in that SIR block in current control-flow
            push_scope();
            lower_block(bid);
            ValueId outv = (last != gaupel::sir::k_invalid_value) ? lower_value(last) : emit_const_null(v.type);
            pop_scope();
            return outv;
        }

        ValueId FuncBuild::lower_if_expr(gaupel::sir::ValueId if_vid) {
            const auto& v = sir->values[if_vid];
            // SIR kIfExpr: v.a = cond, v.b = then blockexpr/value, v.c = else blockexpr/value
            auto cond_sir = (gaupel::sir::ValueId)v.a;
            auto then_sir = (gaupel::sir::ValueId)v.b;
            auto else_sir = (gaupel::sir::ValueId)v.c;

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

        ValueId FuncBuild::lower_value(gaupel::sir::ValueId vid) {
            const auto& v = sir->values[vid];

            switch (v.kind) {
            case gaupel::sir::ValueKind::kIntLit:
                return emit_const_int(v.type, std::string(v.text));

            case gaupel::sir::ValueKind::kBoolLit:
                return emit_const_bool(v.type, v.text == "true");

            case gaupel::sir::ValueKind::kNullLit:
                return emit_const_null(v.type);

            case gaupel::sir::ValueKind::kLocal:
                return read_local(v.sym, v.type);

            case gaupel::sir::ValueKind::kBinary: {
                ValueId lhs = lower_value(v.a);
                ValueId rhs = lower_value(v.b);

                auto tk = static_cast<gaupel::syntax::TokenKind>(v.op);
                BinOp op = map_binop(tk);

                // v0: 대부분 pure로 둔다. (??/비교도 pure)
                return emit_binop(v.type, Effect::Pure, op, lhs, rhs);
            }

            case gaupel::sir::ValueKind::kCast: {
                ValueId src = lower_value(v.a);

                // SIR: v.op는 ast::CastKind 저장(이미 dump_sir_module도 그렇게 해석중)
                auto ck_ast = (gaupel::ast::CastKind)v.op;

                CastKind ck = CastKind::As;
                Effect eff = Effect::Pure;

                switch (ck_ast) {
                    case gaupel::ast::CastKind::kAs:
                        ck = CastKind::As;  eff = Effect::Pure;   break;
                    case gaupel::ast::CastKind::kAsOptional:
                        ck = CastKind::AsQ; eff = Effect::Pure;   break;
                    case gaupel::ast::CastKind::kAsForce:
                        ck = CastKind::AsB; eff = Effect::MayTrap;break;
                }

                return emit_cast(v.type, eff, ck, v.cast_to, src);
            }

            case gaupel::sir::ValueKind::kAssign: {
                // v.a = place, v.b = rhs
                const auto& place = sir->values[v.a];
                ValueId rhs = lower_value(v.b);

                if (place.kind == gaupel::sir::ValueKind::kLocal) {
                    // slot 타입은 place_elem_type 우선 (없으면 기존 place.type)
                    TypeId slot_elem_ty =
                        (place.place_elem_type != gaupel::sir::k_invalid_type)
                            ? (TypeId)place.place_elem_type
                            : (TypeId)place.type;

                    ValueId slot = ensure_slot(place.sym, slot_elem_ty);
                    emit_store(slot, rhs);
                    return rhs; // assign expr result
                }

                // v0: other place kinds not lowered yet
                return rhs;
            }

            case gaupel::sir::ValueKind::kBlockExpr:
                return lower_block_expr(vid);

            case gaupel::sir::ValueKind::kIfExpr:
                return lower_if_expr(vid);

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
            case gaupel::sir::StmtKind::kVarDecl: {
                // let / set
                TypeId declared = s.declared_type;
                ValueId init = (s.init != gaupel::sir::k_invalid_value) ? lower_value(s.init)
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

            case gaupel::sir::StmtKind::kExprStmt:
                if (s.expr != gaupel::sir::k_invalid_value) (void)lower_value(s.expr);
                return;

            case gaupel::sir::StmtKind::kReturn:
                if (s.expr != gaupel::sir::k_invalid_value) {
                    ValueId rv = lower_value(s.expr);
                    ret(rv);
                } else {
                    ret_void();
                }
                return;

            case gaupel::sir::StmtKind::kWhileStmt: {
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
                push_scope();
                lower_block(s.a);
                pop_scope();
                if (!has_term()) br(cond_bb, {});

                // exit
                fn->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case gaupel::sir::StmtKind::kDoScopeStmt: {
                // do { ... } : body를 1회 실행하는 명시 스코프
                push_scope();
                lower_block(s.a);
                pop_scope();
                return;
            }

            case gaupel::sir::StmtKind::kDoWhileStmt: {
                // do-while: body를 먼저 실행하고 조건을 검사한다.
                BlockId body_bb = new_block();
                BlockId cond_bb = new_block();
                BlockId exit_bb = new_block();

                if (!has_term()) br(body_bb, {});

                // body
                fn->blocks.push_back(body_bb);
                cur_bb = body_bb;
                push_scope();
                lower_block(s.a);
                pop_scope();
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

            case gaupel::sir::StmtKind::kIfStmt: {
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
                if (s.b != gaupel::sir::k_invalid_block) lower_block(s.b);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // join
                fn->blocks.push_back(join_bb);
                cur_bb = join_bb;
                return;
            }

            default:
                return;
            }
        }

        void FuncBuild::lower_block(gaupel::sir::BlockId bid) {
            if (bid == gaupel::sir::k_invalid_block) return;

            const auto& b = sir->blocks[bid];
            for (uint32_t i = 0; i < b.stmt_count; i++) {
                uint32_t si = b.stmt_begin + i;
                if (has_term()) break;
                lower_stmt(si);
            }
        }

    } // namespace

    // ------------------------------------------------------------
    // Builder::build
    // ------------------------------------------------------------
    BuildResult Builder::build() {
        BuildResult out{};

        // Build all functions in SIR module.
        // Strategy:
        // - One OIR function per SIR func
        // - Entry OIR block created, then lower SIR entry block into it
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
            fb.fn     = &out.mod.funcs.back();
            fb.cur_bb = entry;

            // seed params? (v0: SIR params are locals; actual param lowering later)
            // current tests don't rely on explicit param slots in SIR func body except locals created by SIR.

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

        return out;
    }

} // namespace gaupel::oir
