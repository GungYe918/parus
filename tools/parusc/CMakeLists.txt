# tools/parusc/cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt)

add_executable(parusc
  src/main.cpp
  ${PARUSC_SOURCES}
)

target_link_libraries(parusc
  PRIVATE parus_compiler
)

target_compile_features(parusc PRIVATE cxx_std_23)

if (MSVC)
  target_compile_options(parusc PRIVATE /W4 /permissive-)
else()
  target_compile_options(parusc PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
