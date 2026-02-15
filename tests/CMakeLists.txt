# tests/cmake
add_executable(parus_parser_tests
    harness/run_parser_tests.cpp
)

add_executable(parus_oir_tests
    harness/run_oir_tests.cpp
)

target_link_libraries(parus_parser_tests PRIVATE parus_compiler)
target_link_libraries(parus_oir_tests PRIVATE parus_compiler)
target_compile_features(parus_parser_tests PRIVATE cxx_std_23)
target_compile_features(parus_oir_tests PRIVATE cxx_std_23)
target_compile_definitions(parus_parser_tests PRIVATE
    PARUS_TEST_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_cases"
)
if (MSVC)
    target_compile_options(parus_parser_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_oir_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_parser_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_oir_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

if (TARGET parus_backend_aot)
    add_executable(parus_oir_llvm_tests
    harness/run_oir_llvm_tests.cpp
    )
    target_link_libraries(parus_oir_llvm_tests PRIVATE parus_compiler parus_backend_aot)
    target_compile_features(parus_oir_llvm_tests PRIVATE cxx_std_23)
    target_compile_definitions(parus_oir_llvm_tests PRIVATE
    PARUS_OIR_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/oir_cases"
    )
    if (MSVC)
    target_compile_options(parus_oir_llvm_tests PRIVATE /W4 /permissive-)
    else()
    target_compile_options(parus_oir_llvm_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
endif()

enable_testing()
add_test(NAME parus_parser_tests COMMAND parus_parser_tests)
add_test(NAME parus_oir_tests COMMAND parus_oir_tests)
if (TARGET parus_oir_llvm_tests)
    add_test(NAME parus_oir_llvm_tests COMMAND parus_oir_llvm_tests)
endif()
