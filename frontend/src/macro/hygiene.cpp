#include <parus/macro/Expander.hpp>

#include <parus/syntax/TokenKind.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace parus::macro {

    namespace {
        using K = syntax::TokenKind;

        static bool is_generated_ident_(
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            size_t i
        ) {
            if (i >= tokens.size() || i >= generated_mask.size()) return false;
            if (!generated_mask[i]) return false;
            return tokens[i].kind == K::kIdent;
        }

        static uint64_t mix64_(uint64_t x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return x;
        }

        static uint64_t hash_name_(std::string_view s) {
            uint64_t h = 1469598103934665603ULL;
            for (const unsigned char c : s) {
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ULL;
            }
            return h;
        }

        static uint64_t derive_default_seed_(const std::vector<Token>& tokens) {
            if (tokens.empty()) return 0x5f4d3c2b1a9e8d7cULL;
            const auto& first = tokens.front().span;
            const auto& last = tokens.back().span;
            uint64_t seed = (static_cast<uint64_t>(first.file_id) << 48) ^
                            (static_cast<uint64_t>(first.lo) << 24) ^
                            static_cast<uint64_t>(last.hi);
            seed ^= mix64_(static_cast<uint64_t>(tokens.size()));
            return seed;
        }

        static void register_binder_(
            ast::AstArena& ast,
            std::unordered_map<std::string_view, std::string_view>& renames,
            std::string_view name,
            size_t binder_index,
            uint64_t seed,
            uint64_t& seq
        ) {
            if (name.empty()) return;
            if (name == "self") return;
            if (renames.find(name) != renames.end()) return;

            const uint64_t serial = ++seq;
            uint64_t id = seed;
            id ^= mix64_(serial);
            id ^= mix64_(static_cast<uint64_t>(binder_index));
            id ^= mix64_(hash_name_(name));
            const auto gensym = ast.add_owned_string("__pm_g" + std::to_string(id));
            renames.emplace(name, gensym);
        }

        static void collect_let_set_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames,
            uint64_t seed,
            uint64_t& seq
        ) {
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!generated_mask[i]) continue;
                const auto k = tokens[i].kind;
                if (k != K::kKwLet && k != K::kKwSet) continue;

                size_t j = i + 1;
                if (j < tokens.size() && tokens[j].kind == K::kKwMut && generated_mask[j]) {
                    ++j;
                }
                if (!is_generated_ident_(tokens, generated_mask, j)) continue;
                register_binder_(ast, renames, tokens[j].lexeme, j, seed, seq);
            }
        }

        static void collect_def_param_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames,
            uint64_t seed,
            uint64_t& seq
        ) {
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!generated_mask[i] || tokens[i].kind != K::kKwFn) continue;

                size_t lparen = i + 1;
                while (lparen < tokens.size() && tokens[lparen].kind != K::kLParen) ++lparen;
                if (lparen >= tokens.size()) continue;

                int depth = 0;
                for (size_t j = lparen; j < tokens.size(); ++j) {
                    const auto k = tokens[j].kind;
                    if (k == K::kLParen) {
                        ++depth;
                        continue;
                    }
                    if (k == K::kRParen) {
                        --depth;
                        if (depth == 0) {
                            i = j;
                            break;
                        }
                        continue;
                    }
                    if (depth != 1) continue;
                    if (!is_generated_ident_(tokens, generated_mask, j)) continue;
                    if (j + 1 >= tokens.size()) continue;
                    if (tokens[j + 1].kind != K::kColon) continue;
                    register_binder_(ast, renames, tokens[j].lexeme, j, seed, seq);
                }
            }
        }

        static void collect_loop_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames,
            uint64_t seed,
            uint64_t& seq
        ) {
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!generated_mask[i] || tokens[i].kind != K::kKwLoop) continue;

                size_t j = i + 1;
                while (j < tokens.size() && tokens[j].kind != K::kLParen) ++j;
                if (j >= tokens.size()) continue;

                int depth = 0;
                for (size_t k = j; k < tokens.size(); ++k) {
                    if (tokens[k].kind == K::kLParen) {
                        ++depth;
                        continue;
                    }
                    if (tokens[k].kind == K::kRParen) {
                        --depth;
                        if (depth == 0) break;
                        continue;
                    }
                    if (depth != 1) continue;
                    if (!is_generated_ident_(tokens, generated_mask, k)) continue;
                    if (k + 1 >= tokens.size()) continue;
                    if (tokens[k + 1].kind != K::kKwIn) continue;
                    register_binder_(ast, renames, tokens[k].lexeme, k, seed, seq);
                    break;
                }
            }
        }

        static void collect_catch_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames,
            uint64_t seed,
            uint64_t& seq
        ) {
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!generated_mask[i] || tokens[i].kind != K::kKwCatch) continue;

                size_t lparen = i + 1;
                while (lparen < tokens.size() && tokens[lparen].kind != K::kLParen &&
                       tokens[lparen].kind != K::kLBrace && tokens[lparen].kind != K::kSemicolon) {
                    ++lparen;
                }
                if (lparen >= tokens.size() || tokens[lparen].kind != K::kLParen) continue;

                int depth = 0;
                for (size_t j = lparen; j < tokens.size(); ++j) {
                    const auto k = tokens[j].kind;
                    if (k == K::kLParen) {
                        ++depth;
                        continue;
                    }
                    if (k == K::kRParen) {
                        --depth;
                        if (depth == 0) break;
                        continue;
                    }
                    if (depth != 1) continue;
                    if (!is_generated_ident_(tokens, generated_mask, j)) continue;
                    register_binder_(ast, renames, tokens[j].lexeme, j, seed, seq);
                    break;
                }
            }
        }

        static void collect_switch_case_bind_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::vector<uint8_t>& no_rename_mask,
            std::unordered_map<std::string_view, std::string_view>& renames,
            uint64_t seed,
            uint64_t& seq
        ) {
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!generated_mask[i] || tokens[i].kind != K::kKwCase) continue;

                int paren = 0;
                int bracket = 0;
                int brace = 0;
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    const auto k = tokens[j].kind;
                    if (k == K::kColon && paren == 0 && bracket == 0 && brace == 0) {
                        break;
                    }
                    if (k == K::kLParen) {
                        ++paren;
                        continue;
                    }
                    if (k == K::kRParen) {
                        if (paren > 0) --paren;
                        continue;
                    }
                    if (k == K::kLBracket) {
                        ++bracket;
                        continue;
                    }
                    if (k == K::kRBracket) {
                        if (bracket > 0) --bracket;
                        continue;
                    }
                    if (k == K::kLBrace) {
                        ++brace;
                        continue;
                    }
                    if (k == K::kRBrace) {
                        if (brace > 0) --brace;
                        continue;
                    }

                    if (paren != 1) continue;
                    if (!is_generated_ident_(tokens, generated_mask, j)) continue;
                    if (j + 2 >= tokens.size()) continue;
                    if (tokens[j + 1].kind != K::kColon) continue;
                    if (!is_generated_ident_(tokens, generated_mask, j + 2)) continue;

                    if (j < no_rename_mask.size()) {
                        no_rename_mask[j] = 1;
                    }
                    register_binder_(ast, renames, tokens[j + 2].lexeme, j + 2, seed, seq);
                    j += 2;
                }
            }
        }
    } // namespace

    void apply_binder_hygiene(
        ast::AstArena& ast,
        std::vector<Token>& tokens,
        const std::vector<uint8_t>& generated_mask,
        uint64_t hygiene_seed
    ) {
        if (tokens.empty()) return;
        if (tokens.size() != generated_mask.size()) return;

        uint64_t seed = hygiene_seed;
        if (seed == 0) {
            seed = derive_default_seed_(tokens);
        }
        seed ^= mix64_(static_cast<uint64_t>(ast.exprs().size()));
        seed ^= mix64_(static_cast<uint64_t>(ast.stmts().size()));

        uint64_t seq = 0;
        std::vector<uint8_t> no_rename_mask(tokens.size(), 0);
        std::unordered_map<std::string_view, std::string_view> renames{};
        collect_let_set_binders_(ast, tokens, generated_mask, renames, seed, seq);
        collect_def_param_binders_(ast, tokens, generated_mask, renames, seed, seq);
        collect_loop_binders_(ast, tokens, generated_mask, renames, seed, seq);
        collect_catch_binders_(ast, tokens, generated_mask, renames, seed, seq);
        collect_switch_case_bind_binders_(ast, tokens, generated_mask, no_rename_mask, renames, seed, seq);
        if (renames.empty()) return;

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_generated_ident_(tokens, generated_mask, i)) continue;
            if (i < no_rename_mask.size() && no_rename_mask[i]) continue;
            const auto it = renames.find(tokens[i].lexeme);
            if (it == renames.end()) continue;
            tokens[i].lexeme = it->second;
        }
    }
}
