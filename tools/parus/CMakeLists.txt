add_executable(parus
    src/main.cpp
    src/cli/Options.cpp
    src/config/Config.cpp
    src/config/TomlLite.cpp
    src/driver/Driver.cpp
    src/proc/Process.cpp
    src/toolchain/Resolver.cpp
    src/doctor/Doctor.cpp
)

target_include_directories(parus
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/frontend/include
)

target_compile_features(parus PRIVATE cxx_std_23)

if (MSVC)
    target_compile_options(parus PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

set_target_properties(parus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/compiler/parusc
)
