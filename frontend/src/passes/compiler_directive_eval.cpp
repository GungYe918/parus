#include <parus/passes/CompilerDirectiveEval.hpp>

#include <parus/diag/DiagCode.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/text/SourceManager.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace parus::passes {

    namespace {

        struct InstValue {
            enum class Kind : uint8_t {
                kInvalid = 0,
                kBool,
                kInt,
                kString,
            };
            Kind kind = Kind::kInvalid;
            bool b = false;
            int64_t i = 0;
            std::string s{};
        };

        struct ExternalInstUnit {
            ast::AstArena ast{};
            ty::TypePool types{};
            ast::StmtId root = ast::k_invalid_stmt;
            ast::StmtId sid = ast::k_invalid_stmt;
        };

        struct InstRef {
            std::string qname{};
            const ast::AstArena* ast = nullptr;
            ast::StmtId sid = ast::k_invalid_stmt;
            std::shared_ptr<ExternalInstUnit> ext_owner{};
        };

        static void report_(diag::Bag& bag, diag::Code code, Span span, std::string_view a0 = {}) {
            diag::Diagnostic d(diag::Severity::kError, code, span);
            if (!a0.empty()) d.add_arg(a0);
            bag.add(std::move(d));
        }

        static bool parse_int_lit_(std::string_view text, int64_t& out) {
            out = 0;
            bool saw = false;
            for (const char c : text) {
                if (c == '_') continue;
                if (c < '0' || c > '9') break;
                saw = true;
                out = out * 10 + static_cast<int64_t>(c - '0');
            }
            return saw;
        }

        static bool value_eq_(const InstValue& a, const InstValue& b) {
            if (a.kind != b.kind) return false;
            switch (a.kind) {
                case InstValue::Kind::kBool: return a.b == b.b;
                case InstValue::Kind::kInt: return a.i == b.i;
                case InstValue::Kind::kString: return a.s == b.s;
                default: return false;
            }
        }

        static std::string qualify_(const std::vector<std::string>& ns, std::string_view base) {
            if (base.empty()) return {};
            if (ns.empty()) return std::string(base);
            std::string out{};
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i) out += "::";
                out += ns[i];
            }
            out += "::";
            out += std::string(base);
            return out;
        }

        static std::vector<std::string> split_ns_(std::string_view qname) {
            std::vector<std::string> out{};
            size_t pos = 0;
            while (pos < qname.size()) {
                const size_t next = qname.find("::", pos);
                if (next == std::string_view::npos) break;
                out.emplace_back(qname.substr(pos, next - pos));
                pos = next + 2;
            }
            return out;
        }

        class DirectiveEvaluator {
        public:
            DirectiveEvaluator(ast::AstArena& ast, ast::StmtId root, diag::Bag& bag, const PassOptions& opt)
                : ast_(ast), root_(root), bag_(bag), opt_(opt) {}

            void run() {
                collect_local_inst_(root_, namespace_stack_);
                collect_external_inst_payloads_();
                prune_stmt_(root_, namespace_stack_);
            }

        private:
            enum class ParamExpected : uint8_t {
                kAny = 0,
                kBool,
                kInt,
                kString,
            };

            struct ExecResult {
                bool ok = true;
                bool returned = false;
                InstValue value{};
            };

            ast::AstArena& ast_;
            ast::StmtId root_ = ast::k_invalid_stmt;
            diag::Bag& bag_;
            const PassOptions& opt_;

            std::vector<std::string> namespace_stack_{};
            std::unordered_map<std::string, InstRef> local_insts_{};
            std::unordered_map<std::string, std::string> external_payloads_{};
            std::unordered_map<std::string, std::shared_ptr<ExternalInstUnit>> external_cache_{};
            std::vector<std::string> call_stack_{};

            bool is_valid_stmt_(const ast::AstArena& arena, ast::StmtId sid) const {
                return sid != ast::k_invalid_stmt && static_cast<size_t>(sid) < arena.stmts().size();
            }
            bool is_valid_expr_(const ast::AstArena& arena, ast::ExprId eid) const {
                return eid != ast::k_invalid_expr && static_cast<size_t>(eid) < arena.exprs().size();
            }

            void collect_external_inst_payloads_() {
                for (const auto& ex : opt_.name_resolve.external_exports) {
                    if (ex.kind != sema::SymbolKind::kInst) continue;
                    if (ex.path.empty()) continue;
                    external_payloads_[ex.path] = ex.inst_payload;
                }
            }

            void collect_local_inst_(ast::StmtId sid, std::vector<std::string>& ns) {
                if (!is_valid_stmt_(ast_, sid)) return;
                const auto& s = ast_.stmt(sid);
                if (s.kind == ast::StmtKind::kBlock ||
                    s.kind == ast::StmtKind::kProtoDecl ||
                    s.kind == ast::StmtKind::kClassDecl ||
                    s.kind == ast::StmtKind::kActorDecl ||
                    s.kind == ast::StmtKind::kActsDecl)
                {
                    const auto& kids = ast_.stmt_children();
                    const uint64_t begin = s.stmt_begin;
                    const uint64_t end = begin + s.stmt_count;
                    if (begin <= kids.size() && end <= kids.size()) {
                        for (uint32_t i = 0; i < s.stmt_count; ++i) {
                            collect_local_inst_(kids[s.stmt_begin + i], ns);
                        }
                    }
                    return;
                }
                if (s.kind == ast::StmtKind::kNestDecl && !s.nest_is_file_directive) {
                    uint32_t pushed = 0;
                    const auto& segs = ast_.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            ns.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }
                    collect_local_inst_(s.a, ns);
                    while (pushed > 0) {
                        ns.pop_back();
                        --pushed;
                    }
                    return;
                }
                if (s.kind == ast::StmtKind::kInstDecl && !s.name.empty()) {
                    InstRef ref{};
                    ref.qname = qualify_(ns, s.name);
                    ref.ast = &ast_;
                    ref.sid = sid;
                    local_insts_.insert({ref.qname, ref});
                }
            }

            void rewrite_children_(ast::Stmt& container, std::vector<ast::StmtId>& out) {
                auto& kids = ast_.stmt_children_mut();
                container.stmt_begin = static_cast<uint32_t>(kids.size());
                container.stmt_count = static_cast<uint32_t>(out.size());
                for (const auto sid : out) {
                    kids.push_back(sid);
                }
            }

            void prune_children_of_(ast::StmtId sid, std::vector<std::string>& ns) {
                if (!is_valid_stmt_(ast_, sid)) return;
                auto& s = ast_.stmt_mut(sid);
                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin > kids.size() || end > kids.size()) return;

                std::vector<ast::StmtId> original{};
                original.reserve(s.stmt_count);
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    original.push_back(kids[s.stmt_begin + i]);
                }

                std::vector<ast::StmtId> rewritten{};
                rewritten.reserve(original.size());
                for (const auto child_sid : original) {
                    if (!is_valid_stmt_(ast_, child_sid)) continue;
                    auto& child = ast_.stmt_mut(child_sid);

                    if (child.kind == ast::StmtKind::kCompilerDirective) {
                        const bool enabled = eval_directive_call_(child.expr, ns, child.span);
                        if (enabled && is_valid_stmt_(ast_, child.a)) {
                            prune_stmt_(child.a, ns);
                            rewritten.push_back(child.a);
                        }
                        continue;
                    }

                    prune_stmt_(child_sid, ns);
                    rewritten.push_back(child_sid);
                }

                rewrite_children_(s, rewritten);
            }

            void prune_stmt_(ast::StmtId sid, std::vector<std::string>& ns) {
                if (!is_valid_stmt_(ast_, sid)) return;
                auto& s = ast_.stmt_mut(sid);
                switch (s.kind) {
                    case ast::StmtKind::kBlock:
                    case ast::StmtKind::kProtoDecl:
                    case ast::StmtKind::kClassDecl:
                    case ast::StmtKind::kActorDecl:
                    case ast::StmtKind::kActsDecl:
                        prune_children_of_(sid, ns);
                        return;

                    case ast::StmtKind::kNestDecl:
                        if (!s.nest_is_file_directive) {
                            uint32_t pushed = 0;
                            const auto& segs = ast_.path_segs();
                            const uint64_t begin = s.nest_path_begin;
                            const uint64_t end = begin + s.nest_path_count;
                            if (begin <= segs.size() && end <= segs.size()) {
                                for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                                    ns.push_back(std::string(segs[s.nest_path_begin + i]));
                                    ++pushed;
                                }
                            }
                            prune_stmt_(s.a, ns);
                            while (pushed > 0) {
                                ns.pop_back();
                                --pushed;
                            }
                        }
                        return;

                    default:
                        return;
                }
            }

            static ParamExpected param_expected_(const ast::AstArena& arena, const ast::Param& p) {
                if (p.type_node == ast::k_invalid_type_node ||
                    static_cast<size_t>(p.type_node) >= arena.type_nodes().size()) {
                    return ParamExpected::kAny;
                }
                const auto& tn = arena.type_node(p.type_node);
                if (tn.kind != ast::TypeNodeKind::kNamedPath || tn.path_count == 0) {
                    return ParamExpected::kAny;
                }
                const auto& segs = arena.path_segs();
                if (tn.path_begin + tn.path_count > segs.size()) return ParamExpected::kAny;
                std::string name = std::string(segs[tn.path_begin + tn.path_count - 1]);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (name == "bool") return ParamExpected::kBool;
                if (name == "string") return ParamExpected::kString;
                if (!name.empty() && (name[0] == 'i' || name[0] == 'u')) {
                    bool all_digits = true;
                    for (size_t i = 1; i < name.size(); ++i) {
                        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                            all_digits = false;
                            break;
                        }
                    }
                    if (all_digits) return ParamExpected::kInt;
                }
                return ParamExpected::kAny;
            }

            static bool arg_matches_param_(const InstValue& v, ParamExpected ex) {
                switch (ex) {
                    case ParamExpected::kAny: return true;
                    case ParamExpected::kBool: return v.kind == InstValue::Kind::kBool;
                    case ParamExpected::kInt: return v.kind == InstValue::Kind::kInt;
                    case ParamExpected::kString: return v.kind == InstValue::Kind::kString;
                }
                return false;
            }

            std::optional<std::string> callee_name_(const ast::AstArena& arena, const ast::Expr& call) {
                if (!is_valid_expr_(arena, call.a)) return std::nullopt;
                const auto& ce = arena.expr(call.a);
                if (ce.kind != ast::ExprKind::kIdent || ce.text.empty()) return std::nullopt;
                return std::string(ce.text);
            }

            std::vector<std::string> resolution_candidates_(std::string_view raw, const std::vector<std::string>& ns) {
                std::vector<std::string> out{};
                if (raw.empty()) return out;
                if (raw.find("::") != std::string_view::npos) {
                    out.emplace_back(raw);
                    return out;
                }
                for (size_t depth = ns.size(); depth > 0; --depth) {
                    std::string q{};
                    for (size_t i = 0; i < depth; ++i) {
                        if (i) q += "::";
                        q += ns[i];
                    }
                    q += "::";
                    q += std::string(raw);
                    out.push_back(std::move(q));
                }
                out.emplace_back(raw);
                return out;
            }

            std::optional<InstRef> ensure_external_inst_loaded_(std::string_view qname, Span use_span) {
                auto it_payload = external_payloads_.find(std::string(qname));
                if (it_payload == external_payloads_.end()) return std::nullopt;
                if (it_payload->second.empty()) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, qname);
                    return std::nullopt;
                }
                auto it_cached = external_cache_.find(std::string(qname));
                if (it_cached != external_cache_.end()) {
                    InstRef ref{};
                    ref.qname = std::string(qname);
                    ref.ast = &it_cached->second->ast;
                    ref.sid = it_cached->second->sid;
                    ref.ext_owner = it_cached->second;
                    return ref;
                }

                auto unit = std::make_shared<ExternalInstUnit>();
                SourceManager sm{};
                diag::Bag local_bag{};
                const uint32_t fid = sm.add("<external-inst>", it_payload->second);
                Lexer lex(sm.content(fid), fid, &local_bag);
                auto toks = lex.lex_all();
                ParserFeatureFlags flags{};
                Parser p(toks, unit->ast, unit->types, &local_bag, /*max_errors=*/32, flags);
                unit->root = p.parse_program();
                if (local_bag.has_error()) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, qname);
                    return std::nullopt;
                }

                if (!is_valid_stmt_(unit->ast, unit->root)) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, qname);
                    return std::nullopt;
                }
                const auto& root_s = unit->ast.stmt(unit->root);
                if (root_s.kind != ast::StmtKind::kBlock) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, qname);
                    return std::nullopt;
                }
                const auto& kids = unit->ast.stmt_children();
                const uint64_t begin = root_s.stmt_begin;
                const uint64_t end = begin + root_s.stmt_count;
                if (begin > kids.size() || end > kids.size()) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, qname);
                    return std::nullopt;
                }
                for (uint32_t i = 0; i < root_s.stmt_count; ++i) {
                    const auto sid = kids[root_s.stmt_begin + i];
                    if (!is_valid_stmt_(unit->ast, sid)) continue;
                    if (unit->ast.stmt(sid).kind == ast::StmtKind::kInstDecl) {
                        unit->sid = sid;
                        break;
                    }
                }
                if (unit->sid == ast::k_invalid_stmt) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, qname);
                    return std::nullopt;
                }

                external_cache_.insert({std::string(qname), unit});
                InstRef ref{};
                ref.qname = std::string(qname);
                ref.ast = &unit->ast;
                ref.sid = unit->sid;
                ref.ext_owner = unit;
                return ref;
            }

            std::optional<InstRef> resolve_inst_(std::string_view raw, const std::vector<std::string>& ns, Span use_span) {
                const auto candidates = resolution_candidates_(raw, ns);
                for (const auto& c : candidates) {
                    auto it_local = local_insts_.find(c);
                    if (it_local != local_insts_.end()) {
                        return it_local->second;
                    }
                    if (auto ext = ensure_external_inst_loaded_(c, use_span)) {
                        return ext;
                    }
                }
                return std::nullopt;
            }

            std::optional<InstValue> eval_expr_(
                const ast::AstArena& arena,
                ast::ExprId eid,
                std::unordered_map<std::string, InstValue>& env,
                const std::vector<std::string>& ns,
                Span use_span
            ) {
                if (!is_valid_expr_(arena, eid)) {
                    report_(bag_, diag::Code::kDirectiveInstExprUnsupported, use_span);
                    return std::nullopt;
                }
                const auto& e = arena.expr(eid);
                switch (e.kind) {
                    case ast::ExprKind::kBoolLit: {
                        InstValue v{};
                        v.kind = InstValue::Kind::kBool;
                        v.b = (e.text == "true");
                        return v;
                    }
                    case ast::ExprKind::kIntLit: {
                        int64_t iv = 0;
                        if (!parse_int_lit_(e.text, iv)) {
                            report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                            return std::nullopt;
                        }
                        InstValue v{};
                        v.kind = InstValue::Kind::kInt;
                        v.i = iv;
                        return v;
                    }
                    case ast::ExprKind::kStringLit: {
                        InstValue v{};
                        v.kind = InstValue::Kind::kString;
                        v.s = std::string(e.text);
                        return v;
                    }
                    case ast::ExprKind::kIdent: {
                        auto it = env.find(std::string(e.text));
                        if (it == env.end()) {
                            report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                            return std::nullopt;
                        }
                        return it->second;
                    }
                    case ast::ExprKind::kUnary: {
                        if (e.op == syntax::TokenKind::kAmp ||
                            e.op == syntax::TokenKind::kTilde ||
                            e.op == syntax::TokenKind::kCaretAmp) {
                            report_(bag_, diag::Code::kDirectiveInstForbiddenOperator, e.span);
                            return std::nullopt;
                        }
                        if (e.op != syntax::TokenKind::kKwNot) {
                            report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                            return std::nullopt;
                        }
                        auto inner = eval_expr_(arena, e.a, env, ns, e.span);
                        if (!inner.has_value() || inner->kind != InstValue::Kind::kBool) {
                            report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                            return std::nullopt;
                        }
                        InstValue out{};
                        out.kind = InstValue::Kind::kBool;
                        out.b = !inner->b;
                        return out;
                    }
                    case ast::ExprKind::kBinary: {
                        if (e.op == syntax::TokenKind::kAmp ||
                            e.op == syntax::TokenKind::kTilde ||
                            e.op == syntax::TokenKind::kCaretAmp) {
                            report_(bag_, diag::Code::kDirectiveInstForbiddenOperator, e.span);
                            return std::nullopt;
                        }
                        auto lhs = eval_expr_(arena, e.a, env, ns, e.span);
                        auto rhs = eval_expr_(arena, e.b, env, ns, e.span);
                        if (!lhs.has_value() || !rhs.has_value()) return std::nullopt;

                        if (e.op == syntax::TokenKind::kKwAnd || e.op == syntax::TokenKind::kKwOr) {
                            if (lhs->kind != InstValue::Kind::kBool || rhs->kind != InstValue::Kind::kBool) {
                                report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                                return std::nullopt;
                            }
                            InstValue out{};
                            out.kind = InstValue::Kind::kBool;
                            out.b = (e.op == syntax::TokenKind::kKwAnd) ? (lhs->b && rhs->b) : (lhs->b || rhs->b);
                            return out;
                        }
                        if (e.op == syntax::TokenKind::kEqEq || e.op == syntax::TokenKind::kBangEq) {
                            const bool eq = value_eq_(*lhs, *rhs);
                            InstValue out{};
                            out.kind = InstValue::Kind::kBool;
                            out.b = (e.op == syntax::TokenKind::kEqEq) ? eq : !eq;
                            return out;
                        }
                        report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                        return std::nullopt;
                    }
                    case ast::ExprKind::kCall: {
                        auto callee = callee_name_(arena, e);
                        if (!callee.has_value()) {
                            report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                            return std::nullopt;
                        }
                        return eval_call_by_name_(arena, *callee, e, env, ns, e.span);
                    }
                    default:
                        report_(bag_, diag::Code::kDirectiveInstExprUnsupported, e.span);
                        return std::nullopt;
                }
            }

            ExecResult eval_stmt_(
                const ast::AstArena& arena,
                ast::StmtId sid,
                std::unordered_map<std::string, InstValue>& env,
                const std::vector<std::string>& ns
            ) {
                ExecResult out{};
                if (!is_valid_stmt_(arena, sid)) {
                    out.ok = false;
                    return out;
                }
                const auto& s = arena.stmt(sid);

                switch (s.kind) {
                    case ast::StmtKind::kEmpty:
                        return out;

                    case ast::StmtKind::kBlock: {
                        const auto& kids = arena.stmt_children();
                        const uint64_t begin = s.stmt_begin;
                        const uint64_t end = begin + s.stmt_count;
                        if (begin > kids.size() || end > kids.size()) {
                            out.ok = false;
                            return out;
                        }
                        for (uint32_t i = 0; i < s.stmt_count; ++i) {
                            auto r = eval_stmt_(arena, kids[s.stmt_begin + i], env, ns);
                            if (!r.ok) return r;
                            if (r.returned) return r;
                        }
                        return out;
                    }

                    case ast::StmtKind::kVar: {
                        if (s.is_set || s.is_static || s.is_const || s.name.empty()) {
                            report_(bag_, diag::Code::kDirectiveInstBodyUnsupportedStmt, s.span);
                            out.ok = false;
                            return out;
                        }
                        auto v = eval_expr_(arena, s.init, env, ns, s.span);
                        if (!v.has_value()) {
                            out.ok = false;
                            return out;
                        }
                        env[std::string(s.name)] = *v;
                        return out;
                    }

                    case ast::StmtKind::kIf: {
                        auto cond = eval_expr_(arena, s.expr, env, ns, s.span);
                        if (!cond.has_value() || cond->kind != InstValue::Kind::kBool) {
                            report_(bag_, diag::Code::kDirectiveInstExprUnsupported, s.span);
                            out.ok = false;
                            return out;
                        }
                        const ast::StmtId target = cond->b ? s.a : s.b;
                        if (target == ast::k_invalid_stmt) return out;
                        return eval_stmt_(arena, target, env, ns);
                    }

                    case ast::StmtKind::kReturn: {
                        auto v = eval_expr_(arena, s.expr, env, ns, s.span);
                        if (!v.has_value() || v->kind != InstValue::Kind::kBool) {
                            report_(bag_, diag::Code::kDirectiveInstReturnMustBeBool, s.span);
                            out.ok = false;
                            return out;
                        }
                        out.returned = true;
                        out.value = *v;
                        return out;
                    }

                    default:
                        report_(bag_, diag::Code::kDirectiveInstBodyUnsupportedStmt, s.span);
                        out.ok = false;
                        return out;
                }
            }

            std::optional<InstValue> eval_inst_call_(
                const InstRef& inst,
                const std::vector<InstValue>& args,
                Span use_span
            ) {
                if (inst.ast == nullptr || !is_valid_stmt_(*inst.ast, inst.sid)) {
                    report_(bag_, diag::Code::kDirectiveInstUnknown, use_span, inst.qname);
                    return std::nullopt;
                }
                if (std::find(call_stack_.begin(), call_stack_.end(), inst.qname) != call_stack_.end()) {
                    report_(bag_, diag::Code::kDirectiveInstRecursion, use_span, inst.qname);
                    return std::nullopt;
                }

                const auto& arena = *inst.ast;
                const auto& decl = arena.stmt(inst.sid);
                if (decl.kind != ast::StmtKind::kInstDecl) {
                    report_(bag_, diag::Code::kDirectiveInstUnknown, use_span, inst.qname);
                    return std::nullopt;
                }
                if (decl.param_count != args.size()) {
                    report_(bag_, diag::Code::kDirectiveInstCallArgMismatch, use_span, inst.qname);
                    return std::nullopt;
                }

                std::unordered_map<std::string, InstValue> env{};
                const auto& params = arena.params();
                const uint64_t pb = decl.param_begin;
                const uint64_t pe = pb + decl.param_count;
                if (pb > params.size() || pe > params.size()) {
                    report_(bag_, diag::Code::kDirectiveInstExternalPayloadInvalid, use_span, inst.qname);
                    return std::nullopt;
                }
                for (uint32_t i = 0; i < decl.param_count; ++i) {
                    const auto& p = params[decl.param_begin + i];
                    const auto expected = param_expected_(arena, p);
                    if (!arg_matches_param_(args[i], expected)) {
                        report_(bag_, diag::Code::kDirectiveInstCallArgMismatch, p.span, inst.qname);
                        return std::nullopt;
                    }
                    env[std::string(p.name)] = args[i];
                }

                call_stack_.push_back(inst.qname);
                const auto ns = split_ns_(inst.qname);
                auto r = eval_stmt_(arena, decl.a, env, ns);
                call_stack_.pop_back();
                if (!r.ok) return std::nullopt;
                if (!r.returned) {
                    report_(bag_, diag::Code::kDirectiveInstMissingReturn, decl.span, inst.qname);
                    return std::nullopt;
                }
                return r.value;
            }

            std::optional<InstValue> eval_call_by_name_(
                const ast::AstArena& arena,
                std::string_view raw_name,
                const ast::Expr& call_expr,
                std::unordered_map<std::string, InstValue>& env,
                const std::vector<std::string>& ns,
                Span use_span
            ) {
                const auto& args = arena.args();
                const uint64_t begin = call_expr.arg_begin;
                const uint64_t end = begin + call_expr.arg_count;
                if (begin > args.size() || end > args.size()) {
                    report_(bag_, diag::Code::kDirectiveInstCallArgMismatch, use_span, raw_name);
                    return std::nullopt;
                }

                std::vector<InstValue> vals{};
                vals.reserve(call_expr.arg_count);
                for (uint32_t i = 0; i < call_expr.arg_count; ++i) {
                    const auto& a = args[call_expr.arg_begin + i];
                    if (a.has_label || a.is_hole) {
                        report_(bag_, diag::Code::kDirectiveInstCallArgMismatch, a.span, raw_name);
                        return std::nullopt;
                    }
                    auto v = eval_expr_(arena, a.expr, env, ns, a.span);
                    if (!v.has_value()) return std::nullopt;
                    vals.push_back(*v);
                }

                if (raw_name == "If") {
                    if (vals.size() != 1 || vals[0].kind != InstValue::Kind::kBool) {
                        report_(bag_, diag::Code::kDirectiveInstCallArgMismatch, use_span, raw_name);
                        return std::nullopt;
                    }
                    return vals[0];
                }

                auto inst = resolve_inst_(raw_name, ns, use_span);
                if (!inst.has_value()) {
                    report_(bag_, diag::Code::kDirectiveInstUnknown, use_span, raw_name);
                    return std::nullopt;
                }
                return eval_inst_call_(*inst, vals, use_span);
            }

            bool eval_directive_call_(ast::ExprId call_eid, const std::vector<std::string>& ns, Span sp) {
                if (!is_valid_expr_(ast_, call_eid)) return false;
                const auto& call = ast_.expr(call_eid);
                if (call.kind != ast::ExprKind::kCall) {
                    report_(bag_, diag::Code::kDirectiveCallExpected, sp);
                    return false;
                }
                auto callee = callee_name_(ast_, call);
                if (!callee.has_value()) {
                    report_(bag_, diag::Code::kDirectiveCallExpected, sp);
                    return false;
                }

                std::unordered_map<std::string, InstValue> env{};
                auto r = eval_call_by_name_(ast_, *callee, call, env, ns, sp);
                if (!r.has_value()) return false;
                if (r->kind != InstValue::Kind::kBool) {
                    report_(bag_, diag::Code::kDirectiveInstReturnMustBeBool, sp, *callee);
                    return false;
                }
                return r->b;
            }
        };

    } // namespace

    void evaluate_compiler_directives(
        ast::AstArena& ast,
        ast::StmtId program_root,
        diag::Bag& bag,
        const PassOptions& opt
    ) {
        if (program_root == ast::k_invalid_stmt) return;
        DirectiveEvaluator eval(ast, program_root, bag, opt);
        eval.run();
    }

} // namespace parus::passes
