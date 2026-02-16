add_executable(parusd
    src/main.cpp
)

target_link_libraries(parusd PRIVATE parus_frontend)

target_include_directories(parusd
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(parusd PRIVATE cxx_std_23)

if (MSVC)
    target_compile_options(parusd PRIVATE /W4 /permissive-)
else()
    target_compile_options(parusd PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

set_target_properties(parusd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/compiler/parusc
)
