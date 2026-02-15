// compiler/parusc/src/driver/Driver.cpp
#include <parusc/driver/Driver.hpp>

#include <parusc/p0/P0Compiler.hpp>

#include <parus/os/File.hpp>

#include <iostream>
#include <string>

namespace parusc::driver {

    namespace {

        /// @brief 입력 파일을 읽고 내부 컴파일러 호출 정보를 구성한다.
        bool prepare_invocation_(
            const cli::Options& opt,
            p0::Invocation& out_inv,
            std::string& out_err
        ) {
            if (opt.inputs.empty()) {
                out_err = "no input file";
                return false;
            }
            const auto& input = opt.inputs.front();

            std::string src;
            std::string io_err;
            if (!parus::open_file(input, src, io_err)) {
                out_err = io_err;
                return false;
            }

            out_inv.input_path = input;
            out_inv.normalized_input_path = parus::normalize_path(input);
            out_inv.source_text = std::move(src);
            out_inv.options = &opt;
            return true;
        }

    } // namespace

    int run(const cli::Options& opt) {
        switch (opt.mode) {
            case cli::Mode::kCompile: {
                p0::Invocation inv{};
                std::string err;
                if (!prepare_invocation_(opt, inv, err)) {
                    std::cerr << "error: " << err << "\n";
                    return 1;
                }
                return p0::run(inv);
            }
            case cli::Mode::kUsage:
            case cli::Mode::kVersion:
            default:
                return 0;
        }
    }

} // namespace parusc::driver
