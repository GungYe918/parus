# frontend/cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/src/parse/CMakeLists.txt)
include(${CMAKE_CURRENT_SOURCE_DIR}/src/tyck/CMakeLists.txt)
include(${CMAKE_CURRENT_SOURCE_DIR}/src/sir/CMakeLists.txt)
include(${CMAKE_CURRENT_SOURCE_DIR}/src/cap/CMakeLists.txt)

add_library(parus_frontend STATIC
    src/diag/render.cpp
    src/text/source_manager.cpp
    src/lex/lexer.cpp

    ${PARUS_PARSE_SOURCES}
    src/macro/expander.cpp
    src/macro/matcher_token.cpp
    src/macro/matcher_typed.cpp
    src/macro/hygiene.cpp
    src/type/type_resolve.cpp
    src/cimport/libclang_probe.cpp
    src/cimport/c_header_import.cpp
    src/cimport/cimport_payload.cpp
    src/cimport/toolchain_resolver.cpp

    src/passes/check_pipe_hole.cpp
    src/passes/check_place_expr.cpp
    src/passes/check_pub_commit.cpp
    src/passes/check_top_level_decl_only.cpp
    src/passes/canonicalize_pipe.cpp
    src/passes/generic_prep.cpp
    src/passes/compiler_directive_eval.cpp
    src/passes/name_resolve.cpp
    src/passes/passes.cpp
    ${PARUS_CAP_SOURCES}

    src/os/file.cpp
    ${PARUS_TYCK_SOURCES}
    src/num/big_int.cpp

    # SIR
    ${PARUS_SIR_SOURCES}

    # OIR (still frontend IR)
    src/oir/oir_builder.cpp
    src/oir/oir_passes.cpp
    src/oir/oir_print.cpp
    src/oir/oir_verify.cpp
)


target_include_directories(parus_frontend
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(parus_frontend PUBLIC cxx_std_23)

if (PARUS_HAS_LIBCLANG AND TARGET parus_libclang)
    target_link_libraries(parus_frontend PRIVATE parus_libclang)
    target_compile_definitions(parus_frontend PUBLIC PARUS_HAS_LIBCLANG=1)
    if (PARUS_LIBCLANG_RESOLVED_INCLUDE_DIR)
        target_compile_definitions(parus_frontend PRIVATE PARUS_LIBCLANG_INCLUDE_DIR="${PARUS_LIBCLANG_RESOLVED_INCLUDE_DIR}")
    endif()
    if (PARUS_LIBCLANG_RESOLVED_LIBRARY)
        target_compile_definitions(parus_frontend PRIVATE PARUS_LIBCLANG_LIBRARY_PATH="${PARUS_LIBCLANG_RESOLVED_LIBRARY}")
    endif()
else()
    target_compile_definitions(parus_frontend PUBLIC PARUS_HAS_LIBCLANG=0)
endif()

# ---- warnings (no -Werror; keep iteration smooth) ----
if (MSVC)
    target_compile_options(parus_frontend PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_frontend PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

# Optional: faster rebuilds for clang/gcc
# target_compile_options(parus_frontend PRIVATE -fno-omit-frame-pointer)
