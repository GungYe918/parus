// compiler/include/gaupel/oir/Inst.hpp
#pragma once

// NOTE:
// This header used to re-define OIR core types (BinOp/Inst/Terminator/Block/Function...),
// which caused ODR/redefinition errors whenever both OIR.hpp and Inst.hpp were included.
// From now on, OIR core definitions live ONLY in OIR.hpp.
// If you need helpers/utilities, put them here (but do not re-define core structs/enums).

#include <gaupel/oir/OIR.hpp>

namespace gaupel::oir {

    // (optional) helper visitors / predicates can live here later.
    // Keep this header lightweight and definition-free for core IR types.

} // namespace gaupel::oir