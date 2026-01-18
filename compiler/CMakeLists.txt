# compiler/cmake
add_library(gaupel_compiler STATIC
  src/diag/diagnostic.cpp
  src/diag/render.cpp
  src/text/source_manager.cpp
  src/lex/lexer.cpp
  src/parse/parser.cpp
  src/parse/parse_decl.cpp
  src/parse/parse_stmt.cpp
  src/parse/parse_expr.cpp
  src/parse/parse_type.cpp
  src/ast/nodes.cpp
  src/passes/check_pipe_hole.cpp
  src/passes/check_pub_commit.cpp
)

target_include_directories(gaupel_compiler
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gaupel_compiler PUBLIC cxx_std_23)

# ---- warnings (no -Werror; keep iteration smooth) ----
if (MSVC)
  target_compile_options(gaupel_compiler PRIVATE /W4 /permissive-)
else()
  target_compile_options(gaupel_compiler PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Optional: faster rebuilds for clang/gcc
# target_compile_options(gaupel_compiler PRIVATE -fno-omit-frame-pointer)