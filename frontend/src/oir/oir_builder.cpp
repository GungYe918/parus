// frontend/src/oir/oir_builder.cpp
#include <parus/oir/Builder.hpp>
#include <parus/oir/OIR.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cctype>
#include <optional>
#include <sstream>
#include <algorithm>


namespace parus::oir {

    namespace {

        // OIR building state per function
        struct FuncBuild {
            Module* out = nullptr;
            const parus::sir::Module* sir = nullptr;
            const parus::ty::TypePool* types = nullptr;
            std::unordered_map<parus::sir::ValueId, ValueId>* escape_value_map = nullptr;
            const std::unordered_map<parus::sir::SymbolId, FuncId>* fn_symbol_to_func = nullptr;
            const std::unordered_map<parus::sir::SymbolId, std::vector<FuncId>>* fn_symbol_to_funcs = nullptr;
            const std::unordered_map<uint32_t, FuncId>* fn_decl_to_func = nullptr;
            const std::unordered_map<parus::sir::SymbolId, uint32_t>* global_symbol_to_global = nullptr;
            const std::unordered_map<TypeId, FuncId>* class_deinit_map = nullptr;
            std::vector<parus::sir::VerifyError>* build_errors = nullptr;

            Function* def = nullptr;
            BlockId cur_bb = kInvalidId;

            // symbol -> SSA value or slot
            struct Binding {
                bool is_slot = false;
                bool is_direct_address = false; // true for symbol that is already an address (e.g. globals)
                ValueId v = kInvalidId; // if is_slot: slot value id, else: SSA value id
                uint32_t cleanup_id = kInvalidId; // cleanup item index for RAII-managed class values
            };

            std::unordered_map<parus::sir::SymbolId, Binding> env;

            struct LoopContext {
                BlockId break_bb = kInvalidId;
                BlockId continue_bb = kInvalidId;
                bool expects_break_value = false;
                TypeId break_ty = kInvalidId;
                size_t scope_depth_base = 0; // number of active scopes to keep on break/continue
            };

            std::vector<LoopContext> loop_stack;

            struct CleanupItem {
                parus::sir::SymbolId sym = parus::sir::k_invalid_symbol;
                ValueId slot = kInvalidId;
                TypeId owner_ty = kInvalidId;
                bool moved = false;
            };

            struct ScopeFrame {
                std::vector<std::pair<parus::sir::SymbolId, Binding>> undo{};
                std::vector<uint32_t> cleanup_items{};
                bool cleaned = false;
            };

            std::vector<ScopeFrame> env_stack;
            std::vector<CleanupItem> cleanup_items;

            const FieldLayoutDecl* find_field_layout_(TypeId t) const {
                if (out == nullptr) return nullptr;
                for (const auto& f : out->fields) {
                    if (f.self_type == t) return &f;
                }
                return nullptr;
            }

            bool lookup_user_deinit_for_class_(TypeId t, FuncId& out_fid) const {
                if (class_deinit_map == nullptr || t == kInvalidId) return false;
                auto it = class_deinit_map->find(t);
                if (it == class_deinit_map->end() || it->second == kInvalidId) return false;
                out_fid = it->second;
                return true;
            }

            bool type_needs_drop_rec_(TypeId t, std::unordered_set<TypeId>& visiting) const {
                if (types == nullptr || t == kInvalidId) return false;
                if (!visiting.insert(t).second) return false;

                const auto& tt = types->get(t);
                switch (tt.kind) {
                    case parus::ty::Kind::kOptional:
                        return (tt.elem != kInvalidId) && type_needs_drop_rec_(tt.elem, visiting);

                    case parus::ty::Kind::kArray:
                        if (!tt.array_has_size) return false; // unsized T[] is a non-owning view
                        return (tt.elem != kInvalidId) && type_needs_drop_rec_(tt.elem, visiting);

                    case parus::ty::Kind::kNamedUser: {
                        FuncId deinit_fid = kInvalidId;
                        const bool has_user_deinit = lookup_user_deinit_for_class_(t, deinit_fid);
                        const FieldLayoutDecl* layout = find_field_layout_(t);
                        if (has_user_deinit) return true;
                        if (layout == nullptr) return false;
                        for (const auto& m : layout->members) {
                            if (type_needs_drop_rec_(m.type, visiting)) return true;
                        }
                        return false;
                    }

                    default:
                        return false;
                }
            }

            bool type_needs_drop_(TypeId t) const {
                std::unordered_set<TypeId> visiting{};
                return type_needs_drop_rec_(t, visiting);
            }

            void emit_drop(TypeId owner_ty, ValueId slot) {
                if (slot == kInvalidId || owner_ty == kInvalidId) return;
                Inst inst{};
                inst.data = InstDrop{slot, owner_ty};
                inst.eff = Effect::MayWriteMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            void emit_cleanup_item(uint32_t cleanup_id) {
                if (cleanup_id == kInvalidId || cleanup_id >= cleanup_items.size()) return;
                auto& item = cleanup_items[cleanup_id];
                if (item.moved || item.slot == kInvalidId || item.owner_ty == kInvalidId) return;
                emit_drop(item.owner_ty, item.slot);
                item.moved = true;
            }

            void emit_cleanups_to_depth(size_t keep_depth) {
                if (keep_depth > env_stack.size()) keep_depth = env_stack.size();
                for (size_t i = env_stack.size(); i > keep_depth; --i) {
                    auto& frame = env_stack[i - 1];
                    if (frame.cleaned) continue;
                    for (auto it = frame.cleanup_items.rbegin(); it != frame.cleanup_items.rend(); ++it) {
                        emit_cleanup_item(*it);
                    }
                    frame.cleaned = true;
                }
            }

            void push_scope() { env_stack.emplace_back(); }
            void pop_scope() {
                if (env_stack.empty()) return;
                auto& frame = env_stack.back();
                if (!frame.cleaned && !has_term()) {
                    for (auto it = frame.cleanup_items.rbegin(); it != frame.cleanup_items.rend(); ++it) {
                        emit_cleanup_item(*it);
                    }
                    frame.cleaned = true;
                }
                for (auto it = frame.undo.rbegin(); it != frame.undo.rend(); ++it) {
                    env[it->first] = it->second;
                }
                env_stack.pop_back();
            }

            uint32_t register_cleanup(parus::sir::SymbolId sym, ValueId slot, TypeId owner_ty) {
                CleanupItem ci{};
                ci.sym = sym;
                ci.slot = slot;
                ci.owner_ty = owner_ty;
                ci.moved = false;
                const uint32_t id = static_cast<uint32_t>(cleanup_items.size());
                cleanup_items.push_back(ci);
                if (!env_stack.empty()) {
                    env_stack.back().cleanup_items.push_back(id);
                }
                return id;
            }

            void mark_symbol_moved(parus::sir::SymbolId sym, bool moved = true) {
                auto it = env.find(sym);
                if (it == env.end()) return;
                if (it->second.cleanup_id == kInvalidId || it->second.cleanup_id >= cleanup_items.size()) return;
                cleanup_items[it->second.cleanup_id].moved = moved;
            }

            void drop_symbol_before_overwrite(parus::sir::SymbolId sym) {
                auto it = env.find(sym);
                if (it == env.end()) return;
                const uint32_t cid = it->second.cleanup_id;
                if (cid == kInvalidId || cid >= cleanup_items.size()) return;
                emit_cleanup_item(cid);
            }

            void bind(parus::sir::SymbolId sym, Binding b) {
                // record previous for undo
                if (!env_stack.empty()) {
                    auto it = env.find(sym);
                    if (it != env.end()) env_stack.back().undo.push_back({sym, it->second});
                    else env_stack.back().undo.push_back({sym, Binding{false, false, kInvalidId, kInvalidId}});
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
                // OIR 값 정의 위치(def_a/def_b)를 즉시 동기화한다.
                // - 일반 inst result: def_a = inst_id, def_b = kInvalidId
                // - no-result inst(store 등): 값 메타 갱신 없음
                if (inst.result != kInvalidId && (size_t)inst.result < out->values.size()) {
                    out->values[inst.result].def_a = iid;
                    out->values[inst.result].def_b = kInvalidId;
                }
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

            ValueId emit_const_float(TypeId ty, std::string text) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstFloat{std::move(text)};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_char(TypeId ty, uint32_t value) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstChar{value};
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

            ValueId emit_const_text(TypeId ty, std::string bytes) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstText{std::move(bytes)};
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

            ValueId emit_func_ref(FuncId fid, const std::string& name) {
                const TypeId ptr_ty =
                    (types != nullptr)
                        ? (TypeId)types->builtin(parus::ty::Builtin::kNull)
                        : kInvalidId;
                ValueId r = make_value(ptr_ty, Effect::Pure);
                Inst inst{};
                inst.data = InstFuncRef{fid, name};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_global_ref(uint32_t gid, const std::string& name) {
                const TypeId ptr_ty =
                    (types != nullptr)
                        ? (TypeId)types->builtin(parus::ty::Builtin::kNull)
                        : kInvalidId;
                ValueId r = make_value(ptr_ty, Effect::Pure);
                Inst inst{};
                inst.data = InstGlobalRef{gid, name};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_call(TypeId ty, ValueId callee, std::vector<ValueId> args, FuncId direct_callee = kInvalidId) {
                ValueId r = make_value(ty, Effect::Call);
                Inst inst{};
                inst.data = InstCall{callee, std::move(args), direct_callee};
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

            void emit_actor_commit_marker() {
                Inst inst{};
                inst.data = InstActorCommit{};
                inst.eff = Effect::MayWriteMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            void emit_actor_recast_marker() {
                Inst inst{};
                inst.data = InstActorRecast{};
                inst.eff = Effect::MayReadMem;
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
                if (it->second.is_direct_address) {
                    if (types != nullptr && want_ty != parus::ty::kInvalidType) {
                        const auto& wt = types->get(want_ty);
                        if (wt.kind == parus::ty::Kind::kNamedUser ||
                            wt.kind == parus::ty::Kind::kArray ||
                            wt.kind == parus::ty::Kind::kOptional) {
                            return it->second.v;
                        }
                    }
                    return emit_load(want_ty, it->second.v);
                }
                return emit_load(want_ty, it->second.v);
            }

            // util: ensure a symbol has a slot (for write), possibly demote SSA to slot
            ValueId ensure_slot(parus::sir::SymbolId sym, TypeId slot_ty) {
                auto it = env.find(sym);
                if (it != env.end() && it->second.is_slot) return it->second.v;

                uint32_t cleanup_id = kInvalidId;
                if (it != env.end()) cleanup_id = it->second.cleanup_id;

                // create a new slot
                ValueId slot = emit_alloca(slot_ty);

                // if previously SSA value existed, initialize slot with it
                if (it != env.end() && !it->second.is_slot && it->second.v != kInvalidId) {
                    emit_store(slot, it->second.v);
                }

                bind(sym, Binding{true, false, slot, cleanup_id});
                return slot;
            }

            // util: boundary coercion (hybrid nullable policy)
            ValueId coerce_value_for_target(TypeId dst_ty, ValueId src) {
                if (src == kInvalidId) return src;
                if (dst_ty == kInvalidId || types == nullptr || out == nullptr) return src;
                if ((size_t)src >= out->values.size()) return src;

                const TypeId src_ty = out->values[src].ty;
                if (src_ty == dst_ty) return src;

                const auto& dt = types->get(dst_ty);
                if (dt.kind == parus::ty::Kind::kOptional) {
                    const TypeId elem_ty = dt.elem;
                    const TypeId null_ty = (TypeId)types->builtin(parus::ty::Builtin::kNull);

                    if (src_ty == null_ty) {
                        return emit_const_null(dst_ty);
                    }
                    if (elem_ty != kInvalidId && src_ty == elem_ty) {
                        // Optional some(T): represent as typed cast at OIR boundary.
                        return emit_cast(dst_ty, Effect::Pure, CastKind::As, dst_ty, src);
                    }
                }

                return src;
            }

            void report_lowering_error(std::string message) {
                if (build_errors == nullptr) return;
                build_errors->push_back(parus::sir::VerifyError{std::move(message)});
            }
        };

        static std::optional<BinOp> map_binop(parus::syntax::TokenKind k) {
            using TK = parus::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:              return std::optional<BinOp>{BinOp::Add};
                case TK::kMinus:             return std::optional<BinOp>{BinOp::Sub};
                case TK::kStar:              return std::optional<BinOp>{BinOp::Mul};
                case TK::kSlash:             return std::optional<BinOp>{BinOp::Div};
                case TK::kPercent:           return std::optional<BinOp>{BinOp::Rem};
                case TK::kLt:                return std::optional<BinOp>{BinOp::Lt};
                case TK::kLtEq:              return std::optional<BinOp>{BinOp::Le};
                case TK::kGt:                return std::optional<BinOp>{BinOp::Gt};
                case TK::kGtEq:              return std::optional<BinOp>{BinOp::Ge};
                case TK::kEqEq:              return std::optional<BinOp>{BinOp::Eq};
                case TK::kBangEq:            return std::optional<BinOp>{BinOp::Ne};
                case TK::kKwAnd:             return std::optional<BinOp>{BinOp::LogicalAnd};
                case TK::kKwOr:              return std::optional<BinOp>{BinOp::LogicalOr};
                case TK::kQuestionQuestion:  return std::optional<BinOp>{BinOp::NullCoalesce};
                default:                     return std::nullopt;
            }
        }

        static std::optional<UnOp> map_unary(parus::syntax::TokenKind k) {
            using TK = parus::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:  return std::optional<UnOp>{UnOp::Plus};
                case TK::kMinus: return std::optional<UnOp>{UnOp::Neg};
                case TK::kBang:
                case TK::kKwNot: return std::optional<UnOp>{UnOp::Not};
                case TK::kCaret: return std::optional<UnOp>{UnOp::BitNot};
                default:         return std::nullopt;
            }
        }

        std::optional<std::string> parse_float_literal_text_(std::string_view text) {
            std::string out;
            out.reserve(text.size());

            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }

            bool saw_digit = false;
            auto append_digits = [&](size_t& pos) {
                while (pos < text.size()) {
                    const char c = text[pos];
                    if (c >= '0' && c <= '9') {
                        out.push_back(c);
                        saw_digit = true;
                        ++pos;
                        continue;
                    }
                    if (c == '_') {
                        ++pos;
                        continue;
                    }
                    break;
                }
            };

            append_digits(i);

            bool has_dot = false;
            if (i < text.size() && text[i] == '.') {
                has_dot = true;
                out.push_back('.');
                ++i;
                append_digits(i);
            }

            bool has_exp = false;
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                has_exp = true;
                out.push_back(text[i++]);
                if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                    out.push_back(text[i++]);
                }
                size_t exp_digits_begin = out.size();
                while (i < text.size()) {
                    const char c = text[i];
                    if (c >= '0' && c <= '9') {
                        out.push_back(c);
                        ++i;
                        continue;
                    }
                    if (c == '_') {
                        ++i;
                        continue;
                    }
                    break;
                }
                if (out.size() == exp_digits_begin) return std::nullopt;
            }

            if (!saw_digit || (!has_dot && !has_exp)) return std::nullopt;

            const std::string_view suffix = text.substr(i);
            if (!(suffix.empty() || suffix == "f" || suffix == "f32" || suffix == "lf" || suffix == "f64" || suffix == "f128")) {
                return std::nullopt;
            }
            return out;
        }

        std::string parse_int_literal_text_(std::string_view text) {
            std::string out;
            out.reserve(text.size());

            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }

            bool saw_digit = false;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (c >= '0' && c <= '9') {
                    out.push_back(c);
                    saw_digit = true;
                    continue;
                }
                if (c == '_') continue;
                break;
            }
            if (!saw_digit) return "0";
            return out;
        }

        std::optional<uint32_t> parse_char_literal_code_(std::string_view text) {
            if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') return std::nullopt;
            std::string_view body = text.substr(1, text.size() - 2);
            if (body.empty()) return std::nullopt;
            if (body.size() == 1) return static_cast<uint32_t>(static_cast<unsigned char>(body[0]));
            if (body[0] != '\\') return std::nullopt;

            if (body.size() == 2) {
                switch (body[1]) {
                    case 'n': return static_cast<uint32_t>('\n');
                    case 'r': return static_cast<uint32_t>('\r');
                    case 't': return static_cast<uint32_t>('\t');
                    case '\\': return static_cast<uint32_t>('\\');
                    case '\'': return static_cast<uint32_t>('\'');
                    case '0': return static_cast<uint32_t>('\0');
                    default: return std::nullopt;
                }
            }
            return std::nullopt;
        }

        bool is_hex_digit_(char c) {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        }

        uint8_t hex_digit_value_(char c) {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
            return 0;
        }

        std::string decode_escaped_string_body_(std::string_view body) {
            std::string out;
            out.reserve(body.size());

            for (size_t i = 0; i < body.size(); ++i) {
                const char c = body[i];
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }

                if (i + 1 >= body.size()) {
                    out.push_back('\\');
                    break;
                }

                const char esc = body[++i];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case '0': out.push_back('\0'); break;
                    case 'x': {
                        if (i + 2 < body.size() &&
                            is_hex_digit_(body[i + 1]) &&
                            is_hex_digit_(body[i + 2])) {
                            const uint8_t hi = hex_digit_value_(body[i + 1]);
                            const uint8_t lo = hex_digit_value_(body[i + 2]);
                            out.push_back(static_cast<char>((hi << 4) | lo));
                            i += 2;
                            break;
                        }
                        out.push_back('x');
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            }
            return out;
        }

        bool starts_with_(std::string_view s, std::string_view pfx) {
            return s.size() >= pfx.size() && s.substr(0, pfx.size()) == pfx;
        }

        bool ends_with_(std::string_view s, std::string_view sfx) {
            return s.size() >= sfx.size() && s.substr(s.size() - sfx.size()) == sfx;
        }

        std::string parse_string_literal_bytes_(std::string_view text) {
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                const auto body = text.substr(1, text.size() - 2);
                return decode_escaped_string_body_(body);
            }

            if (starts_with_(text, "R\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                return std::string(text.substr(4, text.size() - 7));
            }

            if (starts_with_(text, "F\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                // v0: F-string interpolation is not yet lowered. Keep body as raw UTF-8 text.
                return std::string(text.substr(4, text.size() - 7));
            }

            return std::string(text);
        }

        std::string normalize_symbol_fragment_(std::string_view in) {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (std::isalnum(u) || c == '_') out.push_back(c);
                else out.push_back('_');
            }
            if (out.empty()) out = "_";
            return out;
        }

        uint64_t fnv1a64_(std::string_view s) {
            uint64_t h = 1469598103934665603ull;
            for (char c : s) {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        /// @brief 함수 이름 + 시그니처를 기반으로 OIR 내부 함수명을 생성한다.
        std::string mangle_func_name_(
            const parus::sir::Func& sf,
            const parus::ty::TypePool& types,
            std::string_view bundle_name
        ) {
            std::string sig = (sf.sig != parus::ty::kInvalidType)
                ? types.to_string(sf.sig)
                : std::string("def(?)");

            const std::string qname(sf.name);
            std::string path = "_";
            std::string base = qname;
            if (const size_t pos = qname.rfind("::"); pos != std::string::npos) {
                path = qname.substr(0, pos);
                base = qname.substr(pos + 2);
                size_t p = 0;
                while ((p = path.find("::", p)) != std::string::npos) {
                    path.replace(p, 2, "__");
                    p += 2;
                }
            }

            std::string mode = "none";
            switch (sf.fn_mode) {
                case parus::sir::FnMode::kPub: mode = "pub"; break;
                case parus::sir::FnMode::kSub: mode = "sub"; break;
                case parus::sir::FnMode::kNone: default: mode = "none"; break;
            }

            const std::string bundle = bundle_name.empty() ? std::string("main") : std::string(bundle_name);
            const std::string canonical =
                "bundle=" + bundle + "|path=" + path +
                "|name=" + base +
                "|mode=" + mode +
                "|recv=none|sig=" + sig;
            std::ostringstream hs;
            hs << std::hex << fnv1a64_(canonical);

            return "p$" + normalize_symbol_fragment_(bundle) + "$" +
                normalize_symbol_fragment_(path) + "$" +
                normalize_symbol_fragment_(base) + "$M" +
                normalize_symbol_fragment_(mode) + "$Rnone$S" +
                normalize_symbol_fragment_(sig) + "$H" + hs.str();
        }

        std::string make_module_init_symbol_name_(
            std::string_view bundle_name,
            std::string_view current_source_norm
        ) {
            const std::string bundle = bundle_name.empty() ? std::string("main") : std::string(bundle_name);
            const std::string canonical =
                "bundle=" + bundle + "|source=" + std::string(current_source_norm);
            std::ostringstream hs;
            hs << std::hex << fnv1a64_(canonical);
            return "__parus_module_init__" + hs.str();
        }

        /// @brief SIR 함수 ABI를 OIR 함수 ABI로 변환한다.
        FunctionAbi map_func_abi_(parus::sir::FuncAbi abi) {
            switch (abi) {
                case parus::sir::FuncAbi::kC: return FunctionAbi::C;
                case parus::sir::FuncAbi::kParus:
                default: return FunctionAbi::Parus;
            }
        }

        FieldLayout map_field_layout_(parus::sir::FieldLayout layout) {
            switch (layout) {
                case parus::sir::FieldLayout::kC:
                    return FieldLayout::C;
                case parus::sir::FieldLayout::kNone:
                default:
                    return FieldLayout::None;
            }
        }

        uint32_t align_to_(uint32_t value, uint32_t align) {
            if (align == 0 || align == 1) return value;
            const uint32_t rem = value % align;
            return (rem == 0) ? value : (value + (align - rem));
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
            def->blocks.push_back(then_bb);
            cur_bb = then_bb;
            push_scope();
            ValueId then_val = lower_value(then_sir);
            pop_scope();
            if (!has_term()) br(join_bb, {then_val});

            // ELSE
            def->blocks.push_back(else_bb);
            cur_bb = else_bb;
            push_scope();
            ValueId else_val = lower_value(else_sir);
            pop_scope();
            if (!has_term()) br(join_bb, {else_val});

            // JOIN
            def->blocks.push_back(join_bb);
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

            case parus::sir::ValueKind::kFloatLit: {
                const auto lit = parse_float_literal_text_(v.text);
                if (!lit.has_value()) {
                    report_lowering_error(
                        std::string("unsupported or invalid float literal during OIR lowering: '") +
                        std::string(v.text) + "'");
                    return emit_const_null(v.type);
                }
                return emit_const_float(v.type, *lit);
            }

            case parus::sir::ValueKind::kCharLit: {
                const auto code = parse_char_literal_code_(v.text);
                if (!code.has_value()) {
                    report_lowering_error(
                        std::string("unsupported or invalid char literal during OIR lowering: '") +
                        std::string(v.text) + "'");
                    return emit_const_null(v.type);
                }
                return emit_const_char(v.type, *code);
            }

            case parus::sir::ValueKind::kBoolLit:
                return emit_const_bool(v.type, v.text == "true");

            case parus::sir::ValueKind::kStringLit:
                return emit_const_text(v.type, parse_string_literal_bytes_(v.text));

            case parus::sir::ValueKind::kNullLit:
                return emit_const_null(v.type);

            case parus::sir::ValueKind::kArrayLit: {
                // v0: 배열 리터럴은 함수 로컬 slot에 materialize한 뒤
                // 해당 slot 포인터 값을 결과로 전달한다.
                // (LLVM lowering 단계에서 index/store/load가 실제 주소 계산으로 변환됨)
                ValueId arr_slot = emit_alloca(v.type);

                TypeId elem_ty = v.type;
                TypeId idx_ty = v.type;
                if (types != nullptr && v.type != parus::ty::kInvalidType) {
                    const auto& t = types->get(v.type);
                    if (t.kind == parus::ty::Kind::kArray && t.elem != parus::ty::kInvalidType) {
                        elem_ty = (TypeId)t.elem;
                    }
                    idx_ty = (TypeId)types->builtin(parus::ty::Builtin::kI64);
                }

                const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                if (arg_end <= (uint64_t)sir->args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = sir->args[v.arg_begin + i];
                        if (a.is_hole || a.value == parus::sir::k_invalid_value) continue;

                        ValueId elem_val = lower_value(a.value);
                        ValueId idx_val = emit_const_int(idx_ty, std::to_string(i));
                        ValueId elem_place = emit_index(elem_ty, arr_slot, idx_val);
                        emit_store(elem_place, elem_val);
                    }
                }
                return arr_slot;
            }

            case parus::sir::ValueKind::kFieldInit: {
                // v0: struct 리터럴은 임시 슬롯에 멤버를 순서대로 store하여 물질화한다.
                // 반환값은 aggregate slot 포인터 표현을 따른다.
                ValueId obj_slot = emit_alloca(v.type);

                const FieldLayoutDecl* layout = nullptr;
                for (const auto& f : out->fields) {
                    if (f.self_type == v.type) {
                        layout = &f;
                        break;
                    }
                }

                auto lookup_member_ty = [&](std::string_view member) -> TypeId {
                    if (layout == nullptr) return kInvalidId;
                    for (const auto& m : layout->members) {
                        if (m.name == member) return m.type;
                    }
                    return kInvalidId;
                };

                const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                if (arg_end <= (uint64_t)sir->args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = sir->args[v.arg_begin + i];
                        if (a.value == parus::sir::k_invalid_value) continue;

                        ValueId rhs = lower_value(a.value);
                        TypeId member_ty = lookup_member_ty(a.label);
                        if (member_ty == kInvalidId &&
                            rhs != kInvalidId &&
                            (size_t)rhs < out->values.size()) {
                            member_ty = out->values[rhs].ty;
                        }
                        if (member_ty == kInvalidId) member_ty = v.type;

                        rhs = coerce_value_for_target(member_ty, rhs);
                        ValueId place = emit_field(member_ty, obj_slot, std::string(a.label));
                        emit_store(place, rhs);
                    }
                }

                return obj_slot;
            }

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
                    if (v.kind == parus::sir::ValueKind::kEscape &&
                        v.origin_sym != parus::sir::k_invalid_symbol) {
                        mark_symbol_moved(v.origin_sym, /*moved=*/true);
                    }
                    return lowered;
                }

            case parus::sir::ValueKind::kUnary: {
                ValueId src = lower_value(v.a);
                auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                auto op = map_unary(tk);
                if (!op.has_value()) {
                    report_lowering_error(
                        std::string("unsupported unary operator in OIR lowering: ") +
                        std::string(parus::syntax::token_kind_name(tk)));
                    return emit_const_null(v.type);
                }
                return emit_unary(v.type, Effect::Pure, *op, src);
            }

            case parus::sir::ValueKind::kBinary: {
                ValueId lhs = lower_value(v.a);
                ValueId rhs = lower_value(v.b);

                auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                auto op = map_binop(tk);
                if (!op.has_value()) {
                    report_lowering_error(
                        std::string("unsupported binary operator in OIR lowering: ") +
                        std::string(parus::syntax::token_kind_name(tk)));
                    return emit_const_null(v.type);
                }

                // v0: 대부분 pure로 둔다. (??/비교도 pure)
                return emit_binop(v.type, Effect::Pure, *op, lhs, rhs);
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
                std::vector<ValueId> args;
                args.reserve(v.arg_count);

                uint32_t i = 0;
                while (i < v.arg_count) {
                    const uint32_t aid = v.arg_begin + i;
                    if ((size_t)aid >= sir->args.size()) break;
                    const auto& a = sir->args[aid];

                    if (!a.is_hole && a.value != parus::sir::k_invalid_value) {
                        args.push_back(lower_value(a.value));
                    }
                    ++i;
                }

                const bool ctor_call =
                    v.call_is_ctor &&
                    v.ctor_owner_type != parus::sir::k_invalid_type;
                ValueId ctor_tmp_slot = kInvalidId;
                if (ctor_call) {
                    const TypeId owner_ty = (TypeId)v.ctor_owner_type;
                    ctor_tmp_slot = emit_alloca(owner_ty);
                    // lifecycle hidden self is appended as the last runtime argument.
                    args.push_back(ctor_tmp_slot);
                }

                ValueId callee = kInvalidId;
                FuncId direct_callee = kInvalidId;
                if (v.callee_decl_stmt != 0xFFFF'FFFFu && fn_decl_to_func != nullptr) {
                    auto dit = fn_decl_to_func->find(v.callee_decl_stmt);
                    if (dit != fn_decl_to_func->end() &&
                        (size_t)dit->second < out->funcs.size()) {
                        direct_callee = dit->second;
                    }
                }

                // 오버로드 decl-id가 이미 선택된 경우(direct_callee 유효)는
                // 해당 결정을 유지한다. 심볼 기반 재선택은 decl-id 정보가
                // 없는 일반 call 경로에서만 수행한다.
                if (callee == kInvalidId &&
                    direct_callee == kInvalidId &&
                    v.callee_sym != parus::sir::k_invalid_symbol &&
                    fn_symbol_to_funcs != nullptr) {
                    auto fit = fn_symbol_to_funcs->find(v.callee_sym);
                    if (fit != fn_symbol_to_funcs->end()) {
                        FuncId best = kInvalidId;
                        uint32_t best_exact = 0;
                        for (const FuncId fid : fit->second) {
                            if ((size_t)fid >= out->funcs.size()) continue;
                            const auto& f = out->funcs[fid];
                            if (f.entry == kInvalidId || (size_t)f.entry >= out->blocks.size()) continue;
                            const auto& entry = out->blocks[f.entry];
                            if (entry.params.size() != args.size()) continue;

                            uint32_t exact = 0;
                            bool ok = true;
                            for (size_t ai = 0; ai < args.size(); ++ai) {
                                const auto p = entry.params[ai];
                                if ((size_t)p >= out->values.size() || (size_t)args[ai] >= out->values.size()) {
                                    ok = false;
                                    break;
                                }
                                if (out->values[p].ty == out->values[args[ai]].ty) {
                                    exact++;
                                }
                            }
                            if (!ok) continue;
                            if (best == kInvalidId || exact > best_exact) {
                                best = fid;
                                best_exact = exact;
                            }
                        }
                        if (best != kInvalidId) {
                            direct_callee = best;
                        }
                    }
                }

                if (callee == kInvalidId &&
                    direct_callee == kInvalidId &&
                    v.callee_sym != parus::sir::k_invalid_symbol &&
                    fn_symbol_to_func != nullptr) {
                    auto fit = fn_symbol_to_func->find(v.callee_sym);
                    if (fit != fn_symbol_to_func->end() &&
                        (size_t)fit->second < out->funcs.size()) {
                        direct_callee = fit->second;
                    }
                }

                if (callee == kInvalidId && direct_callee == kInvalidId) {
                    callee = lower_value(v.a);
                }

                if (direct_callee != kInvalidId && (size_t)direct_callee < out->funcs.size()) {
                    const auto& f = out->funcs[direct_callee];
                    if (f.entry != kInvalidId && (size_t)f.entry < out->blocks.size()) {
                        const auto& entry = out->blocks[f.entry];
                        const size_t n = std::min(entry.params.size(), args.size());
                        for (size_t ai = 0; ai < n; ++ai) {
                            const ValueId p = entry.params[ai];
                            if ((size_t)p >= out->values.size()) continue;
                            args[ai] = coerce_value_for_target(out->values[p].ty, args[ai]);
                        }
                    }
                }

                if (ctor_call) {
                    const TypeId unit_ty =
                        (types != nullptr)
                            ? (TypeId)types->builtin(parus::ty::Builtin::kUnit)
                            : kInvalidId;
                    (void)emit_call(unit_ty, callee, std::move(args), direct_callee);
                    if (ctor_tmp_slot != kInvalidId) return ctor_tmp_slot;
                    return emit_const_null(v.type);
                }

                return emit_call(v.type, callee, std::move(args), direct_callee);
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
                const bool is_null_coalesce_assign =
                    (v.op == static_cast<uint32_t>(parus::syntax::TokenKind::kQuestionQuestionAssign));

                if (place.kind == parus::sir::ValueKind::kLocal) {
                    // slot 타입은 place_elem_type 우선 (없으면 기존 place.type)
                    TypeId slot_elem_ty =
                        (place.place_elem_type != parus::sir::k_invalid_type)
                            ? (TypeId)place.place_elem_type
                            : (TypeId)place.type;

                    if (is_null_coalesce_assign && types != nullptr && slot_elem_ty != kInvalidId) {
                        const auto& st = types->get(slot_elem_ty);
                        if (st.kind == parus::ty::Kind::kOptional && st.elem != kInvalidId) {
                            const ValueId lhs_cur = read_local(place.sym, slot_elem_ty);
                            const ValueId rhs_elem = coerce_value_for_target(st.elem, rhs);
                            const ValueId merged_elem = emit_binop(
                                st.elem, Effect::Pure, BinOp::NullCoalesce, lhs_cur, rhs_elem);
                            rhs = coerce_value_for_target(slot_elem_ty, merged_elem);
                        }
                    }

                    ValueId slot = ensure_slot(place.sym, slot_elem_ty);
                    drop_symbol_before_overwrite(place.sym);
                    rhs = coerce_value_for_target(slot_elem_ty, rhs);
                    emit_store(slot, rhs);
                    mark_symbol_moved(place.sym, /*moved=*/false);
                    return rhs; // assign expr result
                }

                // local 외 place(index/field 등)은 generic store로 남긴다.
                // (백엔드에서 place 해석을 확장할 수 있도록 형태를 유지)
                ValueId place_v = lower_value(v.a);
                if (place_v != kInvalidId && (size_t)place_v < out->values.size()) {
                    if (is_null_coalesce_assign && types != nullptr) {
                        const TypeId target_ty = out->values[place_v].ty;
                        if (target_ty != kInvalidId) {
                            const auto& tt = types->get(target_ty);
                            if (tt.kind == parus::ty::Kind::kOptional && tt.elem != kInvalidId) {
                                const ValueId lhs_cur = emit_load(target_ty, place_v);
                                const ValueId rhs_elem = coerce_value_for_target(tt.elem, rhs);
                                const ValueId merged_elem = emit_binop(
                                    tt.elem, Effect::Pure, BinOp::NullCoalesce, lhs_cur, rhs_elem);
                                rhs = coerce_value_for_target(target_ty, merged_elem);
                            }
                        }
                    }
                    rhs = coerce_value_for_target(out->values[place_v].ty, rhs);
                }
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

                def->blocks.push_back(body_bb);
                cur_bb = body_bb;
                const size_t loop_scope_base = env_stack.size();
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = body_bb,
                    .expects_break_value = has_value,
                    .break_ty = (TypeId)v.type,
                    .scope_depth_base = loop_scope_base
                });
                push_scope();
                lower_block((parus::sir::BlockId)v.b);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(body_bb, {});

                def->blocks.push_back(exit_bb);
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
                init = coerce_value_for_target(declared, init);

                const bool needs_cleanup = type_needs_drop_(declared);

                if (needs_cleanup) {
                    ValueId slot = emit_alloca(declared);
                    emit_store(slot, init);
                    const uint32_t cleanup_id = register_cleanup(s.sym, slot, declared);
                    bind(s.sym, Binding{true, false, slot, cleanup_id});
                    return;
                }

                // if set or mut => slot
                if (s.is_set || s.is_mut) {
                    ValueId slot = emit_alloca(declared);
                    emit_store(slot, init);
                    bind(s.sym, Binding{true, false, slot, kInvalidId});
                } else {
                    // immutable let => SSA binding
                    bind(s.sym, Binding{false, false, init, kInvalidId});
                }
                return;
            }

            case parus::sir::StmtKind::kExprStmt:
                if (s.expr != parus::sir::k_invalid_value) (void)lower_value(s.expr);
                return;

            case parus::sir::StmtKind::kCommitStmt:
                emit_actor_commit_marker();
                return;

            case parus::sir::StmtKind::kRecastStmt:
                emit_actor_recast_marker();
                return;

            case parus::sir::StmtKind::kReturn:
                if (s.expr != parus::sir::k_invalid_value) {
                    ValueId rv = lower_value(s.expr);
                    if (def != nullptr && def->ret_ty != kInvalidId) {
                        rv = coerce_value_for_target(def->ret_ty, rv);
                    }
                    emit_cleanups_to_depth(/*keep_depth=*/0);
                    ret(rv);
                } else {
                    emit_cleanups_to_depth(/*keep_depth=*/0);
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
                def->blocks.push_back(cond_bb);
                cur_bb = cond_bb;
                ValueId cond = lower_value(s.expr);
                condbr(cond, body_bb, {}, exit_bb, {});

                // body
                def->blocks.push_back(body_bb);
                cur_bb = body_bb;
                const size_t loop_scope_base = env_stack.size();
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = cond_bb,
                    .expects_break_value = false,
                    .break_ty = kInvalidId,
                    .scope_depth_base = loop_scope_base
                });
                push_scope();
                lower_block(s.a);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(cond_bb, {});

                // exit
                def->blocks.push_back(exit_bb);
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
                def->blocks.push_back(body_bb);
                cur_bb = body_bb;
                const size_t loop_scope_base = env_stack.size();
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = cond_bb,
                    .expects_break_value = false,
                    .break_ty = kInvalidId,
                    .scope_depth_base = loop_scope_base
                });
                push_scope();
                lower_block(s.a);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(cond_bb, {});

                // cond
                def->blocks.push_back(cond_bb);
                cur_bb = cond_bb;
                ValueId cond = lower_value(s.expr);
                condbr(cond, body_bb, {}, exit_bb, {});

                // exit
                def->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kManualStmt: {
                // manual 블록은 현재 단계에서 별도 runtime 동작 없이 body만 순차 lowering한다.
                push_scope();
                lower_block(s.a);
                pop_scope();
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
                def->blocks.push_back(then_bb);
                cur_bb = then_bb;
                push_scope();
                lower_block(s.a);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // else
                def->blocks.push_back(else_bb);
                cur_bb = else_bb;
                push_scope();
                if (s.b != parus::sir::k_invalid_block) lower_block(s.b);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // join
                def->blocks.push_back(join_bb);
                cur_bb = join_bb;
                return;
            }

            case parus::sir::StmtKind::kSwitch: {
                const ValueId scrut = lower_value(s.expr);
                const TypeId scrut_ty =
                    (scrut != kInvalidId && (size_t)scrut < out->values.size())
                        ? out->values[scrut].ty
                        : kInvalidId;
                const TypeId bool_ty =
                    (types != nullptr)
                        ? (TypeId)types->builtin(parus::ty::Builtin::kBool)
                        : kInvalidId;

                const auto emit_case_match_cond = [&](const parus::sir::SwitchCase& c) -> ValueId {
                    switch (c.pat_kind) {
                        case parus::sir::SwitchCasePatKind::kInt: {
                            const ValueId pat = emit_const_int(scrut_ty, parse_int_literal_text_(c.pat_text));
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kBool: {
                            const bool bv = (c.pat_text == "true");
                            const ValueId pat = emit_const_bool(scrut_ty, bv);
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kNull: {
                            const ValueId pat = emit_const_null(scrut_ty);
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kChar: {
                            const auto code = parse_char_literal_code_(c.pat_text);
                            const ValueId pat = emit_const_int(
                                scrut_ty,
                                code.has_value() ? std::to_string(*code) : std::string("0")
                            );
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kString:
                        case parus::sir::SwitchCasePatKind::kIdent:
                        case parus::sir::SwitchCasePatKind::kError:
                        default:
                            return emit_const_bool(bool_ty, false);
                    }
                };

                const BlockId exit_bb = new_block();
                std::optional<parus::sir::SwitchCase> default_case;

                if ((uint64_t)s.case_begin + (uint64_t)s.case_count <= (uint64_t)sir->switch_cases.size()) {
                    for (uint32_t i = 0; i < s.case_count; ++i) {
                        const auto& c = sir->switch_cases[s.case_begin + i];
                        if (c.is_default) {
                            default_case = c;
                            continue;
                        }

                        const BlockId match_bb = new_block();
                        const BlockId next_bb = new_block();

                        const ValueId cond = emit_case_match_cond(c);
                        condbr(cond, match_bb, {}, next_bb, {});

                        def->blocks.push_back(match_bb);
                        cur_bb = match_bb;
                        push_scope();
                        lower_block(c.body);
                        pop_scope();
                        if (!has_term()) br(exit_bb, {});

                        def->blocks.push_back(next_bb);
                        cur_bb = next_bb;
                    }
                }

                if (default_case.has_value()) {
                    const BlockId def_bb = new_block();
                    if (!has_term()) br(def_bb, {});

                    def->blocks.push_back(def_bb);
                    cur_bb = def_bb;
                    push_scope();
                    lower_block(default_case->body);
                    pop_scope();
                    if (!has_term()) br(exit_bb, {});
                } else {
                    if (!has_term()) br(exit_bb, {});
                }

                def->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kBreak: {
                if (loop_stack.empty()) return;
                const auto& lc = loop_stack.back();

                if (lc.expects_break_value) {
                    ValueId bv = (s.expr != parus::sir::k_invalid_value)
                               ? lower_value(s.expr)
                               : emit_const_null(lc.break_ty);
                    emit_cleanups_to_depth(lc.scope_depth_base);
                    br(lc.break_bb, {bv});
                } else {
                    emit_cleanups_to_depth(lc.scope_depth_base);
                    br(lc.break_bb, {});
                }
                return;
            }

            case parus::sir::StmtKind::kContinue: {
                if (loop_stack.empty()) return;
                const auto& lc = loop_stack.back();
                emit_cleanups_to_depth(lc.scope_depth_base);
                br(lc.continue_bb, {});
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
            }
            return EscapeBoundaryKind::None;
        }

    } // namespace

    // ------------------------------------------------------------
    // Builder::build
    // ------------------------------------------------------------
    BuildResult Builder::build() {
        BuildResult out{};
        out.mod.bundle_enabled = sir_.bundle_enabled;
        out.mod.bundle_name = sir_.bundle_name;
        out.mod.current_source_norm = sir_.current_source_norm;
        out.mod.bundle_sources_norm = sir_.bundle_sources_norm;

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

        std::unordered_map<parus::ty::TypeId, std::pair<uint32_t, uint32_t>> named_layout_by_type;
        auto type_size_align = [&](const auto& self, parus::ty::TypeId tid) -> std::pair<uint32_t, uint32_t> {
            using TK = parus::ty::Kind;
            using TB = parus::ty::Builtin;

            if (tid == parus::ty::kInvalidType) return {8u, 8u};
            const auto& t = ty_.get(tid);

            switch (t.kind) {
                case TK::kError:
                    return {8u, 8u};

                case TK::kBuiltin:
                    switch (t.builtin) {
                        case TB::kBool:
                        case TB::kI8:
                        case TB::kU8:
                            return {1u, 1u};
                        case TB::kI16:
                        case TB::kU16:
                            return {2u, 2u};
                        case TB::kI32:
                        case TB::kU32:
                        case TB::kF32:
                        case TB::kChar:
                            return {4u, 4u};
                        case TB::kText:
                            return {16u, 8u};
                        case TB::kI128:
                        case TB::kU128:
                        case TB::kF128:
                            return {16u, 16u};
                        case TB::kUnit:
                            return {1u, 1u};
                        case TB::kNever:
                        case TB::kI64:
                        case TB::kU64:
                        case TB::kF64:
                        case TB::kISize:
                        case TB::kUSize:
                        case TB::kNull:
                        case TB::kInferInteger:
                            return {8u, 8u};
                    }
                    return {8u, 8u};

                case TK::kPtr:
                case TK::kBorrow:
                case TK::kEscape:
                case TK::kFn:
                    return {8u, 8u};

                case TK::kOptional: {
                    const auto [elem_size, elem_align] = self(self, t.elem);
                    const uint32_t a = std::max<uint32_t>(1u, elem_align);
                    const uint32_t body = align_to_(1u, a) + std::max<uint32_t>(1u, elem_size);
                    return {body, a};
                }

                case TK::kArray: {
                    const auto [elem_size, elem_align] = self(self, t.elem);
                    const uint32_t e = std::max<uint32_t>(1u, elem_size);
                    const uint32_t a = std::max<uint32_t>(1u, elem_align);
                    if (!t.array_has_size) return {16u, 8u};
                    return {e * std::max<uint32_t>(1u, t.array_size), a};
                }

                case TK::kNamedUser: {
                    auto it = named_layout_by_type.find(tid);
                    if (it != named_layout_by_type.end()) return it->second;
                    return {8u, 8u};
                }
            }

            return {8u, 8u};
        };

        // 필드 레이아웃 메타를 OIR 모듈로 복사한다.
        for (const auto& sf : sir_.fields) {
            FieldLayoutDecl of{};
            of.name = std::string(sf.name);
            of.self_type = sf.self_type;
            of.layout = map_field_layout_(sf.layout);
            of.align = sf.align;

            const uint64_t begin = sf.member_begin;
            const uint64_t end = begin + sf.member_count;
            uint32_t offset = 0;
            uint32_t struct_align = std::max<uint32_t>(1u, of.align);

            if (begin <= sir_.field_members.size() && end <= sir_.field_members.size()) {
                for (uint32_t i = sf.member_begin; i < sf.member_begin + sf.member_count; ++i) {
                    const auto& sm = sir_.field_members[i];
                    const auto [member_size_raw, member_align_raw] = type_size_align(type_size_align, sm.type);
                    const uint32_t member_size = std::max<uint32_t>(1u, member_size_raw);
                    const uint32_t member_align = std::max<uint32_t>(1u, member_align_raw);

                    if (of.layout == FieldLayout::C) {
                        offset = align_to_(offset, member_align);
                    }

                    FieldMemberLayout om{};
                    om.name = std::string(sm.name);
                    om.type = sm.type;
                    om.offset = offset;
                    of.members.push_back(std::move(om));

                    offset += member_size;
                    struct_align = std::max(struct_align, member_align);
                }
            }

            if (of.layout == FieldLayout::C) {
                of.size = std::max<uint32_t>(1u, align_to_(offset, struct_align));
            } else {
                of.size = std::max<uint32_t>(1u, offset);
            }
            if (of.align == 0) of.align = struct_align;

            const uint32_t idx = out.mod.add_field(of);
            (void)idx;
            if (of.self_type != parus::ty::kInvalidType) {
                named_layout_by_type[of.self_type] = {std::max<uint32_t>(1u, of.size), std::max<uint32_t>(1u, of.align)};
            }
        }

        std::unordered_map<parus::sir::SymbolId, uint32_t> global_symbol_to_global;
        struct GlobalInitItem {
            parus::sir::SymbolId sym = parus::sir::k_invalid_symbol;
            uint32_t gid = kInvalidId;
            parus::sir::ValueId init = parus::sir::k_invalid_value;
            TypeId type = kInvalidId;
        };
        std::vector<GlobalInitItem> global_init_items{};
        for (const auto& sg : sir_.globals) {
            GlobalDecl g{};
            if (sg.abi == parus::sir::FuncAbi::kC || sg.is_export) {
                g.name = std::string(sg.name);
            } else {
                g.name = std::string(sg.name) + "$g";
            }
            g.type = sg.declared_type;
            g.abi = map_func_abi_(sg.abi);
            g.is_extern = sg.is_extern;
            // runtime init path(module/bundle init) uses store; keep writable in IR.
            g.is_mut = sg.is_mut || (!sg.is_extern && sg.init != parus::sir::k_invalid_value);
            g.is_export = sg.is_export;

            const uint32_t gid = out.mod.add_global(g);
            if (sg.sym != parus::sir::k_invalid_symbol) {
                global_symbol_to_global[sg.sym] = gid;
            }
            if (!sg.is_extern && sg.init != parus::sir::k_invalid_value) {
                global_init_items.push_back(GlobalInitItem{
                    .sym = sg.sym,
                    .gid = gid,
                    .init = sg.init,
                    .type = g.type
                });
            }
        }

        std::vector<std::pair<parus::sir::SymbolId, uint32_t>> sorted_globals{};
        sorted_globals.reserve(global_symbol_to_global.size());
        for (const auto& kv : global_symbol_to_global) {
            sorted_globals.push_back(kv);
        }
        std::sort(sorted_globals.begin(), sorted_globals.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                  });

        // Build all functions in SIR module.
        // Strategy:
        // 1) 함수 쉘/엔트리 블록을 먼저 전부 생성해 심볼->FuncId를 고정한다.
        // 2) 두 번째 패스에서 바디를 lowering한다.
        // 이렇게 해야 전방 함수 호출/오버로드 호출에서도 direct callee를 안정적으로 참조할 수 있다.
        std::unordered_map<parus::sir::ValueId, ValueId> escape_value_map;
        std::vector<FuncId> sir_to_oir_func(sir_.funcs.size(), kInvalidId);
        std::vector<BlockId> sir_to_entry(sir_.funcs.size(), kInvalidId);
        std::unordered_map<parus::sir::SymbolId, FuncId> fn_symbol_to_func;
        std::unordered_map<parus::sir::SymbolId, std::vector<FuncId>> fn_symbol_to_funcs;
        std::unordered_map<uint32_t, FuncId> fn_decl_to_func;

        for (size_t i = 0; i < sir_.funcs.size(); ++i) {
            const auto& sf = sir_.funcs[i];

            Function f{};
            // C ABI 함수는 심볼을 비맹글 기반으로 유지한다.
            f.name = (sf.abi == parus::sir::FuncAbi::kC)
                ? std::string(sf.name)
                : mangle_func_name_(sf, ty_, sir_.bundle_name);
            f.source_name = sf.name;
            f.abi = map_func_abi_(sf.abi);
            f.is_extern = sf.is_extern;
            f.is_pure = sf.is_pure;
            f.is_comptime = sf.is_comptime;
            f.ret_ty = (TypeId)sf.ret;

            const BlockId entry = out.mod.add_block(Block{});
            f.entry = entry;
            f.blocks.push_back(entry);

            const FuncId fid = out.mod.add_func(f);
            sir_to_oir_func[i] = fid;
            sir_to_entry[i] = entry;

            if (sf.sym != parus::sir::k_invalid_symbol) {
                fn_symbol_to_func[sf.sym] = fid;
                fn_symbol_to_funcs[sf.sym].push_back(fid);
            }
            if (sf.origin_stmt != 0xFFFF'FFFFu) {
                fn_decl_to_func[sf.origin_stmt] = fid;
            }
        }

        std::unordered_map<TypeId, FuncId> class_deinit_map;
        auto has_suffix_ = [](std::string_view s, std::string_view suffix) -> bool {
            return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
        };
        for (size_t i = 0; i < sir_.funcs.size(); ++i) {
            if (i >= sir_to_oir_func.size()) continue;
            const auto& sf = sir_.funcs[i];
            if (!has_suffix_(sf.name, "::deinit")) continue;
            const FuncId fid = sir_to_oir_func[i];
            if (fid == kInvalidId) continue;

            const uint64_t pb = sf.param_begin;
            const uint64_t pe = pb + sf.param_count;
            if (pb > sir_.params.size() || pe > sir_.params.size()) continue;

            for (uint32_t pi = 0; pi < sf.param_count; ++pi) {
                const auto& sp = sir_.params[sf.param_begin + pi];
                const auto& pt = ty_.get(sp.type);
                if (pt.kind != parus::ty::Kind::kBorrow) continue;
                if (pt.elem == parus::ty::kInvalidType) continue;
                const auto& et = ty_.get(pt.elem);
                if (et.kind != parus::ty::Kind::kNamedUser) continue;
                class_deinit_map[(TypeId)pt.elem] = fid;
                break;
            }
        }

        for (size_t i = 0; i < sir_.funcs.size(); ++i) {
            const auto& sf = sir_.funcs[i];
            const FuncId fid = sir_to_oir_func[i];
            const BlockId entry = sir_to_entry[i];
            if (fid == kInvalidId || entry == kInvalidId || (size_t)fid >= out.mod.funcs.size()) {
                continue;
            }

            FuncBuild fb{};
            fb.out = &out.mod;
            fb.sir = &sir_;
            fb.types = &ty_;
            fb.escape_value_map = &escape_value_map;
            fb.fn_symbol_to_func = &fn_symbol_to_func;
            fb.fn_symbol_to_funcs = &fn_symbol_to_funcs;
            fb.fn_decl_to_func = &fn_decl_to_func;
            fb.global_symbol_to_global = &global_symbol_to_global;
            fb.class_deinit_map = &class_deinit_map;
            fb.build_errors = &out.gate_errors;
            fb.def = &out.mod.funcs[fid];
            fb.cur_bb = entry;

            for (const auto& kv : sorted_globals) {
                const auto gid = kv.second;
                if ((size_t)gid >= out.mod.globals.size()) continue;
                const auto& g = out.mod.globals[gid];
                ValueId gref = fb.emit_global_ref(gid, g.name);
                fb.bind(kv.first, FuncBuild::Binding{true, true, gref, kInvalidId});
            }

            fb.push_scope();
            // 함수 파라미터를 entry block parameter로 시드하고 심볼 바인딩을 연결한다.
            const uint64_t pend = (uint64_t)sf.param_begin + (uint64_t)sf.param_count;
            if (pend <= (uint64_t)sir_.params.size()) {
                for (uint32_t pidx = 0; pidx < sf.param_count; ++pidx) {
                    const auto& sp = sir_.params[sf.param_begin + pidx];
                    ValueId pv = fb.add_block_param(entry, (TypeId)sp.type);
                    if (sp.sym == parus::sir::k_invalid_symbol) continue;

                    const bool needs_cleanup = fb.type_needs_drop_((TypeId)sp.type);
                    if (needs_cleanup) {
                        ValueId slot = fb.emit_alloca((TypeId)sp.type);
                        fb.emit_store(slot, pv);
                        const uint32_t cleanup_id = fb.register_cleanup(sp.sym, slot, (TypeId)sp.type);
                        fb.bind(sp.sym, FuncBuild::Binding{true, false, slot, cleanup_id});
                    } else if (sp.is_mut) {
                        ValueId slot = fb.emit_alloca((TypeId)sp.type);
                        fb.emit_store(slot, pv);
                        fb.bind(sp.sym, FuncBuild::Binding{true, false, slot, kInvalidId});
                    } else {
                        fb.bind(sp.sym, FuncBuild::Binding{false, false, pv, kInvalidId});
                    }
                }
            }

            fb.lower_block(sf.entry);
            fb.pop_scope();

            if (!out.mod.blocks[fb.cur_bb].has_term) {
                ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                fb.ret(rv);
            }
        }

        // Build synthesized module init function:
        // - non-bundle: only when there is at least one runtime global initializer
        // - bundle: always emit (leader can call every module init deterministically)
        const bool need_module_init = sir_.bundle_enabled || !global_init_items.empty();
        if (need_module_init) {
            const TypeId unit_ty = (TypeId)ty_.builtin(parus::ty::Builtin::kUnit);
            Function init_fn{};
            init_fn.name = make_module_init_symbol_name_(sir_.bundle_name, sir_.current_source_norm);
            init_fn.source_name = "__parus_module_init";
            init_fn.abi = FunctionAbi::Parus;
            init_fn.is_extern = false;
            init_fn.is_pure = false;
            init_fn.is_comptime = false;
            init_fn.ret_ty = unit_ty;
            const BlockId init_entry = out.mod.add_block(Block{});
            init_fn.entry = init_entry;
            init_fn.blocks.push_back(init_entry);
            const FuncId init_fid = out.mod.add_func(init_fn);
            out.mod.module_init_symbol = out.mod.funcs[init_fid].name;

            std::vector<GlobalInitItem> sorted_init_items = global_init_items;
            std::sort(sorted_init_items.begin(), sorted_init_items.end(),
                      [](const GlobalInitItem& a, const GlobalInitItem& b) {
                          if (a.gid != b.gid) return a.gid < b.gid;
                          return a.sym < b.sym;
                      });

            FuncBuild fb{};
            fb.out = &out.mod;
            fb.sir = &sir_;
            fb.types = &ty_;
            fb.escape_value_map = &escape_value_map;
            fb.fn_symbol_to_func = &fn_symbol_to_func;
            fb.fn_symbol_to_funcs = &fn_symbol_to_funcs;
            fb.fn_decl_to_func = &fn_decl_to_func;
            fb.global_symbol_to_global = &global_symbol_to_global;
            fb.class_deinit_map = &class_deinit_map;
            fb.build_errors = &out.gate_errors;
            fb.def = &out.mod.funcs[init_fid];
            fb.cur_bb = init_entry;

            for (const auto& kv : sorted_globals) {
                const auto gid = kv.second;
                if ((size_t)gid >= out.mod.globals.size()) continue;
                const auto& g = out.mod.globals[gid];
                ValueId gref = fb.emit_global_ref(gid, g.name);
                fb.bind(kv.first, FuncBuild::Binding{true, true, gref, kInvalidId});
            }

            fb.push_scope();
            for (const auto& gi : sorted_init_items) {
                if (gi.init == parus::sir::k_invalid_value) continue;
                if (gi.gid == kInvalidId || (size_t)gi.gid >= out.mod.globals.size()) continue;
                const auto& g = out.mod.globals[gi.gid];
                ValueId slot = fb.emit_global_ref(gi.gid, g.name);
                ValueId init_v = fb.lower_value(gi.init);
                init_v = fb.coerce_value_for_target(gi.type, init_v);
                fb.emit_store(slot, init_v);
            }
            fb.pop_scope();

            if (!out.mod.blocks[fb.cur_bb].has_term) {
                ValueId rv = fb.emit_const_null(unit_ty);
                fb.ret(rv);
            }
        }

        if (!out.gate_errors.empty()) {
            out.gate_passed = false;
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
            out.mod.add_escape_hint(hint);
        }

        return out;
    }

} // namespace parus::oir
