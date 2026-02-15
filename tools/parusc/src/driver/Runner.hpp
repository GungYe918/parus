// tools/parusc/src/driver/Runner.hpp
#pragma once

#include "../cli/Options.hpp"

namespace parusc::driver {

    /// @brief 파싱/타입체크/SIR(OIR) 파이프라인을 실행한다.
    int run(const cli::Options& opt);

} // namespace parusc::driver
