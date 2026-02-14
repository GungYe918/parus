# tools/gaupelc/cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt)

add_executable(gaupelc
  src/main.cpp
  ${GAUPELC_SOURCES}
)

target_link_libraries(gaupelc
  PRIVATE gaupel_compiler
)

target_compile_features(gaupelc PRIVATE cxx_std_23)

if (MSVC)
  target_compile_options(gaupelc PRIVATE /W4 /permissive-)
else()
  target_compile_options(gaupelc PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
