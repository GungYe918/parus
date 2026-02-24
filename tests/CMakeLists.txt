# tests/cmake
add_executable(parus_parser_tests
    harness/run_parser_tests.cpp
)

add_executable(parus_oir_tests
    harness/run_oir_tests.cpp
)

add_executable(parusc_cli_option_tests
    harness/run_cli_option_tests.cpp
    ${CMAKE_SOURCE_DIR}/compiler/parusc/src/cli/Options.cpp
)

if (TARGET parus)
    add_executable(parus_cli_tests
        harness/run_parus_cli_tests.cpp
    )
    add_executable(parus_config_tests
        harness/run_parus_config_tests.cpp
    )
endif()
if (TARGET parusd)
    add_executable(parusd_lsp_tests
        harness/run_parusd_lsp_tests.cpp
    )
endif()

target_link_libraries(parus_parser_tests PRIVATE parus_compiler)
target_link_libraries(parus_oir_tests PRIVATE parus_compiler)
target_link_libraries(parusc_cli_option_tests PRIVATE parus_compiler)
if (TARGET parus_cli_tests)
    target_link_libraries(parus_cli_tests PRIVATE parus_compiler)
endif()
if (TARGET parus_config_tests)
    target_link_libraries(parus_config_tests PRIVATE parus_compiler)
endif()
if (TARGET parusd_lsp_tests)
    target_link_libraries(parusd_lsp_tests PRIVATE parus_compiler)
endif()
target_compile_features(parus_parser_tests PRIVATE cxx_std_23)
target_compile_features(parus_oir_tests PRIVATE cxx_std_23)
target_compile_features(parusc_cli_option_tests PRIVATE cxx_std_23)
if (TARGET parus_cli_tests)
    target_compile_features(parus_cli_tests PRIVATE cxx_std_23)
endif()
if (TARGET parus_config_tests)
    target_compile_features(parus_config_tests PRIVATE cxx_std_23)
endif()
if (TARGET parusd_lsp_tests)
    target_compile_features(parusd_lsp_tests PRIVATE cxx_std_23)
endif()
target_include_directories(parusc_cli_option_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/compiler/parusc/include
)
if (TARGET parus_cli_tests)
    target_compile_definitions(parus_cli_tests PRIVATE
        PARUS_BUILD_BIN="${CMAKE_BINARY_DIR}/compiler/parusc/parus"
        PARUS_MAIN_PR="${CMAKE_SOURCE_DIR}/main.pr"
        PARUS_LEI_CASE_DIR="${CMAKE_SOURCE_DIR}/tools/Lei/tests/cases"
    )
endif()
if (TARGET parus_config_tests)
    target_compile_definitions(parus_config_tests PRIVATE
        PARUS_BUILD_BIN="${CMAKE_BINARY_DIR}/compiler/parusc/parus"
    )
endif()
if (TARGET parusd_lsp_tests)
    target_compile_definitions(parusd_lsp_tests PRIVATE
        PARUSD_BUILD_BIN="${CMAKE_BINARY_DIR}/compiler/parusc/parusd"
        PARUSC_BUILD_BIN="${CMAKE_BINARY_DIR}/compiler/parusc/parusc"
    )
endif()
target_compile_definitions(parus_parser_tests PRIVATE
    PARUS_TEST_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_cases"
)
if (MSVC)
    target_compile_options(parus_parser_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_oir_tests PRIVATE /W4 /permissive-)
    target_compile_options(parusc_cli_option_tests PRIVATE /W4 /permissive-)
    if (TARGET parus_cli_tests)
        target_compile_options(parus_cli_tests PRIVATE /W4 /permissive-)
    endif()
    if (TARGET parus_config_tests)
        target_compile_options(parus_config_tests PRIVATE /W4 /permissive-)
    endif()
    if (TARGET parusd_lsp_tests)
        target_compile_options(parusd_lsp_tests PRIVATE /W4 /permissive-)
    endif()
else()
    target_compile_options(parus_parser_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_oir_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parusc_cli_option_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    if (TARGET parus_cli_tests)
        target_compile_options(parus_cli_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
    if (TARGET parus_config_tests)
        target_compile_options(parus_config_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
    if (TARGET parusd_lsp_tests)
        target_compile_options(parusd_lsp_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
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

    if (TARGET parus_backend_link)
    add_executable(parus_ffi_tests
    harness/run_ffi_tests.cpp
    )
    target_link_libraries(parus_ffi_tests PRIVATE parus_compiler parus_backend_aot parus_backend_link parus_backend_parlib)
    target_compile_features(parus_ffi_tests PRIVATE cxx_std_23)
    target_compile_definitions(parus_ffi_tests PRIVATE
    PARUS_FFI_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ffi"
    PARUS_TEST_LLVM_LANE=${PARUS_LLVM_VERSION}
    )
    if (MSVC)
    target_compile_options(parus_ffi_tests PRIVATE /W4 /permissive-)
    else()
    target_compile_options(parus_ffi_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
    endif()
endif()

if (TARGET parus_backend_parlib)
    add_executable(parus_parlib_tests
    harness/run_parlib_tests.cpp
    )
    target_link_libraries(parus_parlib_tests PRIVATE parus_backend_parlib)
    target_compile_features(parus_parlib_tests PRIVATE cxx_std_23)
    if (MSVC)
    target_compile_options(parus_parlib_tests PRIVATE /W4 /permissive-)
    else()
    target_compile_options(parus_parlib_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
endif()

enable_testing()
add_test(NAME parus_parser_tests COMMAND parus_parser_tests)
add_test(NAME parus_oir_tests COMMAND parus_oir_tests)
add_test(NAME parusc_cli_option_tests COMMAND parusc_cli_option_tests)
if (TARGET parus_cli_tests)
    add_test(NAME parus_cli_tests COMMAND parus_cli_tests)
endif()
if (TARGET parus_config_tests)
    add_test(NAME parus_config_tests COMMAND parus_config_tests)
endif()
if (TARGET parusd_lsp_tests)
    add_test(NAME parusd_lsp_tests COMMAND parusd_lsp_tests)
endif()
if (TARGET parus_oir_llvm_tests)
    add_test(NAME parus_oir_llvm_tests COMMAND parus_oir_llvm_tests)
endif()
if (TARGET parus_ffi_tests)
    add_test(NAME parus_ffi_tests COMMAND parus_ffi_tests)
endif()
if (TARGET parus_parlib_tests)
    add_test(NAME parus_parlib_tests COMMAND parus_parlib_tests)
endif()
