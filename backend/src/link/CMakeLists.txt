# backend/src/link/CMakeLists.txt

add_library(parus_backend_link STATIC
    ${CMAKE_CURRENT_LIST_DIR}/Linker.cpp
)

target_link_libraries(parus_backend_link
    PUBLIC
    parus_backend
)

if (MSVC)
    target_compile_options(parus_backend_link PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_backend_link PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
