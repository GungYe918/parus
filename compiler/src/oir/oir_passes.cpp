// compiler/src/oir/oir_passes.cpp
#include <gaupel/oir/Passes.hpp>


namespace gaupel::oir {

    void run_passes(Module& /*m*/) {
        // v0: nothing yet
        // next: SimplifyCFG + DCE + ConstFold
    }

} // namespace gaupel::oir