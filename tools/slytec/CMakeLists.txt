# tools/slytec/cmake
add_executable(slytec
  src/main.cpp
)

target_link_libraries(slytec
  PRIVATE slyte_compiler
)

target_compile_features(slytec PRIVATE cxx_std_23)

if (MSVC)
  target_compile_options(slytec PRIVATE /W4 /permissive-)
else()
  target_compile_options(slytec PRIVATE -Wall -Wextra -Wpedantic)
endif()
