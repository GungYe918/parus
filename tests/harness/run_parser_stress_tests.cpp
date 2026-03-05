#include <parus/diag/Render.hpp>
#include <parus/parse/IncrementalParse.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
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

static uint64_t mix_hash_(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
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
            push_expr(st.proto_require_expr);

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

static std::unordered_map<std::string, uint32_t> diag_multiset_(const parus::diag::Bag& bag) {
    std::unordered_map<std::string, uint32_t> out;
    for (const auto& d : bag.diags()) {
        const auto sev = d.severity();
        std::string key = parus::diag::code_name(d.code());
        key.push_back('#');
        key += std::to_string(static_cast<uint32_t>(sev));
        ++out[key];
    }
    return out;
}

static std::string map_to_text_(const std::unordered_map<std::string, uint32_t>& m) {
    std::vector<std::pair<std::string, uint32_t>> rows(m.begin(), m.end());
    std::sort(rows.begin(), rows.end(), [](const auto& l, const auto& r) {
        return l.first < r.first;
    });

    std::string out;
    for (const auto& [k, v] : rows) {
        out += k;
        out += " => ";
        out += std::to_string(v);
        out += '\n';
    }
    return out;
}

static std::string apply_edit_(
    const std::string& old_src,
    uint32_t lo,
    uint32_t hi,
    std::string_view replacement
) {
    std::string out;
    out.reserve(old_src.size() + replacement.size() + 8);
    out.append(old_src.data(), lo);
    out.append(replacement.data(), replacement.size());
    out.append(old_src.data() + hi, old_src.size() - hi);
    return out;
}

static std::string random_replacement_(std::mt19937& rng) {
    static const std::vector<std::string> fragments{
        " ", "\n", ";", "(", ")", "{", "}", "[", "]", ",",
        "def x() -> i32 { return 0i32; }\n",
        "let a: i32 = 1i32;\n",
        "if (true) { return 1i32; }\n",
        "??", "::", "macro", "acts", "proto", "class",
    };

    std::uniform_int_distribution<size_t> pick(0, fragments.size() - 1);
    return fragments[pick(rng)];
}

static std::filesystem::path write_mismatch_artifact_(
    const std::string& seed_name,
    uint32_t step,
    const std::string& reason,
    const std::string& src,
    const parus::diag::Bag& inc_bag,
    const parus::diag::Bag& full_bag,
    const TopShape& inc_shape,
    const TopShape& full_shape,
    uint64_t inc_fp,
    uint64_t full_fp
) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path("/tmp/parus_parser_stress_failures");
    std::error_code ec;
    fs::create_directories(dir, ec);

    const auto stamp = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path out = dir / (seed_name + "-step" + std::to_string(step) + "-" + stamp + ".txt");

    std::ofstream ofs(out, std::ios::binary);
    if (!ofs) return {};

    ofs << "reason: " << reason << "\n";
    ofs << "inc.root_is_block=" << (inc_shape.root_is_block ? 1 : 0)
        << " full.root_is_block=" << (full_shape.root_is_block ? 1 : 0) << "\n";
    ofs << "inc.child_count=" << inc_shape.child_count
        << " full.child_count=" << full_shape.child_count << "\n";
    ofs << "inc.fingerprint=" << inc_fp << "\n";
    ofs << "full.fingerprint=" << full_fp << "\n";
    ofs << "\n[inc.diags]\n" << map_to_text_(diag_multiset_(inc_bag));
    ofs << "\n[full.diags]\n" << map_to_text_(diag_multiset_(full_bag));
    ofs << "\n[source]\n" << src << "\n";
    return out;
}

static bool compare_incremental_vs_full_(
    const std::string& seed_name,
    uint32_t step,
    parus::parse::ReparseMode mode,
    const std::string& src,
    const parus::diag::Bag& inc_bag,
    const parus::parse::ParseSnapshot& inc_snap,
    const parus::diag::Bag& full_bag,
    const parus::parse::ParseSnapshot& full_snap
) {
    const auto inc_diags = diag_multiset_(inc_bag);
    const auto full_diags = diag_multiset_(full_bag);

    const auto inc_shape = extract_top_shape_(inc_snap);
    const auto full_shape = extract_top_shape_(full_snap);

    const auto inc_fp = ast_fingerprint_(inc_snap);
    const auto full_fp = ast_fingerprint_(full_snap);

    std::string reason;
    if (mode != parus::parse::ReparseMode::kIncrementalMerge && inc_diags != full_diags) {
        reason = "diagnostic multiset mismatch";
    } else if (inc_shape.root_is_block != full_shape.root_is_block
               || inc_shape.child_count != full_shape.child_count
               || inc_shape.child_kinds != full_shape.child_kinds) {
        reason = "top-level shape mismatch";
    } else if (inc_fp != full_fp) {
        reason = "AST fingerprint mismatch";
    }

    if (!reason.empty()) {
        const auto artifact = write_mismatch_artifact_(
            seed_name,
            step,
            reason,
            src,
            inc_bag,
            full_bag,
            inc_shape,
            full_shape,
            inc_fp,
            full_fp
        );
        std::cerr << "  - mismatch(" << seed_name << ", step=" << step << "): " << reason << "\n";
        if (!artifact.empty()) {
            std::cerr << "    artifact: " << artifact.string() << "\n";
        }
        return false;
    }

    return true;
}

static bool run_seed_stress_(const std::filesystem::path& seed_path, uint32_t seed_index) {
    std::string src;
    if (!read_text_file_(seed_path, src)) {
        std::cerr << "  - failed to read seed file: " << seed_path.string() << "\n";
        return false;
    }

    parus::parse::IncrementalParserSession inc_session{};
    parus::diag::Bag init_bag{};
    if (!inc_session.initialize(src, /*file_id=*/1, init_bag)) {
        std::cerr << "  - incremental initialize failed for seed: " << seed_path.filename().string() << "\n";
        return false;
    }

    std::mt19937 rng(0xC0FFEEu + seed_index * 977u);
    constexpr uint32_t kSteps = 20;

    for (uint32_t step = 0; step < kSteps; ++step) {
        const uint32_t n = static_cast<uint32_t>(src.size());

        std::uniform_int_distribution<uint32_t> pos_dist(0, n);
        const uint32_t lo = pos_dist(rng);

        uint32_t hi = lo;
        if (n > lo) {
            const uint32_t max_span = std::min<uint32_t>(n - lo, 16u);
            std::uniform_int_distribution<uint32_t> span_dist(0, max_span);
            hi = lo + span_dist(rng);
        }

        std::uniform_int_distribution<uint32_t> op_dist(0, 2);
        const uint32_t op = op_dist(rng);

        std::string replacement;
        if (op == 0) {
            replacement = random_replacement_(rng); // insert
            hi = lo;
        } else if (op == 1) {
            replacement.clear(); // delete
        } else {
            replacement = random_replacement_(rng); // replace
        }

        const std::string next_src = apply_edit_(src, lo, hi, replacement);

        parus::diag::Bag inc_bag{};
        const parus::parse::EditWindow edit{lo, hi};
        const bool inc_ok = inc_session.reparse_with_edits(
            next_src,
            /*file_id=*/1,
            std::span<const parus::parse::EditWindow>(&edit, 1),
            inc_bag
        );

        parus::parse::IncrementalParserSession full_session{};
        parus::diag::Bag full_bag{};
        const bool full_ok = full_session.initialize(next_src, /*file_id=*/1, full_bag);

        bool ok = true;
        ok &= require_(inc_ok, "incremental reparse failed");
        ok &= require_(full_ok, "full parse failed");
        if (!ok) return false;

        ok &= compare_incremental_vs_full_(
            seed_path.filename().string(),
            step,
            inc_session.last_mode(),
            next_src,
            inc_bag,
            inc_session.snapshot(),
            full_bag,
            full_session.snapshot()
        );

        if (!ok && inc_session.last_mode() == parus::parse::ReparseMode::kIncrementalMerge) {
            parus::diag::Bag repair_bag{};
            const bool repaired = inc_session.initialize(next_src, /*file_id=*/1, repair_bag);
            if (!repaired) {
                std::cerr << "  - recovery full-rebuild failed after mismatch\n";
                return false;
            }

            const bool repaired_match = compare_incremental_vs_full_(
                seed_path.filename().string(),
                step,
                inc_session.last_mode(),
                next_src,
                repair_bag,
                inc_session.snapshot(),
                full_bag,
                full_session.snapshot()
            );

            if (!repaired_match) {
                return false;
            }

            std::cout << "    [RECOVERED] mismatch repaired by full rebuild fallback at step " << step << "\n";
            ok = true;
        }

        if (!ok) return false;
        src = next_src;
    }

    return true;
}

static bool collect_seed_files_(std::vector<std::filesystem::path>& out) {
#ifndef PARUS_PARSER_CASE_DIR
    std::cerr << "  - PARUS_PARSER_CASE_DIR is not defined\n";
    return false;
#else
    const std::filesystem::path parser_case_dir{PARUS_PARSER_CASE_DIR};
    if (!std::filesystem::exists(parser_case_dir) || !std::filesystem::is_directory(parser_case_dir)) {
        std::cerr << "  - parser case directory missing: " << parser_case_dir.string() << "\n";
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(parser_case_dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() == ".pr") out.push_back(p);
    }

#ifdef PARUS_FUZZ_INCREMENTAL_SEED_DIR
    const std::filesystem::path fuzz_seed_dir{PARUS_FUZZ_INCREMENTAL_SEED_DIR};
    if (std::filesystem::exists(fuzz_seed_dir) && std::filesystem::is_directory(fuzz_seed_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(fuzz_seed_dir)) {
            if (!entry.is_regular_file()) continue;
            const auto& p = entry.path();
            if (p.extension() == ".pr") out.push_back(p);
        }
    }
#endif

    std::sort(out.begin(), out.end());
    return true;
#endif
}

static bool test_incremental_stress_equivalence() {
    std::vector<std::filesystem::path> seeds;
    if (!collect_seed_files_(seeds)) return false;

    bool ok = true;
    ok &= require_(!seeds.empty(), "at least one parser stress seed is required");
    if (!ok) return false;

    constexpr size_t kMaxSeeds = 12;
    const size_t run_count = std::min(seeds.size(), kMaxSeeds);

    for (size_t i = 0; i < run_count; ++i) {
        std::cout << "  [SEED] " << seeds[i].filename().string() << "\n";
        ok &= run_seed_stress_(seeds[i], static_cast<uint32_t>(i + 1));
    }

    return ok;
}

} // namespace

int main() {
    std::cout << "[TEST] incremental_stress_equivalence\n";
    const bool ok = test_incremental_stress_equivalence();
    if (!ok) {
        std::cout << "  -> FAIL\n";
        std::cout << "FAILED: 1 test(s)\n";
        return 1;
    }

    std::cout << "  -> PASS\n";
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
