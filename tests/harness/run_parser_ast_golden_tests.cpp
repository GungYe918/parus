#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ParseOnlyProgram {
    parus::ast::AstArena ast;
    parus::ty::TypePool types;
    parus::diag::Bag bag;
    std::vector<parus::Token> tokens;
    parus::ast::StmtId root = parus::ast::k_invalid_stmt;
};

struct DumpState {
    std::vector<uint8_t> active_stmts{};
    std::vector<uint8_t> active_exprs{};
    std::vector<uint8_t> active_types{};
};

static bool require_(bool cond, const char* msg) {
    if (cond) return true;
    std::cerr << "  - " << msg << "\n";
    return false;
}

static bool read_text_file_(const std::filesystem::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

static bool write_text_file_(const std::filesystem::path& p, std::string_view text) {
    std::ofstream ofs(p, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    return ofs.good();
}

static ParseOnlyProgram parse_program_(const std::string& src) {
    ParseOnlyProgram p{};
    parus::Lexer lx(src, /*file_id=*/1, &p.bag);
    p.tokens = lx.lex_all();
    parus::Parser parser(p.tokens, p.ast, p.types, &p.bag, /*max_errors=*/256);
    p.root = parser.parse_program();
    return p;
}

static void append_indent_(std::string& out, int depth) {
    if (depth <= 0) return;
    out.append(static_cast<size_t>(depth) * 2u, ' ');
}

static void append_line_(std::string& out, int depth, std::string_view line) {
    append_indent_(out, depth);
    out.append(line.data(), line.size());
    out.push_back('\n');
}

static std::string escape_string_(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');

    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                if (std::isprint(c)) {
                    out.push_back(static_cast<char>(c));
                } else {
                    constexpr char kHex[] = "0123456789ABCDEF";
                    out += "\\x";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                }
                break;
            }
        }
    }

    out.push_back('"');
    return out;
}

static const char* expr_kind_name_(parus::ast::ExprKind k) {
    using K = parus::ast::ExprKind;
    switch (k) {
        case K::kError: return "Error";
        case K::kIntLit: return "IntLit";
        case K::kFloatLit: return "FloatLit";
        case K::kStringLit: return "StringLit";
        case K::kCharLit: return "CharLit";
        case K::kBoolLit: return "BoolLit";
        case K::kNullLit: return "NullLit";
        case K::kArrayLit: return "ArrayLit";
        case K::kFieldInit: return "FieldInit";
        case K::kIdent: return "Ident";
        case K::kHole: return "Hole";
        case K::kUnary: return "Unary";
        case K::kPostfixUnary: return "PostfixUnary";
        case K::kBinary: return "Binary";
        case K::kAssign: return "Assign";
        case K::kTernary: return "Ternary";
        case K::kCall: return "Call";
        case K::kIndex: return "Index";
        case K::kMacroCall: return "MacroCall";
        case K::kLoop: return "Loop";
        case K::kIfExpr: return "IfExpr";
        case K::kBlockExpr: return "BlockExpr";
        case K::kCast: return "Cast";
    }
    return "UnknownExpr";
}

static const char* stmt_kind_name_(parus::ast::StmtKind k) {
    using K = parus::ast::StmtKind;
    switch (k) {
        case K::kError: return "Error";
        case K::kEmpty: return "Empty";
        case K::kExprStmt: return "ExprStmt";
        case K::kBlock: return "Block";
        case K::kVar: return "Var";
        case K::kIf: return "If";
        case K::kWhile: return "While";
        case K::kDoScope: return "DoScope";
        case K::kDoWhile: return "DoWhile";
        case K::kManual: return "Manual";
        case K::kReturn: return "Return";
        case K::kRequire: return "Require";
        case K::kThrow: return "Throw";
        case K::kBreak: return "Break";
        case K::kContinue: return "Continue";
        case K::kTryCatch: return "TryCatch";
        case K::kCommitStmt: return "CommitStmt";
        case K::kRecastStmt: return "RecastStmt";
        case K::kSwitch: return "Switch";
        case K::kFnDecl: return "FnDecl";
        case K::kFieldDecl: return "FieldDecl";
        case K::kEnumDecl: return "EnumDecl";
        case K::kProtoDecl: return "ProtoDecl";
        case K::kClassDecl: return "ClassDecl";
        case K::kActorDecl: return "ActorDecl";
        case K::kActsDecl: return "ActsDecl";
        case K::kUse: return "Use";
        case K::kNestDecl: return "NestDecl";
        case K::kCompilerIntrinsicDirective: return "CompilerIntrinsicDirective";
    }
    return "UnknownStmt";
}

static const char* type_kind_name_(parus::ast::TypeNodeKind k) {
    using K = parus::ast::TypeNodeKind;
    switch (k) {
        case K::kError: return "Error";
        case K::kNamedPath: return "NamedPath";
        case K::kOptional: return "Optional";
        case K::kArray: return "Array";
        case K::kBorrow: return "Borrow";
        case K::kEscape: return "Escape";
        case K::kPtr: return "Ptr";
        case K::kFn: return "Fn";
        case K::kMacroCall: return "MacroCall";
    }
    return "UnknownType";
}

static const char* cast_kind_name_(parus::ast::CastKind k) {
    using K = parus::ast::CastKind;
    switch (k) {
        case K::kAs: return "As";
        case K::kAsOptional: return "AsOptional";
        case K::kAsForce: return "AsForce";
    }
    return "UnknownCast";
}

static const char* arg_kind_name_(parus::ast::ArgKind k) {
    using K = parus::ast::ArgKind;
    switch (k) {
        case K::kPositional: return "Positional";
        case K::kLabeled: return "Labeled";
    }
    return "UnknownArg";
}

static const char* self_kind_name_(parus::ast::SelfReceiverKind k) {
    using K = parus::ast::SelfReceiverKind;
    switch (k) {
        case K::kNone: return "None";
        case K::kRead: return "Read";
        case K::kMut: return "Mut";
        case K::kMove: return "Move";
    }
    return "UnknownSelf";
}

static const char* fn_mode_name_(parus::ast::FnMode m) {
    using M = parus::ast::FnMode;
    switch (m) {
        case M::kNone: return "None";
        case M::kPub: return "Pub";
        case M::kSub: return "Sub";
    }
    return "UnknownFnMode";
}

static const char* use_kind_name_(parus::ast::UseKind k) {
    using K = parus::ast::UseKind;
    switch (k) {
        case K::kError: return "Error";
        case K::kImport: return "Import";
        case K::kTypeAlias: return "TypeAlias";
        case K::kPathAlias: return "PathAlias";
        case K::kNestAlias: return "NestAlias";
        case K::kTextSubst: return "TextSubst";
        case K::kActsEnable: return "ActsEnable";
    }
    return "UnknownUse";
}

static const char* proto_fn_role_name_(parus::ast::ProtoFnRole k) {
    using K = parus::ast::ProtoFnRole;
    switch (k) {
        case K::kNone: return "None";
        case K::kRequire: return "Require";
        case K::kProvide: return "Provide";
    }
    return "UnknownProtoFnRole";
}

static const char* proto_require_kind_name_(parus::ast::ProtoRequireKind k) {
    using K = parus::ast::ProtoRequireKind;
    switch (k) {
        case K::kNone: return "None";
        case K::kStruct: return "Struct";
        case K::kEnum: return "Enum";
        case K::kClass: return "Class";
        case K::kActor: return "Actor";
        case K::kActs: return "Acts";
    }
    return "UnknownProtoRequireKind";
}

static const char* case_pat_kind_name_(parus::ast::CasePatKind k) {
    using K = parus::ast::CasePatKind;
    switch (k) {
        case K::kError: return "Error";
        case K::kInt: return "Int";
        case K::kChar: return "Char";
        case K::kString: return "String";
        case K::kBool: return "Bool";
        case K::kNull: return "Null";
        case K::kIdent: return "Ident";
        case K::kEnumVariant: return "EnumVariant";
    }
    return "UnknownCasePat";
}

static std::string token_name_(parus::syntax::TokenKind k) {
    return std::string(parus::syntax::token_kind_name(k));
}

static std::string join_path_(const parus::ast::AstArena& ast, uint32_t begin, uint32_t count) {
    const auto& segs = ast.path_segs();
    const uint64_t b = begin;
    const uint64_t e = b + count;
    if (b > segs.size() || e > segs.size()) return "<invalid-path-slice>";
    if (count == 0) return "";

    std::string out;
    for (uint32_t i = 0; i < count; ++i) {
        if (i) out += "::";
        out.append(segs[begin + i].data(), segs[begin + i].size());
    }
    return out;
}

static void dump_type_node_(
    const parus::ast::AstArena& ast,
    parus::ast::TypeNodeId tid,
    int depth,
    std::string& out,
    DumpState& state
);

static void dump_expr_(
    const parus::ast::AstArena& ast,
    parus::ast::ExprId eid,
    int depth,
    std::string& out,
    DumpState& state
);

static void dump_stmt_(
    const parus::ast::AstArena& ast,
    parus::ast::StmtId sid,
    int depth,
    std::string& out,
    DumpState& state
);

static void dump_expr_ref_(
    const parus::ast::AstArena& ast,
    parus::ast::ExprId eid,
    int depth,
    std::string& out,
    DumpState& state,
    std::string_view label
) {
    append_line_(out, depth, std::string(label) + ":");
    dump_expr_(ast, eid, depth + 1, out, state);
}

static void dump_stmt_ref_(
    const parus::ast::AstArena& ast,
    parus::ast::StmtId sid,
    int depth,
    std::string& out,
    DumpState& state,
    std::string_view label
) {
    append_line_(out, depth, std::string(label) + ":");
    dump_stmt_(ast, sid, depth + 1, out, state);
}

static void dump_type_ref_(
    const parus::ast::AstArena& ast,
    parus::ast::TypeNodeId tid,
    int depth,
    std::string& out,
    DumpState& state,
    std::string_view label
) {
    append_line_(out, depth, std::string(label) + ":");
    dump_type_node_(ast, tid, depth + 1, out, state);
}

static void dump_type_node_(
    const parus::ast::AstArena& ast,
    parus::ast::TypeNodeId tid,
    int depth,
    std::string& out,
    DumpState& state
) {
    if (tid == parus::ast::k_invalid_type_node) {
        append_line_(out, depth, "Type(<invalid>)");
        return;
    }
    if (tid >= ast.type_nodes().size()) {
        append_line_(out, depth, "Type(<out-of-range>)");
        return;
    }

    if (state.active_types.empty()) {
        state.active_types.resize(ast.type_nodes().size(), 0);
    }
    if (state.active_types[tid]) {
        append_line_(out, depth, "Type(<cycle>)");
        return;
    }
    state.active_types[tid] = 1;

    const auto& t = ast.type_node(tid);
    append_line_(out, depth, std::string("Type(") + type_kind_name_(t.kind) + ")");

    switch (t.kind) {
        case parus::ast::TypeNodeKind::kNamedPath: {
            append_line_(out, depth + 1, "path=" + escape_string_(join_path_(ast, t.path_begin, t.path_count)));
            const auto& kids = ast.type_node_children();
            const uint64_t b = t.generic_arg_begin;
            const uint64_t e = b + t.generic_arg_count;
            if (b <= kids.size() && e <= kids.size()) {
                append_line_(out, depth + 1, "generic_args=" + std::to_string(t.generic_arg_count));
                for (uint32_t i = 0; i < t.generic_arg_count; ++i) {
                    dump_type_ref_(ast, kids[t.generic_arg_begin + i], depth + 2, out, state,
                                   "arg[" + std::to_string(i) + "]");
                }
            } else {
                append_line_(out, depth + 1, "generic_args=<invalid-slice>");
            }
            break;
        }

        case parus::ast::TypeNodeKind::kOptional:
        case parus::ast::TypeNodeKind::kArray:
        case parus::ast::TypeNodeKind::kBorrow:
        case parus::ast::TypeNodeKind::kEscape:
        case parus::ast::TypeNodeKind::kPtr: {
            if (t.kind == parus::ast::TypeNodeKind::kArray) {
                append_line_(out, depth + 1, "has_size=" + std::string(t.array_has_size ? "1" : "0"));
                if (t.array_has_size) {
                    append_line_(out, depth + 1, "size=" + std::to_string(t.array_size));
                }
            }
            if (t.kind == parus::ast::TypeNodeKind::kBorrow || t.kind == parus::ast::TypeNodeKind::kPtr) {
                append_line_(out, depth + 1, "is_mut=" + std::string(t.is_mut ? "1" : "0"));
            }
            dump_type_ref_(ast, t.elem, depth + 1, out, state, "elem");
            break;
        }

        case parus::ast::TypeNodeKind::kFn: {
            const auto& kids = ast.type_node_children();
            const uint64_t b = t.fn_param_begin;
            const uint64_t e = b + t.fn_param_count;
            if (b <= kids.size() && e <= kids.size()) {
                append_line_(out, depth + 1, "params=" + std::to_string(t.fn_param_count));
                for (uint32_t i = 0; i < t.fn_param_count; ++i) {
                    dump_type_ref_(ast, kids[t.fn_param_begin + i], depth + 2, out, state,
                                   "param[" + std::to_string(i) + "]");
                }
            } else {
                append_line_(out, depth + 1, "params=<invalid-slice>");
            }
            dump_type_ref_(ast, t.fn_ret, depth + 1, out, state, "ret");
            break;
        }

        case parus::ast::TypeNodeKind::kMacroCall: {
            append_line_(out, depth + 1, "macro_path=" + escape_string_(join_path_(ast, t.macro_path_begin, t.macro_path_count)));
            append_line_(out, depth + 1, "macro_tokens=" + std::to_string(t.macro_arg_count));
            break;
        }

        case parus::ast::TypeNodeKind::kError:
            break;
    }

    state.active_types[tid] = 0;
}

static void dump_expr_(
    const parus::ast::AstArena& ast,
    parus::ast::ExprId eid,
    int depth,
    std::string& out,
    DumpState& state
) {
    if (eid == parus::ast::k_invalid_expr) {
        append_line_(out, depth, "Expr(<invalid>)");
        return;
    }
    if (eid >= ast.exprs().size()) {
        append_line_(out, depth, "Expr(<out-of-range>)");
        return;
    }

    if (state.active_exprs.empty()) {
        state.active_exprs.resize(ast.exprs().size(), 0);
    }
    if (state.active_exprs[eid]) {
        append_line_(out, depth, "Expr(<cycle>)");
        return;
    }
    state.active_exprs[eid] = 1;

    const auto& e = ast.expr(eid);
    append_line_(out, depth, std::string("Expr(") + expr_kind_name_(e.kind) + ")");

    const auto dump_text = [&](std::string_view label, std::string_view value) {
        if (value.empty()) return;
        append_line_(out, depth + 1, std::string(label) + "=" + escape_string_(value));
    };

    switch (e.kind) {
        case parus::ast::ExprKind::kIntLit:
        case parus::ast::ExprKind::kFloatLit:
        case parus::ast::ExprKind::kStringLit:
        case parus::ast::ExprKind::kCharLit:
        case parus::ast::ExprKind::kBoolLit:
        case parus::ast::ExprKind::kNullLit:
        case parus::ast::ExprKind::kIdent:
        case parus::ast::ExprKind::kHole:
        case parus::ast::ExprKind::kError:
            dump_text("text", e.text);
            break;

        case parus::ast::ExprKind::kUnary:
        case parus::ast::ExprKind::kPostfixUnary:
            append_line_(out, depth + 1, "op=" + token_name_(e.op));
            if (e.kind == parus::ast::ExprKind::kUnary) {
                append_line_(out, depth + 1, "is_mut=" + std::string(e.unary_is_mut ? "1" : "0"));
            }
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "operand");
            break;

        case parus::ast::ExprKind::kBinary:
        case parus::ast::ExprKind::kAssign:
            append_line_(out, depth + 1, "op=" + token_name_(e.op));
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "lhs");
            dump_expr_ref_(ast, e.b, depth + 1, out, state, "rhs");
            break;

        case parus::ast::ExprKind::kTernary:
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "cond");
            dump_expr_ref_(ast, e.b, depth + 1, out, state, "then");
            dump_expr_ref_(ast, e.c, depth + 1, out, state, "else");
            break;

        case parus::ast::ExprKind::kCall: {
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "callee");
            const auto& args = ast.args();
            const uint64_t b = e.arg_begin;
            const uint64_t end = b + e.arg_count;
            if (b <= args.size() && end <= args.size()) {
                append_line_(out, depth + 1, "args=" + std::to_string(e.arg_count));
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];
                    std::string line = "arg[" + std::to_string(i) + "] kind=" + std::string(arg_kind_name_(a.kind));
                    line += " label=" + escape_string_(a.label);
                    line += " hole=" + std::string(a.is_hole ? "1" : "0");
                    append_line_(out, depth + 2, line);
                    if (!a.is_hole) {
                        dump_expr_(ast, a.expr, depth + 3, out, state);
                    }
                }
            } else {
                append_line_(out, depth + 1, "args=<invalid-slice>");
            }
            break;
        }

        case parus::ast::ExprKind::kIndex:
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "base");
            dump_expr_ref_(ast, e.b, depth + 1, out, state, "index");
            break;

        case parus::ast::ExprKind::kMacroCall:
            append_line_(out, depth + 1, "path=" + escape_string_(join_path_(ast, e.macro_path_begin, e.macro_path_count)));
            append_line_(out, depth + 1, "tokens=" + std::to_string(e.macro_token_count));
            break;

        case parus::ast::ExprKind::kLoop:
            append_line_(out, depth + 1, "has_header=" + std::string(e.loop_has_header ? "1" : "0"));
            dump_text("loop_var", e.loop_var);
            dump_expr_ref_(ast, e.loop_iter, depth + 1, out, state, "iter");
            dump_stmt_ref_(ast, e.loop_body, depth + 1, out, state, "body");
            break;

        case parus::ast::ExprKind::kIfExpr:
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "cond");
            dump_expr_ref_(ast, e.b, depth + 1, out, state, "then");
            dump_expr_ref_(ast, e.c, depth + 1, out, state, "else");
            break;

        case parus::ast::ExprKind::kBlockExpr:
            dump_stmt_ref_(ast, e.block_stmt, depth + 1, out, state, "block");
            dump_expr_ref_(ast, e.block_tail, depth + 1, out, state, "tail");
            break;

        case parus::ast::ExprKind::kCast:
            append_line_(out, depth + 1, "cast_kind=" + std::string(cast_kind_name_(e.cast_kind)));
            dump_type_ref_(ast, e.cast_type_node, depth + 1, out, state, "target_type");
            dump_expr_ref_(ast, e.a, depth + 1, out, state, "operand");
            break;

        case parus::ast::ExprKind::kArrayLit: {
            const auto& args = ast.args();
            const uint64_t b = e.arg_begin;
            const uint64_t end = b + e.arg_count;
            if (b <= args.size() && end <= args.size()) {
                append_line_(out, depth + 1, "items=" + std::to_string(e.arg_count));
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    dump_expr_ref_(ast, args[e.arg_begin + i].expr, depth + 2, out, state,
                                   "item[" + std::to_string(i) + "]");
                }
            } else {
                append_line_(out, depth + 1, "items=<invalid-slice>");
            }
            break;
        }

        case parus::ast::ExprKind::kFieldInit: {
            dump_text("type_head", e.text);
            dump_type_ref_(ast, e.field_init_type_node, depth + 1, out, state, "type_node");

            const auto& ents = ast.field_init_entries();
            const uint64_t b = e.field_init_begin;
            const uint64_t end = b + e.field_init_count;
            if (b <= ents.size() && end <= ents.size()) {
                append_line_(out, depth + 1, "entries=" + std::to_string(e.field_init_count));
                for (uint32_t i = 0; i < e.field_init_count; ++i) {
                    const auto& ent = ents[e.field_init_begin + i];
                    append_line_(out, depth + 2, "field[" + std::to_string(i) + "]=" + escape_string_(ent.name));
                    dump_expr_(ast, ent.expr, depth + 3, out, state);
                }
            } else {
                append_line_(out, depth + 1, "entries=<invalid-slice>");
            }
            break;
        }
    }

    state.active_exprs[eid] = 0;
}

static void dump_stmt_children_slice_(
    const parus::ast::AstArena& ast,
    uint32_t begin,
    uint32_t count,
    int depth,
    std::string& out,
    DumpState& state,
    std::string_view label
) {
    const auto& kids = ast.stmt_children();
    const uint64_t b = begin;
    const uint64_t end = b + count;
    if (b > kids.size() || end > kids.size()) {
        append_line_(out, depth, std::string(label) + "=<invalid-slice>");
        return;
    }

    append_line_(out, depth, std::string(label) + "=" + std::to_string(count));
    for (uint32_t i = 0; i < count; ++i) {
        dump_stmt_ref_(ast, kids[begin + i], depth + 1, out, state, "child[" + std::to_string(i) + "]");
    }
}

static void dump_stmt_(
    const parus::ast::AstArena& ast,
    parus::ast::StmtId sid,
    int depth,
    std::string& out,
    DumpState& state
) {
    if (sid == parus::ast::k_invalid_stmt) {
        append_line_(out, depth, "Stmt(<invalid>)");
        return;
    }
    if (sid >= ast.stmts().size()) {
        append_line_(out, depth, "Stmt(<out-of-range>)");
        return;
    }

    if (state.active_stmts.empty()) {
        state.active_stmts.resize(ast.stmts().size(), 0);
    }
    if (state.active_stmts[sid]) {
        append_line_(out, depth, "Stmt(<cycle>)");
        return;
    }
    state.active_stmts[sid] = 1;

    const auto& s = ast.stmt(sid);
    append_line_(out, depth, std::string("Stmt(") + stmt_kind_name_(s.kind) + ")");

    switch (s.kind) {
        case parus::ast::StmtKind::kBlock:
        case parus::ast::StmtKind::kActsDecl:
        case parus::ast::StmtKind::kEnumDecl:
        case parus::ast::StmtKind::kClassDecl:
        case parus::ast::StmtKind::kActorDecl:
            dump_stmt_children_slice_(ast, s.stmt_begin, s.stmt_count, depth + 1, out, state, "children");
            break;

        case parus::ast::StmtKind::kExprStmt:
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "expr");
            break;

        case parus::ast::StmtKind::kVar:
            append_line_(out, depth + 1, "name=" + escape_string_(s.name));
            append_line_(out, depth + 1, "is_set=" + std::string(s.is_set ? "1" : "0"));
            append_line_(out, depth + 1, "is_mut=" + std::string(s.is_mut ? "1" : "0"));
            append_line_(out, depth + 1, "is_static=" + std::string(s.is_static ? "1" : "0"));
            append_line_(out, depth + 1, "is_extern=" + std::string(s.is_extern ? "1" : "0"));
            dump_type_ref_(ast, s.type_node, depth + 1, out, state, "type");
            dump_expr_ref_(ast, s.init, depth + 1, out, state, "init");
            if (s.var_has_acts_binding) {
                append_line_(out, depth + 1, "acts_binding=1");
                append_line_(out, depth + 1, "acts_default=" + std::string(s.var_acts_is_default ? "1" : "0"));
                append_line_(out, depth + 1,
                             "acts_set_path=" + escape_string_(join_path_(ast, s.var_acts_set_path_begin, s.var_acts_set_path_count)));
            }
            break;

        case parus::ast::StmtKind::kIf:
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "cond");
            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "then");
            dump_stmt_ref_(ast, s.b, depth + 1, out, state, "else");
            break;

        case parus::ast::StmtKind::kWhile:
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "cond");
            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "body");
            break;

        case parus::ast::StmtKind::kDoScope:
            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "body");
            break;

        case parus::ast::StmtKind::kDoWhile:
            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "body");
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "cond");
            break;

        case parus::ast::StmtKind::kManual:
            append_line_(out, depth + 1, "perm_mask=" + std::to_string(s.manual_perm_mask));
            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "body");
            break;

        case parus::ast::StmtKind::kReturn:
        case parus::ast::StmtKind::kThrow:
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "expr");
            break;

        case parus::ast::StmtKind::kRequire:
            if (s.proto_require_kind != parus::ast::ProtoRequireKind::kNone) {
                append_line_(out, depth + 1, "proto_require_kind=" + std::string(proto_require_kind_name_(s.proto_require_kind)));
                append_line_(out, depth + 1, "proto_require_path=" + escape_string_(join_path_(ast, s.proto_req_path_begin, s.proto_req_path_count)));
            } else {
                dump_expr_ref_(ast, s.expr, depth + 1, out, state, "expr");
            }
            break;

        case parus::ast::StmtKind::kSwitch: {
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "cond");
            const auto& cases = ast.switch_cases();
            const uint64_t b = s.case_begin;
            const uint64_t end = b + s.case_count;
            if (b <= cases.size() && end <= cases.size()) {
                append_line_(out, depth + 1, "cases=" + std::to_string(s.case_count));
                for (uint32_t i = 0; i < s.case_count; ++i) {
                    const auto& c = cases[s.case_begin + i];
                    std::string line = "case[" + std::to_string(i) + "] default=";
                    line += (c.is_default ? "1" : "0");
                    line += " pat_kind=";
                    line += case_pat_kind_name_(c.pat_kind);
                    line += " pat=";
                    line += escape_string_(c.pat_text);
                    append_line_(out, depth + 2, line);
                    dump_stmt_ref_(ast, c.body, depth + 3, out, state, "body");
                }
            } else {
                append_line_(out, depth + 1, "cases=<invalid-slice>");
            }
            break;
        }

        case parus::ast::StmtKind::kTryCatch: {
            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "try_body");
            const auto& clauses = ast.try_catch_clauses();
            const uint64_t b = s.catch_clause_begin;
            const uint64_t end = b + s.catch_clause_count;
            if (b <= clauses.size() && end <= clauses.size()) {
                append_line_(out, depth + 1, "catches=" + std::to_string(s.catch_clause_count));
                for (uint32_t i = 0; i < s.catch_clause_count; ++i) {
                    const auto& c = clauses[s.catch_clause_begin + i];
                    std::string line = "catch[" + std::to_string(i) + "] bind=" + escape_string_(c.bind_name);
                    line += " typed=" + std::string(c.has_typed_bind ? "1" : "0");
                    append_line_(out, depth + 2, line);
                    dump_type_ref_(ast, c.bind_type_node, depth + 3, out, state, "bind_type");
                    dump_stmt_ref_(ast, c.body, depth + 3, out, state, "body");
                }
            } else {
                append_line_(out, depth + 1, "catches=<invalid-slice>");
            }
            break;
        }

        case parus::ast::StmtKind::kFnDecl: {
            append_line_(out, depth + 1, "name=" + escape_string_(s.name));
            append_line_(out, depth + 1, "mode=" + std::string(fn_mode_name_(s.fn_mode)));
            append_line_(out, depth + 1, "export=" + std::string(s.is_export ? "1" : "0"));
            append_line_(out, depth + 1, "extern=" + std::string(s.is_extern ? "1" : "0"));
            append_line_(out, depth + 1, "throwing=" + std::string(s.is_throwing ? "1" : "0"));
            append_line_(out, depth + 1, "pure=" + std::string(s.is_pure ? "1" : "0"));
            append_line_(out, depth + 1, "comptime=" + std::string(s.is_comptime ? "1" : "0"));
            append_line_(out, depth + 1, "operator=" + std::string(s.fn_is_operator ? "1" : "0"));
            if (s.proto_fn_role != parus::ast::ProtoFnRole::kNone) {
                append_line_(out, depth + 1, "proto_fn_role=" + std::string(proto_fn_role_name_(s.proto_fn_role)));
            }
            if (s.fn_is_operator) {
                append_line_(out, depth + 1, "operator_tok=" + token_name_(s.fn_operator_token));
                append_line_(out, depth + 1, "operator_postfix=" + std::string(s.fn_operator_is_postfix ? "1" : "0"));
            }
            if (s.attr_count > 0) {
                append_line_(out, depth + 1, "attrs=" + std::to_string(s.attr_count));
                const auto& attrs = ast.fn_attrs();
                const uint64_t b = s.attr_begin;
                const uint64_t end = b + s.attr_count;
                if (b <= attrs.size() && end <= attrs.size()) {
                    for (uint32_t i = 0; i < s.attr_count; ++i) {
                        append_line_(out, depth + 2, "attr[" + std::to_string(i) + "]=" + escape_string_(attrs[s.attr_begin + i].name));
                    }
                }
            }

            if (s.fn_generic_param_count > 0) {
                append_line_(out, depth + 1, "fn_generics=" + std::to_string(s.fn_generic_param_count));
                const auto& gens = ast.generic_param_decls();
                const uint64_t b = s.fn_generic_param_begin;
                const uint64_t end = b + s.fn_generic_param_count;
                if (b <= gens.size() && end <= gens.size()) {
                    for (uint32_t i = 0; i < s.fn_generic_param_count; ++i) {
                        append_line_(out, depth + 2, "gen[" + std::to_string(i) + "]=" + escape_string_(gens[s.fn_generic_param_begin + i].name));
                    }
                }
            }

            if (s.fn_constraint_count > 0) {
                append_line_(out, depth + 1, "fn_constraints=" + std::to_string(s.fn_constraint_count));
                const auto& cons = ast.fn_constraint_decls();
                const uint64_t b = s.fn_constraint_begin;
                const uint64_t end = b + s.fn_constraint_count;
                if (b <= cons.size() && end <= cons.size()) {
                    for (uint32_t i = 0; i < s.fn_constraint_count; ++i) {
                        const auto& c = cons[s.fn_constraint_begin + i];
                        std::string line = "constraint[" + std::to_string(i) + "] ";
                        line += escape_string_(c.type_param);
                        line += " : ";
                        line += escape_string_(join_path_(ast, c.proto_path_begin, c.proto_path_count));
                        append_line_(out, depth + 2, line);
                    }
                }
            }

            dump_type_ref_(ast, s.fn_ret_type_node, depth + 1, out, state, "ret");

            const auto& params = ast.params();
            const uint64_t pb = s.param_begin;
            const uint64_t pe = pb + s.param_count;
            if (pb <= params.size() && pe <= params.size()) {
                append_line_(out, depth + 1, "params=" + std::to_string(s.param_count));
                append_line_(out, depth + 1, "positional_params=" + std::to_string(s.positional_param_count));
                append_line_(out, depth + 1, "has_named_group=" + std::string(s.has_named_group ? "1" : "0"));
                for (uint32_t i = 0; i < s.param_count; ++i) {
                    const auto& p = params[s.param_begin + i];
                    std::string line = "param[" + std::to_string(i) + "] name=" + escape_string_(p.name);
                    line += " mut=" + std::string(p.is_mut ? "1" : "0");
                    line += " self=" + std::string(p.is_self ? "1" : "0");
                    line += " self_kind=" + std::string(self_kind_name_(p.self_kind));
                    line += " named_group=" + std::string(p.is_named_group ? "1" : "0");
                    line += " has_default=" + std::string(p.has_default ? "1" : "0");
                    append_line_(out, depth + 2, line);
                    dump_type_ref_(ast, p.type_node, depth + 3, out, state, "type");
                    if (p.has_default) {
                        dump_expr_ref_(ast, p.default_expr, depth + 3, out, state, "default");
                    }
                }
            } else {
                append_line_(out, depth + 1, "params=<invalid-slice>");
            }

            dump_stmt_ref_(ast, s.a, depth + 1, out, state, "body");
            break;
        }

        case parus::ast::StmtKind::kFieldDecl: {
            append_line_(out, depth + 1, "name=" + escape_string_(s.name));
            const auto& members = ast.field_members();
            const uint64_t b = s.field_member_begin;
            const uint64_t end = b + s.field_member_count;
            if (b <= members.size() && end <= members.size()) {
                append_line_(out, depth + 1, "members=" + std::to_string(s.field_member_count));
                for (uint32_t i = 0; i < s.field_member_count; ++i) {
                    const auto& m = members[s.field_member_begin + i];
                    append_line_(out, depth + 2, "member[" + std::to_string(i) + "] name=" + escape_string_(m.name));
                    dump_type_ref_(ast, m.type_node, depth + 3, out, state, "type");
                }
            } else {
                append_line_(out, depth + 1, "members=<invalid-slice>");
            }
            break;
        }

        case parus::ast::StmtKind::kProtoDecl:
            append_line_(out, depth + 1, "name=" + escape_string_(s.name));
            dump_stmt_children_slice_(ast, s.stmt_begin, s.stmt_count, depth + 1, out, state, "members");
            break;

        case parus::ast::StmtKind::kNestDecl:
            append_line_(out, depth + 1, "path=" + escape_string_(join_path_(ast, s.nest_path_begin, s.nest_path_count)));
            append_line_(out, depth + 1, "file_directive=" + std::string(s.nest_is_file_directive ? "1" : "0"));
            if (!s.nest_is_file_directive) {
                dump_stmt_ref_(ast, s.a, depth + 1, out, state, "body");
            }
            break;

        case parus::ast::StmtKind::kUse:
            append_line_(out, depth + 1, "kind=" + std::string(use_kind_name_(s.use_kind)));
            append_line_(out, depth + 1, "name=" + escape_string_(s.use_name));
            append_line_(out, depth + 1, "rhs_ident=" + escape_string_(s.use_rhs_ident));
            append_line_(out, depth + 1, "path=" + escape_string_(join_path_(ast, s.use_path_begin, s.use_path_count)));
            dump_expr_ref_(ast, s.expr, depth + 1, out, state, "expr");
            break;

        case parus::ast::StmtKind::kCompilerIntrinsicDirective:
            append_line_(out, depth + 1, "key_path=" + escape_string_(join_path_(ast, s.directive_key_path_begin, s.directive_key_path_count)));
            append_line_(out, depth + 1, "target_path=" + escape_string_(join_path_(ast, s.directive_target_path_begin, s.directive_target_path_count)));
            break;

        default:
            break;
    }

    state.active_stmts[sid] = 0;
}

static std::string dump_program_ast_(const ParseOnlyProgram& p) {
    std::string out;
    out.reserve(4096);

    append_line_(out, 0, "AST-GOLDEN v1");
    append_line_(out, 0, "root:");

    DumpState state{};
    dump_stmt_(p.ast, p.root, 1, out, state);
    return out;
}

static std::string normalize_text_(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 1);

    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') continue;
            out.push_back('\n');
            continue;
        }
        out.push_back(c);
    }

    if (out.empty() || out.back() != '\n') {
        out.push_back('\n');
    }
    return out;
}

struct DiffLine {
    size_t line = 0;
    std::string expected{};
    std::string actual{};
};

static DiffLine first_diff_line_(std::string_view expected, std::string_view actual) {
    std::istringstream eiss{std::string(expected)};
    std::istringstream aiss{std::string(actual)};

    std::string el;
    std::string al;
    size_t line = 1;

    while (true) {
        const bool eok = static_cast<bool>(std::getline(eiss, el));
        const bool aok = static_cast<bool>(std::getline(aiss, al));

        if (!eok && !aok) {
            return DiffLine{};
        }
        if (!eok || !aok || el != al) {
            DiffLine d{};
            d.line = line;
            d.expected = eok ? el : "<EOF>";
            d.actual = aok ? al : "<EOF>";
            return d;
        }

        ++line;
    }
}

static bool env_flag_enabled_(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw) return false;

    std::string v(raw);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return v == "1" || v == "true" || v == "yes" || v == "on";
}

static bool dump_parse_errors_(const parus::diag::Bag& bag) {
    bool first = true;
    for (const auto& d : bag.diags()) {
        if (d.severity() != parus::diag::Severity::kError
            && d.severity() != parus::diag::Severity::kFatal) {
            continue;
        }

        if (first) {
            std::cerr << "    parser errors:\n";
            first = false;
        }

        std::cerr << "      - " << parus::diag::code_name(d.code()) << "\n";
    }
    return !first;
}

static bool run_golden_case_(const std::filesystem::path& p, bool update_mode) {
    std::string src;
    if (!read_text_file_(p, src)) {
        std::cerr << "  - failed to read case file: " << p.string() << "\n";
        return false;
    }

    auto prog = parse_program_(src);

    bool ok = true;
    ok &= require_(prog.root != parus::ast::k_invalid_stmt, "parser must return a valid program root");
    if (prog.root != parus::ast::k_invalid_stmt) {
        ok &= require_(prog.ast.stmt(prog.root).kind == parus::ast::StmtKind::kBlock,
                       "program root must be block stmt");
    }
    ok &= require_(!prog.bag.has_error(), "golden case must parse without parser errors");
    if (!ok) {
        dump_parse_errors_(prog.bag);
        std::cerr << "    file: " << p.filename().string() << "\n";
        return false;
    }

    const std::string actual = normalize_text_(dump_program_ast_(prog));

    auto golden = p;
    golden.replace_extension(".ast");

    std::string expected_raw;
    const bool golden_exists = read_text_file_(golden, expected_raw);
    const std::string expected = golden_exists ? normalize_text_(expected_raw) : std::string();

    if (update_mode) {
        if (!golden_exists || expected != actual) {
            if (!write_text_file_(golden, actual)) {
                std::cerr << "  - failed to write golden file: " << golden.string() << "\n";
                return false;
            }
            std::cout << "    [UPDATE] " << golden.filename().string() << "\n";
        }
        return true;
    }

    if (!golden_exists) {
        std::cerr << "  - missing golden file: " << golden.filename().string() << "\n";
        return false;
    }

    if (expected != actual) {
        const auto d = first_diff_line_(expected, actual);
        std::cerr << "  - AST golden mismatch: " << p.filename().string() << "\n";
        if (d.line != 0) {
            std::cerr << "    line " << d.line << "\n";
            std::cerr << "    expected: " << d.expected << "\n";
            std::cerr << "    actual  : " << d.actual << "\n";
        }
        std::cerr << "    set PARUS_UPDATE_GOLDEN=1 to refresh golden outputs\n";
        return false;
    }

    return true;
}

static bool test_ast_golden_cases_directory() {
#ifndef PARUS_PARSER_AST_GOLDEN_CASE_DIR
    std::cerr << "  - PARUS_PARSER_AST_GOLDEN_CASE_DIR is not defined\n";
    return false;
#else
    const bool update_mode = env_flag_enabled_("PARUS_UPDATE_GOLDEN");
    const std::filesystem::path case_dir{PARUS_PARSER_AST_GOLDEN_CASE_DIR};

    bool ok = true;
    ok &= require_(std::filesystem::exists(case_dir), "parser AST golden case directory does not exist");
    ok &= require_(std::filesystem::is_directory(case_dir), "parser AST golden case path is not a directory");
    if (!ok) return false;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(case_dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() == ".pr") files.push_back(p);
    }

    std::sort(files.begin(), files.end());
    ok &= require_(!files.empty(), "at least one parser AST golden .pr case is required");
    if (!ok) return false;

    if (update_mode) {
        std::cout << "  [MODE] update golden enabled (PARUS_UPDATE_GOLDEN=1)\n";
    }

    for (const auto& p : files) {
        std::cout << "  [CASE] " << p.filename().string() << "\n";
        ok &= run_golden_case_(p, update_mode);
    }

    return ok;
#endif
}

} // namespace

int main() {
    std::cout << "[TEST] parser_ast_golden_cases\n";
    if (!test_ast_golden_cases_directory()) {
        std::cout << "  -> FAIL\n";
        std::cout << "FAILED: 1 test(s)\n";
        return 1;
    }

    std::cout << "  -> PASS\n";
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
