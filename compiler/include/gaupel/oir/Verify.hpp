// compiler/include/gaupel/oir/Verify.hpp
#pragma once
#include <gaupel/oir/Inst.hpp>
#include <string>
#include <vector>


namespace gaupel::oir {

    struct VerifyError { std::string msg; };
    std::vector<VerifyError> verify(const Module& m);

} // namespace gaupel::oir