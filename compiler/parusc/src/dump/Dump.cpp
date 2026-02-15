// compiler/parusc/src/dump/Dump.cpp
#include <parusc/dump/Dump.hpp>

#include <parus/syntax/TokenKind.hpp>

#include <iostream>
#include <queue>
#include <type_traits>


namespace parusc::dump {

    /// @brief AST cast kind를 문자열로 변환한다.
    static const char* ast_cast_kind_name(parus::ast::CastKind k) {
        using K = parus::ast::CastKind;
        switch (k) {
            case K::kAs:         return "as";
            case K::kAsOptional: return "as?";
            case K::kAsForce:    return "as!";
        }
        return "as(?)";
    }

    /// @brief SIR value kind를 문자열로 변환한다.
    static const char* sir_value_kind_name(parus::sir::ValueKind k) {
        using K = parus::sir::ValueKind;
        switch (k) {
            case K::kError: return "Error";
            case K::kIntLit: return "IntLit";
            case K::kFloatLit: return "FloatLit";
            case K::kStringLit: return "StringLit";
            case K::kCharLit: return "CharLit";
            case K::kBoolLit: return "BoolLit";
            case K::kNullLit: return "NullLit";
            case K::kLocal: return "Local";
            case K::kGlobal: return "Global";
            case K::kParam: return "Param";
            case K::kArrayLit: return "ArrayLit";
            case K::kFieldInit: return "FieldInit";
            case K::kBorrow: return "Borrow";
            case K::kEscape: return "Escape";
            case K::kUnary: return "Unary";
            case K::kBinary: return "Binary";
            case K::kAssign: return "Assign";
            case K::kPostfixInc: return "PostfixInc";
            case K::kCall: return "Call";
            case K::kIndex: return "Index";
            case K::kField: return "Field";
            case K::kIfExpr: return "IfExpr";
            case K::kBlockExpr: return "BlockExpr";
            case K::kLoopExpr: return "LoopExpr";
            case K::kCast: return "Cast";
        }
        return "Unknown";
    }

    /// @brief SIR statement kind를 문자열로 변환한다.
    static const char* sir_stmt_kind_name(parus::sir::StmtKind k) {
        using K = parus::sir::StmtKind;
        switch (k) {
            case K::kError: return "Error";
            case K::kExprStmt: return "ExprStmt";
            case K::kVarDecl: return "VarDecl";
            case K::kIfStmt: return "IfStmt";
            case K::kWhileStmt: return "WhileStmt";
            case K::kDoScopeStmt: return "DoScopeStmt";
            case K::kDoWhileStmt: return "DoWhileStmt";
            case K::kReturn: return "Return";
            case K::kBreak: return "Break";
            case K::kContinue: return "Continue";
            case K::kSwitch: return "Switch";
        }
        return "Unknown";
    }

    /// @brief SIR place class를 문자열로 변환한다.
    static const char* sir_place_class_name(parus::sir::PlaceClass p) {
        using P = parus::sir::PlaceClass;
        switch (p) {
            case P::kNotPlace: return "NotPlace";
            case P::kLocal:    return "Local";
            case P::kIndex:    return "Index";
            case P::kField:    return "Field";
            case P::kDeref:    return "Deref";
        }
        return "Unknown";
    }

    /// @brief SIR effect class를 문자열로 변환한다.
    static const char* sir_effect_class_name(parus::sir::EffectClass e) {
        using E = parus::sir::EffectClass;
        switch (e) {
            case E::kPure:     return "Pure";
            case E::kMayWrite: return "MayWrite";
            case E::kUnknown:  return "Unknown";
        }
        return "Unknown";
    }

    /// @brief EscapeHandle kind를 문자열로 변환한다.
    static const char* sir_escape_kind_name(parus::sir::EscapeHandleKind k) {
        using K = parus::sir::EscapeHandleKind;
        switch (k) {
            case K::kTrivial: return "Trivial";
            case K::kStackSlot: return "StackSlot";
            case K::kCallerSlot: return "CallerSlot";
            case K::kHeapBox: return "HeapBox";
        }
        return "Unknown";
    }

    /// @brief EscapeHandle boundary를 문자열로 변환한다.
    static const char* sir_escape_boundary_name(parus::sir::EscapeBoundaryKind k) {
        using K = parus::sir::EscapeBoundaryKind;
        switch (k) {
            case K::kNone: return "None";
            case K::kReturn: return "Return";
            case K::kCallArg: return "CallArg";
            case K::kAbi: return "Abi";
            case K::kFfi: return "Ffi";
        }
        return "Unknown";
    }

    /// @brief OIR effect를 문자열로 변환한다.
    static const char* oir_effect_name(parus::oir::Effect e) {
        using E = parus::oir::Effect;
        switch (e) {
            case E::Pure: return "Pure";
            case E::MayReadMem: return "MayReadMem";
            case E::MayWriteMem: return "MayWriteMem";
            case E::MayTrap: return "MayTrap";
            case E::Call: return "Call";
        }
        return "Unknown";
    }

    /// @brief OIR binop를 문자열로 변환한다.
    static const char* oir_binop_name(parus::oir::BinOp op) {
        using O = parus::oir::BinOp;
        switch (op) {
            case O::Add: return "Add";
            case O::Sub: return "Sub";
            case O::Mul: return "Mul";
            case O::Div: return "Div";
            case O::Rem: return "Rem";
            case O::Lt: return "Lt";
            case O::Le: return "Le";
            case O::Gt: return "Gt";
            case O::Ge: return "Ge";
            case O::Eq: return "Eq";
            case O::Ne: return "Ne";
            case O::NullCoalesce: return "NullCoalesce";
        }
        return "BinOp(?)";
    }

    /// @brief OIR unary op를 문자열로 변환한다.
    static const char* oir_unop_name(parus::oir::UnOp op) {
        using O = parus::oir::UnOp;
        switch (op) {
            case O::Plus: return "Plus";
            case O::Neg: return "Neg";
            case O::Not: return "Not";
            case O::BitNot: return "BitNot";
        }
        return "UnOp(?)";
    }

    /// @brief OIR cast kind를 문자열로 변환한다.
    static const char* oir_cast_kind_name(parus::oir::CastKind k) {
        using K = parus::oir::CastKind;
        switch (k) {
            case K::As:  return "as";
            case K::AsQ: return "as?";
            case K::AsB: return "as!";
        }
        return "cast(?)";
    }

    /// @brief OIR escape-handle kind를 문자열로 변환한다.
    static const char* oir_escape_kind_name(parus::oir::EscapeHandleKind k) {
        using K = parus::oir::EscapeHandleKind;
        switch (k) {
            case K::Trivial:    return "trivial";
            case K::StackSlot:  return "stack_slot";
            case K::CallerSlot: return "caller_slot";
            case K::HeapBox:    return "heap_box";
        }
        return "escape_kind(?)";
    }

    /// @brief OIR escape boundary kind를 문자열로 변환한다.
    static const char* oir_escape_boundary_name(parus::oir::EscapeBoundaryKind k) {
        using K = parus::oir::EscapeBoundaryKind;
        switch (k) {
            case K::None:    return "none";
            case K::Return:  return "return";
            case K::CallArg: return "call_arg";
            case K::Abi:     return "abi";
            case K::Ffi:     return "ffi";
        }
        return "escape_boundary(?)";
    }

    /// @brief AST stmt kind를 문자열로 변환한다.
    static const char* stmt_kind_name(parus::ast::StmtKind k) {
        using K = parus::ast::StmtKind;
        switch (k) {
            case K::kEmpty: return "Empty";
            case K::kExprStmt: return "ExprStmt";
            case K::kBlock: return "Block";
            case K::kVar: return "Var";
            case K::kIf: return "If";
            case K::kWhile: return "While";
            case K::kDoScope: return "DoScope";
            case K::kDoWhile: return "DoWhile";
            case K::kUse:  return "Use";
            case K::kNestDecl: return "NestDecl";
            case K::kReturn: return "Return";
            case K::kBreak: return "Break";
            case K::kContinue: return "Continue";
            case K::kFnDecl: return "FnDecl";
            case K::kFieldDecl: return "FieldDecl";
            case K::kActsDecl: return "ActsDecl";
            case K::kSwitch: return "Switch";
            case K::kError: return "Error";
        }
        return "Unknown";
    }

    /// @brief AST expr kind를 문자열로 변환한다.
    static const char* expr_kind_name(parus::ast::ExprKind k) {
        using K = parus::ast::ExprKind;
        switch (k) {
            case K::kIntLit: return "IntLit";
            case K::kFloatLit: return "FloatLit";
            case K::kStringLit: return "StringLit";
            case K::kCharLit: return "CharLit";
            case K::kBoolLit: return "BoolLit";
            case K::kNullLit: return "NullLit";
            case K::kArrayLit: return "ArrayLit";
            case K::kIdent: return "Ident";
            case K::kHole: return "Hole";
            case K::kUnary: return "Unary";
            case K::kPostfixUnary: return "PostfixUnary";
            case K::kBinary: return "Binary";
            case K::kTernary: return "Ternary";
            case K::kCall: return "Call";
            case K::kIndex: return "Index";
            case K::kError: return "Error";
            case K::kAssign: return "Assign";
            case K::kIfExpr: return "If";
            case K::kLoop: return "Loop";
            case K::kBlockExpr: return "Block";
            case K::kCast: return "Cast";
        }

        return "Unknown";
    }

    /// @brief type id를 사람이 읽기 쉬운 형태로 출력한다.
    static void dump_type(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
        std::cout << types.to_string(ty) << " <id " << static_cast<uint32_t>(ty) << ">";
    }

    void dump_tokens(const std::vector<parus::Token>& tokens) {
        std::cout << "TOKENS:\n";
        for (const auto& t : tokens) {
            std::cout << "  " << parus::syntax::token_kind_name(t.kind)
                    << " '" << t.lexeme << "'"
                    << " [" << t.span.lo << "," << t.span.hi << ")\n";
        }
    }

    static void collect_sir_blocks_from_value_(
        const parus::sir::Module& m,
        parus::sir::ValueId root,
        std::vector<uint8_t>& seen_values,
        std::vector<uint8_t>& queued_blocks,
        std::queue<parus::sir::BlockId>& q
    ) {
        using namespace parus::sir;
        if (root == k_invalid_value || (size_t)root >= m.values.size()) return;
        if (seen_values[root]) return;
        seen_values[root] = 1;

        auto push_block = [&](BlockId bid) {
            if (bid == k_invalid_block || (size_t)bid >= m.blocks.size()) return;
            if (queued_blocks[bid]) return;
            queued_blocks[bid] = 1;
            q.push(bid);
        };
        auto push_value = [&](ValueId vid) {
            collect_sir_blocks_from_value_(m, vid, seen_values, queued_blocks, q);
        };

        const auto& v = m.values[root];
        switch (v.kind) {
            case ValueKind::kUnary:
            case ValueKind::kBorrow:
            case ValueKind::kEscape:
            case ValueKind::kPostfixInc:
            case ValueKind::kCast:
                push_value(v.a);
                break;

            case ValueKind::kBinary:
            case ValueKind::kAssign:
            case ValueKind::kIndex:
                push_value(v.a);
                push_value(v.b);
                break;

            case ValueKind::kIfExpr:
                push_value(v.a);
                push_value(v.b);
                push_value(v.c);
                break;

            case ValueKind::kLoopExpr:
                push_value(v.a);
                push_block((BlockId)v.b);
                break;

            case ValueKind::kBlockExpr:
                push_block((BlockId)v.a);
                push_value(v.b);
                break;

            case ValueKind::kCall:
                push_value(v.a);
                if ((uint64_t)v.arg_begin + (uint64_t)v.arg_count <= (uint64_t)m.args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = m.args[v.arg_begin + i];
                        if (a.kind == ArgKind::kNamedGroup) {
                            if ((uint64_t)a.child_begin + (uint64_t)a.child_count <= (uint64_t)m.args.size()) {
                                for (uint32_t j = 0; j < a.child_count; ++j) {
                                    push_value(m.args[a.child_begin + j].value);
                                }
                            }
                        } else {
                            push_value(a.value);
                        }
                    }
                }
                break;

            case ValueKind::kArrayLit:
                if ((uint64_t)v.arg_begin + (uint64_t)v.arg_count <= (uint64_t)m.args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        push_value(m.args[v.arg_begin + i].value);
                    }
                }
                break;

            default:
                break;
        }
    }

    static void collect_sir_blocks_from_stmt_(
        const parus::sir::Module& m,
        const parus::sir::Stmt& s,
        std::vector<uint8_t>& seen_values,
        std::vector<uint8_t>& queued_blocks,
        std::queue<parus::sir::BlockId>& q
    ) {
        using namespace parus::sir;
        auto push_block = [&](BlockId bid) {
            if (bid == k_invalid_block || (size_t)bid >= m.blocks.size()) return;
            if (queued_blocks[bid]) return;
            queued_blocks[bid] = 1;
            q.push(bid);
        };

        switch (s.kind) {
            case StmtKind::kExprStmt:
                collect_sir_blocks_from_value_(m, s.expr, seen_values, queued_blocks, q);
                break;

            case StmtKind::kVarDecl:
                collect_sir_blocks_from_value_(m, s.init, seen_values, queued_blocks, q);
                break;

            case StmtKind::kIfStmt:
                collect_sir_blocks_from_value_(m, s.expr, seen_values, queued_blocks, q);
                push_block(s.a);
                push_block(s.b);
                break;

            case StmtKind::kWhileStmt:
                collect_sir_blocks_from_value_(m, s.expr, seen_values, queued_blocks, q);
                push_block(s.a);
                break;
            case StmtKind::kDoScopeStmt:
                push_block(s.a);
                break;
            case StmtKind::kDoWhileStmt:
                push_block(s.a);
                collect_sir_blocks_from_value_(m, s.expr, seen_values, queued_blocks, q);
                break;

            case StmtKind::kReturn:
            case StmtKind::kBreak:
                collect_sir_blocks_from_value_(m, s.expr, seen_values, queued_blocks, q);
                break;

            default:
                break;
        }
    }

    static std::vector<parus::sir::BlockId> collect_reachable_sir_blocks_(
        const parus::sir::Module& m,
        parus::sir::BlockId entry
    ) {
        using namespace parus::sir;
        std::vector<BlockId> out;
        if (entry == k_invalid_block || (size_t)entry >= m.blocks.size()) return out;

        std::vector<uint8_t> seen_blocks(m.blocks.size(), 0);
        std::vector<uint8_t> queued_blocks(m.blocks.size(), 0);
        std::vector<uint8_t> seen_values(m.values.size(), 0);
        std::queue<BlockId> q;

        q.push(entry);
        queued_blocks[entry] = 1;

        while (!q.empty()) {
            const BlockId bid = q.front();
            q.pop();
            if (seen_blocks[bid]) continue;
            seen_blocks[bid] = 1;
            out.push_back(bid);

            const auto& b = m.blocks[bid];
            for (uint32_t i = 0; i < b.stmt_count; ++i) {
                const uint32_t sid = b.stmt_begin + i;
                if ((size_t)sid >= m.stmts.size()) break;
                collect_sir_blocks_from_stmt_(m, m.stmts[sid], seen_values, queued_blocks, q);
            }
        }

        return out;
    }

    static void dump_one_sir_stmt_(
        const parus::sir::Module& m,
        const parus::ty::TypePool& types,
        uint32_t sid,
        const parus::sir::Stmt& s
    ) {
        using namespace parus::sir;
        std::cout << "      stmt #" << sid
                << " " << sir_stmt_kind_name(s.kind);

        if (s.kind == StmtKind::kVarDecl) {
            std::cout << " name=" << s.name
                    << " sym=" << s.sym
                    << " mut=" << (s.is_mut ? "true" : "false")
                    << " static=" << (s.is_static ? "true" : "false")
                    << " set=" << (s.is_set ? "true" : "false")
                    << " decl_ty=" << types.to_string(s.declared_type) << " <id " << (uint32_t)s.declared_type << ">"
                    << " init=" << s.init;
        } else {
            if (s.expr != k_invalid_value) std::cout << " expr=" << s.expr;
            if (s.a != k_invalid_block) std::cout << " a=" << s.a;
            if (s.b != k_invalid_block) std::cout << " b=" << s.b;
        }

        std::cout << " span=[" << s.span.lo << "," << s.span.hi << ")";
        std::cout << "\n";

        (void)m;
    }

    void dump_sir_module(const parus::sir::Module& m, const parus::ty::TypePool& types) {
        using namespace parus;

        std::cout << "\nSIR:\n";
        std::cout << "  funcs=" << m.funcs.size()
                << " blocks=" << m.blocks.size()
                << " stmts=" << m.stmts.size()
                << " values=" << m.values.size()
                << " args=" << m.args.size()
                << " params=" << m.params.size()
                << " attrs=" << m.attrs.size()
                << " fields=" << m.fields.size()
                << " field_members=" << m.field_members.size()
                << " acts=" << m.acts.size()
                << " escape_handles=" << m.escape_handles.size()
                << "\n";

        if (!m.fields.empty()) {
            std::cout << "\n  fields:\n";
            for (size_t fi = 0; fi < m.fields.size(); ++fi) {
                const auto& f = m.fields[fi];
                std::cout << "    field #" << fi
                        << " name=" << f.name
                        << " sym=" << f.sym
                        << " export=" << (f.is_export ? "true" : "false")
                        << " members=" << f.member_count
                        << "\n";

                for (uint32_t i = 0; i < f.member_count; ++i) {
                    const uint32_t mid = f.member_begin + i;
                    if ((size_t)mid >= m.field_members.size()) break;
                    const auto& mem = m.field_members[mid];
                    std::cout << "      member#" << mid
                            << " " << mem.name
                            << ": " << types.to_string(mem.type) << " <id " << (uint32_t)mem.type << ">"
                            << "\n";
                }
            }
        }

        if (!m.acts.empty()) {
            std::cout << "\n  acts:\n";
            for (size_t ai = 0; ai < m.acts.size(); ++ai) {
                const auto& a = m.acts[ai];
                std::cout << "    acts #" << ai
                        << " name=" << a.name
                        << " sym=" << a.sym
                        << " export=" << (a.is_export ? "true" : "false")
                        << " funcs=" << a.func_count
                        << "\n";
            }
        }

        for (size_t fi = 0; fi < m.funcs.size(); ++fi) {
            const auto& f = m.funcs[fi];

            std::cout << "\n  fn #" << fi
                    << " name=" << f.name
                    << " sym=" << f.sym
                    << " entry=" << f.entry
                    << " has_any_write=" << (f.has_any_write ? "true" : "false")
                    << " acts_member=" << (f.is_acts_member ? "true" : "false")
                    << " owner_acts=" << f.owner_acts
                    << "\n";

            std::cout << "    sig=" << types.to_string(f.sig) << " <id " << (uint32_t)f.sig << ">\n";
            std::cout << "    ret=" << types.to_string(f.ret) << " <id " << (uint32_t)f.ret << ">\n";

            std::cout << "    attrs (" << f.attr_count << "):\n";
            for (uint32_t i = 0; i < f.attr_count; ++i) {
                const uint32_t aid = f.attr_begin + i;
                if ((size_t)aid >= m.attrs.size()) break;
                std::cout << "      @" << m.attrs[aid].name << " (aid=" << aid << ")\n";
            }

            std::cout << "    params (" << f.param_count << "):\n";
            for (uint32_t i = 0; i < f.param_count; ++i) {
                const uint32_t pid = f.param_begin + i;
                if ((size_t)pid >= m.params.size()) break;
                const auto& p = m.params[pid];
                std::cout << "      p#" << pid
                        << " name=" << p.name
                        << " sym=" << p.sym
                        << " ty=" << types.to_string(p.type) << " <id " << (uint32_t)p.type << ">"
                        << " mut=" << (p.is_mut ? "true" : "false")
                        << " named_group=" << (p.is_named_group ? "true" : "false")
                        << " default=" << (p.has_default ? "yes" : "no");
                if (p.has_default) std::cout << " default_value=" << p.default_value;
                std::cout << "\n";
            }

            const auto reachable = collect_reachable_sir_blocks_(m, f.entry);
            std::cout << "    reachable_blocks=" << reachable.size() << "\n";
            for (auto bid : reachable) {
                if ((size_t)bid >= m.blocks.size()) continue;
                const auto& b = m.blocks[bid];
                std::cout << "    block #" << bid
                        << " stmt_begin=" << b.stmt_begin
                        << " stmt_count=" << b.stmt_count
                        << " span=[" << b.span.lo << "," << b.span.hi << ")\n";
                for (uint32_t i = 0; i < b.stmt_count; ++i) {
                    const uint32_t sid = b.stmt_begin + i;
                    if ((size_t)sid >= m.stmts.size()) break;
                    dump_one_sir_stmt_(m, types, sid, m.stmts[sid]);
                }
            }
        }

        std::cout << "\n  args:\n";
        for (size_t ai = 0; ai < m.args.size(); ++ai) {
            const auto& a = m.args[ai];
            std::cout << "    arg#" << ai
                    << " kind=" << (a.kind == sir::ArgKind::kPositional ? "positional"
                                    : a.kind == sir::ArgKind::kLabeled ? "labeled" : "named_group")
                    << " label=";
            if (a.has_label) std::cout << a.label;
            else std::cout << "<none>";
            std::cout << " hole=" << (a.is_hole ? "true" : "false")
                    << " value=" << a.value;
            if (a.kind == sir::ArgKind::kNamedGroup) {
                std::cout << " child_begin=" << a.child_begin
                        << " child_count=" << a.child_count;
            }
            std::cout << "\n";
        }

        std::cout << "\n  values:\n";
        for (size_t vi = 0; vi < m.values.size(); ++vi) {
            const auto& v = m.values[vi];

            std::cout << "    v#" << vi
                    << " " << sir_value_kind_name(v.kind)
                    << " ty=" << types.to_string(v.type) << " <id " << (uint32_t)v.type << ">"
                    << " place=" << sir_place_class_name(v.place)
                    << " effect=" << sir_effect_class_name(v.effect)
                    << " a=" << v.a << " b=" << v.b << " c=" << v.c;

            if (!v.text.empty()) std::cout << " text=" << v.text;
            if (v.sym != sir::k_invalid_symbol) std::cout << " sym=" << v.sym;
            if (v.origin_sym != sir::k_invalid_symbol) std::cout << " origin_sym=" << v.origin_sym;
            if (v.kind == sir::ValueKind::kBorrow) {
                std::cout << " borrow_mut=" << (v.borrow_is_mut ? "true" : "false");
            }

            if (v.kind == sir::ValueKind::kCall || v.kind == sir::ValueKind::kArrayLit) {
                std::cout << " arg_begin=" << v.arg_begin
                        << " arg_count=" << v.arg_count;
            }
            if (v.kind == sir::ValueKind::kLoopExpr) {
                std::cout << " loop_body_block=" << (parus::sir::BlockId)v.b;
            }
            if (v.kind == sir::ValueKind::kBlockExpr) {
                std::cout << " block_id=" << (parus::sir::BlockId)v.a;
            }

            if (v.kind == sir::ValueKind::kCast) {
                auto ck = (parus::ast::CastKind)v.op;
                std::cout << " cast_kind=" << ast_cast_kind_name(ck)
                        << " cast_to=" << types.to_string(v.cast_to) << " <id " << (uint32_t)v.cast_to << ">";
            }

            std::cout << "\n";
        }

        if (!m.escape_handles.empty()) {
            std::cout << "\n  escape_handles:\n";
            for (size_t hi = 0; hi < m.escape_handles.size(); ++hi) {
                const auto& h = m.escape_handles[hi];
                std::cout << "    h#" << hi
                        << " value=" << h.escape_value
                        << " origin_sym=" << h.origin_sym
                        << " pointee_ty=" << types.to_string(h.pointee_type) << " <id " << (uint32_t)h.pointee_type << ">"
                        << " kind=" << sir_escape_kind_name(h.kind)
                        << " boundary=" << sir_escape_boundary_name(h.boundary)
                        << " from_static=" << (h.from_static ? "true" : "false")
                        << " has_drop=" << (h.has_drop ? "true" : "false")
                        << " abi_pack=" << (h.abi_pack_required ? "true" : "false")
                        << " ffi_pack=" << (h.ffi_pack_required ? "true" : "false")
                        << " materialize_count=" << h.materialize_count
                        << " span=[" << h.span.lo << "," << h.span.hi << ")"
                        << "\n";
            }
        }
    }

    void dump_oir_module(const parus::oir::Module& m, const parus::ty::TypePool& types) {
        using namespace parus;

        std::cout << "\nOIR:\n";
        std::cout << "  funcs=" << m.funcs.size()
                << " blocks=" << m.blocks.size()
                << " insts=" << m.insts.size()
                << " values=" << m.values.size()
                << "\n";
        std::cout << "  opt_stats:"
                << " critical_edges_split=" << m.opt_stats.critical_edges_split
                << " loop_canonicalized=" << m.opt_stats.loop_canonicalized
                << " mem2reg_promoted_slots=" << m.opt_stats.mem2reg_promoted_slots
                << " mem2reg_phi_params=" << m.opt_stats.mem2reg_phi_params
                << " gvn_cse_eliminated=" << m.opt_stats.gvn_cse_eliminated
                << " licm_hoisted=" << m.opt_stats.licm_hoisted
                << " escape_pack_elided=" << m.opt_stats.escape_pack_elided
                << " escape_boundary_rewrites=" << m.opt_stats.escape_boundary_rewrites
                << "\n";
        std::cout << "  escape_hints=" << m.escape_hints.size() << "\n";
        for (size_t hi = 0; hi < m.escape_hints.size(); ++hi) {
            const auto& h = m.escape_hints[hi];
            std::cout << "    eh#" << hi
                    << " value=v" << h.value
                    << " pointee_ty=" << types.to_string((ty::TypeId)h.pointee_type) << " <id " << h.pointee_type << ">"
                    << " kind=" << oir_escape_kind_name(h.kind)
                    << " boundary=" << oir_escape_boundary_name(h.boundary)
                    << " from_static=" << (h.from_static ? "true" : "false")
                    << " has_drop=" << (h.has_drop ? "true" : "false")
                    << " abi_pack=" << (h.abi_pack_required ? "true" : "false")
                    << " ffi_pack=" << (h.ffi_pack_required ? "true" : "false")
                    << "\n";
        }

        for (size_t fi = 0; fi < m.funcs.size(); ++fi) {
            const auto& f = m.funcs[fi];
            std::cout << "\n  fn #" << fi
                    << " name=" << f.name
                    << " ret=" << types.to_string((ty::TypeId)f.ret_ty) << " <id " << f.ret_ty << ">"
                    << " entry=" << f.entry
                    << " blocks=" << f.blocks.size()
                    << "\n";

            for (auto bbid : f.blocks) {
                if (bbid == oir::kInvalidId || (size_t)bbid >= m.blocks.size()) continue;
                const auto& b = m.blocks[bbid];

                std::cout << "    bb #" << bbid
                        << " params=" << b.params.size()
                        << " insts=" << b.insts.size()
                        << " term=" << (b.has_term ? "yes" : "no")
                        << "\n";

                for (size_t pi = 0; pi < b.params.size(); ++pi) {
                    auto vid = b.params[pi];
                    if ((size_t)vid >= m.values.size()) continue;
                    const auto& vv = m.values[vid];
                    std::cout << "      param v" << vid
                            << " ty=" << types.to_string((ty::TypeId)vv.ty) << " <id " << vv.ty << ">\n";
                }

                for (auto iid : b.insts) {
                    if ((size_t)iid >= m.insts.size()) continue;
                    const auto& inst = m.insts[iid];

                    std::cout << "      i" << iid
                            << " eff=" << oir_effect_name(inst.eff);

                    if (inst.result != oir::kInvalidId) {
                        if ((size_t)inst.result < m.values.size()) {
                            const auto& rv = m.values[inst.result];
                            std::cout << " -> v" << inst.result
                                    << " ty=" << types.to_string((ty::TypeId)rv.ty) << " <id " << rv.ty << ">";
                        } else {
                            std::cout << " -> v" << inst.result << " <bad-value-id>";
                        }
                    }
                    std::cout << " : ";

                    std::visit([&](auto&& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, oir::InstConstInt>) {
                            std::cout << "ConstInt \"" << x.text << "\"";
                        } else if constexpr (std::is_same_v<T, oir::InstConstBool>) {
                            std::cout << "ConstBool " << (x.value ? "true" : "false");
                        } else if constexpr (std::is_same_v<T, oir::InstConstNull>) {
                            std::cout << "ConstNull";
                        } else if constexpr (std::is_same_v<T, oir::InstUnary>) {
                            std::cout << "Unary " << oir_unop_name(x.op) << " v" << x.src;
                        } else if constexpr (std::is_same_v<T, oir::InstBinOp>) {
                            std::cout << "BinOp " << oir_binop_name(x.op)
                                    << " v" << x.lhs << ", v" << x.rhs;
                        } else if constexpr (std::is_same_v<T, oir::InstCast>) {
                            std::cout << "Cast " << oir_cast_kind_name(x.kind)
                                    << " to=" << types.to_string((ty::TypeId)x.to) << " <id " << x.to << ">"
                                    << " v" << x.src;
                        } else if constexpr (std::is_same_v<T, oir::InstFuncRef>) {
                            std::cout << "FuncRef f#" << x.func;
                            if (!x.name.empty()) {
                                std::cout << " name=" << x.name;
                            }
                        } else if constexpr (std::is_same_v<T, oir::InstCall>) {
                            std::cout << "Call callee=v" << x.callee << " args=[";
                            for (size_t ai = 0; ai < x.args.size(); ++ai) {
                                if (ai) std::cout << ", ";
                                std::cout << "v" << x.args[ai];
                            }
                            std::cout << "]";
                        } else if constexpr (std::is_same_v<T, oir::InstIndex>) {
                            std::cout << "Index base=v" << x.base << " idx=v" << x.index;
                        } else if constexpr (std::is_same_v<T, oir::InstField>) {
                            std::cout << "Field base=v" << x.base << " ." << x.field;
                        } else if constexpr (std::is_same_v<T, oir::InstAllocaLocal>) {
                            std::cout << "AllocaLocal slot_ty="
                                    << types.to_string((ty::TypeId)x.slot_ty) << " <id " << x.slot_ty << ">";
                        } else if constexpr (std::is_same_v<T, oir::InstLoad>) {
                            std::cout << "Load slot=v" << x.slot;
                        } else if constexpr (std::is_same_v<T, oir::InstStore>) {
                            std::cout << "Store slot=v" << x.slot << " val=v" << x.value;
                        } else {
                            std::cout << "<unknown inst>";
                        }
                    }, inst.data);

                    std::cout << "\n";
                }

                if (b.has_term) {
                    std::visit([&](auto&& t) {
                        using T = std::decay_t<decltype(t)>;
                        if constexpr (std::is_same_v<T, oir::TermRet>) {
                            if (!t.has_value) std::cout << "      term: ret\n";
                            else std::cout << "      term: ret v" << t.value << "\n";
                        } else if constexpr (std::is_same_v<T, oir::TermBr>) {
                            std::cout << "      term: br bb#" << t.target
                                    << " args=" << t.args.size() << "\n";
                        } else if constexpr (std::is_same_v<T, oir::TermCondBr>) {
                            std::cout << "      term: condbr v" << t.cond
                                    << " then=bb#" << t.then_bb
                                    << " else=bb#" << t.else_bb
                                    << "\n";
                        }
                    }, b.term);
                }
            }
        }
    }

    static void dump_fn_decl(const parus::ast::AstArena& ast, const parus::ty::TypePool& types, const parus::ast::Stmt& s, int indent) {
        for (int i = 0; i < indent + 1; ++i) std::cout << "  ";

        std::cout << "name=" << s.name;

        if (s.is_throwing) std::cout << " throwing=true";
        if (s.is_export)   std::cout << " export=true";
        if (s.is_pure)     std::cout << " pure=true";
        if (s.is_comptime) std::cout << " comptime=true";

        std::cout << " ret=";
        dump_type(types, s.type);
        std::cout << "\n";

        {
            const auto& attrs = ast.fn_attrs();
            for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
            std::cout << "attrs:";
            if (s.attr_count == 0) {
                std::cout << " <none>\n";
            } else {
                std::cout << "\n";
                for (uint32_t i = 0; i < s.attr_count; ++i) {
                    const auto& a = attrs[s.attr_begin + i];
                    for (int j = 0; j < indent + 2; ++j) std::cout << "  ";
                    std::cout << "- " << a.name << " span=[" << a.span.lo << "," << a.span.hi << ")\n";
                }
            }
        }

        {
            const auto& ps = ast.params();
            for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
            std::cout << "params:\n";

            for (uint32_t i = 0; i < s.param_count; ++i) {
                const auto& p = ps[s.param_begin + i];
                for (int j = 0; j < indent + 2; ++j) std::cout << "  ";
                std::cout << p.name << ": ";
                dump_type(types, p.type);

                if (p.has_default) {
                    std::cout << " = <default-expr>";
                }
                if (p.is_named_group) {
                    std::cout << " (named-group)";
                }
                std::cout << " span=[" << p.span.lo << "," << p.span.hi << ")\n";
            }
        }

        for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
        std::cout << "body:\n";
        dump_stmt(ast, types, s.a, indent + 2);
    }

    void dump_expr(const parus::ast::AstArena& ast, parus::ast::ExprId id, int indent) {
        const auto& e = ast.expr(id);
        for (int i = 0; i < indent; ++i) std::cout << "  ";

        std::cout << expr_kind_name(e.kind);

        if (e.op != parus::syntax::TokenKind::kError) {
            std::cout << " op=" << parus::syntax::token_kind_name(e.op);
        }
        if (e.kind == parus::ast::ExprKind::kUnary &&
            e.op == parus::syntax::TokenKind::kAmp &&
            e.unary_is_mut) {
            std::cout << " unary_mut=true";
        }
        if (!e.text.empty()) {
            std::cout << " text=" << e.text;
        }

        if (e.target_type != parus::ast::k_invalid_type) {
            std::cout << " target_ty=<id " << (uint32_t)e.target_type << ">";
        }

        if (e.kind == parus::ast::ExprKind::kCast) {
            std::cout << " cast_to=<id " << (uint32_t)e.cast_type << ">"
                    << " cast_kind=" << (int)e.cast_kind;
        }

        std::cout << " span=[" << e.span.lo << "," << e.span.hi << ")\n";

        switch (e.kind) {
            case parus::ast::ExprKind::kUnary:
            case parus::ast::ExprKind::kPostfixUnary:
                dump_expr(ast, e.a, indent + 1);
                break;

            case parus::ast::ExprKind::kBinary:
            case parus::ast::ExprKind::kAssign:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                break;

            case parus::ast::ExprKind::kTernary:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                dump_expr(ast, e.c, indent + 1);
                break;

            case parus::ast::ExprKind::kCall: {
                dump_expr(ast, e.a, indent + 1);

                const auto& args = ast.args();
                const auto& ngs  = ast.named_group_args();

                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];

                    for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                    std::cout << "Arg ";

                    if (a.kind == parus::ast::ArgKind::kNamedGroup) {
                        std::cout << "{\n";

                        for (uint32_t k = 0; k < a.child_count; ++k) {
                            const auto& entry = ngs[a.child_begin + k];

                            for (int j = 0; j < indent + 2; ++j) std::cout << "  ";
                            std::cout << entry.label << ": ";

                            if (entry.is_hole) {
                                std::cout << "_\n";
                                continue;
                            }

                            std::cout << "\n";
                            if (entry.expr == parus::ast::k_invalid_expr) {
                                for (int j = 0; j < indent + 3; ++j) std::cout << "  ";
                                std::cout << "<invalid-expr>\n";
                            } else {
                                dump_expr(ast, entry.expr, indent + 3);
                            }
                        }

                        for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                        std::cout << "}\n";
                        continue;
                    }

                    if (a.has_label) std::cout << a.label << ": ";

                    if (a.is_hole) {
                        std::cout << "_\n";
                    } else {
                        std::cout << "\n";
                        if (a.expr == parus::ast::k_invalid_expr) {
                            for (int j = 0; j < indent + 2; ++j) std::cout << "  ";
                            std::cout << "<invalid-expr>\n";
                        } else {
                            dump_expr(ast, a.expr, indent + 2);
                        }
                    }
                }
                break;
            }

            case parus::ast::ExprKind::kArrayLit: {
                const auto& args = ast.args();
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];
                    for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                    std::cout << "Elem[" << i << "]";
                    if (a.is_hole || a.expr == parus::ast::k_invalid_expr) {
                        std::cout << " _\n";
                        continue;
                    }
                    std::cout << "\n";
                    dump_expr(ast, a.expr, indent + 2);
                }
                break;
            }

            case parus::ast::ExprKind::kIndex:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                break;

            case parus::ast::ExprKind::kIfExpr:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                dump_expr(ast, e.c, indent + 1);
                break;

            case parus::ast::ExprKind::kCast:
                dump_expr(ast, e.a, indent + 1);
                break;

            default:
                break;
        }
    }

    void dump_stmt(const parus::ast::AstArena& ast, const parus::ty::TypePool& types, parus::ast::StmtId id, int indent) {
        const auto& s = ast.stmt(id);
        for (int i = 0; i < indent; ++i) std::cout << "  ";

        std::cout << stmt_kind_name(s.kind)
                << " span=[" << s.span.lo << "," << s.span.hi << ")";

        if (s.kind == parus::ast::StmtKind::kVar) {
            std::cout << " kw=" << (s.is_set ? "set" : "let");
            std::cout << " mut=" << (s.is_mut ? "true" : "false");
            std::cout << " name=" << s.name;

            if (s.type != parus::ast::k_invalid_type) {
                std::cout << " type=";
                dump_type(types, s.type);
            }
        }
        std::cout << "\n";

        switch (s.kind) {
            case parus::ast::StmtKind::kExprStmt:
                dump_expr(ast, s.expr, indent + 1);
                break;

            case parus::ast::StmtKind::kVar:
                if (s.init != parus::ast::k_invalid_expr) {
                    for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                    std::cout << "Init:\n";
                    dump_expr(ast, s.init, indent + 2);
                }
                break;

            case parus::ast::StmtKind::kIf:
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Cond:\n";
                dump_expr(ast, s.expr, indent + 2);

                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Then:\n";
                dump_stmt(ast, types, s.a, indent + 2);

                if (s.b != parus::ast::k_invalid_stmt) {
                    for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                    std::cout << "Else:\n";
                    dump_stmt(ast, types, s.b, indent + 2);
                }
                break;

            case parus::ast::StmtKind::kWhile:
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Cond:\n";
                dump_expr(ast, s.expr, indent + 2);

                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Body:\n";
                dump_stmt(ast, types, s.a, indent + 2);
                break;

            case parus::ast::StmtKind::kDoScope:
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "DoBody:\n";
                dump_stmt(ast, types, s.a, indent + 2);
                break;

            case parus::ast::StmtKind::kDoWhile:
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "DoBody:\n";
                dump_stmt(ast, types, s.a, indent + 2);
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Cond:\n";
                dump_expr(ast, s.expr, indent + 2);
                break;

            case parus::ast::StmtKind::kReturn:
                if (s.expr != parus::ast::k_invalid_expr) {
                    dump_expr(ast, s.expr, indent + 1);
                }
                break;

            case parus::ast::StmtKind::kBlock: {
                const auto& kids = ast.stmt_children();
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    dump_stmt(ast, types, kids[s.stmt_begin + i], indent + 1);
                }
                break;
            }

            case parus::ast::StmtKind::kFnDecl:
                dump_fn_decl(ast, types, s, indent);
                break;

            case parus::ast::StmtKind::kFieldDecl: {
                const uint32_t begin = s.field_member_begin;
                const uint32_t end = s.field_member_begin + s.field_member_count;
                if (begin < ast.field_members().size() && end <= ast.field_members().size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto& m = ast.field_members()[i];
                        for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                        std::cout << "member " << m.name << ": ";
                        dump_type(types, m.type);
                        std::cout << " span=[" << m.span.lo << "," << m.span.hi << ")\n";
                    }
                }
                break;
            }

            case parus::ast::StmtKind::kActsDecl: {
                const auto& kids = ast.stmt_children();
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    dump_stmt(ast, types, kids[s.stmt_begin + i], indent + 1);
                }
                break;
            }

            case parus::ast::StmtKind::kUse:
                if (s.expr != parus::ast::k_invalid_expr) {
                    dump_expr(ast, s.expr, indent + 1);
                }
                break;

            case parus::ast::StmtKind::kNestDecl:
                if (!s.nest_is_file_directive && s.a != parus::ast::k_invalid_stmt) {
                    dump_stmt(ast, types, s.a, indent + 1);
                }
                break;

            default:
                break;
        }
    }

} // namespace parusc::dump
