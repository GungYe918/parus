# backend/src/parlib/CMakeLists.txt

add_library(parus_backend_parlib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/Parlib.cpp
)

target_link_libraries(parus_backend_parlib
    PUBLIC
    parus_backend
)

if (MSVC)
    target_compile_options(parus_backend_parlib PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_backend_parlib PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
