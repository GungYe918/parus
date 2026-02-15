# backend/src/wasm/CMakeLists.txt

add_library(parus_backend_wasm STATIC
  ${CMAKE_CURRENT_LIST_DIR}/WasmBackend.cpp
)

target_link_libraries(parus_backend_wasm
  PUBLIC
  parus_backend
)

if (MSVC)
  target_compile_options(parus_backend_wasm PRIVATE /W4 /permissive-)
else()
  target_compile_options(parus_backend_wasm PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
