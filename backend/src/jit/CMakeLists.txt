# backend/src/jit/CMakeLists.txt

add_library(parus_backend_jit STATIC
  ${CMAKE_CURRENT_LIST_DIR}/JITBackend.cpp
)

target_link_libraries(parus_backend_jit
  PUBLIC
  parus_backend
)

if (MSVC)
  target_compile_options(parus_backend_jit PRIVATE /W4 /permissive-)
else()
  target_compile_options(parus_backend_jit PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
