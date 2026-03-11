#include <parus/parse/IncrementalParse.hpp>
#include <parus/diag/Render.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct TopShape {
    bool root_is_block = false;
    uint32_t child_count = 0;
    std::vector<uint32_t> child_kinds{};
};

static bool read_text_file_(const std::filesystem::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

static uint64_t mix_hash_(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

static std::unordered_map<std::string, uint32_t> diag_multiset_(const parus::diag::Bag& bag) {
    std::unordered_map<std::string, uint32_t> out;
    for (const auto& d : bag.diags()) {
        std::string key = parus::diag::code_name(d.code());
        key.push_back('#');
        key += std::to_string(static_cast<uint32_t>(d.severity()));
        ++out[key];
    }
    return out;
}

static TopShape extract_top_shape_(const parus::parse::ParseSnapshot& snap) {
    TopShape out{};
    if (snap.root == parus::ast::k_invalid_stmt) return out;

    const auto& root = snap.ast.stmt(snap.root);
    out.root_is_block = (root.kind == parus::ast::StmtKind::kBlock);
    if (!out.root_is_block) return out;

    out.child_count = root.stmt_count;
    const auto& children = snap.ast.stmt_children();
    if (root.stmt_begin + root.stmt_count > children.size()) return out;

    out.child_kinds.reserve(root.stmt_count);
    for (uint32_t i = 0; i < root.stmt_count; ++i) {
        const auto sid = children[root.stmt_begin + i];
        if (sid == parus::ast::k_invalid_stmt) {
            out.child_kinds.push_back(0xFFFFu);
            continue;
        }
        out.child_kinds.push_back(static_cast<uint32_t>(snap.ast.stmt(sid).kind));
    }
    return out;
}

static uint64_t ast_fingerprint_(const parus::parse::ParseSnapshot& snap) {
    if (snap.root == parus::ast::k_invalid_stmt) return 0;

    const auto& ast = snap.ast;
    const auto& children = ast.stmt_children();
    const auto& args = ast.args();
    const auto& field_inits = ast.field_init_entries();
    const auto& fparts = ast.fstring_parts();

    std::vector<uint8_t> seen_stmt(ast.stmts().size(), 0);
    std::vector<uint8_t> seen_expr(ast.exprs().size(), 0);
    std::vector<parus::ast::StmtId> stmt_stack;
    std::vector<parus::ast::ExprId> expr_stack;

    auto push_stmt = [&](parus::ast::StmtId sid) {
        if (sid == parus::ast::k_invalid_stmt) return;
        if (sid >= seen_stmt.size()) return;
        if (seen_stmt[sid]) return;
        seen_stmt[sid] = 1;
        stmt_stack.push_back(sid);
    };

    auto push_expr = [&](parus::ast::ExprId eid) {
        if (eid == parus::ast::k_invalid_expr) return;
        if (eid >= seen_expr.size()) return;
        if (seen_expr[eid]) return;
        seen_expr[eid] = 1;
        expr_stack.push_back(eid);
    };

    uint64_t h = 1469598103934665603ull;
    push_stmt(snap.root);

    while (!stmt_stack.empty() || !expr_stack.empty()) {
        while (!stmt_stack.empty()) {
            const auto sid = stmt_stack.back();
            stmt_stack.pop_back();
            const auto& st = ast.stmt(sid);

            h = mix_hash_(h, static_cast<uint64_t>(st.kind));
            h = mix_hash_(h, st.stmt_count);
            h = mix_hash_(h, st.param_count);
            h = mix_hash_(h, st.attr_count);
            h = mix_hash_(h, st.case_count);

            push_stmt(st.a);
            push_stmt(st.b);
            push_expr(st.expr);
            push_expr(st.init);

            if (st.stmt_begin + st.stmt_count <= children.size()) {
                for (uint32_t i = 0; i < st.stmt_count; ++i) {
                    push_stmt(children[st.stmt_begin + i]);
                }
            }
        }

        while (!expr_stack.empty()) {
            const auto eid = expr_stack.back();
            expr_stack.pop_back();
            const auto& ex = ast.expr(eid);

            h = mix_hash_(h, static_cast<uint64_t>(ex.kind));
            h = mix_hash_(h, static_cast<uint64_t>(ex.op));
            h = mix_hash_(h, ex.arg_count);
            h = mix_hash_(h, ex.field_init_count);
            h = mix_hash_(h, ex.string_part_count);

            push_expr(ex.a);
            push_expr(ex.b);
            push_expr(ex.c);
            push_expr(ex.loop_iter);
            push_expr(ex.block_tail);
            push_stmt(ex.block_stmt);
            push_stmt(ex.loop_body);

            if (ex.arg_begin + ex.arg_count <= args.size()) {
                for (uint32_t i = 0; i < ex.arg_count; ++i) {
                    push_expr(args[ex.arg_begin + i].expr);
                }
            }
            if (ex.field_init_begin + ex.field_init_count <= field_inits.size()) {
                for (uint32_t i = 0; i < ex.field_init_count; ++i) {
                    push_expr(field_inits[ex.field_init_begin + i].expr);
                }
            }
            if (ex.string_part_begin + ex.string_part_count <= fparts.size()) {
                for (uint32_t i = 0; i < ex.string_part_count; ++i) {
                    if (fparts[ex.string_part_begin + i].is_expr) {
                        push_expr(fparts[ex.string_part_begin + i].expr);
                    }
                }
            }
        }
    }

    return h;
}

static bool compare_snapshots_strict_(
    const std::string& tag,
    const parus::diag::Bag& inc_bag,
    const parus::parse::ParseSnapshot& inc_snap,
    const parus::diag::Bag& full_bag,
    const parus::parse::ParseSnapshot& full_snap
) {
    const auto inc_diags = diag_multiset_(inc_bag);
    const auto full_diags = diag_multiset_(full_bag);
    if (inc_diags != full_diags) {
        std::cerr << "  - " << tag << ": diagnostic multiset mismatch\n";
        return false;
    }

    const auto inc_shape = extract_top_shape_(inc_snap);
    const auto full_shape = extract_top_shape_(full_snap);
    if (inc_shape.root_is_block != full_shape.root_is_block
        || inc_shape.child_count != full_shape.child_count
        || inc_shape.child_kinds != full_shape.child_kinds) {
        std::cerr << "  - " << tag << ": top-level shape mismatch\n";
        return false;
    }

    const auto inc_fp = ast_fingerprint_(inc_snap);
    const auto full_fp = ast_fingerprint_(full_snap);
    if (inc_fp != full_fp) {
        std::cerr << "  - " << tag << ": AST fingerprint mismatch\n";
        return false;
    }

    return true;
}

static std::string apply_insert_(const std::string& src, uint32_t at, std::string_view repl) {
    std::string out;
    out.reserve(src.size() + repl.size() + 8);
    out.append(src.data(), at);
    out.append(repl.data(), repl.size());
    out.append(src.data() + at, src.size() - at);
    return out;
}

static uint32_t find_merge_friendly_insert_pos_(const std::string& src, const parus::parse::TopItemMeta& item) {
    const uint32_t lo = std::min<uint32_t>(item.lo, static_cast<uint32_t>(src.size()));
    const uint32_t hi = std::min<uint32_t>(item.hi, static_cast<uint32_t>(src.size()));
    if (lo >= hi) return lo;

    const std::string_view slice(src.data() + lo, hi - lo);
    if (const size_t ret = slice.find("return "); ret != std::string_view::npos) {
        return lo + static_cast<uint32_t>(ret + 7u);
    }
    if (const size_t eq = slice.find(" = "); eq != std::string_view::npos) {
        return lo + static_cast<uint32_t>(eq + 3u);
    }

    for (uint32_t i = lo; i < hi; ++i) {
        const char ch = src[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            return i;
        }
    }
    return lo;
}

static const char* mode_name_(parus::parse::ReparseMode mode) {
    using parus::parse::ReparseMode;
    switch (mode) {
        case ReparseMode::kNone: return "None";
        case ReparseMode::kFullRebuild: return "FullRebuild";
        case ReparseMode::kIncrementalMerge: return "IncrementalMerge";
        case ReparseMode::kFallbackFullRebuild: return "FallbackFullRebuild";
    }
    return "Unknown";
}

static bool run_case_(const std::filesystem::path& path) {
    std::string src;
    if (!read_text_file_(path, src)) {
        std::cerr << "  - cannot read case: " << path.string() << "\n";
        return false;
    }

    parus::parse::IncrementalParserSession inc{};
    parus::diag::Bag init_inc_bag{};
    if (!inc.initialize(src, /*file_id=*/1, init_inc_bag)) {
        std::cerr << "  - initial incremental parse failed: " << path.filename().string() << "\n";
        return false;
    }

    parus::parse::IncrementalParserSession full{};
    parus::diag::Bag init_full_bag{};
    if (!full.initialize(src, /*file_id=*/1, init_full_bag)) {
        std::cerr << "  - initial full parse failed: " << path.filename().string() << "\n";
        return false;
    }

    if (!compare_snapshots_strict_("initial", init_inc_bag, inc.snapshot(), init_full_bag, full.snapshot())) {
        std::cerr << "  - initial mismatch: " << path.filename().string() << "\n";
        return false;
    }

    // Phase 1: strict merge-only verification on edits known to be merge-friendly.
    constexpr uint32_t kStrictMergeSteps = 12;
    // Phase 2: long-running edit stream where merge/fallback are both allowed,
    // but result equivalence vs full parse must always hold.
    constexpr uint32_t kLongRunSteps = 16;

    static const char* kStrictInsertions[] = {" ", "  ", "\t"};
    static const char* kLongRunInsertions[] = {" ", "\n", "\n "};
    uint32_t merge_hits = 0;
    uint32_t fallback_hits = 0;

    const auto run_step = [&](uint32_t step, bool require_merge) -> bool {
        const auto& items = inc.snapshot().top_items;
        if (items.size() < 2) {
            std::cerr << "  - need >=2 top items for merge test: " << path.filename().string() << "\n";
            return false;
        }

        const size_t idx = 1 + (step % (items.size() - 1));
        uint32_t at = find_merge_friendly_insert_pos_(src, items[idx]);
        at = std::min<uint32_t>(at, static_cast<uint32_t>(src.size()));

        const char* repl = require_merge
            ? kStrictInsertions[step % 3]
            : kLongRunInsertions[step % 3];
        const std::string next_src = apply_insert_(src, at, repl);
        const parus::parse::EditWindow edit{at, at};

        parus::diag::Bag inc_bag{};
        if (!inc.reparse_with_edits(
                next_src,
                /*file_id=*/1,
                std::span<const parus::parse::EditWindow>(&edit, 1),
                inc_bag)) {
            std::cerr << "  - incremental reparse failed at step " << step
                      << ": " << path.filename().string() << "\n";
            return false;
        }

        const auto mode = inc.last_mode();
        if (mode == parus::parse::ReparseMode::kIncrementalMerge) {
            ++merge_hits;
        } else if (mode == parus::parse::ReparseMode::kFallbackFullRebuild) {
            ++fallback_hits;
        } else {
            std::cerr << "  - unexpected reparse mode=" << mode_name_(mode)
                      << " at step " << step << ": " << path.filename().string() << "\n";
            return false;
        }

        if (require_merge && mode != parus::parse::ReparseMode::kIncrementalMerge) {
            std::cerr << "  - expected incremental merge but got mode=" << mode_name_(mode)
                      << " at step " << step << ": " << path.filename().string() << "\n";
            return false;
        }

        parus::parse::IncrementalParserSession full_step{};
        parus::diag::Bag full_bag{};
        if (!full_step.initialize(next_src, /*file_id=*/1, full_bag)) {
            std::cerr << "  - full parse failed at step " << step
                      << ": " << path.filename().string() << "\n";
            return false;
        }

        const std::string tag = path.filename().string() + ":step=" + std::to_string(step);
        if (!compare_snapshots_strict_(tag, inc_bag, inc.snapshot(), full_bag, full_step.snapshot())) {
            return false;
        }

        src = next_src;
        return true;
    };

    for (uint32_t step = 0; step < kStrictMergeSteps; ++step) {
        if (!run_step(step, /*require_merge=*/true)) return false;
    }
    for (uint32_t i = 0; i < kLongRunSteps; ++i) {
        const uint32_t step = kStrictMergeSteps + i;
        if (!run_step(step, /*require_merge=*/false)) return false;
    }

    if (merge_hits == 0) {
        std::cerr << "  - no incremental merge steps executed: " << path.filename().string() << "\n";
        return false;
    }
    if (fallback_hits == 0) {
        std::cerr << "  - long-run phase never exercised fallback rebuild: " << path.filename().string() << "\n";
        return false;
    }
    return true;
}

static bool collect_case_files_(std::vector<std::filesystem::path>& out) {
#ifndef PARUS_INCREMENTAL_MERGE_CASE_DIR
    std::cerr << "PARUS_INCREMENTAL_MERGE_CASE_DIR is not defined\n";
    return false;
#else
    const std::filesystem::path root = std::filesystem::path(PARUS_INCREMENTAL_MERGE_CASE_DIR);
    if (!std::filesystem::exists(root)) {
        std::cerr << "incremental merge case dir not found: " << root.string() << "\n";
        return false;
    }

    for (const auto& e : std::filesystem::directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto p = e.path();
        if (p.extension() != ".pr") continue;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return !out.empty();
#endif
}

static bool test_structural_newline_forces_fallback_() {
    const std::string src =
        "actor Counter {\n"
        "  draft {\n"
        "    value: i32;\n"
        "  }\n"
        "\n"
        "  init(seed: i32) {\n"
        "    draft.value = seed;\n"
        "  }\n"
        "\n"
        "  def sub get() -> i32 {\n"
        "    recast;\n"
        "    return draft.value;\n"
        "  }\n"
        "};\n"
        "\n"
        "def main() -> i32 {\n"
        "  set c = Counter(seed: 1i32);\n"
        "  return c.get();\n"
        "}\n";

    const auto pos = src.find("    return draft.value;");
    if (pos == std::string::npos) {
        std::cerr << "  - structural fallback fixture is malformed\n";
        return false;
    }

    parus::parse::IncrementalParserSession inc{};
    parus::diag::Bag init_inc_bag{};
    if (!inc.initialize(src, /*file_id=*/1, init_inc_bag)) {
        std::cerr << "  - structural fallback incremental init failed\n";
        return false;
    }

    const std::string next_src = apply_insert_(src, static_cast<uint32_t>(pos), "\n");
    const parus::parse::EditWindow edit{static_cast<uint32_t>(pos), static_cast<uint32_t>(pos)};

    parus::diag::Bag inc_bag{};
    if (!inc.reparse_with_edits(next_src, /*file_id=*/1, std::span<const parus::parse::EditWindow>(&edit, 1), inc_bag)) {
        std::cerr << "  - structural fallback incremental reparse failed\n";
        return false;
    }
    if (inc.last_mode() != parus::parse::ReparseMode::kFallbackFullRebuild) {
        std::cerr << "  - structural newline edit must force fallback rebuild, got "
                  << mode_name_(inc.last_mode()) << "\n";
        return false;
    }

    parus::parse::IncrementalParserSession full{};
    parus::diag::Bag full_bag{};
    if (!full.initialize(next_src, /*file_id=*/1, full_bag)) {
        std::cerr << "  - structural fallback full parse failed\n";
        return false;
    }

    return compare_snapshots_strict_("structural-newline", inc_bag, inc.snapshot(), full_bag, full.snapshot());
}

} // namespace

int main() {
    setenv("PARUS_DISABLE_INCREMENTAL_MERGE", "0", 1);

    std::vector<std::filesystem::path> files{};
    if (!collect_case_files_(files)) return 1;

    std::cout << "[incremental-merge] cases=" << files.size() << "\n";
    size_t passed = 0;
    for (const auto& p : files) {
        std::cout << "  * " << p.filename().string() << "\n";
        if (!run_case_(p)) return 1;
        ++passed;
    }
    if (!test_structural_newline_forces_fallback_()) return 1;

    std::cout << "[incremental-merge] passed " << passed << "/" << files.size() << "\n";
    return 0;
}
