#include <parus/macro/Expander.hpp>

#include <parus/diag/DiagCode.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parus::macro {

    namespace {
        using K = syntax::TokenKind;

        static void add_diag_(diag::Bag& diags, diag::Code code, Span span, std::string_view a0 = {}) {
            diag::Diagnostic d(diag::Severity::kError, code, span);
            if (!a0.empty()) d.add_arg(a0);
            diags.add(std::move(d));
        }

        static bool is_open_delim_(K k) {
            return k == K::kLParen || k == K::kLBrace || k == K::kLBracket;
        }

        static bool is_close_delim_(K k) {
            return k == K::kRParen || k == K::kRBrace || k == K::kRBracket;
        }

        static K matching_close_(K open) {
            switch (open) {
                case K::kLParen: return K::kRParen;
                case K::kLBrace: return K::kRBrace;
                case K::kLBracket: return K::kRBracket;
                default: return K::kError;
            }
        }

        static bool token_literal_eq_(const Token& a, const Token& b) {
            if (a.kind != b.kind) return false;
            switch (a.kind) {
                case K::kIdent:
                case K::kHole:
                case K::kIntLit:
                case K::kFloatLit:
                case K::kStringLit:
                case K::kCharLit:
                    return a.lexeme == b.lexeme;
                default:
                    return true;
            }
        }

        static bool parse_frag_kind_(const Token& t, ast::MacroFragKind& out) {
            if (t.kind != K::kIdent) return false;
            if (t.lexeme == "expr") {
                out = ast::MacroFragKind::kExpr;
                return true;
            }
            if (t.lexeme == "stmt") {
                out = ast::MacroFragKind::kStmt;
                return true;
            }
            if (t.lexeme == "item") {
                out = ast::MacroFragKind::kItem;
                return true;
            }
            if (t.lexeme == "type") {
                out = ast::MacroFragKind::kType;
                return true;
            }
            if (t.lexeme == "path") {
                out = ast::MacroFragKind::kPath;
                return true;
            }
            if (t.lexeme == "ident") {
                out = ast::MacroFragKind::kIdent;
                return true;
            }
            if (t.lexeme == "block") {
                out = ast::MacroFragKind::kBlock;
                return true;
            }
            if (t.lexeme == "strlit") {
                out = ast::MacroFragKind::kStrLit;
                return true;
            }
            if (t.lexeme == "rawstrlit") {
                out = ast::MacroFragKind::kRawStrLit;
                return true;
            }
            if (t.lexeme == "tt") {
                out = ast::MacroFragKind::kTt;
                return true;
            }
            return false;
        }

        static bool is_quantifier_(K k) {
            return k == K::kStar || k == K::kPlus || k == K::kQuestion;
        }

        static uint32_t ellipsis_token_count_at_(const std::vector<Token>& toks, uint32_t i, uint32_t end) {
            if (i >= end) return 0;
            if (toks[i].kind == K::kDotDot) {
                if (i + 1 < end && toks[i + 1].kind == K::kDot) return 2;
                return 0;
            }
            if (toks[i].kind == K::kDot &&
                i + 2 < end &&
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
            auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out, 10);
            if (ec != std::errc{} || ptr != sv.data() + sv.size()) return std::nullopt;
            return out;
        }

        static std::optional<uint32_t> find_matching_close_token_(
            const std::vector<Token>& toks,
            uint32_t begin,
            uint32_t end,
            K open,
            K close
        ) {
            if (begin >= end || begin >= toks.size()) return std::nullopt;
            if (toks[begin].kind != open) return std::nullopt;

            std::vector<K> stack{};
            stack.push_back(open);
            for (uint32_t i = begin + 1; i < end && i < toks.size(); ++i) {
                const auto k = toks[i].kind;
                if (is_open_delim_(k)) {
                    stack.push_back(k);
                    continue;
                }
                if (!is_close_delim_(k)) continue;
                if (stack.empty()) return std::nullopt;
                const auto expected = matching_close_(stack.back());
                if (expected != k) return std::nullopt;
                stack.pop_back();
                if (stack.empty()) {
                    if (k != close) return std::nullopt;
                    return i;
                }
            }
            return std::nullopt;
        }

        static bool is_decl_stmt_kind_(ast::StmtKind k) {
            switch (k) {
                case ast::StmtKind::kFnDecl:
                case ast::StmtKind::kFieldDecl:
                case ast::StmtKind::kEnumDecl:
                case ast::StmtKind::kProtoDecl:
                case ast::StmtKind::kClassDecl:
                case ast::StmtKind::kActorDecl:
                case ast::StmtKind::kActsDecl:
                case ast::StmtKind::kInstDecl:
                case ast::StmtKind::kUse:
                case ast::StmtKind::kNestDecl:
                case ast::StmtKind::kVar:
                case ast::StmtKind::kCompilerDirective:
                    return true;
                default:
                    return false;
            }
        }

        enum class TokenPatternNodeKind : uint8_t {
            kLiteral = 0,
            kCapture,
            kGroup,
            kRepeat,
        };

        struct TokenPatternNode {
            TokenPatternNodeKind kind = TokenPatternNodeKind::kLiteral;
            Token literal{};

            std::string_view capture_name{};
            ast::MacroFragKind capture_frag = ast::MacroFragKind::kExpr;
            Span capture_span{};

            K group_open = K::kError;
            K group_close = K::kError;
            std::vector<TokenPatternNode> elems{};

            K repeat_op = K::kError;
            bool repeat_has_sep = false;
            Token repeat_sep{};
            Span span{};
        };

        class TokenPatternParser {
        public:
            TokenPatternParser(
                const std::vector<Token>& toks,
                uint32_t begin,
                uint32_t count,
                Span call_span,
                diag::Bag& diags
            )
                : toks_(toks), end_(begin + count), i_(begin), call_span_(call_span), diags_(diags) {}

            bool parse(std::vector<TokenPatternNode>& out) {
                out.clear();
                if (!parse_seq_(out, K::kError)) return false;
                return !failed_;
            }

        private:
            bool parse_seq_(std::vector<TokenPatternNode>& out, K until) {
                while (i_ < end_ && i_ < toks_.size()) {
                    if (until != K::kError && toks_[i_].kind == until) {
                        ++i_;
                        return true;
                    }
                    TokenPatternNode n{};
                    if (!parse_elem_(n)) return false;
                    out.push_back(std::move(n));
                }
                if (until != K::kError) {
                    fail_("missing closing delimiter in token pattern", call_span_);
                    return false;
                }
                return true;
            }

            bool parse_elem_(TokenPatternNode& out) {
                if (i_ >= end_ || i_ >= toks_.size()) return false;
                const Token t = toks_[i_];

                if (t.kind == K::kDollar) {
                    if (i_ + 1 < end_ && i_ + 1 < toks_.size() && toks_[i_ + 1].kind == K::kLParen) {
                        return parse_repeat_(out);
                    }
                    return parse_capture_(out);
                }

                if (t.kind == K::kLParen || t.kind == K::kLBrace || t.kind == K::kLBracket) {
                    return parse_group_(out);
                }

                out.kind = TokenPatternNodeKind::kLiteral;
                out.literal = t;
                out.span = t.span;
                ++i_;
                return true;
            }

            bool parse_capture_(TokenPatternNode& out) {
                const Token dol = toks_[i_];
                ++i_;
                if (i_ >= end_ || i_ >= toks_.size()) {
                    fail_("capture name expected after '$'", dol.span);
                    return false;
                }
                const Token name = toks_[i_];
                if (name.kind != K::kIdent) {
                    fail_("capture name must be identifier", name.span);
                    return false;
                }
                ++i_;
                if (i_ >= end_ || i_ >= toks_.size() || toks_[i_].kind != K::kColon) {
                    fail_("':' expected after capture name", name.span);
                    return false;
                }
                ++i_;
                if (i_ >= end_ || i_ >= toks_.size()) {
                    fail_("fragment kind expected after ':'", name.span);
                    return false;
                }
                ast::MacroFragKind fk = ast::MacroFragKind::kExpr;
                if (!parse_frag_kind_(toks_[i_], fk)) {
                    fail_("invalid capture fragment kind", toks_[i_].span);
                    return false;
                }
                const Token frag_tok = toks_[i_];
                ++i_;

                out.kind = TokenPatternNodeKind::kCapture;
                out.capture_name = name.lexeme;
                out.capture_frag = fk;
                out.capture_span = span_join_(dol.span, frag_tok.span);
                out.span = out.capture_span;
                return true;
            }

            bool parse_group_(TokenPatternNode& out) {
                const Token open = toks_[i_];
                const K close = matching_close_(open.kind);
                ++i_;
                std::vector<TokenPatternNode> inner{};
                if (!parse_seq_(inner, close)) return false;

                out.kind = TokenPatternNodeKind::kGroup;
                out.group_open = open.kind;
                out.group_close = close;
                out.elems = std::move(inner);
                out.span = open.span;
                return true;
            }

            bool parse_repeat_(TokenPatternNode& out) {
                const Token dol = toks_[i_];
                i_ += 2; // '$' '('
                std::vector<TokenPatternNode> inner{};
                if (!parse_seq_(inner, K::kRParen)) return false;

                if (i_ >= end_ || i_ >= toks_.size()) {
                    fail_("repeat operator expected after '$(...)'", dol.span);
                    return false;
                }

                bool has_sep = false;
                Token sep{};
                Token op_tok = toks_[i_];
                if (is_quantifier_(op_tok.kind)) {
                    ++i_;
                } else {
                    if (i_ + 1 >= end_ || i_ + 1 >= toks_.size() || !is_quantifier_(toks_[i_ + 1].kind)) {
                        fail_("repeat operator expected after '$(...)'", toks_[i_].span);
                        return false;
                    }
                    has_sep = true;
                    sep = toks_[i_];
                    op_tok = toks_[i_ + 1];
                    i_ += 2;
                }

                out.kind = TokenPatternNodeKind::kRepeat;
                out.elems = std::move(inner);
                out.repeat_op = op_tok.kind;
                out.repeat_has_sep = has_sep;
                out.repeat_sep = sep;
                out.span = span_join_(dol.span, op_tok.span);
                return true;
            }

            void fail_(std::string_view msg, Span at) {
                if (failed_) return;
                failed_ = true;
                add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, at, msg);
            }

            static Span span_join_(Span a, Span b) {
                Span s = a;
                if (s.file_id == 0) s.file_id = b.file_id;
                s.lo = std::min(a.lo, b.lo);
                s.hi = std::max(a.hi, b.hi);
                return s;
            }

            const std::vector<Token>& toks_;
            uint32_t end_ = 0;
            uint32_t i_ = 0;
            Span call_span_{};
            diag::Bag& diags_;
            bool failed_ = false;
        };

        class CaptureStore {
        public:
            std::vector<MacroCaptureBinding> snapshot() const {
                return captures_;
            }

            void restore(std::vector<MacroCaptureBinding> snap) {
                captures_ = std::move(snap);
                by_name_.clear();
                for (size_t i = 0; i < captures_.size(); ++i) {
                    by_name_[captures_[i].name] = i;
                }
            }

            bool bind(std::string_view name, MacroTokenRange r) {
                auto it = by_name_.find(name);
                if (it == by_name_.end()) {
                    MacroCaptureBinding b{};
                    b.name = name;
                    b.variadic = false;
                    b.ranges.push_back(r);
                    by_name_[name] = captures_.size();
                    captures_.push_back(std::move(b));
                    return true;
                }
                auto& b = captures_[it->second];
                b.ranges.push_back(r);
                if (b.ranges.size() > 1) b.variadic = true;
                return true;
            }

            const std::vector<MacroCaptureBinding>& captures() const { return captures_; }

        private:
            std::vector<MacroCaptureBinding> captures_{};
            std::unordered_map<std::string_view, size_t> by_name_{};
        };

        class TokenPatternMatcher {
        public:
            TokenPatternMatcher(
                ast::AstArena& ast,
                ty::TypePool& types,
                uint32_t call_begin,
                uint32_t call_count,
                Span call_span,
                diag::Bag& diags
            )
                : ast_(ast),
                  call_begin_(call_begin),
                  call_end_(call_begin + call_count),
                  call_span_(call_span),
                  diags_(diags) {
                (void)types;
            }

            bool hard_error() const { return hard_error_; }
            const std::vector<MacroCaptureBinding>& captures() const { return store_.captures(); }

            bool match(const std::vector<TokenPatternNode>& seq) {
                return match_seq_(
                    seq,
                    0,
                    call_begin_,
                    call_end_,
                    [&](uint32_t end_pos) { return end_pos == call_end_; }
                );
            }

        private:
            using Cont = std::function<bool(uint32_t)>;

            bool match_seq_(
                const std::vector<TokenPatternNode>& seq,
                size_t idx,
                uint32_t pos,
                uint32_t end,
                const Cont& cont
            ) {
                if (hard_error_) return false;
                if (idx >= seq.size()) return cont(pos);

                const auto& n = seq[idx];
                const auto next = [&](uint32_t np) {
                    return match_seq_(seq, idx + 1, np, end, cont);
                };

                switch (n.kind) {
                    case TokenPatternNodeKind::kLiteral: {
                        if (pos >= end || pos >= ast_.macro_tokens().size()) return false;
                        if (!token_literal_eq_(n.literal, ast_.macro_tokens()[pos])) return false;
                        return next(pos + 1);
                    }

                    case TokenPatternNodeKind::kCapture: {
                        auto cand = fragment_candidate_ends_(n.capture_frag, pos, end);
                        for (const auto e : cand) {
                            if (e <= pos) continue;
                            auto snap = store_.snapshot();
                            (void)store_.bind(n.capture_name, MacroTokenRange{pos, e - pos});
                            if (next(e)) return true;
                            store_.restore(std::move(snap));
                        }
                        return false;
                    }

                    case TokenPatternNodeKind::kGroup: {
                        if (pos >= end || pos >= ast_.macro_tokens().size()) return false;
                        if (ast_.macro_tokens()[pos].kind != n.group_open) return false;
                        const auto close = find_matching_close_token_(
                            ast_.macro_tokens(), pos, end, n.group_open, n.group_close);
                        if (!close.has_value()) return false;

                        auto snap = store_.snapshot();
                        const bool ok = match_seq_(
                            n.elems,
                            0,
                            pos + 1,
                            *close,
                            [&](uint32_t inner_end) {
                                if (inner_end != *close) return false;
                                return next(*close + 1);
                            }
                        );
                        if (ok) return true;
                        store_.restore(std::move(snap));
                        return false;
                    }

                    case TokenPatternNodeKind::kRepeat: {
                        return match_repeat_(n, pos, end, 0, next);
                    }
                }
                return false;
            }

            bool match_repeat_(
                const TokenPatternNode& rep,
                uint32_t pos,
                uint32_t end,
                uint32_t count,
                const Cont& cont
            ) {
                if (hard_error_) return false;
                if (seq_can_match_empty_(rep.elems)) {
                    add_diag_(diags_, diag::Code::kMacroRepeatEmpty, rep.span);
                    hard_error_ = true;
                    return false;
                }
                if (repeat_allows_stop_(rep.repeat_op, count)) {
                    auto snap = store_.snapshot();
                    if (cont(pos)) return true;
                    store_.restore(std::move(snap));
                }
                if (!repeat_allows_more_(rep.repeat_op, count)) return false;

                auto before_occ = store_.snapshot();
                const bool matched_one = match_seq_(
                    rep.elems,
                    0,
                    pos,
                    end,
                    [&](uint32_t body_end) {
                        if (body_end == pos) {
                            add_diag_(diags_, diag::Code::kMacroRepeatEmpty, call_span_);
                            hard_error_ = true;
                            return false;
                        }

                        {
                            auto after_one = store_.snapshot();
                            if (repeat_allows_stop_(rep.repeat_op, count + 1) && cont(body_end)) return true;
                            store_.restore(std::move(after_one));
                        }

                        uint32_t next_pos = body_end;
                        if (rep.repeat_has_sep) {
                            if (next_pos >= end || next_pos >= ast_.macro_tokens().size()) return false;
                            if (!token_literal_eq_(rep.repeat_sep, ast_.macro_tokens()[next_pos])) return false;
                            ++next_pos;
                        }
                        return match_repeat_(rep, next_pos, end, count + 1, cont);
                    }
                );
                if (matched_one) return true;
                store_.restore(std::move(before_occ));
                return false;
            }

            static bool repeat_allows_stop_(K op, uint32_t count) {
                if (op == K::kStar) return true;
                if (op == K::kPlus) return count >= 1;
                if (op == K::kQuestion) return count <= 1;
                return false;
            }

            static bool repeat_allows_more_(K op, uint32_t count) {
                if (op == K::kStar) return true;
                if (op == K::kPlus) return true;
                if (op == K::kQuestion) return count < 1;
                return false;
            }

            static bool node_can_match_empty_(const TokenPatternNode& n) {
                switch (n.kind) {
                    case TokenPatternNodeKind::kLiteral:
                    case TokenPatternNodeKind::kCapture:
                        return false;
                    case TokenPatternNodeKind::kGroup:
                        return seq_can_match_empty_(n.elems);
                    case TokenPatternNodeKind::kRepeat:
                        if (n.repeat_op == K::kStar || n.repeat_op == K::kQuestion) return true;
                        if (n.repeat_op == K::kPlus) return seq_can_match_empty_(n.elems);
                        return false;
                }
                return false;
            }

            static bool seq_can_match_empty_(const std::vector<TokenPatternNode>& seq) {
                if (seq.empty()) return true;
                for (const auto& n : seq) {
                    if (!node_can_match_empty_(n)) return false;
                }
                return true;
            }

            uint64_t probe_key_(ast::MacroFragKind frag, uint32_t begin, uint32_t end) const {
                return (static_cast<uint64_t>(frag) << 56)
                    ^ (static_cast<uint64_t>(begin) << 28)
                    ^ static_cast<uint64_t>(end);
            }

            bool probe_fragment_slice_(ast::MacroFragKind frag, uint32_t begin, uint32_t end) {
                const auto key = probe_key_(frag, begin, end);
                if (const auto it = probe_cache_.find(key); it != probe_cache_.end()) {
                    return it->second;
                }

                bool ok = false;
                std::vector<Token> toks{};
                toks.reserve((end > begin ? (end - begin) : 0) + 1);
                for (uint32_t i = begin; i < end && i < ast_.macro_tokens().size(); ++i) {
                    toks.push_back(ast_.macro_tokens()[i]);
                }
                Token eof{};
                eof.kind = K::kEof;
                eof.span = toks.empty() ? call_span_ : toks.back().span;
                toks.push_back(eof);

                diag::Bag local_bag{};
                ast::AstArena local_ast{};
                ty::TypePool local_types{};
                Parser p(toks, local_ast, local_types, &local_bag, 32);

                if (frag == ast::MacroFragKind::kExpr) {
                    const auto eid = p.parse_expr_full();
                    ok = !local_bag.has_error()
                        && eid != ast::k_invalid_expr
                        && local_ast.expr(eid).kind != ast::ExprKind::kError;
                } else if (frag == ast::MacroFragKind::kType) {
                    const auto nid = p.parse_type_full_for_macro(nullptr);
                    ok = !local_bag.has_error() && nid != ast::k_invalid_type_node;
                } else if (frag == ast::MacroFragKind::kStmt || frag == ast::MacroFragKind::kItem) {
                    const auto rid = p.parse_program();
                    if (!local_bag.has_error() && rid != ast::k_invalid_stmt) {
                        const auto& r = local_ast.stmt(rid);
                        if (r.kind == ast::StmtKind::kBlock && r.stmt_count == 1) {
                            const auto sid = local_ast.stmt_children()[r.stmt_begin];
                            if (frag == ast::MacroFragKind::kStmt) {
                                ok = true;
                            } else {
                                ok = is_decl_stmt_kind_(local_ast.stmt(sid).kind);
                            }
                        }
                    }
                } else {
                    ok = false;
                }

                probe_cache_[key] = ok;
                return ok;
            }

            std::vector<uint32_t> fragment_candidate_ends_(
                ast::MacroFragKind frag,
                uint32_t begin,
                uint32_t end
            ) {
                std::vector<uint32_t> out{};
                if (begin >= end || begin >= ast_.macro_tokens().size()) return out;
                const auto& toks = ast_.macro_tokens();

                if (frag == ast::MacroFragKind::kIdent) {
                    if (toks[begin].kind == K::kIdent) out.push_back(begin + 1);
                    return out;
                }
                if (frag == ast::MacroFragKind::kPath) {
                    if (toks[begin].kind != K::kIdent) return out;
                    uint32_t p = begin + 1;
                    out.push_back(p);
                    while (p < end) {
                        if (toks[p].kind == K::kColonColon && p + 1 < end && toks[p + 1].kind == K::kIdent) {
                            p += 2;
                            out.push_back(p);
                            continue;
                        }
                        if (toks[p].kind == K::kColon &&
                            p + 2 < end &&
                            toks[p + 1].kind == K::kColon &&
                            toks[p + 2].kind == K::kIdent) {
                            p += 3;
                            out.push_back(p);
                            continue;
                        }
                        break;
                    }
                    std::sort(out.rbegin(), out.rend());
                    return out;
                }
                if (frag == ast::MacroFragKind::kBlock) {
                    if (toks[begin].kind != K::kLBrace) return out;
                    const auto close = find_matching_close_token_(toks, begin, end, K::kLBrace, K::kRBrace);
                    if (close.has_value()) out.push_back(*close + 1);
                    return out;
                }
                if (frag == ast::MacroFragKind::kStrLit) {
                    if (toks[begin].kind != K::kStringLit) return out;
                    const auto lex = toks[begin].lexeme;
                    if (lex.size() >= 2 && lex.front() == '"' && lex.back() == '"') {
                        out.push_back(begin + 1);
                    }
                    return out;
                }
                if (frag == ast::MacroFragKind::kRawStrLit) {
                    if (toks[begin].kind != K::kStringLit) return out;
                    const auto lex = toks[begin].lexeme;
                    if (lex.size() >= 7 && lex.rfind("R\"\"\"", 0) == 0 &&
                        lex.substr(lex.size() - 3) == "\"\"\"") {
                        out.push_back(begin + 1);
                    }
                    return out;
                }
                if (frag == ast::MacroFragKind::kTt) {
                    const auto k = toks[begin].kind;
                    if (is_open_delim_(k)) {
                        const auto close = find_matching_close_token_(toks, begin, end, k, matching_close_(k));
                        if (close.has_value()) {
                            out.push_back(*close + 1);
                            return out;
                        }
                        return out;
                    }
                    out.push_back(begin + 1);
                    return out;
                }

                int paren = 0;
                int brace = 0;
                int bracket = 0;
                for (uint32_t i = begin; i < end && i < toks.size(); ++i) {
                    const auto k = toks[i].kind;
                    if (k == K::kLParen) ++paren;
                    else if (k == K::kRParen) --paren;
                    else if (k == K::kLBrace) ++brace;
                    else if (k == K::kRBrace) --brace;
                    else if (k == K::kLBracket) ++bracket;
                    else if (k == K::kRBracket) --bracket;
                    if (paren < 0 || brace < 0 || bracket < 0) break;
                    if (paren == 0 && brace == 0 && bracket == 0) {
                        out.push_back(i + 1);
                    }
                }
                std::sort(out.rbegin(), out.rend());

                std::vector<uint32_t> filtered{};
                filtered.reserve(out.size());
                for (const auto e : out) {
                    if (probe_fragment_slice_(frag, begin, e)) filtered.push_back(e);
                }
                return filtered;
            }

            ast::AstArena& ast_;
            uint32_t call_begin_ = 0;
            uint32_t call_end_ = 0;
            Span call_span_{}; // cached for diagnostics
            diag::Bag& diags_;
            bool hard_error_ = false;
            CaptureStore store_{};
            std::unordered_map<uint64_t, bool> probe_cache_{};
        };

        struct TemplateCaptureRef {
            size_t cap_index = 0;
            bool ellipsis = false;
            uint32_t consumed = 0;
        };

        class TokenTemplateExpander {
        public:
            TokenTemplateExpander(
                ast::AstArena& ast,
                const ast::MacroArm& arm,
                const std::vector<MacroCaptureBinding>& captures,
                Span call_span,
                diag::Bag& diags
            )
                : ast_(ast), arm_(arm), captures_(captures), call_span_(call_span), diags_(diags) {
                for (size_t i = 0; i < captures_.size(); ++i) {
                    cap_by_name_[captures_[i].name] = i;
                }
            }

            bool expand(std::vector<Token>& out_tokens) {
                out_tokens_.clear();
                generated_mask_.clear();
                const uint32_t begin = arm_.template_token_begin;
                const uint32_t end = arm_.template_token_begin + arm_.template_token_count;
                if (!expand_range_(begin, end)) return false;
                const uint64_t hygiene_seed =
                    (static_cast<uint64_t>(call_span_.file_id) << 48) ^
                    (static_cast<uint64_t>(call_span_.lo) << 16) ^
                    static_cast<uint64_t>(call_span_.hi);
                apply_binder_hygiene(ast_, out_tokens_, generated_mask_, hygiene_seed);
                out_tokens = std::move(out_tokens_);
                return true;
            }

        private:
            bool expand_range_(uint32_t begin, uint32_t end) {
                const auto& toks = ast_.macro_tokens();
                uint32_t i = begin;
                while (i < end && i < toks.size()) {
                    if (toks[i].kind == K::kDollar && i + 1 < end) {
                        if (toks[i + 1].kind == K::kLParen) {
                            uint32_t next_i = i;
                            if (!expand_repeat_at_(i, end, next_i)) return false;
                            i = next_i;
                            continue;
                        }

                        TemplateCaptureRef ref{};
                        if (!parse_capture_ref_(i, end, ref, /*diag_on_fail=*/true)) return false;
                        if (!emit_capture_ref_(ref)) return false;
                        i += ref.consumed;
                        continue;
                    }

                    out_tokens_.push_back(toks[i]);
                    generated_mask_.push_back(1);
                    ++i;
                }
                return true;
            }

            bool parse_capture_ref_(
                uint32_t pos,
                uint32_t end,
                TemplateCaptureRef& out,
                bool diag_on_fail
            ) {
                const auto& toks = ast_.macro_tokens();
                if (pos >= end || pos >= toks.size() || toks[pos].kind != K::kDollar) return false;
                if (pos + 1 >= end || pos + 1 >= toks.size()) {
                    if (diag_on_fail) add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, call_span_, "capture reference expected after '$'");
                    return false;
                }

                const Token& ref_tok = toks[pos + 1];
                std::optional<size_t> idx{};
                if (ref_tok.kind == K::kIdent) {
                    if (const auto it = cap_by_name_.find(ref_tok.lexeme); it != cap_by_name_.end()) {
                        idx = it->second;
                    }
                } else {
                    idx = parse_positional_index_(ref_tok);
                }
                if (!idx.has_value() || *idx >= captures_.size()) {
                    if (diag_on_fail) {
                        add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, ref_tok.span, "unknown capture reference");
                    }
                    return false;
                }

                out.cap_index = *idx;
                out.ellipsis = false;
                out.consumed = 2;
                if (pos + 2 < end) {
                    const auto ell = ellipsis_token_count_at_(toks, pos + 2, end);
                    if (ell > 0) {
                        out.ellipsis = true;
                        out.consumed += ell;
                    }
                }
                return true;
            }

            bool emit_capture_ref_(const TemplateCaptureRef& ref) {
                if (ref.cap_index >= captures_.size()) return false;
                const auto& cap = captures_[ref.cap_index];
                if (cap.ranges.empty()) {
                    add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, call_span_, "capture has no matched token range");
                    return false;
                }

                if (ref.ellipsis) {
                    for (size_t i = 0; i < cap.ranges.size(); ++i) {
                        if (!emit_range_(cap.ranges[i])) return false;
                        if (i + 1 < cap.ranges.size()) {
                            Token comma{};
                            comma.kind = K::kComma;
                            comma.lexeme = ",";
                            comma.span = call_span_;
                            out_tokens_.push_back(comma);
                            generated_mask_.push_back(1);
                        }
                    }
                    return true;
                }

                if (!cap.variadic) {
                    return emit_range_(cap.ranges.front());
                }

                const auto it = active_indices_.find(ref.cap_index);
                if (it == active_indices_.end()) {
                    add_diag_(diags_, diag::Code::kMacroTokenVariadicOutsideRepeat, call_span_, cap.name);
                    return false;
                }
                if (it->second >= cap.ranges.size()) {
                    add_diag_(diags_, diag::Code::kMacroTokenRepeatLengthMismatch, call_span_, "active repetition index is out of capture range");
                    return false;
                }
                return emit_range_(cap.ranges[it->second]);
            }

            bool emit_range_(MacroTokenRange r) {
                const auto& toks = ast_.macro_tokens();
                for (uint32_t i = 0; i < r.count; ++i) {
                    const uint32_t idx = r.begin + i;
                    if (idx >= toks.size()) return false;
                    out_tokens_.push_back(toks[idx]);
                    generated_mask_.push_back(0);
                }
                return true;
            }

            bool parse_repeat_bounds_(
                uint32_t dollar_pos,
                uint32_t end,
                uint32_t& body_begin,
                uint32_t& body_end,
                uint32_t& after_repeat,
                bool& has_sep,
                Token& sep_tok,
                K& op
            ) {
                const auto& toks = ast_.macro_tokens();
                if (dollar_pos + 1 >= end || toks[dollar_pos].kind != K::kDollar || toks[dollar_pos + 1].kind != K::kLParen) {
                    return false;
                }

                uint32_t i = dollar_pos + 2;
                int depth = 1;
                uint32_t close = 0;
                bool found_close = false;
                while (i < end && i < toks.size()) {
                    const auto k = toks[i].kind;
                    if (k == K::kLParen) {
                        ++depth;
                    } else if (k == K::kRParen) {
                        --depth;
                        if (depth == 0) {
                            close = i;
                            found_close = true;
                            break;
                        }
                    }
                    ++i;
                }
                if (!found_close) {
                    add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, call_span_, "unclosed template repetition '$(...)'");
                    return false;
                }

                if (close + 1 >= end || close + 1 >= toks.size()) {
                    add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, call_span_, "repetition quantifier expected after '$(...)'");
                    return false;
                }

                has_sep = false;
                sep_tok = Token{};
                op = K::kError;
                uint32_t tail = close + 1;
                if (is_quantifier_(toks[tail].kind)) {
                    op = toks[tail].kind;
                    after_repeat = tail + 1;
                } else if (tail + 1 < end && tail + 1 < toks.size() && is_quantifier_(toks[tail + 1].kind)) {
                    has_sep = true;
                    sep_tok = toks[tail];
                    op = toks[tail + 1].kind;
                    after_repeat = tail + 2;
                } else {
                    add_diag_(diags_, diag::Code::kMacroTokenPatternInvalid, toks[tail].span, "repetition quantifier expected after '$(...)'");
                    return false;
                }

                body_begin = dollar_pos + 2;
                body_end = close;
                return true;
            }

            bool collect_repeat_driver_info_(
                uint32_t body_begin,
                uint32_t body_end,
                std::vector<size_t>& driver_caps,
                std::vector<size_t>& lengths
            ) {
                const auto& toks = ast_.macro_tokens();
                std::unordered_map<size_t, bool> seen_driver{};

                uint32_t i = body_begin;
                while (i < body_end && i < toks.size()) {
                    if (toks[i].kind == K::kDollar && i + 1 < body_end && toks[i + 1].kind == K::kLParen) {
                        uint32_t nested_body_begin = 0;
                        uint32_t nested_body_end = 0;
                        uint32_t after_nested = i;
                        bool nested_sep = false;
                        Token nested_sep_tok{};
                        K nested_op = K::kError;
                        if (!parse_repeat_bounds_(
                                i, body_end, nested_body_begin, nested_body_end, after_nested, nested_sep, nested_sep_tok, nested_op)) {
                            return false;
                        }
                        std::vector<size_t> nested_driver_caps{};
                        std::vector<size_t> nested_lengths{};
                        if (!collect_repeat_driver_info_(
                                nested_body_begin, nested_body_end, nested_driver_caps, nested_lengths)) {
                            return false;
                        }
                        lengths.insert(lengths.end(), nested_lengths.begin(), nested_lengths.end());
                        for (const auto cap_idx : nested_driver_caps) {
                            if (!seen_driver.contains(cap_idx)) {
                                seen_driver[cap_idx] = true;
                                driver_caps.push_back(cap_idx);
                            }
                        }
                        i = after_nested;
                        continue;
                    }

                    if (toks[i].kind == K::kDollar) {
                        TemplateCaptureRef ref{};
                        if (!parse_capture_ref_(i, body_end, ref, /*diag_on_fail=*/true)) return false;
                        if (ref.cap_index >= captures_.size()) return false;
                        const auto& cap = captures_[ref.cap_index];
                        if (cap.variadic) {
                            const auto fixed = active_indices_.find(ref.cap_index);
                            const size_t len = (fixed == active_indices_.end()) ? cap.ranges.size() : 1u;
                            lengths.push_back(len);
                            if (!ref.ellipsis && fixed == active_indices_.end() && !seen_driver.contains(ref.cap_index)) {
                                seen_driver[ref.cap_index] = true;
                                driver_caps.push_back(ref.cap_index);
                            }
                        }
                        i += ref.consumed;
                        continue;
                    }
                    ++i;
                }
                return true;
            }

            bool expand_repeat_at_(uint32_t pos, uint32_t end, uint32_t& out_next) {
                uint32_t body_begin = 0;
                uint32_t body_end = 0;
                uint32_t after = 0;
                bool has_sep = false;
                Token sep{};
                K op = K::kError;
                if (!parse_repeat_bounds_(pos, end, body_begin, body_end, after, has_sep, sep, op)) return false;

                std::vector<size_t> driver_caps{};
                std::vector<size_t> lengths{};
                if (!collect_repeat_driver_info_(body_begin, body_end, driver_caps, lengths)) return false;
                if (lengths.empty()) {
                    add_diag_(
                        diags_,
                        diag::Code::kMacroTokenRepeatLengthMismatch,
                        call_span_,
                        "repetition body must reference at least one variadic capture");
                    return false;
                }

                const size_t count = lengths.front();
                for (const auto n : lengths) {
                    if (n != count) {
                        add_diag_(
                            diags_,
                            diag::Code::kMacroTokenRepeatLengthMismatch,
                            call_span_,
                            "variadic captures in repetition must have the same length");
                        return false;
                    }
                }

                if (op == K::kPlus && count == 0) {
                    add_diag_(
                        diags_,
                        diag::Code::kMacroTokenRepeatLengthMismatch,
                        call_span_,
                        "repetition '+' requires at least one element");
                    return false;
                }
                if (op == K::kQuestion && count > 1) {
                    add_diag_(
                        diags_,
                        diag::Code::kMacroTokenRepeatLengthMismatch,
                        call_span_,
                        "repetition '?' expects at most one element");
                    return false;
                }

                size_t emit_count = count;
                if (op == K::kQuestion) emit_count = std::min<size_t>(emit_count, 1);
                for (size_t i = 0; i < emit_count; ++i) {
                    const auto saved_active = active_indices_;
                    for (const auto cap_idx : driver_caps) {
                        if (!active_indices_.contains(cap_idx)) {
                            active_indices_[cap_idx] = i;
                        }
                    }
                    if (!expand_range_(body_begin, body_end)) return false;
                    active_indices_ = saved_active;

                    if (has_sep && i + 1 < emit_count) {
                        out_tokens_.push_back(sep);
                        generated_mask_.push_back(1);
                    }
                }

                out_next = after;
                return true;
            }

            ast::AstArena& ast_;
            const ast::MacroArm& arm_;
            const std::vector<MacroCaptureBinding>& captures_;
            Span call_span_{};
            diag::Bag& diags_;
            std::unordered_map<std::string_view, size_t> cap_by_name_{};
            std::unordered_map<size_t, size_t> active_indices_{};
            std::vector<Token> out_tokens_{};
            std::vector<uint8_t> generated_mask_{};
        };

    } // namespace

    TokenArmMatchStatus match_token_arm(
        ast::AstArena& ast,
        ty::TypePool& types,
        const ast::MacroArm& arm,
        uint32_t arg_begin,
        uint32_t arg_count,
        Span call_span,
        diag::Bag& diags,
        std::vector<MacroCaptureBinding>& out_captures
    ) {
        out_captures.clear();

        std::vector<TokenPatternNode> pattern{};
        TokenPatternParser parser(ast.macro_tokens(), arm.pattern_token_begin, arm.pattern_token_count, call_span, diags);
        if (!parser.parse(pattern)) return TokenArmMatchStatus::kError;

        TokenPatternMatcher matcher(ast, types, arg_begin, arg_count, call_span, diags);
        if (!matcher.match(pattern)) {
            return matcher.hard_error() ? TokenArmMatchStatus::kError : TokenArmMatchStatus::kNoMatch;
        }
        out_captures = matcher.captures();
        return TokenArmMatchStatus::kMatch;
    }

    bool substitute_token_template(
        ast::AstArena& ast,
        const ast::MacroArm& arm,
        const std::vector<MacroCaptureBinding>& captures,
        Span call_span,
        diag::Bag& diags,
        std::vector<Token>& out_tokens
    ) {
        TokenTemplateExpander exp(ast, arm, captures, call_span, diags);
        return exp.expand(out_tokens);
    }

} // namespace parus::macro
