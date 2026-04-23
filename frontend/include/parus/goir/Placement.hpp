#pragma once

#include <parus/goir/Builder.hpp>

namespace parus::goir {

    struct PlacementResult {
        bool ok = false;
        Module mod{};
        std::vector<Message> messages{};
    };

    PlacementResult place_module(const Module& open_module);

} // namespace parus::goir
