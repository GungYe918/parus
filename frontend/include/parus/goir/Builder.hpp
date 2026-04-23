#pragma once

#include <parus/goir/GOIR.hpp>
#include <parus/sir/SIR.hpp>
#include <parus/ty/TypePool.hpp>

#include <string>
#include <vector>

namespace parus::goir {

    struct Message {
        std::string msg{};
    };

    struct BuildResult {
        bool ok = false;
        Module mod{};
        std::vector<Message> messages{};
    };

    BuildResult build_from_sir(
        const parus::sir::Module& sir,
        const parus::ty::TypePool& types
    );

} // namespace parus::goir
