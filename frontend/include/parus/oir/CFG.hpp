// frontend/include/parus/oir/CFG.hpp
#pragma once
#include <parus/oir/Inst.hpp>
#include <vector>


namespace parus::oir {

    // 기초 스켈레톤만 작성 (TODO: pred/succ 계산 확장)
    struct CFGView {
        const Module* m = nullptr;
        const Function* f = nullptr;

        explicit CFGView(const Module& mod, const Function& def) : m(&mod), f(&def) {}
    };

} // namespace parus::oir