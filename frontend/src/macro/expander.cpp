#include <parus/macro/Expander.hpp>

#include <parus/diag/DiagCode.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parus::macro {

    namespace {
        using K = syntax::TokenKind;

        enum class CallContext : uint8_t {
            kExpr,
            kStmt,
            kItem,
            kType,
        };

        struct TokenRange {
            uint32_t begin = 0;
            uint32_t count = 0;
        };

        struct CaptureBinding {
            std::string_view name{};
            bool variadic = false;
            std::vector<TokenRange> ranges{};
        };

        struct ArmMatchResult {
            bool ok = false;
            std::vector<CaptureBinding> captures{};
        };

        struct ExpandResult {
            bool ok = false;
            ast::MacroOutKind out_kind = ast::MacroOutKind::kExpr;
            std::vector<Token> tokens{};
            std::string_view macro_name{};
        };

        static void add_diag_(diag::Bag& diags, diag::Code code, Span span, std::string_view a0 = {}) {
            diag::Diagnostic d(diag::Severity::kError, code, span);
            if (!a0.empty()) d.add_arg(a0);
            diags.add(std::move(d));
        }

        static std::string_view path_last_seg_(const ast::AstArena& ast, uint32_t begin, uint32_t count) {
            if (count == 0) return {};
            const auto& segs = ast.path_segs();
            if (begin >= segs.size() || begin + count > segs.size()) return {};
            return segs[begin + count - 1];
        }

        static std::optional<size_t> find_decl_index_(
            const ast::AstArena& ast,
            std::string_view macro_name,
            uint32_t scope_depth
        ) {
            const auto& decls = ast.macro_decls();
            std::optional<size_t> best{};
            uint32_t best_depth = 0;
            for (size_t i = 0; i < decls.size(); ++i) {
                const auto& d = decls[i];
                if (d.name != macro_name) continue;
                if (d.scope_depth > scope_depth) continue;
                if (!best.has_value() || d.scope_depth > best_depth || (d.scope_depth == best_depth && i > *best)) {
                    best = i;
                    best_depth = d.scope_depth;
                }
            }
            return best;
        }

        static std::optional<size_t> find_group_index_(
            const ast::AstArena& ast,
            const ast::MacroDecl& decl,
            CallContext ctx
        ) {
            const auto& groups = ast.macro_groups();
            for (uint32_t i = 0; i < decl.group_count; ++i) {
                const auto gi = decl.group_begin + i;
                if (gi >= groups.size()) break;
                const auto& g = groups[gi];
                if (ctx == CallContext::kExpr && g.match_kind == ast::MacroMatchKind::kExpr) return gi;
                if (ctx == CallContext::kStmt && g.match_kind == ast::MacroMatchKind::kStmt) return gi;
                if (ctx == CallContext::kItem && g.match_kind == ast::MacroMatchKind::kItem) return gi;
                if (ctx == CallContext::kType && g.match_kind == ast::MacroMatchKind::kType) return gi;
            }
            return std::nullopt;
        }

        static std::vector<size_t> group_search_order_(
            const ast::AstArena& ast,
            const ast::MacroDecl& decl,
            CallContext ctx
        ) {
            std::vector<size_t> out{};
            const auto exact = find_group_index_(ast, decl, ctx);
            if (exact.has_value()) out.push_back(*exact);

            const auto& groups = ast.macro_groups();
            for (uint32_t i = 0; i < decl.group_count; ++i) {
                const auto gi = decl.group_begin + i;
                if (gi >= groups.size()) break;
                bool already = false;
                for (const auto seen : out) {
                    if (seen == gi) {
                        already = true;
                        break;
                    }
                }
                if (!already) out.push_back(gi);
            }
            return out;
        }

        static std::vector<TokenRange> split_top_level_args_(
            const ast::AstArena& ast,
            uint32_t begin,
            uint32_t count
        ) {
            std::vector<TokenRange> out{};
            if (count == 0) return out;

            const auto& toks = ast.macro_tokens();
            const uint32_t end = begin + count;
            uint32_t cur_begin = begin;
            int paren = 0;
            int brace = 0;
            int bracket = 0;

            for (uint32_t i = begin; i < end; ++i) {
                if (i >= toks.size()) break;
                const auto k = toks[i].kind;
                if (k == K::kLParen) { ++paren; continue; }
                if (k == K::kRParen && paren > 0) { --paren; continue; }
                if (k == K::kLBrace) { ++brace; continue; }
                if (k == K::kRBrace && brace > 0) { --brace; continue; }
                if (k == K::kLBracket) { ++bracket; continue; }
                if (k == K::kRBracket && bracket > 0) { --bracket; continue; }

                if (k == K::kComma && paren == 0 && brace == 0 && bracket == 0) {
                    const uint32_t seg_count = (i > cur_begin) ? (i - cur_begin) : 0;
                    if (seg_count > 0) {
                        out.push_back(TokenRange{cur_begin, seg_count});
                    }
                    cur_begin = i + 1;
                }
            }

            if (cur_begin < end) {
                out.push_back(TokenRange{cur_begin, end - cur_begin});
            }
            return out;
        }

        static bool is_path_tokens_(const ast::AstArena& ast, TokenRange r) {
            const auto& toks = ast.macro_tokens();
            if (r.count == 0) return false;
            bool expect_ident = true;
            for (uint32_t i = 0; i < r.count; ++i) {
                const auto idx = r.begin + i;
                if (idx >= toks.size()) return false;
                const auto& t = toks[idx];
                if (expect_ident) {
                    if (t.kind != K::kIdent) return false;
                    expect_ident = false;
                    continue;
                }
                if (t.kind == K::kColonColon) {
                    expect_ident = true;
                    continue;
                }
                if (t.kind == K::kColon) {
                    const auto nidx = idx + 1;
                    if (nidx >= toks.size()) return false;
                    if (toks[nidx].kind != K::kColon) return false;
                    ++i;
                    expect_ident = true;
                    continue;
                }
                return false;
            }
            return !expect_ident;
        }

        static bool validate_fragment_(
            const ast::AstArena& ast,
            TokenRange r,
            ast::MacroFragKind frag,
            Span call_span
        ) {
            if (frag == ast::MacroFragKind::kExpr ||
                frag == ast::MacroFragKind::kStmt ||
                frag == ast::MacroFragKind::kItem ||
                frag == ast::MacroFragKind::kType ||
                frag == ast::MacroFragKind::kTt) {
                return r.count > 0;
            }
            if (frag == ast::MacroFragKind::kIdent) {
                const auto& toks = ast.macro_tokens();
                if (r.count != 1 || r.begin >= toks.size()) return false;
                return toks[r.begin].kind == K::kIdent;
            }
            if (frag == ast::MacroFragKind::kPath) {
                return is_path_tokens_(ast, r);
            }
            if (frag == ast::MacroFragKind::kBlock) {
                const auto& toks = ast.macro_tokens();
                if (r.count < 2 || r.begin >= toks.size()) return false;
                const uint32_t end = r.begin + r.count - 1;
                if (end >= toks.size()) return false;
                return toks[r.begin].kind == K::kLBrace && toks[end].kind == K::kRBrace;
            }
            (void)call_span;
            return false;
        }

        static ArmMatchResult match_typed_arm_(
            const ast::AstArena& ast,
            const ast::MacroArm& arm,
            uint32_t arg_begin,
            uint32_t arg_count,
            Span call_span
        ) {
            ArmMatchResult out{};
            const auto& caps = ast.macro_captures();

            std::vector<ast::MacroTypedCapture> capture_vec{};
            capture_vec.reserve(arm.capture_count);
            for (uint32_t i = 0; i < arm.capture_count; ++i) {
                const auto ci = arm.capture_begin + i;
                if (ci >= caps.size()) return out;
                capture_vec.push_back(caps[ci]);
            }

            const auto args = split_top_level_args_(ast, arg_begin, arg_count);

            int variadic_idx = -1;
            for (uint32_t i = 0; i < capture_vec.size(); ++i) {
                if (capture_vec[i].variadic) {
                    variadic_idx = static_cast<int>(i);
                    break;
                }
            }

            if (variadic_idx >= 0 && variadic_idx != static_cast<int>(capture_vec.size()) - 1) {
                return out;
            }

            std::vector<CaptureBinding> binds{};
            binds.reserve(capture_vec.size());
            if (variadic_idx < 0) {
                if (args.size() != capture_vec.size()) return out;
                for (uint32_t i = 0; i < capture_vec.size(); ++i) {
                    const auto& c = capture_vec[i];
                    if (!validate_fragment_(ast, args[i], c.frag, call_span)) return out;
                    CaptureBinding b{};
                    b.name = c.name;
                    b.variadic = false;
                    b.ranges.push_back(args[i]);
                    binds.push_back(std::move(b));
                }
            } else {
                const uint32_t fixed = static_cast<uint32_t>(variadic_idx);
                if (args.size() < fixed) return out;
                for (uint32_t i = 0; i < fixed; ++i) {
                    const auto& c = capture_vec[i];
                    if (!validate_fragment_(ast, args[i], c.frag, call_span)) return out;
                    CaptureBinding b{};
                    b.name = c.name;
                    b.variadic = false;
                    b.ranges.push_back(args[i]);
                    binds.push_back(std::move(b));
                }
                CaptureBinding vb{};
                vb.name = capture_vec[fixed].name;
                vb.variadic = true;
                for (uint32_t i = fixed; i < args.size(); ++i) {
                    if (!validate_fragment_(ast, args[i], capture_vec[fixed].frag, call_span)) return out;
                    vb.ranges.push_back(args[i]);
                }
                binds.push_back(std::move(vb));
            }

            out.ok = true;
            out.captures = std::move(binds);
            return out;
        }

        static bool is_ellipsis_at_(
            const std::vector<Token>& toks,
            size_t i
        ) {
            if (i >= toks.size()) return false;
            if (toks[i].kind == K::kDotDot) {
                return (i + 1 < toks.size() && toks[i + 1].kind == K::kDot);
            }
            if (toks[i].kind == K::kDot) {
                return i + 2 < toks.size() && toks[i + 1].kind == K::kDot && toks[i + 2].kind == K::kDot;
            }
            return false;
        }

        static size_t ellipsis_token_count_at_(const std::vector<Token>& toks, size_t i) {
            if (i >= toks.size()) return 0;
            if (toks[i].kind == K::kDotDot) {
                if (i + 1 < toks.size() && toks[i + 1].kind == K::kDot) return 2;
                return 0;
            }
            if (toks[i].kind == K::kDot &&
                i + 2 < toks.size() &&
                toks[i + 1].kind == K::kDot &&
                toks[i + 2].kind == K::kDot) {
                return 3;
            }
            return 0;
        }

        static std::optional<size_t> parse_positional_index_(const Token& t) {
            if (t.kind != K::kIntLit) return std::nullopt;
            size_t out = 0;
            const auto sv = t.lexeme;
            const auto* begin = sv.data();
            const auto* end = begin + sv.size();
            auto [ptr, ec] = std::from_chars(begin, end, out, 10);
            if (ec != std::errc{} || ptr != end) return std::nullopt;
            return out;
        }

        static bool substitute_template_(
            ast::AstArena& ast,
            const ast::MacroArm& arm,
            const std::vector<CaptureBinding>& captures,
            Span call_span,
            std::vector<Token>& out_tokens
        ) {
            out_tokens.clear();
            std::vector<uint8_t> generated_mask{};
            const auto& tpl = ast.macro_tokens();
            const auto& arg_toks = ast.macro_tokens();
            const size_t begin = arm.template_token_begin;
            const size_t end = arm.template_token_begin + arm.template_token_count;

            auto emit_range = [&](TokenRange r) {
                for (uint32_t i = 0; i < r.count; ++i) {
                    const uint32_t idx = r.begin + i;
                    if (idx >= arg_toks.size()) break;
                    out_tokens.push_back(arg_toks[idx]);
                    generated_mask.push_back(0);
                }
            };

            for (size_t i = begin; i < end && i < tpl.size(); ++i) {
                const auto& t = tpl[i];
                if (t.kind != K::kDollar || i + 1 >= end || i + 1 >= tpl.size()) {
                    out_tokens.push_back(t);
                    generated_mask.push_back(1);
                    continue;
                }

                const Token& ref = tpl[i + 1];
                std::optional<size_t> idx{};
                if (ref.kind == K::kIdent) {
                    for (size_t c = 0; c < captures.size(); ++c) {
                        if (captures[c].name == ref.lexeme) {
                            idx = c;
                            break;
                        }
                    }
                } else {
                    idx = parse_positional_index_(ref);
                }

                if (!idx.has_value() || *idx >= captures.size()) {
                    return false;
                }

                bool variadic = false;
                size_t variadic_tok_count = 0;
                if (i + 2 < end && i + 2 < tpl.size() && is_ellipsis_at_(tpl, i + 2)) {
                    variadic = true;
                    variadic_tok_count = ellipsis_token_count_at_(tpl, i + 2);
                }

                const auto& cap = captures[*idx];
                if (variadic) {
                    for (size_t r = 0; r < cap.ranges.size(); ++r) {
                        emit_range(cap.ranges[r]);
                        if (r + 1 < cap.ranges.size()) {
                            Token comma{};
                            comma.kind = K::kComma;
                            comma.lexeme = ",";
                            comma.span = call_span;
                            out_tokens.push_back(comma);
                            generated_mask.push_back(1);
                        }
                    }
                    i += 1 + variadic_tok_count;
                } else {
                    if (cap.ranges.empty()) return false;
                    emit_range(cap.ranges.front());
                    i += 1;
                }
            }

            apply_binder_hygiene(ast, out_tokens, generated_mask);
            return true;
        }

        static bool expand_macro_call_to_tokens_(
            MacroExpansionContext& ctx,
            std::string_view macro_name,
            uint32_t path_begin,
            uint32_t path_count,
            uint32_t arg_begin,
            uint32_t arg_count,
            Span call_span,
            uint32_t scope_depth,
            CallContext call_ctx,
            ExpandResult& out
        ) {
            (void)path_begin;
            (void)path_count;
            ++ctx.steps;
            if (ctx.steps > ctx.budget.max_steps) {
                add_diag_(ctx.diags, diag::Code::kMacroRecursionBudget, call_span, macro_name);
                return false;
            }

            if (ctx.stack.size() >= ctx.budget.max_depth) {
                add_diag_(ctx.diags, diag::Code::kMacroRecursionBudget, call_span, macro_name);
                return false;
            }

            const auto decl_index = find_decl_index_(ctx.ast, macro_name, scope_depth);
            if (!decl_index.has_value()) {
                add_diag_(ctx.diags, diag::Code::kMacroNoMatch, call_span, macro_name);
                return false;
            }

            const auto& decl = ctx.ast.macro_decls()[*decl_index];
            const auto group_order = group_search_order_(ctx.ast, decl, call_ctx);
            if (group_order.empty()) {
                add_diag_(ctx.diags, diag::Code::kMacroNoMatch, call_span, macro_name);
                return false;
            }
            const auto& arms = ctx.ast.macro_arms();
            for (const auto gi : group_order) {
                if (gi >= ctx.ast.macro_groups().size()) continue;
                const auto& group = ctx.ast.macro_groups()[gi];
                if (group.phase2_token_group || group.match_kind == ast::MacroMatchKind::kToken) {
                    add_diag_(ctx.diags, diag::Code::kMacroTokenUnimplemented, call_span);
                    return false;
                }
                for (uint32_t i = 0; i < group.arm_count; ++i) {
                    const auto ai = group.arm_begin + i;
                    if (ai >= arms.size()) break;
                    const auto& arm = arms[ai];
                    const auto match = match_typed_arm_(ctx.ast, arm, arg_begin, arg_count, call_span);
                    if (!match.ok) continue;

                    std::vector<Token> sub{};
                    if (!substitute_template_(ctx.ast, arm, match.captures, call_span, sub)) {
                        add_diag_(ctx.diags, diag::Code::kMacroReparseFail, call_span, macro_name);
                        return false;
                    }
                    if (sub.size() > ctx.budget.max_output_tokens) {
                        add_diag_(ctx.diags, diag::Code::kMacroRecursionBudget, call_span, macro_name);
                        return false;
                    }

                    out.ok = true;
                    out.out_kind = arm.out_kind;
                    out.tokens = std::move(sub);
                    out.macro_name = macro_name;
                    return true;
                }
            }

            add_diag_(ctx.diags, diag::Code::kMacroNoMatch, call_span, macro_name);
            return false;
        }

        static void append_eof_(std::vector<Token>& toks, Span fallback) {
            Token eof{};
            eof.kind = K::kEof;
            eof.span = toks.empty() ? fallback : toks.back().span;
            toks.push_back(eof);
        }

        static std::optional<ast::ExprId> reparse_expr_(
            MacroExpansionContext& ctx,
            std::vector<Token> toks,
            Span span
        ) {
            append_eof_(toks, span);
            Parser p(toks, ctx.ast, ctx.types, &ctx.diags, 128);
            const auto e = p.parse_expr_full();
            if (e == ast::k_invalid_expr) return std::nullopt;
            if (ctx.ast.expr(e).kind == ast::ExprKind::kError) return std::nullopt;
            return e;
        }

        static std::optional<ast::StmtId> reparse_single_stmt_(
            MacroExpansionContext& ctx,
            std::vector<Token> toks,
            Span span
        ) {
            append_eof_(toks, span);
            Parser p(toks, ctx.ast, ctx.types, &ctx.diags, 128);
            const auto root = p.parse_program();
            if (root == ast::k_invalid_stmt) return std::nullopt;
            const auto& b = ctx.ast.stmt(root);
            if (b.kind != ast::StmtKind::kBlock || b.stmt_count != 1) return std::nullopt;
            const auto idx = b.stmt_begin;
            if (idx >= ctx.ast.stmt_children().size()) return std::nullopt;
            (void)span;
            return ctx.ast.stmt_children()[idx];
        }

        static std::optional<ast::TypeNodeId> reparse_type_(
            MacroExpansionContext& ctx,
            std::vector<Token> toks,
            Span span,
            ty::TypeId* out_ty = nullptr
        ) {
            append_eof_(toks, span);
            Parser p(toks, ctx.ast, ctx.types, &ctx.diags, 128);
            const auto n = p.parse_type_full_for_macro(out_ty);
            if (n == ast::k_invalid_type_node) return std::nullopt;
            return n;
        }

        struct ExpandWalk {
            MacroExpansionContext& ctx;

            bool expand_type_node(ast::TypeNodeId& nid, uint32_t scope_depth, uint32_t depth) {
                if (nid == ast::k_invalid_type_node || nid >= ctx.ast.type_nodes().size()) return true;
                const auto n = ctx.ast.type_node(nid);

                switch (n.kind) {
                    case ast::TypeNodeKind::kOptional:
                    case ast::TypeNodeKind::kArray:
                    case ast::TypeNodeKind::kBorrow:
                    case ast::TypeNodeKind::kEscape:
                    case ast::TypeNodeKind::kPtr: {
                        auto elem = n.elem;
                        if (!expand_type_node(elem, scope_depth, depth)) return false;
                        if (nid >= ctx.ast.type_nodes().size()) return false;
                        ctx.ast.type_node_mut(nid).elem = elem;
                        return true;
                    }

                    case ast::TypeNodeKind::kFn: {
                        auto fn_ret = n.fn_ret;
                        if (!expand_type_node(fn_ret, scope_depth, depth)) return false;
                        if (nid >= ctx.ast.type_nodes().size()) return false;
                        ctx.ast.type_node_mut(nid).fn_ret = fn_ret;
                        for (uint32_t i = 0; i < n.fn_param_count; ++i) {
                            const auto ci = n.fn_param_begin + i;
                            if (ci >= ctx.ast.type_node_children().size()) break;
                            auto child = ctx.ast.type_node_children()[ci];
                            if (!expand_type_node(child, scope_depth, depth)) return false;
                            if (ci >= ctx.ast.type_node_children().size()) return false;
                            ctx.ast.type_node_children_mut()[ci] = child;
                        }
                        return true;
                    }

                    case ast::TypeNodeKind::kMacroCall: {
                        if (depth >= ctx.budget.max_depth) {
                            add_diag_(ctx.diags, diag::Code::kMacroRecursionBudget, n.span, "type");
                            return false;
                        }
                        const auto macro_name = path_last_seg_(ctx.ast, n.macro_path_begin, n.macro_path_count);
                        if (macro_name.empty()) {
                            add_diag_(ctx.diags, diag::Code::kMacroNoMatch, n.span);
                            return false;
                        }

                        ExpandResult ex{};
                        if (!expand_macro_call_to_tokens_(
                                ctx,
                                macro_name,
                                n.macro_path_begin,
                                n.macro_path_count,
                                n.macro_arg_begin,
                                n.macro_arg_count,
                                n.span,
                                scope_depth,
                                CallContext::kType,
                                ex)) {
                            return false;
                        }
                        if (ex.out_kind != ast::MacroOutKind::kType) {
                            add_diag_(ctx.diags, diag::Code::kMacroReparseFail, n.span, macro_name);
                            return false;
                        }

                        ty::TypeId parsed_ty = ty::kInvalidType;
                        auto parsed = reparse_type_(ctx, std::move(ex.tokens), n.span, &parsed_ty);
                        if (!parsed.has_value()) {
                            add_diag_(ctx.diags, diag::Code::kMacroReparseFail, n.span, macro_name);
                            return false;
                        }

                        nid = *parsed;
                        if (nid != ast::k_invalid_type_node && nid < ctx.ast.type_nodes().size()) {
                            ctx.ast.type_node_mut(nid).resolved_type = parsed_ty;
                        }
                        return expand_type_node(nid, scope_depth, depth + 1);
                    }

                    case ast::TypeNodeKind::kError:
                    case ast::TypeNodeKind::kNamedPath:
                        return true;
                }
                return true;
            }

            bool expand_expr(ast::ExprId& eid, uint32_t scope_depth, uint32_t depth) {
                if (eid == ast::k_invalid_expr || eid >= ctx.ast.exprs().size()) return true;
                const auto e = ctx.ast.expr(eid);

                switch (e.kind) {
                    case ast::ExprKind::kUnary:
                    case ast::ExprKind::kPostfixUnary: {
                        auto a = e.a;
                        if (!expand_expr(a, scope_depth, depth)) return false;
                        if (eid >= ctx.ast.exprs().size()) return false;
                        ctx.ast.expr_mut(eid).a = a;
                        break;
                    }
                    case ast::ExprKind::kCast:
                    {
                        auto a = e.a;
                        auto cast_ty = e.cast_type_node;
                        if (!expand_expr(a, scope_depth, depth)) return false;
                        if (!expand_type_node(cast_ty, scope_depth, depth)) return false;
                        if (eid >= ctx.ast.exprs().size()) return false;
                        auto& em = ctx.ast.expr_mut(eid);
                        em.a = a;
                        em.cast_type_node = cast_ty;
                        break;
                    }
                    case ast::ExprKind::kBinary:
                    case ast::ExprKind::kAssign:
                    case ast::ExprKind::kCall:
                    case ast::ExprKind::kIndex: {
                        auto a = e.a;
                        auto b = e.b;
                        if (!expand_expr(a, scope_depth, depth)) return false;
                        if (!expand_expr(b, scope_depth, depth)) return false;
                        if (eid >= ctx.ast.exprs().size()) return false;
                        auto& em = ctx.ast.expr_mut(eid);
                        em.a = a;
                        em.b = b;
                        break;
                    }
                    case ast::ExprKind::kTernary:
                    {
                        auto a = e.a;
                        auto b = e.b;
                        auto c = e.c;
                        if (!expand_expr(a, scope_depth, depth)) return false;
                        if (!expand_expr(b, scope_depth, depth)) return false;
                        if (!expand_expr(c, scope_depth, depth)) return false;
                        if (eid >= ctx.ast.exprs().size()) return false;
                        auto& em = ctx.ast.expr_mut(eid);
                        em.a = a;
                        em.b = b;
                        em.c = c;
                        break;
                    }
                    case ast::ExprKind::kIfExpr:
                    {
                        auto a = e.a;
                        if (!expand_expr(a, scope_depth, depth)) return false;
                        if (eid >= ctx.ast.exprs().size()) return false;
                        ctx.ast.expr_mut(eid).a = a;
                        break;
                    }
                    default:
                        break;
                }

                if (eid >= ctx.ast.exprs().size()) return false;
                const auto em = ctx.ast.expr(eid);
                if (em.kind != ast::ExprKind::kMacroCall) return true;
                if (depth >= ctx.budget.max_depth) {
                    add_diag_(ctx.diags, diag::Code::kMacroRecursionBudget, em.span, "expr");
                    return false;
                }

                const auto macro_name = path_last_seg_(ctx.ast, em.macro_path_begin, em.macro_path_count);
                if (macro_name.empty()) {
                    add_diag_(ctx.diags, diag::Code::kMacroNoMatch, em.span);
                    return false;
                }

                ExpandResult ex{};
                if (!expand_macro_call_to_tokens_(
                        ctx,
                        macro_name,
                        em.macro_path_begin,
                        em.macro_path_count,
                        em.macro_token_begin,
                        em.macro_token_count,
                        em.span,
                        scope_depth,
                        CallContext::kExpr,
                        ex)) {
                    return false;
                }

                if (ex.out_kind != ast::MacroOutKind::kExpr) {
                    add_diag_(ctx.diags, diag::Code::kMacroReparseFail, em.span, macro_name);
                    return false;
                }

                auto parsed = reparse_expr_(ctx, std::move(ex.tokens), em.span);
                if (!parsed.has_value()) {
                    add_diag_(ctx.diags, diag::Code::kMacroReparseFail, em.span, macro_name);
                    return false;
                }
                eid = *parsed;
                return expand_expr(eid, scope_depth, depth + 1);
            }

            bool expand_stmt(ast::StmtId& sid, uint32_t scope_depth, uint32_t depth) {
                if (sid == ast::k_invalid_stmt || sid >= ctx.ast.stmts().size()) return true;
                const auto s = ctx.ast.stmt(sid);

                if (s.kind == ast::StmtKind::kExprStmt &&
                    s.expr != ast::k_invalid_expr &&
                    s.expr < ctx.ast.exprs().size() &&
                    ctx.ast.expr(s.expr).kind == ast::ExprKind::kMacroCall) {
                    const auto mc = ctx.ast.expr(s.expr);
                    const auto macro_name = path_last_seg_(ctx.ast, mc.macro_path_begin, mc.macro_path_count);
                    if (macro_name.empty()) {
                        add_diag_(ctx.diags, diag::Code::kMacroNoMatch, mc.span);
                        return false;
                    }

                    const auto try_ctx = [&](CallContext cc, ExpandResult& out) {
                        return expand_macro_call_to_tokens_(
                            ctx,
                            macro_name,
                            mc.macro_path_begin,
                            mc.macro_path_count,
                            mc.macro_token_begin,
                            mc.macro_token_count,
                            mc.span,
                            scope_depth,
                            cc,
                            out);
                    };

                    ExpandResult ex{};
                    bool matched = try_ctx(CallContext::kStmt, ex);
                    if (!matched) {
                        matched = try_ctx(CallContext::kItem, ex);
                    }
                    if (!matched) {
                        matched = try_ctx(CallContext::kExpr, ex);
                    }
                    if (!matched) return false;

                    if (ex.out_kind == ast::MacroOutKind::kExpr) {
                        auto parsed = reparse_expr_(ctx, std::move(ex.tokens), mc.span);
                        if (!parsed.has_value()) {
                            add_diag_(ctx.diags, diag::Code::kMacroReparseFail, mc.span, macro_name);
                            return false;
                        }
                        if (sid >= ctx.ast.stmts().size()) return false;
                        ctx.ast.stmt_mut(sid).expr = *parsed;
                        auto expr_id = *parsed;
                        if (!expand_expr(expr_id, scope_depth, depth + 1)) return false;
                        if (sid >= ctx.ast.stmts().size()) return false;
                        ctx.ast.stmt_mut(sid).expr = expr_id;
                    } else {
                        auto parsed = reparse_single_stmt_(ctx, std::move(ex.tokens), mc.span);
                        if (!parsed.has_value()) {
                            add_diag_(ctx.diags, diag::Code::kMacroReparseFail, mc.span, macro_name);
                            return false;
                        }
                        sid = *parsed;
                        return expand_stmt(sid, scope_depth, depth + 1);
                    }
                }

                auto expr = ctx.ast.stmt(sid).expr;
                if (!expand_expr(expr, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).expr = expr;

                auto init = ctx.ast.stmt(sid).init;
                if (!expand_expr(init, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).init = init;

                auto type_node = ctx.ast.stmt(sid).type_node;
                if (!expand_type_node(type_node, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).type_node = type_node;

                auto fn_ret_type_node = ctx.ast.stmt(sid).fn_ret_type_node;
                if (!expand_type_node(fn_ret_type_node, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).fn_ret_type_node = fn_ret_type_node;

                auto acts_target_type_node = ctx.ast.stmt(sid).acts_target_type_node;
                if (!expand_type_node(acts_target_type_node, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).acts_target_type_node = acts_target_type_node;

                auto var_acts_target_type_node = ctx.ast.stmt(sid).var_acts_target_type_node;
                if (!expand_type_node(var_acts_target_type_node, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).var_acts_target_type_node = var_acts_target_type_node;

                const auto stmt_now = ctx.ast.stmt(sid);
                if (stmt_now.kind == ast::StmtKind::kFnDecl) {
                    auto& params = ctx.ast.params_mut();
                    for (uint32_t i = 0; i < stmt_now.param_count; ++i) {
                        const auto pi = stmt_now.param_begin + i;
                        if (pi >= params.size()) break;
                        if (!expand_type_node(params[pi].type_node, scope_depth, depth)) return false;
                    }
                }

                if (stmt_now.kind == ast::StmtKind::kFieldDecl) {
                    auto& members = ctx.ast.field_members_mut();
                    for (uint32_t i = 0; i < stmt_now.field_member_count; ++i) {
                        const auto fi = stmt_now.field_member_begin + i;
                        if (fi >= members.size()) break;
                        if (!expand_type_node(members[fi].type_node, scope_depth, depth)) return false;
                    }
                }

                uint32_t child_scope = scope_depth;
                if (stmt_now.kind == ast::StmtKind::kNestDecl) {
                    child_scope = scope_depth + 1;
                }

                auto a = stmt_now.a;
                if (!expand_stmt(a, child_scope, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).a = a;

                auto b = ctx.ast.stmt(sid).b;
                if (!expand_stmt(b, scope_depth, depth)) return false;
                if (sid >= ctx.ast.stmts().size()) return false;
                ctx.ast.stmt_mut(sid).b = b;

                const auto stmt_after_children = ctx.ast.stmt(sid);
                if (stmt_after_children.stmt_count > 0) {
                    for (uint32_t i = 0; i < stmt_after_children.stmt_count; ++i) {
                        const auto idx = stmt_after_children.stmt_begin + i;
                        if (idx >= ctx.ast.stmt_children().size()) break;
                        auto child = ctx.ast.stmt_children()[idx];
                        if (!expand_stmt(child, child_scope, depth)) return false;
                        if (idx >= ctx.ast.stmt_children().size()) return false;
                        ctx.ast.stmt_children_mut()[idx] = child;
                    }
                }
                return true;
            }
        };

    } // namespace

    ExpansionBudget default_budget_aot() {
        ExpansionBudget b{};
        b.max_depth = 64;
        b.max_steps = 20000;
        b.max_output_tokens = 200000;
        return b;
    }

    ExpansionBudget default_budget_jit() {
        ExpansionBudget b{};
        b.max_depth = 32;
        b.max_steps = 8000;
        b.max_output_tokens = 80000;
        return b;
    }

    BudgetClampResult clamp_budget(ExpansionBudget& budget) {
        BudgetClampResult out{};
        auto clamp_one = [&](uint32_t& v, uint32_t hard_max, bool& mark) {
            if (v == 0) {
                v = 1;
                mark = true;
                out.any = true;
                return;
            }
            if (v > hard_max) {
                v = hard_max;
                mark = true;
                out.any = true;
            }
        };

        clamp_one(budget.max_depth, k_macro_budget_hard_max_depth, out.depth);
        clamp_one(budget.max_steps, k_macro_budget_hard_max_steps, out.steps);
        clamp_one(budget.max_output_tokens, k_macro_budget_hard_max_output_tokens, out.output_tokens);
        return out;
    }

    bool expand_program(
        ast::AstArena& ast,
        ty::TypePool& types,
        ast::StmtId root,
        diag::Bag& diags,
        ExpansionBudget budget
    ) {
        (void)clamp_budget(budget);
        MacroExpansionContext ctx{ast, types, diags, budget};
        ExpandWalk walk{ctx};
        auto rid = root;
        if (!walk.expand_stmt(rid, 0, 0)) return false;
        for (ast::TypeNodeId i = 0; i < ast.type_nodes().size(); ++i) {
            auto nid = i;
            if (!walk.expand_type_node(nid, 0, 0)) return false;
        }
        return true;
    }

} // namespace parus::macro
