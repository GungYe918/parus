// tools/gaupelc/src/dump/Dump.cpp
#include "Dump.hpp"

#include <gaupel/syntax/TokenKind.hpp>

#include <iostream>
#include <queue>
#include <type_traits>


namespace gaupelc::dump {

    /// @brief AST cast kind를 문자열로 변환한다.
    static const char* ast_cast_kind_name(gaupel::ast::CastKind k) {
        using K = gaupel::ast::CastKind;
        switch (k) {
            case K::kAs:         return "as";
            case K::kAsOptional: return "as?";
            case K::kAsForce:    return "as!";
        }
        return "as(?)";
    }

    /// @brief SIR value kind를 문자열로 변환한다.
    static const char* sir_value_kind_name(gaupel::sir::ValueKind k) {
        using K = gaupel::sir::ValueKind;
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
    static const char* sir_stmt_kind_name(gaupel::sir::StmtKind k) {
        using K = gaupel::sir::StmtKind;
        switch (k) {
            case K::kError: return "Error";
            case K::kExprStmt: return "ExprStmt";
            case K::kVarDecl: return "VarDecl";
            case K::kIfStmt: return "IfStmt";
            case K::kWhileStmt: return "WhileStmt";
            case K::kReturn: return "Return";
            case K::kBreak: return "Break";
            case K::kContinue: return "Continue";
            case K::kSwitch: return "Switch";
        }
        return "Unknown";
    }

    /// @brief SIR place class를 문자열로 변환한다.
    static const char* sir_place_class_name(gaupel::sir::PlaceClass p) {
        using P = gaupel::sir::PlaceClass;
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
    static const char* sir_effect_class_name(gaupel::sir::EffectClass e) {
        using E = gaupel::sir::EffectClass;
        switch (e) {
            case E::kPure:     return "Pure";
            case E::kMayWrite: return "MayWrite";
            case E::kUnknown:  return "Unknown";
        }
        return "Unknown";
    }

    /// @brief OIR effect를 문자열로 변환한다.
    static const char* oir_effect_name(gaupel::oir::Effect e) {
        using E = gaupel::oir::Effect;
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
    static const char* oir_binop_name(gaupel::oir::BinOp op) {
        using O = gaupel::oir::BinOp;
        switch (op) {
            case O::Add: return "Add";
            case O::Lt: return "Lt";
            case O::NullCoalesce: return "NullCoalesce";
        }
        return "BinOp(?)";
    }

    /// @brief OIR cast kind를 문자열로 변환한다.
    static const char* oir_cast_kind_name(gaupel::oir::CastKind k) {
        using K = gaupel::oir::CastKind;
        switch (k) {
            case K::As:  return "as";
            case K::AsQ: return "as?";
            case K::AsB: return "as!";
        }
        return "cast(?)";
    }

    /// @brief AST stmt kind를 문자열로 변환한다.
    static const char* stmt_kind_name(gaupel::ast::StmtKind k) {
        using K = gaupel::ast::StmtKind;
        switch (k) {
            case K::kEmpty: return "Empty";
            case K::kExprStmt: return "ExprStmt";
            case K::kBlock: return "Block";
            case K::kVar: return "Var";
            case K::kIf: return "If";
            case K::kWhile: return "While";
            case K::kUse:  return "Use";
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
    static const char* expr_kind_name(gaupel::ast::ExprKind k) {
        using K = gaupel::ast::ExprKind;
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
    static void dump_type(const gaupel::ty::TypePool& types, gaupel::ty::TypeId ty) {
        std::cout << types.to_string(ty) << " <id " << static_cast<uint32_t>(ty) << ">";
    }

    void dump_tokens(const std::vector<gaupel::Token>& tokens) {
        std::cout << "TOKENS:\n";
        for (const auto& t : tokens) {
            std::cout << "  " << gaupel::syntax::token_kind_name(t.kind)
                    << " '" << t.lexeme << "'"
                    << " [" << t.span.lo << "," << t.span.hi << ")\n";
        }
    }

    static void collect_sir_blocks_from_value_(
        const gaupel::sir::Module& m,
        gaupel::sir::ValueId root,
        std::vector<uint8_t>& seen_values,
        std::vector<uint8_t>& queued_blocks,
        std::queue<gaupel::sir::BlockId>& q
    ) {
        using namespace gaupel::sir;
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
        const gaupel::sir::Module& m,
        const gaupel::sir::Stmt& s,
        std::vector<uint8_t>& seen_values,
        std::vector<uint8_t>& queued_blocks,
        std::queue<gaupel::sir::BlockId>& q
    ) {
        using namespace gaupel::sir;
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

            case StmtKind::kReturn:
            case StmtKind::kBreak:
                collect_sir_blocks_from_value_(m, s.expr, seen_values, queued_blocks, q);
                break;

            default:
                break;
        }
    }

    static std::vector<gaupel::sir::BlockId> collect_reachable_sir_blocks_(
        const gaupel::sir::Module& m,
        gaupel::sir::BlockId entry
    ) {
        using namespace gaupel::sir;
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
        const gaupel::sir::Module& m,
        const gaupel::ty::TypePool& types,
        uint32_t sid,
        const gaupel::sir::Stmt& s
    ) {
        using namespace gaupel::sir;
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

    void dump_sir_module(const gaupel::sir::Module& m, const gaupel::ty::TypePool& types) {
        using namespace gaupel;

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
                std::cout << " loop_body_block=" << (gaupel::sir::BlockId)v.b;
            }
            if (v.kind == sir::ValueKind::kBlockExpr) {
                std::cout << " block_id=" << (gaupel::sir::BlockId)v.a;
            }

            if (v.kind == sir::ValueKind::kCast) {
                auto ck = (gaupel::ast::CastKind)v.op;
                std::cout << " cast_kind=" << ast_cast_kind_name(ck)
                        << " cast_to=" << types.to_string(v.cast_to) << " <id " << (uint32_t)v.cast_to << ">";
            }

            std::cout << "\n";
        }
    }

    void dump_oir_module(const gaupel::oir::Module& m, const gaupel::ty::TypePool& types) {
        using namespace gaupel;

        std::cout << "\nOIR:\n";
        std::cout << "  funcs=" << m.funcs.size()
                << " blocks=" << m.blocks.size()
                << " insts=" << m.insts.size()
                << " values=" << m.values.size()
                << "\n";

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
                        } else if constexpr (std::is_same_v<T, oir::InstBinOp>) {
                            std::cout << "BinOp " << oir_binop_name(x.op)
                                    << " v" << x.lhs << ", v" << x.rhs;
                        } else if constexpr (std::is_same_v<T, oir::InstCast>) {
                            std::cout << "Cast " << oir_cast_kind_name(x.kind)
                                    << " to=" << types.to_string((ty::TypeId)x.to) << " <id " << x.to << ">"
                                    << " v" << x.src;
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

    static void dump_fn_decl(const gaupel::ast::AstArena& ast, const gaupel::ty::TypePool& types, const gaupel::ast::Stmt& s, int indent) {
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

    void dump_expr(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId id, int indent) {
        const auto& e = ast.expr(id);
        for (int i = 0; i < indent; ++i) std::cout << "  ";

        std::cout << expr_kind_name(e.kind);

        if (e.op != gaupel::syntax::TokenKind::kError) {
            std::cout << " op=" << gaupel::syntax::token_kind_name(e.op);
        }
        if (e.kind == gaupel::ast::ExprKind::kUnary &&
            e.op == gaupel::syntax::TokenKind::kAmp &&
            e.unary_is_mut) {
            std::cout << " unary_mut=true";
        }
        if (!e.text.empty()) {
            std::cout << " text=" << e.text;
        }

        if (e.target_type != gaupel::ast::k_invalid_type) {
            std::cout << " target_ty=<id " << (uint32_t)e.target_type << ">";
        }

        if (e.kind == gaupel::ast::ExprKind::kCast) {
            std::cout << " cast_to=<id " << (uint32_t)e.cast_type << ">"
                    << " cast_kind=" << (int)e.cast_kind;
        }

        std::cout << " span=[" << e.span.lo << "," << e.span.hi << ")\n";

        switch (e.kind) {
            case gaupel::ast::ExprKind::kUnary:
            case gaupel::ast::ExprKind::kPostfixUnary:
                dump_expr(ast, e.a, indent + 1);
                break;

            case gaupel::ast::ExprKind::kBinary:
            case gaupel::ast::ExprKind::kAssign:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                break;

            case gaupel::ast::ExprKind::kTernary:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                dump_expr(ast, e.c, indent + 1);
                break;

            case gaupel::ast::ExprKind::kCall: {
                dump_expr(ast, e.a, indent + 1);

                const auto& args = ast.args();
                const auto& ngs  = ast.named_group_args();

                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];

                    for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                    std::cout << "Arg ";

                    if (a.kind == gaupel::ast::ArgKind::kNamedGroup) {
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
                            if (entry.expr == gaupel::ast::k_invalid_expr) {
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
                        if (a.expr == gaupel::ast::k_invalid_expr) {
                            for (int j = 0; j < indent + 2; ++j) std::cout << "  ";
                            std::cout << "<invalid-expr>\n";
                        } else {
                            dump_expr(ast, a.expr, indent + 2);
                        }
                    }
                }
                break;
            }

            case gaupel::ast::ExprKind::kArrayLit: {
                const auto& args = ast.args();
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];
                    for (int j = 0; j < indent + 1; ++j) std::cout << "  ";
                    std::cout << "Elem[" << i << "]";
                    if (a.is_hole || a.expr == gaupel::ast::k_invalid_expr) {
                        std::cout << " _\n";
                        continue;
                    }
                    std::cout << "\n";
                    dump_expr(ast, a.expr, indent + 2);
                }
                break;
            }

            case gaupel::ast::ExprKind::kIndex:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                break;

            case gaupel::ast::ExprKind::kIfExpr:
                dump_expr(ast, e.a, indent + 1);
                dump_expr(ast, e.b, indent + 1);
                dump_expr(ast, e.c, indent + 1);
                break;

            case gaupel::ast::ExprKind::kCast:
                dump_expr(ast, e.a, indent + 1);
                break;

            default:
                break;
        }
    }

    void dump_stmt(const gaupel::ast::AstArena& ast, const gaupel::ty::TypePool& types, gaupel::ast::StmtId id, int indent) {
        const auto& s = ast.stmt(id);
        for (int i = 0; i < indent; ++i) std::cout << "  ";

        std::cout << stmt_kind_name(s.kind)
                << " span=[" << s.span.lo << "," << s.span.hi << ")";

        if (s.kind == gaupel::ast::StmtKind::kVar) {
            std::cout << " kw=" << (s.is_set ? "set" : "let");
            std::cout << " mut=" << (s.is_mut ? "true" : "false");
            std::cout << " name=" << s.name;

            if (s.type != gaupel::ast::k_invalid_type) {
                std::cout << " type=";
                dump_type(types, s.type);
            }
        }
        std::cout << "\n";

        switch (s.kind) {
            case gaupel::ast::StmtKind::kExprStmt:
                dump_expr(ast, s.expr, indent + 1);
                break;

            case gaupel::ast::StmtKind::kVar:
                if (s.init != gaupel::ast::k_invalid_expr) {
                    for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                    std::cout << "Init:\n";
                    dump_expr(ast, s.init, indent + 2);
                }
                break;

            case gaupel::ast::StmtKind::kIf:
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Cond:\n";
                dump_expr(ast, s.expr, indent + 2);

                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Then:\n";
                dump_stmt(ast, types, s.a, indent + 2);

                if (s.b != gaupel::ast::k_invalid_stmt) {
                    for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                    std::cout << "Else:\n";
                    dump_stmt(ast, types, s.b, indent + 2);
                }
                break;

            case gaupel::ast::StmtKind::kWhile:
                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Cond:\n";
                dump_expr(ast, s.expr, indent + 2);

                for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
                std::cout << "Body:\n";
                dump_stmt(ast, types, s.a, indent + 2);
                break;

            case gaupel::ast::StmtKind::kReturn:
                if (s.expr != gaupel::ast::k_invalid_expr) {
                    dump_expr(ast, s.expr, indent + 1);
                }
                break;

            case gaupel::ast::StmtKind::kBlock: {
                const auto& kids = ast.stmt_children();
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    dump_stmt(ast, types, kids[s.stmt_begin + i], indent + 1);
                }
                break;
            }

            case gaupel::ast::StmtKind::kFnDecl:
                dump_fn_decl(ast, types, s, indent);
                break;

            case gaupel::ast::StmtKind::kFieldDecl: {
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

            case gaupel::ast::StmtKind::kActsDecl: {
                const auto& kids = ast.stmt_children();
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    dump_stmt(ast, types, kids[s.stmt_begin + i], indent + 1);
                }
                break;
            }

            default:
                break;
        }
    }

} // namespace gaupelc::dump
