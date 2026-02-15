// compiler/include/parus/oir/Verify.hpp
#pragma once
#include <parus/oir/Inst.hpp>
#include <string>
#include <vector>


namespace parus::oir {

    struct VerifyError { std::string msg; };
    std::vector<VerifyError> verify(const Module& m);

} // namespace parus::oir