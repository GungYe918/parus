// frontend/src/sir/sir_builder_decl.cpp
#include <parus/sir/Builder.hpp>
#include "sir_builder_internal.hpp"
#include <unordered_set>


namespace parus::sir::detail {

    namespace {
        struct ParsedCImportPayload {
            bool is_c_import = false;
            bool is_c_decl = false;
            bool is_c_abi = false;
            bool is_variadic = false;
            CCallConv callconv = CCallConv::kDefault;
        };

        struct ParsedCImportGlobalPayload {
            bool is_c_import_global = false;
            bool is_const = false;
            CThreadLocalKind tls_kind = CThreadLocalKind::kNone;
        };

        ParsedCImportPayload parse_cimport_payload_(std::string_view payload) {
            ParsedCImportPayload out{};
            if (payload.starts_with("parus_c_import|")) {
                out.is_c_import = true;
                out.is_c_abi = true;
            } else if (payload.starts_with("parus_c_abi_decl|")) {
                out.is_c_decl = true;
                out.is_c_abi = true;
            } else {
                return out;
            }

            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                const size_t eq = part.find('=');
                if (eq != std::string_view::npos && eq + 1 < part.size()) {
                    const std::string_view key = part.substr(0, eq);
                    const std::string_view val = part.substr(eq + 1);
                    if (key == "variadic") {
                        out.is_variadic = (val == "1" || val == "true");
                    } else if (key == "callconv") {
                        if (val == "cdecl") out.callconv = CCallConv::kCdecl;
                        else if (val == "stdcall") out.callconv = CCallConv::kStdCall;
                        else if (val == "fastcall") out.callconv = CCallConv::kFastCall;
                        else if (val == "vectorcall") out.callconv = CCallConv::kVectorCall;
                        else if (val == "win64") out.callconv = CCallConv::kWin64;
                        else if (val == "sysv") out.callconv = CCallConv::kSysV;
                        else out.callconv = CCallConv::kDefault;
                    }
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }

        ParsedCImportGlobalPayload parse_cimport_global_payload_(std::string_view payload) {
            ParsedCImportGlobalPayload out{};
            if (!payload.starts_with("parus_c_import_global|")) return out;
            out.is_c_import_global = true;

            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                const size_t eq = part.find('=');
                if (eq != std::string_view::npos && eq + 1 < part.size()) {
                    const std::string_view key = part.substr(0, eq);
                    const std::string_view val = part.substr(eq + 1);
                    if (key == "const") {
                        out.is_const = (val == "1" || val == "true");
                    } else if (key == "tls") {
                        if (val == "dynamic") out.tls_kind = CThreadLocalKind::kDynamic;
                        else if (val == "static") out.tls_kind = CThreadLocalKind::kStatic;
                        else out.tls_kind = CThreadLocalKind::kNone;
                    }
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }

        bool parse_external_throwing_payload_(std::string_view payload) {
            if (payload.starts_with("parus_c_import|") ||
                payload.starts_with("parus_c_abi_decl|")) {
                return false;
            }
            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                if (part == "throwing=1") return true;
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return false;
        }

        CCallConv lower_ty_callconv_(parus::ty::CCallConv cc) {
            switch (cc) {
                case parus::ty::CCallConv::kCdecl: return CCallConv::kCdecl;
                case parus::ty::CCallConv::kStdCall: return CCallConv::kStdCall;
                case parus::ty::CCallConv::kFastCall: return CCallConv::kFastCall;
                case parus::ty::CCallConv::kVectorCall: return CCallConv::kVectorCall;
                case parus::ty::CCallConv::kWin64: return CCallConv::kWin64;
                case parus::ty::CCallConv::kSysV: return CCallConv::kSysV;
                case parus::ty::CCallConv::kDefault:
                default:
                    return CCallConv::kDefault;
            }
        }
    } // namespace

    FnMode lower_fn_mode(parus::ast::FnMode m) {
        switch (m) {
            case parus::ast::FnMode::kPub: return FnMode::kPub;
            case parus::ast::FnMode::kSub: return FnMode::kSub;
            case parus::ast::FnMode::kNone:
            default: return FnMode::kNone;
        }
    }

    FieldLayout lower_field_layout(parus::ast::FieldLayout l) {
        switch (l) {
            case parus::ast::FieldLayout::kC:
                return FieldLayout::kC;
            case parus::ast::FieldLayout::kNone:
            default:
                return FieldLayout::kNone;
        }
    }

    /// @brief AST 함수 선언 1개를 SIR Func로 lower하여 모듈에 추가한다.
    FuncId lower_func_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        const parus::ty::TypePool& types,
        parus::ast::StmtId sid,
        bool is_acts_member,
        ActsId owner_acts,
        bool is_actor_member,
        bool is_actor_init,
        TypeId actor_owner_type
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kFnDecl) {
            return k_invalid_func;
        }

        Func f{};
        f.span = s.span;

        // signature & ret
        f.sig = s.type;
        f.ret = s.fn_ret;

        // decl symbol (def name)
        f.sym = resolve_symbol_from_stmt(nres, tyck, sid);

        // nested namespace를 통과한 내부 함수는 qualified symbol name을 사용한다.
        // C ABI 함수는 선언 원문 이름을 그대로 유지한다(no-mangle).
        if (s.link_abi == parus::ast::LinkAbi::kC) {
            f.name = s.name;
        } else {
            auto qit = tyck.fn_qualified_names.find(sid);
            if (qit != tyck.fn_qualified_names.end()) {
                f.name = std::string_view(qit->second);
            } else if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
                f.name = sym.symbol(f.sym).name;
            } else {
                f.name = s.name;
            }
        }
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.external_payload = sym.symbol(f.sym).external_payload;
        }

        // qualifiers / mode
        f.is_export = s.is_export;
        f.is_extern = s.is_extern;
        f.fn_mode = lower_fn_mode(s.fn_mode);
        f.abi = (s.link_abi == parus::ast::LinkAbi::kC) ? FuncAbi::kC : FuncAbi::kParus;
        f.c_callconv = (s.type != parus::ty::kInvalidType && types.is_fn(s.type))
            ? lower_ty_callconv_(types.fn_callconv(s.type))
            : CCallConv::kDefault;
        f.is_c_variadic = (s.type != parus::ty::kInvalidType && types.is_fn(s.type))
            ? types.fn_is_c_variadic(s.type)
            : s.fn_is_c_variadic;
        f.c_fixed_param_count = s.param_count;

        f.is_pure = s.is_pure;
        f.is_comptime = s.is_comptime;
        f.is_const = s.fn_is_const;
        f.is_commit = s.is_commit;
        f.is_recast = s.is_recast;
        f.is_throwing = s.is_throwing;

        f.positional_param_count = s.positional_param_count;
        f.has_named_group = s.has_named_group;
        f.is_acts_member = is_acts_member;
        f.owner_acts = owner_acts;
        f.is_actor_member = is_actor_member;
        f.is_actor_init = is_actor_init;
        f.actor_owner_type = actor_owner_type;

        // attrs slice
        f.attr_begin = (uint32_t)m.attrs.size();
        f.attr_count = 0;
        for (uint32_t ai = 0; ai < s.attr_count; ++ai) {
            const auto& aa = ast.fn_attrs()[s.attr_begin + ai];
            Attr sa{};
            sa.name = aa.name;
            sa.span = aa.span;
            m.add_attr(sa);
            f.attr_count++;
        }

        // params slice
        f.param_begin = (uint32_t)m.params.size();
        f.param_count = 0;

        bool has_any_write = false;
        for (uint32_t pi = 0; pi < s.param_count; ++pi) {
            const uint32_t param_index = s.param_begin + pi;
            const auto& p = ast.params()[param_index];

            Param sp{};
            sp.name = p.name;
            sp.type = p.type;
            sp.is_mut = p.is_mut;
            sp.is_self = p.is_self;
            sp.is_named_group = p.is_named_group;
            sp.span = p.span;

            sp.has_default = p.has_default;
            if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                sp.default_value = lower_expr(m, has_any_write, ast, sym, nres, tyck, p.default_expr);
            }

            // param SymbolId binding
            sp.sym = resolve_symbol_from_param_index(nres, tyck, param_index);

            m.add_param(sp);
            f.param_count++;
        }

        // body
        if (s.a != ast::k_invalid_stmt) {
            f.entry = lower_block_stmt(m, has_any_write, ast, sym, nres, tyck, s.a);
        }
        f.origin_stmt = sid;

        f.has_any_write = has_any_write;
        return m.add_func(f);
    }

    /// @brief AST field 선언을 SIR field 메타로 lower한다.
    FieldId lower_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kFieldDecl) {
            return k_invalid_field;
        }

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = lower_field_layout(s.field_layout);
        f.align = s.field_align;
        f.self_type = s.type;

        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin < ast.field_members().size() && end <= ast.field_members().size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto& am = ast.field_members()[i];
                FieldMember sm{};
                sm.name = am.name;
                sm.type = am.type;
                sm.span = am.span;
                m.add_field_member(sm);
                f.member_count++;
            }
        }

        return m.add_field(f);
    }

    /// @brief AST class 선언의 인스턴스 필드 메타를 SIR field 메타로 lower한다.
    FieldId lower_class_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const parus::ty::TypePool& types,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kClassDecl) {
            return k_invalid_field;
        }
        if (s.field_member_count == 0) {
            return k_invalid_field;
        }
        auto& ast_mut = const_cast<parus::ast::AstArena&>(ast);

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (s.type != k_invalid_type) {
            f.name = ast_mut.add_owned_string(types.to_string(s.type));
        } else if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = FieldLayout::kNone;
        f.align = 0;
        f.self_type = s.type;

        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin <= ast.field_members().size() && end <= ast.field_members().size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto& am = ast.field_members()[i];
                FieldMember sm{};
                sm.name = am.name;
                sm.type = am.type;
                sm.span = am.span;
                m.add_field_member(sm);
                f.member_count++;
            }
        }

        if (f.member_count == 0) {
            return k_invalid_field;
        }
        return m.add_field(f);
    }

    /// @brief AST actor 선언의 draft 필드 메타를 SIR field 메타로 lower한다.
    FieldId lower_actor_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kActorDecl) {
            return k_invalid_field;
        }
        if (s.field_member_count == 0) {
            return k_invalid_field;
        }

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = FieldLayout::kNone;
        f.align = 0;
        f.self_type = s.type;

        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin <= ast.field_members().size() && end <= ast.field_members().size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto& am = ast.field_members()[i];
                FieldMember sm{};
                sm.name = am.name;
                sm.type = am.type;
                sm.span = am.span;
                m.add_field_member(sm);
                f.member_count++;
            }
        }

        if (f.member_count == 0) {
            return k_invalid_field;
        }
        return m.add_field(f);
    }

    /// @brief AST enum 선언의 내부 레이아웃 메타를 SIR field 메타로 lower한다.
    FieldId lower_enum_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const parus::ty::TypePool& types,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kEnumDecl) {
            return k_invalid_field;
        }
        auto& ast_mut = const_cast<parus::ast::AstArena&>(ast);

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = lower_field_layout(s.field_layout);
        f.align = s.field_align;
        f.self_type = s.type;
        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        // Every enum uses an internal tag lane.
        FieldMember tag{};
        tag.name = "__tag";
        tag.type = types.builtin(parus::ty::Builtin::kI32);
        tag.span = s.span;
        m.add_field_member(tag);
        f.member_count++;

        const uint64_t vb = s.enum_variant_begin;
        const uint64_t ve = vb + s.enum_variant_count;
        if (vb <= ast.enum_variant_decls().size() && ve <= ast.enum_variant_decls().size()) {
            for (uint32_t vi = 0; vi < s.enum_variant_count; ++vi) {
                const auto& v = ast.enum_variant_decls()[s.enum_variant_begin + vi];
                const uint64_t pb = v.payload_begin;
                const uint64_t pe = pb + v.payload_count;
                if (pb > ast.field_members().size() || pe > ast.field_members().size()) continue;
                for (uint32_t mi = v.payload_begin; mi < v.payload_begin + v.payload_count; ++mi) {
                    const auto& am = ast.field_members()[mi];
                    FieldMember sm{};
                    // Unique per-variant storage lane name.
                    sm.name = ast_mut.add_owned_string(
                        std::string("__v") + std::to_string(vi) + "_" + std::string(am.name));
                    sm.type = am.type;
                    sm.span = am.span;
                    m.add_field_member(sm);
                    f.member_count++;
                }
            }
        }

        return m.add_field(f);
    }

    /// @brief AST var 선언 1개를 SIR global 메타로 lower한다.
    void lower_global_var_decl_(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid,
        std::string_view forced_name = {},
        SymbolId forced_sym = k_invalid_symbol
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kVar) return;

        GlobalVarDecl g{};
        g.span = s.span;
        g.sym = (forced_sym != k_invalid_symbol) ? forced_sym : resolve_symbol_from_stmt(nres, tyck, sid);
        g.is_set = s.is_set;
        g.is_mut = s.is_mut;
        g.is_static = s.is_static;
        g.is_const = s.is_const;
        g.is_export = s.is_export;
        g.is_extern = s.is_extern;
        g.abi = (s.link_abi == parus::ast::LinkAbi::kC) ? FuncAbi::kC : FuncAbi::kParus;

        if (g.abi == FuncAbi::kC) {
            g.name = s.name;
        } else if (!forced_name.empty()) {
            g.name = forced_name;
        } else if (g.sym != k_invalid_symbol && (size_t)g.sym < sym.symbols().size()) {
            g.name = sym.symbol(g.sym).name;
        } else {
            g.name = s.name;
        }

        g.declared_type = s.type;
        if (g.declared_type == k_invalid_type &&
            g.sym != k_invalid_symbol &&
            (size_t)g.sym < sym.symbols().size()) {
            g.declared_type = sym.symbol(g.sym).declared_type;
        }
        if (g.declared_type == k_invalid_type) {
            g.declared_type = resolve_decl_type_from_symbol_uses(ast, sym, nres, tyck, g.sym);
        }
        if (g.declared_type == k_invalid_type && s.init != ast::k_invalid_expr) {
            g.declared_type = best_effort_type_of_ast_expr(ast, sym, nres, tyck, s.init);
        }

        if (s.init != ast::k_invalid_expr) {
            g.init = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.init);
        }

        if (g.sym != k_invalid_symbol) {
            if (auto it = tyck.const_symbol_values.find(g.sym); it != tyck.const_symbol_values.end()) {
                switch (it->second.kind) {
                    case tyck::ConstInitKind::kInt:   g.const_init.kind = ConstInitKind::kInt; break;
                    case tyck::ConstInitKind::kFloat: g.const_init.kind = ConstInitKind::kFloat; break;
                    case tyck::ConstInitKind::kBool:  g.const_init.kind = ConstInitKind::kBool; break;
                    case tyck::ConstInitKind::kChar:  g.const_init.kind = ConstInitKind::kChar; break;
                    case tyck::ConstInitKind::kNone:
                    default:                          g.const_init.kind = ConstInitKind::kNone; break;
                }
                g.const_init.text = it->second.text;
            }
        }

        (void)m.add_global(g);
    }

} // namespace parus::sir::detail

namespace parus::sir {

    using namespace detail;

    Module build_sir_module(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        const ty::TypePool& types,
        const BuildOptions& opt
    ) {
        (void)sym;

        Module m{};
        set_active_type_pool_for_sir_build_(const_cast<ty::TypePool*>(&types));
        struct ActiveTypesReset {
            ~ActiveTypesReset() { set_active_type_pool_for_sir_build_(nullptr); }
        } active_types_reset{};
        m.bundle_enabled = opt.bundle_enabled;
        m.bundle_name = opt.bundle_name;
        m.current_source_norm = opt.current_source_norm;
        m.bundle_sources_norm = opt.bundle_sources_norm;
        bool global_init_has_any_write = false;
        std::unordered_set<ast::StmtId> lowered_fn_stmt_ids;
        std::unordered_set<ast::StmtId> lowered_decl_stmt_ids;
        std::unordered_set<ast::StmtId> generic_acts_template_sid_set(
            tyck.generic_acts_template_sids.begin(),
            tyck.generic_acts_template_sids.end()
        );

        const auto lower_fn_once = [&](ast::StmtId fn_sid,
                                       bool is_acts_member,
                                       ActsId owner_acts,
                                       bool is_actor_member = false,
                                       bool is_actor_init = false,
                                       TypeId actor_owner_type = k_invalid_type) -> FuncId {
            if (fn_sid == ast::k_invalid_stmt || (size_t)fn_sid >= ast.stmts().size()) {
                return k_invalid_func;
            }
            if (!lowered_fn_stmt_ids.insert(fn_sid).second) {
                return k_invalid_func;
            }
            return lower_func_decl_(
                m, ast, sym, nres, tyck, types, fn_sid, is_acts_member, owner_acts,
                is_actor_member, is_actor_init, actor_owner_type);
        };

        // program root must be a block
        if (program_root == ast::k_invalid_stmt || (size_t)program_root >= ast.stmts().size()) {
            return m;
        }
        const auto& prog = ast.stmt(program_root);
        if (prog.kind != ast::StmtKind::kBlock) {
            return m;
        }

        const auto lower_stmt_recursive = [&](auto&& self, parus::ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast.stmts().size()) return;
            const auto& s = ast.stmt(sid);

            if (s.kind == ast::StmtKind::kFnDecl) {
                if (s.fn_generic_param_count > 0) return;
                (void)lower_fn_once(sid, /*is_acts_member=*/false, k_invalid_acts);
                return;
            }

            if (s.kind == ast::StmtKind::kFieldDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                (void)lower_field_decl_(m, ast, sym, nres, sid);
                return;
            }

            if (s.kind == ast::StmtKind::kProtoDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto member_sid = kids[i];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        const auto& member = ast.stmt(member_sid);
                        if (member.kind != ast::StmtKind::kFnDecl) continue;
                        if (member.a == ast::k_invalid_stmt) continue; // signature-only proto member
                        if (member.fn_generic_param_count > 0) continue;
                        (void)lower_fn_once(member_sid, /*is_acts_member=*/false, k_invalid_acts);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kEnumDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                (void)lower_enum_field_decl_(m, ast, sym, nres, types, sid);
                return;
            }

            if (s.kind == ast::StmtKind::kClassDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                (void)lower_class_field_decl_(m, ast, sym, nres, types, sid);

                std::string class_qname = std::string(s.name);
                if (s.type != k_invalid_type) {
                    class_qname = types.to_string(s.type);
                } else if (auto class_sym = resolve_symbol_from_stmt(nres, tyck, sid);
                    class_sym != k_invalid_symbol && (size_t)class_sym < sym.symbols().size()) {
                    class_qname = sym.symbol(class_sym).name;
                }

                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto member_sid = kids[i];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        const auto& member = ast.stmt(member_sid);
                        if (member.kind == ast::StmtKind::kFnDecl) {
                            if (member.a == ast::k_invalid_stmt) continue; // declaration-only member is not lowered
                            if (member.fn_generic_param_count > 0) continue;
                            (void)lower_fn_once(member_sid, /*is_acts_member=*/false, k_invalid_acts);
                            continue;
                        }

                        if (member.kind == ast::StmtKind::kVar) {
                            std::string vqname = class_qname;
                            if (!vqname.empty()) vqname += "::";
                            vqname += std::string(member.name);

                            SymbolId v_sym = resolve_symbol_from_stmt(nres, tyck, member_sid);
                            if (v_sym == k_invalid_symbol) {
                                if (auto sid_sym = sym.lookup(vqname)) {
                                    v_sym = *sid_sym;
                                }
                            }
                            lower_global_var_decl_(m, global_init_has_any_write, ast, sym, nres, tyck, member_sid, vqname, v_sym);
                            continue;
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kActorDecl) {
                (void)lower_actor_field_decl_(m, ast, sym, nres, sid);
                if (s.type != k_invalid_type) {
                    m.actor_types.push_back(s.type);
                }

                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto member_sid = kids[i];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        const auto& member = ast.stmt(member_sid);
                        if (member.kind != ast::StmtKind::kFnDecl) continue;
                        if (member.a == ast::k_invalid_stmt) continue;
                        if (member.fn_generic_param_count > 0) continue;
                        (void)lower_fn_once(
                            member_sid,
                            /*is_acts_member=*/false,
                            k_invalid_acts,
                            member.fn_mode == ast::FnMode::kPub || member.fn_mode == ast::FnMode::kSub,
                            member.name == "init",
                            s.type
                        );
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kActsDecl) {
                if (generic_acts_template_sid_set.find(sid) != generic_acts_template_sid_set.end()) {
                    return;
                }
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                ActsDecl a{};
                a.span = s.span;
                a.sym = resolve_symbol_from_stmt(nres, tyck, sid);
                if (a.sym != k_invalid_symbol && (size_t)a.sym < sym.symbols().size()) {
                    a.name = sym.symbol(a.sym).name;
                } else {
                    a.name = s.name;
                }
                a.is_export = s.is_export;
                a.is_for = s.acts_is_for;
                a.has_set_name = s.acts_has_set_name;
                a.target_type = s.acts_target_type;
                a.func_begin = (uint32_t)m.funcs.size();
                a.func_count = 0;

                const ActsId aid = m.add_acts(a);

                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t k = begin; k < end; ++k) {
                        const auto member_sid = kids[k];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        if (ast.stmt(member_sid).kind != ast::StmtKind::kFnDecl) continue;
                        if (ast.stmt(member_sid).fn_generic_param_count > 0) continue;

                        const FuncId fid = lower_fn_once(member_sid, /*is_acts_member=*/true, aid);
                        if (fid != k_invalid_func) {
                            m.acts[aid].func_count++;
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kVar) {
                lower_global_var_decl_(m, global_init_has_any_write, ast, sym, nres, tyck, sid);
                return;
            }

            if (s.kind == ast::StmtKind::kNestDecl) {
                if (!s.nest_is_file_directive) {
                    self(self, s.a);
                }
                return;
            }

            if (s.kind == ast::StmtKind::kBlock) {
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        self(self, kids[s.stmt_begin + i]);
                    }
                }
                return;
            }
        };

        lower_stmt_recursive(lower_stmt_recursive, program_root);

        for (const auto inst_sid : tyck.generic_instantiated_fn_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            const auto& fs = ast.stmt(inst_sid);
            if (fs.kind != ast::StmtKind::kFnDecl) continue;
            if (fs.a == ast::k_invalid_stmt) continue;
            (void)lower_fn_once(inst_sid, /*is_acts_member=*/false, k_invalid_acts);
        }

        for (const auto inst_sid : tyck.generic_instantiated_field_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_proto_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_class_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_acts_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_enum_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }

        // Inject externally imported function symbols into SIR so OIR can emit stable function refs
        // for cross-module/cross-bundle calls without materializing null callee placeholders.
        std::unordered_set<SymbolId> lowered_fn_symbols{};
        lowered_fn_symbols.reserve(m.funcs.size());
        std::unordered_set<std::string> lowered_fn_decl_keys{};
        lowered_fn_decl_keys.reserve(m.funcs.size());
        auto make_fn_decl_key = [](std::string_view link_name, std::string_view fallback_name, ty::TypeId sig) {
            std::string key{};
            const auto head = !link_name.empty() ? link_name : fallback_name;
            key.reserve(head.size() + 24u);
            key.append(head);
            key.push_back('#');
            key.append(std::to_string(static_cast<uint64_t>(sig)));
            return key;
        };
        for (const auto& f : m.funcs) {
            if (f.sym != k_invalid_symbol) lowered_fn_symbols.insert(f.sym);
            lowered_fn_decl_keys.insert(make_fn_decl_key(f.external_link_name, f.name, f.sig));
        }

        const auto& symbols = sym.symbols();
        for (uint32_t sid = 0; sid < static_cast<uint32_t>(symbols.size()); ++sid) {
            const auto& ss = symbols[sid];
            if (!ss.is_external) continue;
            if (ss.kind != sema::SymbolKind::kFn) continue;
            if (!lowered_fn_symbols.insert(sid).second) continue;
            if (ss.declared_type == k_invalid_type) continue;
            if (!types.is_fn(ss.declared_type)) continue;
            if (!lowered_fn_decl_keys.insert(
                    make_fn_decl_key(ss.link_name, ss.name, ss.declared_type)).second) {
                continue;
            }

            Func f{};
            f.span = ss.decl_span;
            f.name = ss.name;
            f.external_link_name = ss.link_name;
            f.external_payload = ss.external_payload;
            f.sym = sid;
            f.sig = ss.declared_type;
            f.ret = types.get(ss.declared_type).ret;
            f.is_export = ss.is_export;
            f.is_extern = true;
            f.fn_mode = FnMode::kNone;
            f.abi = FuncAbi::kParus;
            f.c_callconv = CCallConv::kDefault;
            f.is_c_variadic = false;
            f.c_fixed_param_count = 0;
            f.is_pure = false;
            f.is_comptime = false;
            f.is_const = false;
            f.is_commit = false;
            f.is_recast = false;
            f.is_throwing = parse_external_throwing_payload_(ss.external_payload);
            f.entry = k_invalid_block;
            f.origin_stmt = 0xFFFF'FFFFu;
            f.has_any_write = false;
            f.is_acts_member = false;
            f.owner_acts = k_invalid_acts;
            f.is_actor_member = false;
            f.is_actor_init = false;
            f.actor_owner_type = k_invalid_type;

            const auto& sig = types.get(ss.declared_type);
            f.c_fixed_param_count = sig.param_count;
            const bool type_is_c_abi = types.fn_is_c_abi(ss.declared_type);
            const auto parsed = parse_cimport_payload_(ss.external_payload);
            if (type_is_c_abi || parsed.is_c_abi) {
                f.abi = FuncAbi::kC;
                f.c_callconv = type_is_c_abi
                    ? lower_ty_callconv_(types.fn_callconv(ss.declared_type))
                    : parsed.callconv;
                f.is_c_variadic = type_is_c_abi
                    ? types.fn_is_c_variadic(ss.declared_type)
                    : parsed.is_variadic;
            }
            f.param_begin = static_cast<uint32_t>(m.params.size());
            f.param_count = 0;
            f.positional_param_count = types.fn_positional_count(ss.declared_type);
            f.has_named_group = (sig.param_count > f.positional_param_count);
            for (uint32_t pi = 0; pi < sig.param_count; ++pi) {
                Param p{};
                p.name = types.fn_param_label_at(ss.declared_type, pi);
                p.type = types.fn_param_at(ss.declared_type, pi);
                p.is_mut = false;
                p.has_default = types.fn_param_has_default_at(ss.declared_type, pi);
                p.default_value = k_invalid_value;
                p.is_named_group = (pi >= f.positional_param_count);
                p.sym = k_invalid_symbol;
                p.span = ss.decl_span;
                (void)m.add_param(p);
                f.param_count++;
            }

            (void)m.add_func(f);
        }

        std::unordered_set<SymbolId> lowered_global_symbols{};
        lowered_global_symbols.reserve(m.globals.size());
        std::unordered_set<std::string> lowered_global_decl_keys{};
        lowered_global_decl_keys.reserve(m.globals.size());
        auto make_global_decl_key = [](std::string_view name, ty::TypeId ty, CThreadLocalKind tls_kind) {
            std::string key{};
            key.reserve(name.size() + 32u);
            key.append(name);
            key.push_back('#');
            key.append(std::to_string(static_cast<uint64_t>(ty)));
            key.push_back('#');
            key.append(std::to_string(static_cast<uint32_t>(tls_kind)));
            return key;
        };
        for (const auto& g : m.globals) {
            if (g.sym != k_invalid_symbol) lowered_global_symbols.insert(g.sym);
            lowered_global_decl_keys.insert(make_global_decl_key(g.name, g.declared_type, g.c_tls_kind));
        }

        for (uint32_t sid = 0; sid < static_cast<uint32_t>(symbols.size()); ++sid) {
            const auto& ss = symbols[sid];
            if (!ss.is_external) continue;
            if (ss.kind != sema::SymbolKind::kVar) continue;
            if (!lowered_global_symbols.insert(sid).second) continue;
            if (ss.declared_type == k_invalid_type) continue;

            const auto parsed = parse_cimport_global_payload_(ss.external_payload);
            if (!parsed.is_c_import_global) continue;

            const std::string gname = ss.link_name.empty() ? ss.name : ss.link_name;
            if (!lowered_global_decl_keys.insert(
                    make_global_decl_key(gname, ss.declared_type, parsed.tls_kind)).second) {
                continue;
            }

            GlobalVarDecl g{};
            g.span = ss.decl_span;
            g.name = gname;
            g.sym = sid;
            g.is_set = false;
            g.is_mut = !parsed.is_const;
            g.is_static = false;
            g.is_const = parsed.is_const;
            g.is_export = ss.is_export;
            g.is_extern = true;
            g.abi = FuncAbi::kC;
            g.c_tls_kind = parsed.tls_kind;
            g.declared_type = ss.declared_type;
            g.init = k_invalid_value;

            (void)m.add_global(g);
        }

        if (!tyck.actor_type_ids.empty()) {
            m.actor_types = tyck.actor_type_ids;
        }

        return m;
    }

} // namespace parus::sir
