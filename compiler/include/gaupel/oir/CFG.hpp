// compiler/include/gaupel/oir/CFG.hpp
#pragma once
#include <gaupel/oir/Inst.hpp>
#include <vector>


namespace gaupel::oir {

    // 기초 스켈레톤만 작성 (TODO: pred/succ 계산 확장)
    struct CFGView {
        const Module* m = nullptr;
        const Function* f = nullptr;

        explicit CFGView(const Module& mod, const Function& fn) : m(&mod), f(&fn) {}
    };

} // namespace gaupel::oir