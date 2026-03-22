#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace parus {

    inline std::string compute_module_head(
        std::string_view bundle_root,
        std::string_view source_path,
        std::string_view bundle_name
    ) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        fs::path root(bundle_root);
        if (root.is_relative()) root = fs::absolute(root, ec);
        if (ec) {
            ec.clear();
            root = fs::path(bundle_root);
        }
        fs::path root_norm = fs::weakly_canonical(root, ec);
        if (ec || root_norm.empty()) {
            ec.clear();
            root_norm = root.lexically_normal();
        }

        fs::path src(source_path);
        if (src.is_relative()) src = fs::absolute(src, ec);
        if (ec) {
            ec.clear();
            src = fs::path(source_path);
        }
        fs::path src_norm = fs::weakly_canonical(src, ec);
        if (ec || src_norm.empty()) {
            ec.clear();
            src_norm = src.lexically_normal();
        }

        fs::path rel = src_norm.lexically_relative(root_norm);
        const std::string rel_s = rel.generic_string();
        if (rel.empty() || rel_s.empty() || rel_s == "." || rel_s.starts_with("..")) {
            rel = src_norm.filename();
        }

        fs::path dir = rel.parent_path();
        std::vector<std::string> segs{};
        bool stripped_src = false;
        for (const auto& seg : dir) {
            const std::string s = seg.string();
            if (s.empty() || s == ".") continue;
            if (!stripped_src && s == "src") {
                stripped_src = true;
                continue;
            }
            segs.push_back(s);
        }

        if (segs.empty()) return std::string(bundle_name);
        std::string out{};
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i) out += "::";
            out += segs[i];
        }
        return out;
    }

    inline std::string normalize_import_head_top(std::string_view import_head) {
        if (import_head.empty()) return {};
        std::string_view s = import_head;
        if (s.starts_with("::")) s.remove_prefix(2);
        if (s.empty() || s.ends_with("::")) return {};
        const size_t pos = s.find("::");
        std::string_view top = (pos == std::string_view::npos) ? s : s.substr(0, pos);
        if (top.empty() || top.find(':') != std::string_view::npos) return {};
        return std::string(top);
    }

    inline std::string normalize_core_public_module_head(
        std::string_view bundle_name,
        std::string_view module_head
    ) {
        if (bundle_name != "core") {
            return std::string(module_head);
        }
        if (module_head.empty()) {
            return std::string("core");
        }
        if (module_head == "core" || module_head.starts_with("core::")) {
            return std::string(module_head);
        }
        std::string out = "core::";
        out += module_head;
        return out;
    }

    inline std::string strip_module_prefix(
        std::string_view path,
        std::string_view module_head
    ) {
        if (path.empty()) return {};
        if (module_head.empty()) return std::string(path);
        if (path == module_head) return {};

        const std::string prefix = std::string(module_head) + "::";
        if (path.starts_with(prefix)) {
            return std::string(path.substr(prefix.size()));
        }
        return std::string(path);
    }

    inline std::string short_core_module_head(
        std::string_view decl_bundle_name,
        std::string_view module_head
    ) {
        if (decl_bundle_name != "core") return {};
        if (!module_head.starts_with("core::")) return {};
        return std::string(module_head.substr(std::string_view("core::").size()));
    }

    inline bool same_external_module_head(
        std::string_view current_module_head,
        std::string_view external_module_head,
        std::string_view decl_bundle_name
    ) {
        if (current_module_head.empty() || external_module_head.empty()) return false;
        if (current_module_head == external_module_head) return true;

        const std::string short_head = short_core_module_head(decl_bundle_name, external_module_head);
        if (!short_head.empty() && current_module_head == short_head) return true;

        if (decl_bundle_name == "core" &&
            current_module_head.starts_with("core::") &&
            !short_head.empty() &&
            current_module_head.substr(std::string_view("core::").size()) == short_head) {
            return true;
        }
        return false;
    }

    inline std::vector<std::string> candidate_names_for_external_export(
        std::string_view path,
        std::string_view module_head,
        std::string_view decl_bundle_name,
        std::string_view current_module_head
    ) {
        std::vector<std::string> names{};
        names.reserve(6);
        if (!path.empty()) names.push_back(std::string(path));

        const std::string local = strip_module_prefix(path, module_head);
        if (!module_head.empty() && !local.empty()) {
            const std::string module_qualified = std::string(module_head) + "::" + local;
            names.push_back(module_qualified);

            if (same_external_module_head(current_module_head, module_head, decl_bundle_name)) {
                names.push_back(local);
            }

            const std::string short_head = short_core_module_head(decl_bundle_name, module_head);
            if (!short_head.empty()) {
                names.push_back(short_head + "::" + local);
            }

            if (!decl_bundle_name.empty()) {
                std::string qualified_module(module_head);
                const std::string bundle_prefix = std::string(decl_bundle_name) + "::";
                if (!(qualified_module == decl_bundle_name || qualified_module.starts_with(bundle_prefix))) {
                    qualified_module = bundle_prefix + qualified_module;
                }
                names.push_back(qualified_module + "::" + local);
            }
        } else if (!decl_bundle_name.empty() && !path.empty()) {
            names.push_back(std::string(decl_bundle_name) + "::" + std::string(path));
        }

        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

} // namespace parus
