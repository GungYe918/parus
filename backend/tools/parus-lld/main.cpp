// backend/tools/parus-lld/main.cpp
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

    /// @brief 단일 쉘 인자를 안전한 single-quote 문자열로 변환한다.
    std::string shell_quote_(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    /// @brief clang++ 실행 파일 경로를 선택한다.
    std::string select_clangxx_() {
        namespace fs = std::filesystem;
        if (fs::exists("/usr/bin/clang++")) return "/usr/bin/clang++";
        return "clang++";
    }

    /// @brief parus-lld의 심플 링크 모드를 clang++ -fuse-ld=lld로 위임한다.
    int run_simple_driver_mode_(int argc, char** argv) {
        if (argc < 2) {
            std::cerr << "parus-lld: no input objects\n";
            return 1;
        }

        std::string cmd;
        cmd.reserve(512);
        cmd += shell_quote_(select_clangxx_());
        cmd += " -fuse-ld=lld";

        for (int i = 1; i < argc; ++i) {
            cmd += " ";
            cmd += shell_quote_(argv[i]);
        }

        return std::system(cmd.c_str());
    }

} // namespace

/// @brief parusc 드라이버가 호출하는 경량 링크 프런트. 현재는 lld 기반 clang 드라이버를 래핑한다.
int main(int argc, char** argv) {
    return run_simple_driver_mode_(argc, argv);
}
