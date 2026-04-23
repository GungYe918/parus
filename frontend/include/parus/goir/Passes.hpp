#pragma once

#include <parus/goir/GOIR.hpp>

namespace parus::goir {

    void run_open_passes(Module& module);
    void run_placed_passes(Module& module);

} // namespace parus::goir
