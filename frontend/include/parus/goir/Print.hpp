#pragma once

#include <parus/goir/GOIR.hpp>
#include <parus/ty/TypePool.hpp>

#include <string>

namespace parus::goir {

    std::string to_string(const Module& module, const parus::ty::TypePool* types = nullptr);

} // namespace parus::goir
