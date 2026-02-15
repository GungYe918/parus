// compiler/parusc/include/parusc/driver/Driver.hpp
#pragma once

#include <parusc/cli/Options.hpp>

namespace parusc::driver {

    /// @brief 단일 입력 파일에 대해 프론트엔드+백엔드를 실행한다.
    int run(const cli::Options& opt);

} // namespace parusc::driver
