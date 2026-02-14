# tests/cmake
add_executable(gaupel_parser_tests
  harness/run_parser_tests.cpp
)

target_link_libraries(gaupel_parser_tests PRIVATE gaupel_compiler)
target_compile_features(gaupel_parser_tests PRIVATE cxx_std_23)
target_compile_definitions(gaupel_parser_tests PRIVATE
  GAUPEL_TEST_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases"
)
if (MSVC)
  target_compile_options(gaupel_parser_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(gaupel_parser_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

enable_testing()
add_test(NAME gaupel_parser_tests COMMAND gaupel_parser_tests)
