include_guard(GLOBAL)

function(parus_configure_libclang)
    if (NOT PARUS_ENABLE_CIMPORT)
        set(PARUS_HAS_LIBCLANG OFF CACHE INTERNAL "Whether libclang is available" FORCE)
        message(STATUS "parus: C import preparation disabled (PARUS_ENABLE_CIMPORT=OFF)")
        return()
    endif()

    set(_include_hints)
    set(_library_hints)

    if (PARUS_LIBCLANG_ROOT)
        list(APPEND _include_hints
            "${PARUS_LIBCLANG_ROOT}/include"
            "${PARUS_LIBCLANG_ROOT}/include/clang-c"
        )
        list(APPEND _library_hints
            "${PARUS_LIBCLANG_ROOT}/lib"
            "${PARUS_LIBCLANG_ROOT}/lib64"
        )
    endif()

    list(APPEND _include_hints
        "/opt/homebrew/opt/llvm/include"
        "/usr/local/opt/llvm/include"
        "/usr/lib/llvm-20/include"
        "/usr/lib/llvm-21/include"
        "/usr/include"
    )
    list(APPEND _library_hints
        "/opt/homebrew/opt/llvm/lib"
        "/usr/local/opt/llvm/lib"
        "/usr/lib/llvm-20/lib"
        "/usr/lib/llvm-21/lib"
        "/usr/lib"
    )

    if (PARUS_LIBCLANG_INCLUDE_DIR)
        set(_libclang_include_dir "${PARUS_LIBCLANG_INCLUDE_DIR}")
    else()
        find_path(_libclang_include_dir
            NAMES clang-c/Index.h
            HINTS ${_include_hints}
        )
    endif()

    if (PARUS_LIBCLANG_LIBRARY)
        set(_libclang_library "${PARUS_LIBCLANG_LIBRARY}")
    else()
        find_library(_libclang_library
            NAMES clang libclang
            HINTS ${_library_hints}
        )
    endif()

    if (_libclang_include_dir AND _libclang_library)
        if (NOT TARGET parus_libclang)
            add_library(parus_libclang INTERFACE)
        endif()
        target_include_directories(parus_libclang INTERFACE "${_libclang_include_dir}")
        target_link_libraries(parus_libclang INTERFACE "${_libclang_library}")

        set(PARUS_HAS_LIBCLANG ON CACHE INTERNAL "Whether libclang is available" FORCE)
        set(PARUS_LIBCLANG_RESOLVED_INCLUDE_DIR "${_libclang_include_dir}" CACHE INTERNAL "Resolved clang-c include directory" FORCE)
        set(PARUS_LIBCLANG_RESOLVED_LIBRARY "${_libclang_library}" CACHE INTERNAL "Resolved libclang library path" FORCE)

        message(STATUS "parus: libclang configured")
        message(STATUS "  include: ${_libclang_include_dir}")
        message(STATUS "  library: ${_libclang_library}")
        return()
    endif()

    set(PARUS_HAS_LIBCLANG OFF CACHE INTERNAL "Whether libclang is available" FORCE)
    set(PARUS_LIBCLANG_RESOLVED_INCLUDE_DIR "" CACHE INTERNAL "Resolved clang-c include directory" FORCE)
    set(PARUS_LIBCLANG_RESOLVED_LIBRARY "" CACHE INTERNAL "Resolved libclang library path" FORCE)

    if (PARUS_CIMPORT_REQUIRE_LIBCLANG)
        message(FATAL_ERROR
            "parus: libclang is required but not found.\n"
            "  PARUS_LIBCLANG_ROOT='${PARUS_LIBCLANG_ROOT}'\n"
            "  PARUS_LIBCLANG_INCLUDE_DIR='${PARUS_LIBCLANG_INCLUDE_DIR}'\n"
            "  PARUS_LIBCLANG_LIBRARY='${PARUS_LIBCLANG_LIBRARY}'\n"
        )
    else()
        message(WARNING
            "parus: libclang not found; C import preparation code will build in disabled mode.\n"
            "  Set PARUS_CIMPORT_REQUIRE_LIBCLANG=ON to make this a hard error."
        )
    endif()
endfunction()
