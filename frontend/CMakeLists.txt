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
    src/macro/matcher_typed.cpp
    src/macro/hygiene.cpp
    src/type/type_resolve.cpp

    src/passes/check_pipe_hole.cpp
    src/passes/check_place_expr.cpp
    src/passes/check_pub_commit.cpp
    src/passes/check_top_level_decl_only.cpp
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

# Backward-compatible alias (temporary migration path)
add_library(parus_compiler ALIAS parus_frontend)

target_include_directories(parus_frontend
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(parus_frontend PUBLIC cxx_std_23)

# ---- warnings (no -Werror; keep iteration smooth) ----
if (MSVC)
    target_compile_options(parus_frontend PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_frontend PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

# Optional: faster rebuilds for clang/gcc
# target_compile_options(parus_frontend PRIVATE -fno-omit-frame-pointer)
