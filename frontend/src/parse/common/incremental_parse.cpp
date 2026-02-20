#include <parus/parse/IncrementalParse.hpp>

#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace parus::parse {

    namespace {

        static constexpr uint32_t kInvalidU32 = std::numeric_limits<uint32_t>::max();

        Span span_join_(Span a, Span b) {
            Span s = a;
            if (s.file_id == 0) s.file_id = b.file_id;
            s.lo = std::min(a.lo, b.lo);
            s.hi = std::max(a.hi, b.hi);
            return s;
        }

        void append_diag_bag_(diag::Bag& dst, const diag::Bag& src) {
            for (const auto& d : src.diags()) {
                diag::Diagnostic copied(d.severity(), d.code(), d.span());
                for (const auto& arg : d.args()) copied.add_arg(arg);
                dst.add(std::move(copied));
            }
        }

        std::vector<TopItemMeta> collect_top_items_(const ast::AstArena& ast, ast::StmtId root) {
            std::vector<TopItemMeta> out{};
            if (root == ast::k_invalid_stmt) return out;

            const auto& r = ast.stmt(root);
            if (r.kind != ast::StmtKind::kBlock) return out;

            const auto& kids = ast.stmt_children();
            if (r.stmt_begin > kids.size()) return out;
            if (r.stmt_begin + r.stmt_count > kids.size()) return out;

            out.reserve(r.stmt_count);
            for (uint32_t i = 0; i < r.stmt_count; ++i) {
                const auto sid = kids[r.stmt_begin + i];
                if (sid == ast::k_invalid_stmt) continue;
                const auto& st = ast.stmt(sid);
                out.push_back(TopItemMeta{sid, st.span.lo, st.span.hi});
            }
            return out;
        }

        size_t find_first_affected_item_(const std::vector<TopItemMeta>& items, uint32_t edit_lo) {
            for (size_t i = 0; i < items.size(); ++i) {
                if (edit_lo <= items[i].hi) return i;
            }
            return items.size();
        }

        uint32_t earliest_edit_lo_(std::span<const EditWindow> edits) {
            uint32_t lo = kInvalidU32;
            for (const auto& e : edits) {
                lo = std::min(lo, std::min(e.lo, e.hi));
            }
            return (lo == kInvalidU32) ? 0u : lo;
        }

        size_t find_token_begin_(const std::vector<Token>& toks, uint32_t parse_lo) {
            size_t i = 0;
            while (i < toks.size()) {
                const auto& t = toks[i];
                if (t.kind == syntax::TokenKind::kEof) break;
                if (t.span.lo >= parse_lo) break;
                ++i;
            }
            return i;
        }

        void append_unique_owner_(std::vector<std::shared_ptr<std::string>>& out,
                                  const std::shared_ptr<std::string>& owner) {
            if (!owner) return;
            for (const auto& p : out) {
                if (p.get() == owner.get()) return;
            }
            out.push_back(owner);
        }

    } // namespace

    bool IncrementalParserSession::initialize(std::string_view source, uint32_t file_id, diag::Bag& bag) {
        return full_rebuild_(source, file_id, bag, ReparseMode::kFullRebuild);
    }

    bool IncrementalParserSession::reparse_with_edits(std::string_view source,
                                                      uint32_t file_id,
                                                      std::span<const EditWindow> edits,
                                                      diag::Bag& bag) {
        if (!ready_) {
            return initialize(source, file_id, bag);
        }

        if (edits.empty()) {
            return full_rebuild_(source, file_id, bag, ReparseMode::kFullRebuild);
        }

        if (try_incremental_merge_(source, file_id, edits, bag)) {
            last_mode_ = ReparseMode::kIncrementalMerge;
            return true;
        }

        return full_rebuild_(source, file_id, bag, ReparseMode::kFallbackFullRebuild);
    }

    bool IncrementalParserSession::full_rebuild_(std::string_view source,
                                                 uint32_t file_id,
                                                 diag::Bag& bag,
                                                 ReparseMode mode) {
        auto source_owner = std::make_shared<std::string>(source);

        Lexer lx(*source_owner, file_id, &bag);
        auto toks = lx.lex_all();

        ast::AstArena arena{};
        ty::TypePool types{};
        Parser parser(toks, arena, types, &bag, /*max_errors=*/256);
        const auto root = parser.parse_program();

        ParseSnapshot next{};
        next.ast = std::move(arena);
        next.types = std::move(types);
        next.root = root;
        next.tokens = std::move(toks);
        next.top_items = collect_top_items_(next.ast, next.root);
        next.revision = ++revision_seq_;

        snapshot_ = std::move(next);
        source_owners_.clear();
        source_owners_.push_back(std::move(source_owner));

        ready_ = true;
        last_mode_ = mode;
        return true;
    }

    bool IncrementalParserSession::try_incremental_merge_(std::string_view source,
                                                          uint32_t file_id,
                                                          std::span<const EditWindow> edits,
                                                          diag::Bag& bag) {
        if (!ready_) return false;
        if (snapshot_.root == ast::k_invalid_stmt) return false;
        if (source_owners_.size() > 16) return false; // old source retention compact trigger

        const auto earliest_lo = earliest_edit_lo_(edits);
        const auto old_items = snapshot_.top_items;
        if (old_items.empty()) return false;

        size_t first = find_first_affected_item_(old_items, earliest_lo);
        if (first == 0) return false; // 시작 item부터 영향이면 full parse로 복귀
        if (first >= old_items.size()) {
            first = old_items.size() - 1;
        }

        auto source_owner = std::make_shared<std::string>(source);

        diag::Bag local_bag;
        Lexer lx(*source_owner, file_id, &local_bag);
        auto new_tokens = lx.lex_all();
        if (local_bag.has_fatal()) return false;
        if (new_tokens.empty()) return false;

        const uint32_t parse_lo = std::min(old_items[first].lo, earliest_lo);
        const size_t tok_begin = find_token_begin_(new_tokens, parse_lo);
        if (tok_begin >= new_tokens.size()) return false;

        std::vector<Token> partial_tokens{};
        partial_tokens.reserve(new_tokens.size() - tok_begin);
        for (size_t i = tok_begin; i < new_tokens.size(); ++i) {
            partial_tokens.push_back(new_tokens[i]);
        }
        if (partial_tokens.empty()) return false;

        ast::AstArena arena = snapshot_.ast;
        ty::TypePool types = snapshot_.types;

        Parser partial_parser(partial_tokens, arena, types, &local_bag, /*max_errors=*/256);
        const auto partial_root = partial_parser.parse_program();
        if (partial_root == ast::k_invalid_stmt) return false;

        const auto& old_root = arena.stmt(snapshot_.root);
        if (old_root.kind != ast::StmtKind::kBlock) return false;

        const auto& partial_root_stmt = arena.stmt(partial_root);
        if (partial_root_stmt.kind != ast::StmtKind::kBlock) return false;

        const auto& children = arena.stmt_children();
        if (old_root.stmt_begin > children.size() || old_root.stmt_begin + old_root.stmt_count > children.size()) {
            return false;
        }
        if (partial_root_stmt.stmt_begin > children.size()
            || partial_root_stmt.stmt_begin + partial_root_stmt.stmt_count > children.size()) {
            return false;
        }

        std::vector<ast::StmtId> merged_children{};
        merged_children.reserve(static_cast<size_t>(first) + partial_root_stmt.stmt_count);
        for (size_t i = 0; i < first; ++i) {
            merged_children.push_back(children[old_root.stmt_begin + static_cast<uint32_t>(i)]);
        }
        for (uint32_t i = 0; i < partial_root_stmt.stmt_count; ++i) {
            merged_children.push_back(children[partial_root_stmt.stmt_begin + i]);
        }

        uint32_t merged_begin = static_cast<uint32_t>(arena.stmt_children().size());
        for (const auto sid : merged_children) {
            arena.add_stmt_child(sid);
        }

        ast::Stmt merged_root{};
        merged_root.kind = ast::StmtKind::kBlock;
        merged_root.stmt_begin = merged_begin;
        merged_root.stmt_count = static_cast<uint32_t>(merged_children.size());
        if (!merged_children.empty()) {
            const auto first_span = arena.stmt(merged_children.front()).span;
            const auto last_span = arena.stmt(merged_children.back()).span;
            merged_root.span = span_join_(first_span, last_span);
        } else {
            merged_root.span = partial_root_stmt.span;
        }

        const auto new_root = arena.add_stmt(merged_root);

        ParseSnapshot next{};
        next.ast = std::move(arena);
        next.types = std::move(types);
        next.root = new_root;
        next.tokens = std::move(new_tokens);
        next.top_items = collect_top_items_(next.ast, next.root);
        next.revision = ++revision_seq_;

        snapshot_ = std::move(next);

        std::vector<std::shared_ptr<std::string>> next_owners{};
        next_owners.reserve(source_owners_.size() + 1);
        for (const auto& old_owner : source_owners_) append_unique_owner_(next_owners, old_owner);
        append_unique_owner_(next_owners, source_owner);
        source_owners_ = std::move(next_owners);

        append_diag_bag_(bag, local_bag);
        ready_ = true;
        return true;
    }

} // namespace parus::parse
