# tests/cmake
add_executable(slyte_parser_tests
  harness/run_parser_tests.cpp
)

target_link_libraries(slyte_parser_tests PRIVATE slyte_compiler)
target_compile_features(slyte_parser_tests PRIVATE cxx_std_23)

enable_testing()
add_test(NAME slyte_parser_tests COMMAND slyte_parser_tests)
