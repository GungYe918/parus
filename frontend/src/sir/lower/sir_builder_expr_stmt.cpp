// frontend/src/sir/sir_builder_expr_stmt.cpp
#include "sir_builder_internal.hpp"
#include <parus/syntax/TokenKind.hpp>


namespace parus::sir::detail {

    namespace {
        enum class ImplBindingKind : uint8_t {
            kNone = 0,
            kSpinLoop,
            kSizeOf,
            kAlignOf,
        };

        ImplBindingKind parse_impl_binding_payload_(std::string_view payload) {
            if (!payload.starts_with("parus_impl_binding|key=")) return ImplBindingKind::kNone;
            std::string_view rest = payload.substr(std::string_view("parus_impl_binding|key=").size());
            std::string_view key = rest;
            std::string_view mode = "compiler";
            if (const size_t split = rest.find('|'); split != std::string_view::npos) {
                key = rest.substr(0, split);
                rest = rest.substr(split + 1);
                while (!rest.empty()) {
                    const size_t next = rest.find('|');
                    const std::string_view part =
                        (next == std::string_view::npos) ? rest : rest.substr(0, next);
                    if (part.starts_with("mode=")) {
                        mode = part.substr(std::string_view("mode=").size());
                    }
                    if (next == std::string_view::npos) break;
                    rest = rest.substr(next + 1);
                }
            }
            if (mode != "compiler") return ImplBindingKind::kNone;
            if (key == "Impl::SpinLoop") return ImplBindingKind::kSpinLoop;
            if (key == "Impl::SizeOf") return ImplBindingKind::kSizeOf;
            if (key == "Impl::AlignOf") return ImplBindingKind::kAlignOf;
            return ImplBindingKind::kNone;
        }
    } // namespace

    static SwitchCasePatKind lower_switch_pat_kind_(parus::ast::CasePatKind k) {
        using A = parus::ast::CasePatKind;
        switch (k) {
            case A::kInt:    return SwitchCasePatKind::kInt;
            case A::kChar:   return SwitchCasePatKind::kChar;
            case A::kString: return SwitchCasePatKind::kString;
            case A::kBool:   return SwitchCasePatKind::kBool;
            case A::kNull:   return SwitchCasePatKind::kNull;
            case A::kIdent:  return SwitchCasePatKind::kIdent;
            case A::kEnumVariant: return SwitchCasePatKind::kEnumVariant;
            case A::kError:
            default:
                return SwitchCasePatKind::kError;
        }
    }

    ValueId lower_expr(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId eid
    ) {
        (void)sym; // SIR builder does not sym.lookup (nres provides SymbolId)

        if (eid == parus::ast::k_invalid_expr) return k_invalid_value;

        const auto& e = ast.expr(eid);

        Value v{};
        v.span = e.span;
        v.type = best_effort_type_of_ast_expr(ast, sym, nres, tyck, eid);
        const ast::StmtId overload_sid =
            ((size_t)eid < tyck.expr_overload_target.size())
                ? tyck.expr_overload_target[eid]
                : ast::k_invalid_stmt;

        switch (e.kind) {
            case parus::ast::ExprKind::kIntLit:
                v.kind = ValueKind::kIntLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kFloatLit:
                v.kind = ValueKind::kFloatLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kStringLit:
                if ((size_t)eid < tyck.expr_fstring_runtime_expr.size()) {
                    const auto runtime_eid = tyck.expr_fstring_runtime_expr[eid];
                    if (runtime_eid != parus::ast::k_invalid_expr && runtime_eid != eid) {
                        return lower_expr(m, out_has_any_write, ast, sym, nres, tyck, runtime_eid);
                    }
                }
                v.kind = ValueKind::kStringLit;
                v.text = e.string_folded_text.empty() ? e.text : e.string_folded_text;
                break;
            case parus::ast::ExprKind::kCharLit:
                v.kind = ValueKind::kCharLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kBoolLit:
                v.kind = ValueKind::kBoolLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kNullLit:
                v.kind = ValueKind::kNullLit;
                break;

            case parus::ast::ExprKind::kIdent: {
                const auto cit = tyck.expr_external_const_values.find(eid);
                if (cit != tyck.expr_external_const_values.end()) {
                    switch (cit->second.kind) {
                        case tyck::ConstInitKind::kInt:
                            v.kind = ValueKind::kIntLit;
                            v.text = cit->second.text;
                            break;
                        case tyck::ConstInitKind::kFloat:
                            v.kind = ValueKind::kFloatLit;
                            v.text = cit->second.text;
                            break;
                        case tyck::ConstInitKind::kBool:
                            v.kind = ValueKind::kBoolLit;
                            v.text = (cit->second.text == "0") ? std::string_view("false") : std::string_view("true");
                            break;
                        case tyck::ConstInitKind::kChar:
                            v.kind = ValueKind::kCharLit;
                            v.text = cit->second.text;
                            break;
                        case tyck::ConstInitKind::kString:
                            v.kind = ValueKind::kStringLit;
                            v.text = cit->second.text;
                            break;
                        case tyck::ConstInitKind::kNone:
                        default:
                            break;
                    }
                    if (v.kind != ValueKind::kError) break;
                }
                v.kind = ValueKind::kLocal;
                v.text = e.text;
                v.sym = resolve_symbol_from_expr(nres, tyck, eid);
                break;
            }

            case parus::ast::ExprKind::kUnary: {
                if (e.op == parus::syntax::TokenKind::kAmp) {
                    v.kind = ValueKind::kBorrow;
                    v.borrow_is_mut = e.unary_is_mut;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.origin_sym = resolve_root_place_symbol_from_expr(ast, nres, tyck, e.a);
                    break;
                }
                if (e.op == parus::syntax::TokenKind::kTilde) {
                    v.kind = ValueKind::kEscape;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.origin_sym = resolve_root_place_symbol_from_expr(ast, nres, tyck, e.a);
                    break;
                }
                if ((e.op == parus::syntax::TokenKind::kKwCopy ||
                     e.op == parus::syntax::TokenKind::kKwClone) &&
                    overload_sid != ast::k_invalid_stmt) {
                    v.kind = ValueKind::kCall;
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;
                    v.a = k_invalid_value;
                    const ValueId operand =
                        lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                    v.arg_begin = (uint32_t)m.args.size();
                    v.arg_count = 0;
                    Arg a0{};
                    a0.kind = ArgKind::kPositional;
                    a0.value = operand;
                    a0.span = ast.expr(e.a).span;
                    m.add_arg(a0);
                    v.arg_count = 1;
                    break;
                }

                v.kind = ValueKind::kUnary;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                break;
            }

            case parus::ast::ExprKind::kPostfixUnary: {
                if (e.op == parus::syntax::TokenKind::kBang) {
                    v.kind = ValueKind::kCast;
                    v.op = (uint32_t)parus::ast::CastKind::kAsForce;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.cast_to = best_effort_type_of_ast_expr(ast, sym, nres, tyck, eid);
                    break;
                }

                // v0: postfix++ only
                if (overload_sid != ast::k_invalid_stmt) {
                    v.kind = ValueKind::kCall;
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;
                    v.a = k_invalid_value;
                    const ValueId operand =
                        lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                    v.arg_begin = (uint32_t)m.args.size();
                    v.arg_count = 0;

                    Arg a0{};
                    a0.kind = ArgKind::kPositional;
                    a0.value = operand;
                    a0.span = e.span;
                    m.add_arg(a0);
                    v.arg_count = 1;
                } else {
                    v.kind = ValueKind::kPostfixInc;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                }
                break;
            }

            case parus::ast::ExprKind::kBinary: {
                const bool is_proto_arrow_form =
                    (e.op == parus::syntax::TokenKind::kArrow) ||
                    (e.op == parus::syntax::TokenKind::kDot &&
                     e.a != parus::ast::k_invalid_expr &&
                     ast.expr(e.a).kind == parus::ast::ExprKind::kBinary &&
                     ast.expr(e.a).op == parus::syntax::TokenKind::kArrow);
                if (is_proto_arrow_form) {
                    const ast::StmtId const_decl_sid =
                        ((size_t)eid < tyck.expr_proto_const_decl.size())
                            ? tyck.expr_proto_const_decl[eid]
                            : ast::k_invalid_stmt;
                    if (const_decl_sid != ast::k_invalid_stmt &&
                        (size_t)const_decl_sid < ast.stmts().size()) {
                        const auto& decl = ast.stmt(const_decl_sid);
                        if (decl.kind == ast::StmtKind::kVar &&
                            decl.var_is_proto_provide &&
                            decl.is_const &&
                            decl.init != ast::k_invalid_expr) {
                            return lower_expr(m, out_has_any_write, ast, sym, nres, tyck, decl.init);
                        }
                    }
                    v.kind = ValueKind::kError;
                    break;
                }

                if (e.op == parus::syntax::TokenKind::kDot) {
                    v.kind = ValueKind::kField;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    if (e.b != parus::ast::k_invalid_expr) {
                        const auto& rhs = ast.expr(e.b);
                        v.text = rhs.text;
                    }
                    if ((size_t)eid < tyck.expr_external_c_bitfield.size()) {
                        const auto& access = tyck.expr_external_c_bitfield[eid];
                        v.external_c_bitfield = {
                            .is_valid = access.is_valid,
                            .storage_offset_bytes = access.storage_offset_bytes,
                            .bit_offset = access.bit_offset,
                            .bit_width = access.bit_width,
                            .bit_signed = access.bit_signed,
                        };
                    }
                    break;
                }

                if (overload_sid != ast::k_invalid_stmt) {
                    v.kind = ValueKind::kCall;
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;
                    v.a = k_invalid_value;
                    const ValueId lhs =
                        lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    const ValueId rhs =
                        lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                    v.arg_begin = (uint32_t)m.args.size();
                    v.arg_count = 0;

                    Arg a0{};
                    a0.kind = ArgKind::kPositional;
                    a0.value = lhs;
                    a0.span = ast.expr(e.a).span;
                    m.add_arg(a0);
                    v.arg_count++;

                    Arg a1{};
                    a1.kind = ArgKind::kPositional;
                    a1.value = rhs;
                    a1.span = ast.expr(e.b).span;
                    m.add_arg(a1);
                    v.arg_count++;
                } else {
                    v.kind = ValueKind::kBinary;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                }
                break;
            }

            case parus::ast::ExprKind::kAssign: {
                v.kind = ValueKind::kAssign;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case parus::ast::ExprKind::kTernary: {
                // keep as if-expr in SIR
                v.kind = ValueKind::kIfExpr;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                v.c = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.c);
                break;
            }

            case parus::ast::ExprKind::kIfExpr: {
                // structured if-expr:
                // - v.a = cond
                // - v.b = then value (or wrapped block)
                // - v.c = else value (or wrapped block)
                v.kind = ValueKind::kIfExpr;

                // cond is always ExprId in v0.
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                v.c = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.c);
                break;
            }

            case parus::ast::ExprKind::kBlockExpr: {
                const parus::ast::StmtId blk = e.block_stmt;
                if (is_valid_stmt_id_(ast, blk)) {
                    // create dedicated kBlockExpr node, return it directly (no extra wrapper)
                    return lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck,
                                            blk, e.block_tail, e.span, v.type);
                }
                v.kind = ValueKind::kError;
                break;
            }

            case parus::ast::ExprKind::kLoop: {
                // loop expr lowering:
                // - v.op               : loop_has_header (0/1)
                // - v.loop_source_kind : loop iterable classification
                // - v.loop_binder_type : binder type (header loop only)
                // - v.sym              : resolved loop binder symbol
                // - v.a                : iter value or range start
                // - v.c                : range end (range loops only)
                // - v.b                : BlockId (stored in ValueId slot)
                v.kind = ValueKind::kLoopExpr;
                v.op = e.loop_has_header ? 1u : 0u;
                v.text = e.loop_var;
                v.sym = resolve_loop_symbol_from_expr(nres, eid);
                if ((size_t)eid < tyck.expr_loop_source_kind.size()) {
                    v.loop_source_kind =
                        static_cast<parus::LoopSourceKind>(tyck.expr_loop_source_kind[eid]);
                }
                if ((size_t)eid < tyck.expr_loop_binder_type.size()) {
                    v.loop_binder_type = tyck.expr_loop_binder_type[eid];
                }

                if (e.loop_has_header && e.loop_iter != parus::ast::k_invalid_expr &&
                    (v.loop_source_kind == parus::LoopSourceKind::kRangeExclusive ||
                     v.loop_source_kind == parus::LoopSourceKind::kRangeInclusive)) {
                    const auto& range_expr = ast.expr(e.loop_iter);
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, range_expr.a);
                    v.c = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, range_expr.b);
                } else {
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.loop_iter);
                }

                const parus::ast::StmtId body = e.loop_body;
                if (is_valid_stmt_id_(ast, body)) {
                    const BlockId bid = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, body);
                    v.b = (ValueId)bid; // BlockId stored in ValueId slot by convention.
                } else {
                    v.b = k_invalid_value;
                }

                break;
            }

            case parus::ast::ExprKind::kCall: {
                v.kind = e.call_from_pipe ? ValueKind::kPipeCall : ValueKind::kCall;
                if ((size_t)eid < tyck.expr_call_is_c_abi.size()) {
                    v.call_is_c_abi = (tyck.expr_call_is_c_abi[eid] != 0u);
                }
                if ((size_t)eid < tyck.expr_call_is_c_variadic.size()) {
                    v.call_is_c_variadic = (tyck.expr_call_is_c_variadic[eid] != 0u);
                }
                if ((size_t)eid < tyck.expr_call_c_callconv.size()) {
                    v.call_c_callconv = tyck.expr_call_c_callconv[eid];
                }
                if ((size_t)eid < tyck.expr_call_c_fixed_param_count.size()) {
                    v.call_c_fixed_param_count = tyck.expr_call_c_fixed_param_count[eid];
                }
                if ((size_t)eid < tyck.expr_enum_ctor_owner_type.size()) {
                    const auto owner_ty = tyck.expr_enum_ctor_owner_type[eid];
                    if (owner_ty != parus::ty::kInvalidType) {
                        v.kind = ValueKind::kEnumCtor;
                        v.call_is_enum_ctor = true;
                        v.ctor_owner_type = owner_ty;
                        if ((size_t)eid < tyck.expr_enum_ctor_variant_index.size()) {
                            v.enum_ctor_variant_index = tyck.expr_enum_ctor_variant_index[eid];
                        }
                        if ((size_t)eid < tyck.expr_enum_ctor_tag_value.size()) {
                            v.enum_ctor_tag_value = tyck.expr_enum_ctor_tag_value[eid];
                        }
                    }
                }
                if (!v.call_is_enum_ctor &&
                    e.a != parus::ast::k_invalid_expr &&
                    e.arg_count == 0) {
                    const auto& callee_expr = ast.expr(e.a);
                    if (callee_expr.kind == parus::ast::ExprKind::kIdent) {
                        const std::string callee_text(callee_expr.text);
                        const size_t split = callee_text.rfind("::");
                        if (split != std::string::npos && split > 0 && split + 2 < callee_text.size()) {
                            const std::string owner_text = callee_text.substr(0, split);
                            const std::string variant_text = callee_text.substr(split + 2);

                            parus::ast::StmtId enum_sid = parus::ast::k_invalid_stmt;
                            bool ambiguous = false;
                            for (parus::ast::StmtId sid = 0;
                                 sid < static_cast<parus::ast::StmtId>(ast.stmts().size());
                                 ++sid) {
                                const auto& cand = ast.stmt(sid);
                                if (cand.kind != parus::ast::StmtKind::kEnumDecl) continue;
                                if (cand.name != owner_text) continue;
                                if (enum_sid != parus::ast::k_invalid_stmt && enum_sid != sid) {
                                    ambiguous = true;
                                    break;
                                }
                                enum_sid = sid;
                            }

                            if (!ambiguous &&
                                enum_sid != parus::ast::k_invalid_stmt &&
                                static_cast<size_t>(enum_sid) < ast.stmts().size()) {
                                const auto& enum_decl = ast.stmt(enum_sid);
                                if (enum_decl.type != parus::ty::kInvalidType &&
                                    enum_decl.decl_generic_param_count == 0) {
                                    int64_t next_tag = 0;
                                    const auto& variants = ast.enum_variant_decls();
                                    const uint64_t begin = enum_decl.enum_variant_begin;
                                    const uint64_t end = begin + enum_decl.enum_variant_count;
                                    if (end <= variants.size()) {
                                        for (uint32_t i = 0; i < enum_decl.enum_variant_count; ++i) {
                                            const auto& variant = variants[begin + i];
                                            const int64_t tag = variant.has_discriminant
                                                ? variant.discriminant
                                                : next_tag;
                                            next_tag = tag + 1;
                                            if (variant.name != variant_text) continue;
                                            if (variant.payload_count != 0) break;
                                            v.kind = ValueKind::kEnumCtor;
                                            v.call_is_enum_ctor = true;
                                            v.ctor_owner_type = enum_decl.type;
                                            v.enum_ctor_variant_index = i;
                                            v.enum_ctor_tag_value = tag;
                                            v.type = enum_decl.type;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if ((size_t)eid < tyck.expr_ctor_owner_type.size()) {
                    const auto owner_ty = tyck.expr_ctor_owner_type[eid];
                    if (owner_ty != parus::ty::kInvalidType) {
                        v.call_is_ctor = true;
                        v.ctor_owner_type = owner_ty;
                    }
                }
                bool inject_implicit_receiver = false;
                parus::ast::ExprId receiver_eid = parus::ast::k_invalid_expr;
                uint32_t receiver_param_index = 0xFFFF'FFFFu;
                bool use_external_callee = false;
                uint32_t external_callee_sym = k_invalid_symbol;
                parus::ast::ExprId external_receiver_eid = parus::ast::k_invalid_expr;
                if ((size_t)eid < tyck.expr_external_callee_symbol.size()) {
                    external_callee_sym = tyck.expr_external_callee_symbol[eid];
                    use_external_callee = (external_callee_sym != sema::SymbolTable::kNoScope &&
                                           external_callee_sym != k_invalid_symbol);
                }
                if ((size_t)eid < tyck.expr_external_receiver_expr.size()) {
                    external_receiver_eid = tyck.expr_external_receiver_expr[eid];
                }
                if (overload_sid != ast::k_invalid_stmt) {
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;

                    // acts-for method call sugar:
                    //   obj.m(...)  -> T::m(obj, ...)  when selected overload has `self` receiver.
                    if ((size_t)overload_sid < ast.stmts().size() &&
                        e.a != parus::ast::k_invalid_expr) {
                        const auto& callee_expr = ast.expr(e.a);
                        if (callee_expr.kind == parus::ast::ExprKind::kBinary &&
                            callee_expr.op == parus::syntax::TokenKind::kDot &&
                            callee_expr.a != parus::ast::k_invalid_expr) {
                            const auto& def = ast.stmt(overload_sid);
                            if (def.kind == parus::ast::StmtKind::kFnDecl && def.param_count > 0) {
                                for (uint32_t pi = 0; pi < def.param_count; ++pi) {
                                    const auto& p = ast.params()[def.param_begin + pi];
                                    if (!p.is_self) continue;
                                    inject_implicit_receiver = true;
                                    receiver_eid = callee_expr.a;
                                    receiver_param_index = pi;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (use_external_callee) {
                    v.callee_sym = external_callee_sym;
                    v.callee_decl_stmt = ast::k_invalid_stmt;
                }
                if (v.callee_sym != k_invalid_symbol && v.callee_sym < sym.symbols().size()) {
                    const auto& callee_sym = sym.symbol(v.callee_sym);
                    const ImplBindingKind impl_binding = parse_impl_binding_payload_(callee_sym.external_payload);
                    auto trailing_name_segment_ = [](std::string_view name) -> std::string_view {
                        const size_t pos = name.rfind("::");
                        return (pos == std::string_view::npos) ? name : name.substr(pos + 2);
                    };
                    const auto& type_args = ast.type_args();
                    const uint64_t begin = e.call_type_arg_begin;
                    const uint64_t end = begin + e.call_type_arg_count;
                    switch (impl_binding) {
                        case ImplBindingKind::kSpinLoop:
                            v.core_call_kind = CoreCallKind::kHintSpinLoop;
                            break;
                        case ImplBindingKind::kSizeOf:
                            if (e.call_type_arg_count == 1 && begin <= type_args.size() && end <= type_args.size()) {
                                v.core_call_type_arg = type_args[e.call_type_arg_begin];
                                v.core_call_kind = CoreCallKind::kMemSizeOf;
                            }
                            break;
                        case ImplBindingKind::kAlignOf:
                            if (e.call_type_arg_count == 1 && begin <= type_args.size() && end <= type_args.size()) {
                                v.core_call_type_arg = type_args[e.call_type_arg_begin];
                                v.core_call_kind = CoreCallKind::kMemAlignOf;
                            }
                            break;
                        case ImplBindingKind::kNone:
                            break;
                    }
                    if (v.core_call_kind == CoreCallKind::kNone) {
                        const bool is_core_mem =
                            callee_sym.kind == parus::sema::SymbolKind::kFn &&
                            callee_sym.decl_bundle_name == "core" &&
                            (callee_sym.decl_module_head == "mem" ||
                             callee_sym.decl_module_head == "core::mem");
                        if (is_core_mem) {
                            const std::string_view tail = trailing_name_segment_(callee_sym.name);
                            if (tail == "swap") {
                                if (e.call_type_arg_count == 1 && begin <= type_args.size() && end <= type_args.size()) {
                                    v.core_call_type_arg = type_args[e.call_type_arg_begin];
                                    v.core_call_kind = CoreCallKind::kMemSwap;
                                }
                            } else if (tail == "replace") {
                                if (e.call_type_arg_count == 1 && begin <= type_args.size() && end <= type_args.size()) {
                                    v.core_call_type_arg = type_args[e.call_type_arg_begin];
                                    v.core_call_kind = CoreCallKind::kMemReplace;
                                }
                            }
                        }
                    }
                }

                // callee
                if (v.kind == ValueKind::kEnumCtor) {
                    v.a = k_invalid_value;
                } else if (overload_sid != ast::k_invalid_stmt || use_external_callee) {
                    v.a = k_invalid_value;
                } else {
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                }

                // Build parent args first, then append as one contiguous slice.
                std::vector<Arg> pending_args;
                pending_args.reserve(e.arg_count + (inject_implicit_receiver ? 1u : 0u));

                if (inject_implicit_receiver &&
                    receiver_param_index == 0 &&
                    receiver_eid != parus::ast::k_invalid_expr) {
                    Arg recv{};
                    recv.kind = ArgKind::kPositional;
                    recv.has_label = false;
                    recv.is_hole = false;
                    recv.span = ast.expr(receiver_eid).span;
                    recv.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, receiver_eid);
                    pending_args.push_back(recv);
                }
                if (!inject_implicit_receiver &&
                    use_external_callee &&
                    external_receiver_eid != parus::ast::k_invalid_expr) {
                    Arg recv{};
                    recv.kind = ArgKind::kPositional;
                    recv.has_label = false;
                    recv.is_hole = false;
                    recv.span = ast.expr(external_receiver_eid).span;
                    recv.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, external_receiver_eid);
                    pending_args.push_back(recv);
                }

                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& aa = ast.args()[e.arg_begin + i];
                    Arg parent{};
                    parent.span = aa.span;
                    parent.has_label = aa.has_label;
                    parent.is_hole = aa.is_hole;
                    parent.label = aa.label;
                    parent.kind = (aa.kind == parus::ast::ArgKind::kLabeled)
                        ? ArgKind::kLabeled
                        : ArgKind::kPositional;

                    if (!aa.is_hole && aa.expr != parus::ast::k_invalid_expr) {
                        parent.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, aa.expr);
                    } else {
                        parent.value = k_invalid_value;
                    }

                    pending_args.push_back(parent);
                }

                if (inject_implicit_receiver &&
                    receiver_param_index != 0 &&
                    receiver_eid != parus::ast::k_invalid_expr) {
                    Arg recv{};
                    recv.kind = ArgKind::kPositional;
                    recv.has_label = false;
                    recv.is_hole = false;
                    recv.span = ast.expr(receiver_eid).span;
                    recv.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, receiver_eid);
                    pending_args.push_back(recv);
                }

                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;
                for (const auto& a : pending_args) {
                    m.add_arg(a);
                    v.arg_count++;
                }

                break;
            }

            case parus::ast::ExprKind::kArrayLit: {
                v.kind = ValueKind::kArrayLit;
                std::vector<Arg> pending_items;
                pending_items.reserve(e.arg_count);

                const auto& args = ast.args();
                const uint32_t end = e.arg_begin + e.arg_count;
                if (e.arg_begin < args.size() && end <= args.size()) {
                    for (uint32_t i = 0; i < e.arg_count; ++i) {
                        const auto& aa = args[e.arg_begin + i];

                        Arg item{};
                        item.kind = ArgKind::kPositional;
                        item.has_label = false;
                        item.is_hole = aa.is_hole;
                        item.span = aa.span;

                        if (!aa.is_hole && aa.expr != parus::ast::k_invalid_expr) {
                            item.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, aa.expr);
                        } else {
                            item.value = k_invalid_value;
                        }

                        pending_items.push_back(item);
                    }
                }
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;
                for (const auto& item : pending_items) {
                    m.add_arg(item);
                    v.arg_count++;
                }
                break;
            }

            case parus::ast::ExprKind::kFieldInit: {
                v.kind = ValueKind::kFieldInit;
                v.text = e.text;
                std::vector<Arg> pending_fields;
                pending_fields.reserve(e.field_init_count);

                const auto& inits = ast.field_init_entries();
                const uint64_t begin = e.field_init_begin;
                const uint64_t end = begin + e.field_init_count;
                if (begin <= inits.size() && end <= inits.size()) {
                    for (uint32_t i = 0; i < e.field_init_count; ++i) {
                        const auto& ent = inits[e.field_init_begin + i];
                        Arg a{};
                        a.kind = ArgKind::kLabeled;
                        a.has_label = true;
                        a.label = ent.name;
                        a.span = ent.span;
                        if (ent.expr != parus::ast::k_invalid_expr) {
                            a.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, ent.expr);
                        }
                        pending_fields.push_back(a);
                    }
                }
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;
                for (const auto& a : pending_fields) {
                    m.add_arg(a);
                    v.arg_count++;
                }
                break;
            }

            case parus::ast::ExprKind::kIndex: {
                v.kind = ValueKind::kIndex;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case parus::ast::ExprKind::kCast: {
                v.kind = ValueKind::kCast;

                // operand
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                // cast kind: as / as? / as!
                v.op = (uint32_t)e.cast_kind;

                // cast target type: "T"
                v.cast_to = e.cast_type;

                // v.type is already set at function entry:
                //   v.type = type_of_ast_expr(tyck, eid);
                // so dump/lowering can use:
                //   - cast_to for syntactic target
                //   - type for normalized RESULT type (as? => T?)
                break;
            }

            // v0 not-lowered-yet expr kinds
            case parus::ast::ExprKind::kHole:
            default:
                v.kind = ValueKind::kError;
                break;
        }

        v.place = classify_place_from_ast(ast, eid);
        v.effect = classify_effect(v.kind);
        auto join_child = [&](ValueId cid) {
            if (cid != k_invalid_value && (size_t)cid < m.values.size()) {
                v.effect = join_effect_(v.effect, m.values[cid].effect);
            }
        };

        switch (v.kind) {
            case ValueKind::kUnary:
            case ValueKind::kBorrow:
            case ValueKind::kEscape:
            case ValueKind::kPostfixInc:
            case ValueKind::kCast:
                join_child(v.a);
                break;

            case ValueKind::kBinary:
            case ValueKind::kAssign:
            case ValueKind::kIndex:
                join_child(v.a);
                join_child(v.b);
                break;
            case ValueKind::kField:
                join_child(v.a);
                break;

            case ValueKind::kIfExpr:
                join_child(v.a);
                join_child(v.b);
                join_child(v.c);
                break;

            case ValueKind::kCall:
            case ValueKind::kPipeCall:
                join_child(v.a);
                break;

            case ValueKind::kEnumCtor:
                if ((uint64_t)v.arg_begin + (uint64_t)v.arg_count <= (uint64_t)m.args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        join_child(m.args[v.arg_begin + i].value);
                    }
                }
                break;

            case ValueKind::kArrayLit:
            case ValueKind::kFieldInit:
                if ((uint64_t)v.arg_begin + (uint64_t)v.arg_count <= (uint64_t)m.args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        join_child(m.args[v.arg_begin + i].value);
                    }
                }
                break;

            case ValueKind::kLoopExpr: {
                join_child(v.a);
                join_child(v.c);
                const BlockId body = (BlockId)v.b;
                v.effect = join_effect_(v.effect, effect_of_block_(m, body));
                break;
            }

            default:
                break;
        }

        if (v.effect == EffectClass::kMayWrite) {
            out_has_any_write = true;
        }

        return m.add_value(v);
    }

    Stmt lower_stmt_(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);

        Stmt out{};
        out.span = s.span;

        switch (s.kind) {
            case parus::ast::StmtKind::kExprStmt:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kVar: {
                out.kind = StmtKind::kVarDecl;
                out.is_set = s.is_set;
                out.is_mut = s.is_mut;
                out.is_static = s.is_static;
                out.is_const = s.is_const;
                out.name = s.name;
                out.init = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.init);

                // decl symbol from stmt
                out.sym = resolve_symbol_from_stmt(nres, sid);

                const bool has_sym = (out.sym != k_invalid_symbol && (size_t)out.sym < sym.symbols().size());
                const TypeId use_derived_type = resolve_decl_type_from_symbol_uses(ast, sym, nres, tyck, out.sym);

                // declared_type policy:
                // - let: prefer declared symbol type, fallback annotation
                // - set: inference can leave the symbol table on a transient literal type
                //   until a later use forces a concrete declaration type. Prefer the
                //   use-derived recovery before falling back to the raw symbol/AST type.
                if (!s.is_set) {
                    out.declared_type = has_sym ? sym.symbol(out.sym).declared_type : k_invalid_type;
                    if (out.declared_type == k_invalid_type) out.declared_type = s.type;
                } else {
                    out.declared_type = use_derived_type;
                    if (out.declared_type == k_invalid_type) {
                        out.declared_type = has_sym ? sym.symbol(out.sym).declared_type : k_invalid_type;
                    }
                    if (out.declared_type == k_invalid_type) out.declared_type = s.type;
                    if (out.declared_type == k_invalid_type) {
                        out.declared_type = best_effort_type_of_ast_expr(ast, sym, nres, tyck, s.init);
                    }
                }
                break;
            }

            case parus::ast::StmtKind::kThrow:
                out.kind = StmtKind::kThrowStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kTryCatch: {
                out.kind = StmtKind::kTryCatchStmt;
                if (s.a != parus::ast::k_invalid_stmt) {
                    out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                }

                out.catch_clause_begin = static_cast<uint32_t>(m.try_catch_clauses.size());
                out.catch_clause_count = 0;

                const auto& clauses = ast.try_catch_clauses();
                const uint64_t cb = s.catch_clause_begin;
                const uint64_t ce = cb + s.catch_clause_count;
                if (cb <= clauses.size() && ce <= clauses.size()) {
                    for (uint32_t i = 0; i < s.catch_clause_count; ++i) {
                        const auto& ac = clauses[s.catch_clause_begin + i];
                        TryCatchClause sc{};
                        sc.bind_name = ac.bind_name;
                        sc.has_typed_bind = ac.has_typed_bind;
                        sc.bind_type = ac.bind_type;
                        sc.bind_sym = ac.resolved_symbol;
                        sc.span = ac.span;
                        if (ac.body != parus::ast::k_invalid_stmt) {
                            sc.body = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, ac.body);
                        }
                        (void)m.add_try_catch_clause(sc);
                        out.catch_clause_count++;
                    }
                }
                break;
            }

            case parus::ast::StmtKind::kIf:
                out.kind = StmtKind::kIfStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                if (s.b != parus::ast::k_invalid_stmt) out.b = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.b);
                break;

            case parus::ast::StmtKind::kWhile:
                out.kind = StmtKind::kWhileStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;
            case parus::ast::StmtKind::kDoScope:
                out.kind = StmtKind::kDoScopeStmt;
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;
            case parus::ast::StmtKind::kDoWhile:
                out.kind = StmtKind::kDoWhileStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;
            case parus::ast::StmtKind::kManual:
                out.kind = StmtKind::kManualStmt;
                out.manual_perm_mask = s.manual_perm_mask;
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;

            case parus::ast::StmtKind::kCommitStmt:
                out.kind = StmtKind::kCommitStmt;
                break;

            case parus::ast::StmtKind::kRecastStmt:
                out.kind = StmtKind::kRecastStmt;
                break;

            case parus::ast::StmtKind::kReturn:
                out.kind = StmtKind::kReturn;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kRequire:
                // compile-time assertion: keep as pure expr stmt placeholder (no runtime side-effect intent)
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kBreak:
                out.kind = StmtKind::kBreak;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kContinue:
                out.kind = StmtKind::kContinue;
                break;

            case parus::ast::StmtKind::kBlock:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck, sid,
                                            parus::ast::k_invalid_expr, s.span, k_invalid_type);
                break;

            case parus::ast::StmtKind::kSwitch: {
                out.kind = StmtKind::kSwitch;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                out.case_begin = (uint32_t)m.switch_cases.size();
                out.case_count = 0;
                out.has_default = s.has_default;

                const uint64_t cb = s.case_begin;
                const uint64_t ce = cb + s.case_count;
                if (cb <= ast.switch_cases().size() && ce <= ast.switch_cases().size()) {
                    for (uint32_t i = 0; i < s.case_count; ++i) {
                        const auto& ac = ast.switch_cases()[s.case_begin + i];
                        SwitchCase sc{};
                        sc.is_default = ac.is_default;
                        sc.pat_kind = lower_switch_pat_kind_(ac.pat_kind);
                        sc.pat_text = ac.pat_text;
                        sc.enum_type = ac.enum_type;
                        sc.enum_variant_name = ac.enum_variant_name;
                        sc.enum_tag_value = ac.enum_tag_value;
                        sc.enum_bind_begin = (uint32_t)m.switch_enum_binds.size();
                        sc.enum_bind_count = 0;

                        const uint64_t ebb = ac.enum_bind_begin;
                        const uint64_t ebe = ebb + ac.enum_bind_count;
                        if (ebb <= ast.switch_enum_binds().size() && ebe <= ast.switch_enum_binds().size()) {
                            for (uint32_t bi = ac.enum_bind_begin; bi < ac.enum_bind_begin + ac.enum_bind_count; ++bi) {
                                const auto& ab = ast.switch_enum_binds()[bi];
                                SwitchEnumBind sb{};
                                sb.field_name = ab.field_name;
                                sb.bind_name = ab.bind_name;
                                sb.storage_name = ab.storage_name;
                                sb.bind_type = ab.bind_type;
                                sb.bind_sym = ab.resolved_symbol;
                                sb.span = ab.span;
                                m.add_switch_enum_bind(sb);
                                sc.enum_bind_count++;
                            }
                        }
                        sc.span = ac.span;
                        if (ac.body != parus::ast::k_invalid_stmt) {
                            sc.body = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, ac.body);
                        }
                        (void)m.add_switch_case(sc);
                        out.case_count++;
                    }
                }
                break;
            }

            default:
                out.kind = StmtKind::kError;
                break;
        }

        return out;
    }

} // namespace parus::sir::detail
