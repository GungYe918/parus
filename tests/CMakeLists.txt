# tests/cmake
add_executable(parus_parser_tests
    harness/run_parser_tests.cpp
)

add_executable(parus_frontend_integration_tests
    harness/run_frontend_integration_tests.cpp
)

add_executable(parus_parser_stress_tests
    harness/run_parser_stress_tests.cpp
)

add_executable(parus_parser_incremental_merge_tests
    harness/run_parser_incremental_merge_tests.cpp
)

add_executable(parus_parser_crash_tests
    harness/run_parser_crash_tests.cpp
)

add_executable(parus_parser_ast_golden_tests
    harness/run_parser_ast_golden_tests.cpp
)

add_executable(parus_oir_tests
    harness/run_oir_tests.cpp
)

add_executable(parus_goir_tests
    harness/run_goir_tests.cpp
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

target_link_libraries(parus_parser_tests PRIVATE parus_frontend)
target_link_libraries(parus_frontend_integration_tests PRIVATE parus_frontend)
target_link_libraries(parus_parser_stress_tests PRIVATE parus_frontend)
target_link_libraries(parus_parser_incremental_merge_tests PRIVATE parus_frontend)
target_link_libraries(parus_parser_crash_tests PRIVATE parus_frontend)
target_link_libraries(parus_parser_ast_golden_tests PRIVATE parus_frontend)
target_link_libraries(parus_oir_tests PRIVATE parus_frontend)
target_link_libraries(parus_goir_tests PRIVATE parus_frontend)
target_link_libraries(parusc_cli_option_tests PRIVATE parus_frontend)
if (TARGET parus_cli_tests)
    target_link_libraries(parus_cli_tests PRIVATE parus_frontend)
endif()
if (TARGET parus_config_tests)
    target_link_libraries(parus_config_tests PRIVATE parus_frontend)
endif()
if (TARGET parusd_lsp_tests)
    target_link_libraries(parusd_lsp_tests PRIVATE parus_frontend)
endif()

target_compile_features(parus_parser_tests PRIVATE cxx_std_23)
target_compile_features(parus_frontend_integration_tests PRIVATE cxx_std_23)
target_compile_features(parus_parser_stress_tests PRIVATE cxx_std_23)
target_compile_features(parus_parser_incremental_merge_tests PRIVATE cxx_std_23)
target_compile_features(parus_parser_crash_tests PRIVATE cxx_std_23)
target_compile_features(parus_parser_ast_golden_tests PRIVATE cxx_std_23)
target_compile_features(parus_oir_tests PRIVATE cxx_std_23)
target_compile_features(parus_goir_tests PRIVATE cxx_std_23)
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
        PARUSC_BUILD_BIN="${CMAKE_BINARY_DIR}/compiler/parusc/parusc"
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
    PARUS_PARSER_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_cases"
)

target_compile_definitions(parus_frontend_integration_tests PRIVATE
    PARUS_FRONTEND_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/frontend_cases"
    PARUS_PARSER_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_cases"
)

target_compile_definitions(parus_parser_stress_tests PRIVATE
    PARUS_PARSER_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_cases"
    PARUS_FUZZ_INCREMENTAL_SEED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/incremental_seed"
)

target_compile_definitions(parus_parser_incremental_merge_tests PRIVATE
    PARUS_INCREMENTAL_MERGE_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/incremental_merge_cases"
)

target_compile_definitions(parus_parser_crash_tests PRIVATE
    PARUS_PARSER_CRASH_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_crashes"
)

target_compile_definitions(parus_parser_ast_golden_tests PRIVATE
    PARUS_PARSER_AST_GOLDEN_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/parser_ast_golden_cases"
)

if (MSVC)
    target_compile_options(parus_parser_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_frontend_integration_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_parser_stress_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_parser_incremental_merge_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_parser_crash_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_parser_ast_golden_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_oir_tests PRIVATE /W4 /permissive-)
    target_compile_options(parus_goir_tests PRIVATE /W4 /permissive-)
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
    target_compile_options(parus_frontend_integration_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_parser_stress_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_parser_incremental_merge_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_parser_crash_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_parser_ast_golden_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_oir_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_goir_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
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
    target_link_libraries(parus_oir_llvm_tests PRIVATE parus_frontend parus_backend_aot)
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
    target_link_libraries(parus_ffi_tests PRIVATE parus_frontend parus_backend_aot parus_backend_link parus_backend_parlib)
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

if (TARGET parus_backend_mlir)
    add_executable(parus_goir_mlir_tests
        harness/run_goir_mlir_tests.cpp
    )
    target_link_libraries(parus_goir_mlir_tests PRIVATE parus_frontend parus_backend_mlir)
    target_compile_features(parus_goir_mlir_tests PRIVATE cxx_std_23)
    target_compile_definitions(parus_goir_mlir_tests PRIVATE
        PARUS_TEST_MLIR_LANE=${PARUS_MLIR_VERSION}
    )
    if (MSVC)
        target_compile_options(parus_goir_mlir_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(parus_goir_mlir_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
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

if (TARGET parus_backend_prt_hosted)
    add_executable(parus_prt_hosted_tests
    harness/run_prt_tests.cpp
    )
    target_link_libraries(parus_prt_hosted_tests PRIVATE parus_backend_prt_hosted)
    target_compile_features(parus_prt_hosted_tests PRIVATE cxx_std_23)
    target_compile_definitions(parus_prt_hosted_tests PRIVATE
    PARUS_PRT_VARIANT="hosted"
    )
    if (MSVC)
    target_compile_options(parus_prt_hosted_tests PRIVATE /W4 /permissive-)
    else()
    target_compile_options(parus_prt_hosted_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
endif()

if (TARGET parus_backend_prt_freestanding)
    add_executable(parus_prt_freestanding_tests
    harness/run_prt_tests.cpp
    )
    target_link_libraries(parus_prt_freestanding_tests PRIVATE parus_backend_prt_freestanding)
    target_compile_features(parus_prt_freestanding_tests PRIVATE cxx_std_23)
    target_compile_definitions(parus_prt_freestanding_tests PRIVATE
    PARUS_PRT_VARIANT="freestanding"
    )
    if (MSVC)
    target_compile_options(parus_prt_freestanding_tests PRIVATE /W4 /permissive-)
    else()
    target_compile_options(parus_prt_freestanding_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    endif()
endif()

if (PARUS_BUILD_FUZZERS)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(parus_fuzz_parser_smoke
            fuzz/fuzz_parser_smoke.cpp
        )
        target_link_libraries(parus_fuzz_parser_smoke PRIVATE parus_frontend)
        target_compile_features(parus_fuzz_parser_smoke PRIVATE cxx_std_23)
        target_compile_options(parus_fuzz_parser_smoke PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(parus_fuzz_parser_smoke PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
        )

        add_executable(parus_fuzz_parser_incremental
            fuzz/fuzz_parser_incremental.cpp
        )
        target_link_libraries(parus_fuzz_parser_incremental PRIVATE parus_frontend)
        target_compile_features(parus_fuzz_parser_incremental PRIVATE cxx_std_23)
        target_compile_options(parus_fuzz_parser_incremental PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(parus_fuzz_parser_incremental PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
        )
    else()
        message(WARNING "PARUS_BUILD_FUZZERS=ON ignored: clang/libFuzzer toolchain is required")
    endif()
endif()

enable_testing()
add_test(NAME parus_parser_tests COMMAND parus_parser_tests)
add_test(NAME parus_frontend_integration_tests COMMAND parus_frontend_integration_tests)
add_test(NAME parus_parser_stress_tests COMMAND parus_parser_stress_tests)
add_test(NAME parus_parser_incremental_merge_tests COMMAND parus_parser_incremental_merge_tests)
add_test(NAME parus_parser_crash_tests COMMAND parus_parser_crash_tests)
add_test(NAME parus_parser_ast_golden_tests COMMAND parus_parser_ast_golden_tests)
add_test(NAME parus_oir_tests COMMAND parus_oir_tests)
add_test(NAME parus_goir_tests COMMAND parus_goir_tests)
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
if (TARGET parus_goir_mlir_tests)
    add_test(NAME parus_goir_mlir_tests COMMAND parus_goir_mlir_tests)
endif()
if (TARGET parus_parlib_tests)
    add_test(NAME parus_parlib_tests COMMAND parus_parlib_tests)
endif()
if (TARGET parus_prt_hosted_tests)
    add_test(NAME parus_prt_hosted_tests COMMAND parus_prt_hosted_tests)
endif()
if (TARGET parus_prt_freestanding_tests)
    add_test(NAME parus_prt_freestanding_tests COMMAND parus_prt_freestanding_tests)
endif()
