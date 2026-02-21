#include <parus/macro/Expander.hpp>

#include <parus/syntax/TokenKind.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace parus::macro {

    namespace {
        using K = syntax::TokenKind;

        static uint64_t g_hygiene_seq = 0;

        static bool is_generated_ident_(
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            size_t i
        ) {
            if (i >= tokens.size() || i >= generated_mask.size()) return false;
            if (!generated_mask[i]) return false;
            return tokens[i].kind == K::kIdent;
        }

        static void register_binder_(
            ast::AstArena& ast,
            std::unordered_map<std::string_view, std::string_view>& renames,
            std::string_view name
        ) {
            if (name.empty()) return;
            if (name == "self") return;
            if (renames.find(name) != renames.end()) return;

            const auto gensym = ast.add_owned_string("__pm_g" + std::to_string(++g_hygiene_seq));
            renames.emplace(name, gensym);
        }

        static void collect_let_set_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames
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
                register_binder_(ast, renames, tokens[j].lexeme);
            }
        }

        static void collect_def_param_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames
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
                    register_binder_(ast, renames, tokens[j].lexeme);
                }
            }
        }

        static void collect_loop_binders_(
            ast::AstArena& ast,
            const std::vector<Token>& tokens,
            const std::vector<uint8_t>& generated_mask,
            std::unordered_map<std::string_view, std::string_view>& renames
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
                    register_binder_(ast, renames, tokens[k].lexeme);
                    break;
                }
            }
        }
    } // namespace

    void apply_binder_hygiene(
        ast::AstArena& ast,
        std::vector<Token>& tokens,
        const std::vector<uint8_t>& generated_mask
    ) {
        if (tokens.empty()) return;
        if (tokens.size() != generated_mask.size()) return;

        std::unordered_map<std::string_view, std::string_view> renames{};
        collect_let_set_binders_(ast, tokens, generated_mask, renames);
        collect_def_param_binders_(ast, tokens, generated_mask, renames);
        collect_loop_binders_(ast, tokens, generated_mask, renames);
        if (renames.empty()) return;

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_generated_ident_(tokens, generated_mask, i)) continue;
            const auto it = renames.find(tokens[i].lexeme);
            if (it == renames.end()) continue;
            tokens[i].lexeme = it->second;
        }
    }
}
