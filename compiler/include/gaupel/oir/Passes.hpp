// compiler/include/gaupel/oir/Passes.hpp
#pragma once
#include <gaupel/oir/Inst.hpp>


namespace gaupel::oir {

    // v0는 빌더/프린트/검증이 우선.
    // passes는 이후에 DCE/SimplifyCFG부터 추가.
    void run_passes(Module& m);

} // namespace gaupel::oir