#pragma once

#include <parus/ty/Type.hpp>
#include <parus/ty/TypePool.hpp>

#include <optional>
#include <string_view>

namespace parus::cimport {

    std::optional<ty::Builtin> parse_core_builtin_use_payload(std::string_view payload);

    ty::TypeId canonicalize_core_ext_type_repr(ty::TypeId t, ty::TypePool& types);

    ty::TypeId parse_external_type_repr(
        std::string_view type_repr,
        std::string_view type_semantic,
        std::string_view inst_payload,
        ty::TypePool& types
    );

    inline ty::TypeId parse_external_type_repr(
        std::string_view type_repr,
        std::string_view inst_payload,
        ty::TypePool& types
    ) {
        return parse_external_type_repr(type_repr, {}, inst_payload, types);
    }

} // namespace parus::cimport
