#include <parus/cimport/LibClangProbe.hpp>

#if defined(PARUS_HAS_LIBCLANG) && PARUS_HAS_LIBCLANG
#include <clang-c/Index.h>
#endif


namespace parus::cimport {

    LibClangProbeResult probe_libclang() {
        LibClangProbeResult out{};

#if defined(PARUS_HAS_LIBCLANG) && PARUS_HAS_LIBCLANG
        out.available = true;

        const CXString v = clang_getClangVersion();
        const char* c = clang_getCString(v);
        if (c != nullptr) {
            out.version = c;
        }
        clang_disposeString(v);

#if defined(PARUS_LIBCLANG_INCLUDE_DIR)
        out.include_dir = PARUS_LIBCLANG_INCLUDE_DIR;
#endif
#if defined(PARUS_LIBCLANG_LIBRARY_PATH)
        out.library_path = PARUS_LIBCLANG_LIBRARY_PATH;
#endif
#else
        out.available = false;
        out.version = "libclang unavailable";
#endif

        return out;
    }

} // namespace parus::cimport
