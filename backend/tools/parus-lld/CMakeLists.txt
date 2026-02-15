# backend/tools/parus-lld/CMakeLists.txt

add_executable(parus-lld
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
)

target_compile_features(parus-lld PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(parus-lld PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus-lld PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

# parusc와 같은 디렉터리에 배치해 드라이버가 자동으로 발견하도록 한다.
set_target_properties(parus-lld PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/compiler/parusc"
)
