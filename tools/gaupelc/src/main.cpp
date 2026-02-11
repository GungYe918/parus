#include <iostream>
#include <string_view>
#include <vector>
#include <optional>

#include "gaupel/Version.hpp"
#include "gaupel/lex/Lexer.hpp"
#include "gaupel/parse/Parser.hpp"
#include "gaupel/ast/Nodes.hpp"
#include "gaupel/syntax/TokenKind.hpp"
#include "gaupel/ty/TypePool.hpp"

#include "gaupel/diag/Diagnostic.hpp"
#include "gaupel/diag/Render.hpp"
#include "gaupel/diag/DiagCode.hpp"
#include "gaupel/text/SourceManager.hpp"

#include "gaupel/passes/Passes.hpp"
#include "gaupel/tyck/TypeCheck.hpp"

// SIR
#include "gaupel/sir/SIR.hpp"
#include "gaupel/sir/Builder.hpp"
#include "gaupel/sir/MutAnalysis.hpp"

// OIR
#include "gaupel/oir/OIR.hpp"
#include "gaupel/oir/Builder.hpp"
#include "gaupel/oir/Verify.hpp"
#include "gaupel/oir/Inst.hpp"

#include <gaupel/os/File.hpp>


/// @brief gaupelc 사용법 출력
static void print_usage() {
    std::cout
        << "gaupelc\n"
        << "  --version\n"
        << "  --expr \"<expr>\" [--lang en|ko] [--context N]\n"
        << "  --stmt \"<stmt>\" [--lang en|ko] [--context N]\n"
        << "  --all  \"<program>\" [--lang en|ko] [--context N] [--dump oir]\n"
        << "  --file <path> [--lang en|ko] [--context N] [--dump oir]\n"
        << "\n"
        << "Options:\n"
        << "  -fmax-errors=N\n"
        << "  -Wshadow            (emit warning on shadowing)\n"
        << "  -Werror=shadow      (treat shadowing as error)\n"
        << "  --dump oir          (dump OIR after SIR build)\n";
}

static gaupel::diag::Language parse_lang(const std::vector<std::string_view>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--lang") {
            if (args[i + 1] == "ko") return gaupel::diag::Language::kKo;
            return gaupel::diag::Language::kEn;
        }
    }
    return gaupel::diag::Language::kEn;
}

static uint32_t parse_max_errors(const std::vector<std::string_view>& args) {
    // default: 64
    uint32_t v = 64;

    for (auto a : args) {
        // 형태: -fmax-errors=128
        std::string_view key = "-fmax-errors=";
        if (a.size() >= key.size() && a.substr(0, key.size()) == key) {
            auto tail = a.substr(key.size());
            try {
                int n = std::stoi(std::string(tail));
                if (n < 1) n = 1;
                v = static_cast<uint32_t>(n);
            } catch (...) {
                // ignore -> keep default
            }
        }
    }
    return v;
}

/// @brief 진단 컨텍스트 줄 수를 파싱한다.
static uint32_t parse_context(const std::vector<std::string_view>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--context") {
            try {
                int v = std::stoi(std::string(args[i + 1]));
                if (v < 0) v = 0;
                return static_cast<uint32_t>(v);
            } catch (...) {
                return 2;
            }
        }
    }
    return 2; // 기본: 위/아래 2줄
}

static gaupel::passes::ShadowingMode parse_shadowing_mode(const std::vector<std::string_view>& args) {
    using M = gaupel::passes::ShadowingMode;

    bool warn = false;
    bool err  = false;

    for (auto a : args) {
        if (a == "-Wshadow") warn = true;
        if (a == "-Werror=shadow") err = true;
    }

    if (err)  return M::kError;
    if (warn) return M::kWarn;
    return M::kAllow;
}

/// @brief 토큰 목록 출력
static void dump_tokens(const std::vector<gaupel::Token>& tokens) {
    std::cout << "TOKENS:\n";
    for (const auto& t : tokens) {
        std::cout << "  " << gaupel::syntax::token_kind_name(t.kind)
                  << " '" << t.lexeme << "'"
                  << " [" << t.span.lo << "," << t.span.hi << ")\n";
    }
}

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

static const char* sir_effect_class_name(gaupel::sir::EffectClass e) {
    using E = gaupel::sir::EffectClass;
    switch (e) {
        case E::kPure:     return "Pure";
        case E::kMayWrite: return "MayWrite";
        case E::kUnknown:  return "Unknown";
    }
    return "Unknown";
}

static const char* ast_cast_kind_name(gaupel::ast::CastKind k) {
    using K = gaupel::ast::CastKind;
    switch (k) {
        case K::kAs:         return "as";
        case K::kAsOptional: return "as?";
        case K::kAsForce:    return "as!";
    }
    return "as(?)";
}

static void dump_sir_module(const gaupel::sir::Module& m, const gaupel::ty::TypePool& types) {
    using namespace gaupel;

    std::cout << "\nSIR:\n";
    std::cout << "  funcs=" << m.funcs.size()
              << " blocks=" << m.blocks.size()
              << " stmts=" << m.stmts.size()
              << " values=" << m.values.size()
              << " args=" << m.args.size()
              << "\n";

    // funcs
    for (size_t fi = 0; fi < m.funcs.size(); ++fi) {
        const auto& f = m.funcs[fi];

        std::cout << "\n  fn #" << fi
                  << " name=" << f.name
                  << " sym=" << f.sym
                  << " entry=" << f.entry
                  << " has_any_write=" << (f.has_any_write ? "true" : "false")
                  << "\n";

        std::cout << "    sig=" << types.to_string(f.sig) << " <id " << (uint32_t)f.sig << ">\n";
        std::cout << "    ret=" << types.to_string(f.ret) << " <id " << (uint32_t)f.ret << ">\n";

        // blocks + statements (v0: entry block only)
        if (f.entry != sir::k_invalid_block && (size_t)f.entry < m.blocks.size()) {
            const auto& b = m.blocks[f.entry];
            std::cout << "    block #" << f.entry
                      << " stmt_begin=" << b.stmt_begin
                      << " stmt_count=" << b.stmt_count
                      << "\n";

            for (uint32_t i = 0; i < b.stmt_count; ++i) {
                const uint32_t sid = b.stmt_begin + i;
                if ((size_t)sid >= m.stmts.size()) break;

                const auto& s = m.stmts[sid];
                std::cout << "      stmt #" << sid
                          << " " << sir_stmt_kind_name(s.kind);

                if (s.kind == sir::StmtKind::kVarDecl) {
                    std::cout << " name=" << s.name
                              << " sym=" << s.sym
                              << " mut=" << (s.is_mut ? "true" : "false")
                              << " set=" << (s.is_set ? "true" : "false")
                              << " decl_ty=" << types.to_string(s.declared_type) << " <id " << (uint32_t)s.declared_type << ">"
                              << " init=" << s.init;
                } else {
                    if (s.expr != sir::k_invalid_value) std::cout << " expr=" << s.expr;
                    if (s.a != sir::k_invalid_block) std::cout << " a=" << s.a;
                    if (s.b != sir::k_invalid_block) std::cout << " b=" << s.b;
                }
                std::cout << "\n";
            }
        }
    }

    // values
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

        if (v.kind == sir::ValueKind::kCall) {
            std::cout << " arg_begin=" << v.arg_begin
                      << " arg_count=" << v.arg_count;
        }

        if (v.kind == sir::ValueKind::kCast) {
            auto ck = (gaupel::ast::CastKind)v.op;
            std::cout << " cast_kind=" << ast_cast_kind_name(ck)
                      << " cast_to=" << types.to_string(v.cast_to) << " <id " << (uint32_t)v.cast_to << ">";
        }

        std::cout << "\n";
    }
}

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

static const char* oir_binop_name(gaupel::oir::BinOp op) {
    using O = gaupel::oir::BinOp;
    switch (op) {
        case O::Add: return "Add";
        case O::Lt: return "Lt";
        case O::NullCoalesce: return "NullCoalesce";
    }
    return "BinOp(?)";
}

static const char* oir_cast_kind_name(gaupel::oir::CastKind k) {
    using K = gaupel::oir::CastKind;
    switch (k) {
        case K::As:  return "as";
        case K::AsQ: return "as?";
        case K::AsB: return "as!";
    }
    return "cast(?)";
}

static void dump_oir_module(const gaupel::oir::Module& m, const gaupel::ty::TypePool& types) {
    using namespace gaupel;

    std::cout << "\nOIR:\n";
    std::cout << "  funcs=" << m.funcs.size()
              << " blocks=" << m.blocks.size()
              << " insts=" << m.insts.size()
              << " values=" << m.values.size()
              << "\n";

    // funcs + blocks
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

            // params
            for (size_t pi = 0; pi < b.params.size(); ++pi) {
                auto vid = b.params[pi];
                if ((size_t)vid >= m.values.size()) continue;
                const auto& vv = m.values[vid];
                std::cout << "      param v" << vid
                          << " ty=" << types.to_string((ty::TypeId)vv.ty) << " <id " << vv.ty << ">\n";
            }

            // insts
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

            // terminator
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
/// @brief StmtKind를 사람이 읽기 쉬운 이름으로 변경
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
        case K::kSwitch: return "Switch";
        case K::kError: return "Error";
    }
    return "Unknown";
}

static const char* expr_kind_name(gaupel::ast::ExprKind k) {
    using K = gaupel::ast::ExprKind;
    switch (k) {
        case K::kIntLit: return "IntLit";
        case K::kFloatLit: return "FloatLit";
        case K::kStringLit: return "StringLit";
        case K::kCharLit: return "CharLit";
        case K::kBoolLit: return "BoolLit";
        case K::kNullLit: return "NullLit";
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

/// @brief 진단 출력(컨텍스트 포함) + 종료코드 계산
static int flush_diags(
    const gaupel::diag::Bag& bag,
    gaupel::diag::Language lang,
    const gaupel::SourceManager& sm,
    uint32_t context_lines
) {
    std::cout << "\nDIAGNOSTICS:\n";
    if (bag.diags().empty()) {
        std::cout << "no error.\n";
        return 0;
    }

    for (const auto& d : bag.diags()) {
        std::cerr << gaupel::diag::render_one_context(d, lang, sm, context_lines) << "\n";
    }
    return bag.has_error() ? 1 : 0;
}

static void dump_expr(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId id, int indent) {
    const auto& e = ast.expr(id);
    for (int i = 0; i < indent; ++i) std::cout << "  ";

    std::cout << expr_kind_name(e.kind);

    if (e.op != gaupel::syntax::TokenKind::kError) {
        std::cout << " op=" << gaupel::syntax::token_kind_name(e.op);
    }
    if (!e.text.empty()) {
        std::cout << " text=" << e.text;
    }

    // NEW: expected type slot (id only; 실제 문자열은 types가 필요하니 여기선 id만)
    if (e.target_type != gaupel::ast::k_invalid_type) {
        std::cout << " target_ty=<id " << (uint32_t)e.target_type << ">";
    }

    // cast details
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

                // 1) named-group: { ... }
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

                // 2) labeled / positional
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

static void dump_type(const gaupel::ty::TypePool& types, gaupel::ty::TypeId ty) {
    std::cout << types.to_string(ty) << " <id " << static_cast<uint32_t>(ty) << ">";
}

static void dump_stmt(const gaupel::ast::AstArena& ast, const gaupel::ty::TypePool& types, gaupel::ast::StmtId id, int indent);

static void dump_fn_decl(const gaupel::ast::AstArena& ast, const gaupel::ty::TypePool& types, const gaupel::ast::Stmt& s, int indent) {
    // indent already printed by dump_stmt header line; 여기선 디테일만 출력
    for (int i = 0; i < indent + 1; ++i) std::cout << "  ";

    std::cout << "name=" << s.name;

    if (s.is_throwing) std::cout << " throwing=true";
    if (s.is_export)   std::cout << " export=true";
    if (s.is_pure)     std::cout << " pure=true";
    if (s.is_comptime) std::cout << " comptime=true";

    std::cout << " ret=";
    dump_type(types, s.type);
    std::cout << "\n";

    // attrs
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

    // params
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

    // body
    for (int i = 0; i < indent + 1; ++i) std::cout << "  ";
    std::cout << "body:\n";
    dump_stmt(ast, types, s.a, indent + 2);
}

/// @brief stmt 1개를 출력하고, 필요한 경우 하위 노드를 출력
static void dump_stmt(const gaupel::ast::AstArena& ast, const gaupel::ty::TypePool& types, gaupel::ast::StmtId id, int indent) {
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

        default:
            break;
    }
}

static std::vector<gaupel::Token> lex_with_sm(
    gaupel::SourceManager& sm,
    uint32_t file_id,
    gaupel::diag::Bag* bag
) {
    gaupel::Lexer lex(sm.content(file_id), file_id, bag);
    return lex.lex_all();
}

/// @brief expr 파싱 실행
static int run_expr(std::string_view src_arg, gaupel::diag::Language lang, uint32_t context_lines, uint32_t max_errors) {
    gaupel::SourceManager sm;
    std::string src_owned(src_arg);
    const uint32_t file_id = sm.add("<expr>", src_owned);

    gaupel::diag::Bag bag;
    auto tokens = lex_with_sm(sm, file_id, &bag);
    dump_tokens(tokens);

    gaupel::ast::AstArena ast;
    gaupel::ty::TypePool types;
    gaupel::Parser p(tokens, ast, types, &bag, max_errors);

    auto root = p.parse_expr();

    // NOTE: Passes.hpp에선 run_all_on_expr가 아니라 run_on_expr만 제공됨
    gaupel::passes::run_on_expr(ast, root, bag);

    std::cout << "\nAST:\n";
    dump_expr(ast, root, 0);

    std::cout << "\nTYPES:\n";
    types.dump(std::cout);

    return flush_diags(bag, lang, sm, context_lines);
}

/// @brief stmt 1개 파싱 실행
static int run_stmt(
    std::string_view src_arg,
    gaupel::diag::Language lang,
    uint32_t context_lines,
    uint32_t max_errors,
    const gaupel::passes::PassOptions& pass_opt
) {
    gaupel::SourceManager sm;
    std::string src_owned(src_arg);
    const uint32_t file_id = sm.add("<stmt>", src_owned);

    gaupel::diag::Bag bag;
    auto tokens = lex_with_sm(sm, file_id, &bag);
    dump_tokens(tokens);

    gaupel::ast::AstArena ast;
    gaupel::ty::TypePool types;
    gaupel::Parser p(tokens, ast, types, &bag, max_errors);

    auto root = p.parse_stmt();

    // -----------------------
    // PASSES (official API)
    // -----------------------
    auto pres = gaupel::passes::run_on_stmt_tree(ast, root, bag, pass_opt);

    std::cout << "\nAST(STMT):\n";
    dump_stmt(ast, types, root, 0);

    std::cout << "\nTYPES:\n";
    types.dump(std::cout);

    // (원하면) pres.sym / pres.name_resolve를 활용한 후속 단계 추가 가능
    return flush_diags(bag, lang, sm, context_lines);
}

/// @brief 프로그램(여러 stmt) 파싱 실행
static int run_all(
    std::string_view src_arg,
    gaupel::diag::Language lang,
    uint32_t context_lines,
    std::string_view name,
    uint32_t max_errors,
    const gaupel::passes::PassOptions& pass_opt,
    bool dump_oir
) {
    gaupel::SourceManager sm;
    std::string src_owned(src_arg);
    const uint32_t file_id = sm.add(std::string(name), std::move(src_owned));

    gaupel::diag::Bag bag;
    auto tokens = lex_with_sm(sm, file_id, &bag);
    dump_tokens(tokens);

    gaupel::ast::AstArena ast;
    gaupel::ty::TypePool types;
    gaupel::Parser p(tokens, ast, types, &bag, max_errors);

    auto root = p.parse_program();

    auto pres = gaupel::passes::run_on_program(ast, root, bag, pass_opt);

    std::cout << "\nAST(PROGRAM):\n";
    dump_stmt(ast, types, root, 0);

    std::cout << "\nTYPES:\n";
    types.dump(std::cout);

    // TYCK
    gaupel::tyck::TyckResult tyck_res;
    {
        gaupel::tyck::TypeChecker tc(ast, types, bag);
        tyck_res = tc.check_program(root);

        std::cout << "\nTYCK:\n";
        if (tyck_res.errors.empty()) std::cout << "tyck ok.\n";
        else std::cout << "tyck errors: " << tyck_res.errors.size() << "\n";
    }

    // SIR BUILD + MUT ANALYSIS
    gaupel::sir::Module sir_mod;
    {
        gaupel::sir::BuildOptions bopt{};
        sir_mod = gaupel::sir::build_sir_module(
            ast,
            root,
            pres.sym,
            pres.name_resolve,
            tyck_res,
            types,
            bopt
        );

        dump_sir_module(sir_mod, types);

        auto mut = gaupel::sir::analyze_mut(sir_mod, bag);
        std::cout << "\nMUT:\n";
        std::cout << "tracked symbols: " << mut.by_symbol.size() << "\n";
    }

    // OIR (optional)
    if (dump_oir) {
        gaupel::oir::Builder ob(sir_mod, types);
        auto oir_res = ob.build();

        dump_oir_module(oir_res.mod, types);

        auto verrs = gaupel::oir::verify(oir_res.mod);
        std::cout << "\nOIR VERIFY:\n";
        if (verrs.empty()) {
            std::cout << "verify ok.\n";
        } else {
            std::cout << "verify errors: " << verrs.size() << "\n";
            for (auto& e : verrs) std::cout << "  - " << e.msg << "\n";
        }
    }

    int diag_rc = flush_diags(bag, lang, sm, context_lines);
    return diag_rc;
}

/// @brief 파일을 읽어서 프로그램 모드로 파싱
static int run_file(
    const std::string& path,
    gaupel::diag::Language lang,
    uint32_t context_lines,
    uint32_t max_errors,
    const gaupel::passes::PassOptions& pass_opt,
    bool dump_oir
) {
    std::string content;
    std::string err;

    if (!gaupel::open_file(path, content, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::string norm = gaupel::normalize_path(path);
    return run_all(content, lang, context_lines, norm, max_errors, pass_opt, dump_oir);
}

int main(int argc, char** argv) {
    if (argc <= 1) {
        std::cout << gaupel::k_version_string << "\n";
        print_usage();
        return 0;
    }

    std::vector<std::string_view> args;
    args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    // --version
    for (auto a : args) {
        if (a == "--version") {
            std::cout << gaupel::k_version_string << "\n";
            return 0;
        }
    }

    const auto lang = parse_lang(args);
    const auto context_lines = parse_context(args);
    const auto max_errors = parse_max_errors(args);

    gaupel::passes::PassOptions pass_opt{};
    pass_opt.name_resolve.shadowing = parse_shadowing_mode(args);

    auto find_flag = [&](std::string_view key) -> std::optional<size_t> {
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == key) return i;
        }
        return std::nullopt;
    };
    
    bool dump_oir = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--dump") {
            if (i + 1 < args.size() && args[i + 1] == "oir") dump_oir = true;
        } else if (args[i] == "--dump-oir") {
            dump_oir = true; // compatibility
        }
    }

    if (auto i = find_flag("--expr")) {
        if (*i + 1 >= args.size()) {
            std::cerr << "error: --expr requires a string\n";
            return 1;
        }
        return run_expr(args[*i + 1], lang, context_lines, max_errors);
    }

    if (auto i = find_flag("--stmt")) {
        if (*i + 1 >= args.size()) {
            std::cerr << "error: --stmt requires a string\n";
            return 1;
        }
        return run_stmt(args[*i + 1], lang, context_lines, max_errors, pass_opt);
    }

    if (auto i = find_flag("--all")) {
        if (*i + 1 >= args.size()) {
            std::cerr << "error: --all requires a string\n";
            return 1;
        }
        return run_all(args[*i + 1], lang, context_lines, "<all>", max_errors, pass_opt, dump_oir);
    }

    if (auto i = find_flag("--file")) {
        if (*i + 1 >= args.size()) {
            std::cerr << "error: --file requires a path\n";
            return 1;
        }
        return run_file(std::string(args[*i + 1]), lang, context_lines, max_errors, pass_opt, dump_oir);
    }

    print_usage();
    return 0;
}