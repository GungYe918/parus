// compiler/src/os/file.cpp
#include <parus/os/File.hpp>

#include <cstdio>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <fileapi.h>
#else
    // POSIX (Linux, macOS)
    #include <cerrno>   // errno
    #include <cstring>  // std::strerror
    #include <cstdlib>  // realpath
    #include <limits.h> // PATH_MAX (may be missing on some platforms)

    // Some macOS setups may not define PATH_MAX reliably from <limits.h>.
    // If PATH_MAX is missing, fall back to a conservative value.
    #ifndef PATH_MAX
        #define PATH_MAX 4096
    #endif
#endif

namespace parus {

    static void normalize_newlines_inplace(std::string& s) {
        // CRLF -> LF, 단독 CR 제거
        std::string out;
        out.reserve(s.size());

        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\r') {
                // \r\n -> \n : \r은 버리고, 다음 루프에서 \n은 그대로 push됨
                // 단독 \r도 제거
                continue;
            }
            out.push_back(c);
        }

        s.swap(out);
    }

    bool open_file(const std::string& path, std::string& out_content, std::string& out_error) {
        out_error.clear();
        out_content.clear();

        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) {
        #if defined(_WIN32)
            out_error = "CANNOT open file.";
        #else
            // POSIX: errno 기반 메시지
            out_error = std::string("CANNOT open file: ") + std::strerror(errno);
        #endif
            return false;
        }

        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);

        if (sz < 0) {
            std::fclose(fp);
            out_error = "파일 크기를 읽을 수 없습니다.";
            return false;
        }

        out_content.resize(static_cast<size_t>(sz));
        size_t n = std::fread(out_content.data(), 1, out_content.size(), fp);
        std::fclose(fp);

        if (n != out_content.size()) {
            out_error = "파일 읽기 중 일부만 읽혔습니다.";
            return false;
        }

        normalize_newlines_inplace(out_content);
        return true;
    }

    std::string normalize_path(const std::string& path) {
    #if defined(_WIN32)
        // Windows: GetFullPathNameA로 절대경로화(간단 버전)
        char buf[MAX_PATH];
        DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, buf, nullptr);
        if (n == 0 || n >= MAX_PATH) return path;
        return std::string(buf);
    #else
        // POSIX (Linux, macOS): realpath
        // realpath는 "실제 존재하는 경로"가 아니면 실패할 수 있음.
        char buf[PATH_MAX];
        if (::realpath(path.c_str(), buf) != nullptr) {
            return std::string(buf);
        }
        return path;
    #endif
    }

} // namespace parus