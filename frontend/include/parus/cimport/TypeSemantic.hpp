#pragma once

#include <parus/ty/TypePool.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace parus::cimport {

    enum class TypeSemanticKind : uint8_t {
        kBuiltin = 0,
        kNamed,
        kOptional,
        kBorrow,
        kEscape,
        kPtr,
        kArray,
        kFn,
    };

    struct TypeSemanticNode {
        TypeSemanticKind kind = TypeSemanticKind::kBuiltin;
        std::string name{};
        bool ptr_is_mut = false;
        bool array_has_size = false;
        uint32_t array_size = 0;
        bool fn_is_throwing = false;
        bool fn_is_c_abi = false;
        bool fn_is_variadic = false;
        ty::CCallConv fn_callconv = ty::CCallConv::kDefault;
        std::vector<TypeSemanticNode> children{};
    };

    bool parse_type_semantic(std::string_view text, TypeSemanticNode& out);

    std::string serialize_type_semantic(const TypeSemanticNode& node);

    std::string rewrite_type_semantic_with_alias(
        std::string_view text,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names
    );

    ty::TypeId build_type_from_semantic(const TypeSemanticNode& node, ty::TypePool& types);
    bool build_type_semantic_from_type(ty::TypeId type, const ty::TypePool& types, TypeSemanticNode& out);
    std::string serialize_type_semantic_from_type(ty::TypeId type, const ty::TypePool& types);

} // namespace parus::cimport
