# tests/cmake
add_executable(gaupel_parser_tests
  harness/run_parser_tests.cpp
)

target_link_libraries(gaupel_parser_tests PRIVATE gaupel_compiler)
target_compile_features(gaupel_parser_tests PRIVATE cxx_std_23)

enable_testing()
add_test(NAME gaupel_parser_tests COMMAND gaupel_parser_tests)
