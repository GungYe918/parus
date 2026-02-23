if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(lei VERSION 0.1.0 LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    option(LEI_BUILD_TESTS "Build LEI tests" ON)
    enable_testing()
else()
    if(NOT DEFINED LEI_BUILD_TESTS)
        if(DEFINED PARUS_BUILD_TESTS)
            set(LEI_BUILD_TESTS ${PARUS_BUILD_TESTS})
        else()
            set(LEI_BUILD_TESTS ON)
        endif()
    endif()
endif()

add_library(lei_core
    src/lex/lexer.cpp
    src/text/utf8.cpp
    src/os/file.cpp
)

target_include_directories(lei_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(lei_core PUBLIC cxx_std_23)

if (MSVC)
    target_compile_options(lei_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_core PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_library(lei_parser
    src/parse/parser.cpp
)
target_link_libraries(lei_parser PUBLIC lei_core)
target_include_directories(lei_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lei_parser PUBLIC cxx_std_23)
if (MSVC)
    target_compile_options(lei_parser PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_parser PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_library(lei_eval
    src/eval/evaluator.cpp
    src/builtins/register.cpp
    src/builtins/constants.cpp
    src/builtins/core_functions.cpp
    src/builtins/string_functions.cpp
    src/builtins/array_object_functions.cpp
    src/builtins/path_fs_functions.cpp
    src/builtins/semver_functions.cpp
    src/builtins/parus_helper_functions.cpp
)
target_link_libraries(lei_eval PUBLIC lei_parser)
target_include_directories(lei_eval PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lei_eval PUBLIC cxx_std_23)

set(LEI_PARUS_ENABLE_AOT_BACKEND_DEF 1)
if(DEFINED PARUS_ENABLE_AOT_BACKEND AND NOT PARUS_ENABLE_AOT_BACKEND)
    set(LEI_PARUS_ENABLE_AOT_BACKEND_DEF 0)
endif()

set(LEI_PARUS_ENABLE_JIT_BACKEND_DEF 1)
if(DEFINED PARUS_ENABLE_JIT_BACKEND AND NOT PARUS_ENABLE_JIT_BACKEND)
    set(LEI_PARUS_ENABLE_JIT_BACKEND_DEF 0)
endif()

set(LEI_PARUS_ENABLE_WASM_BACKEND_DEF 1)
if(DEFINED PARUS_ENABLE_WASM_BACKEND AND NOT PARUS_ENABLE_WASM_BACKEND)
    set(LEI_PARUS_ENABLE_WASM_BACKEND_DEF 0)
endif()

set(LEI_PARUS_AOT_ENABLE_LLVM_DEF 1)
if(DEFINED PARUS_AOT_ENABLE_LLVM AND NOT PARUS_AOT_ENABLE_LLVM)
    set(LEI_PARUS_AOT_ENABLE_LLVM_DEF 0)
endif()

set(LEI_PARUS_LLVM_LANE_SELECTED_DEF 20)
if(DEFINED PARUS_LLVM_VERSION)
    set(LEI_PARUS_LLVM_LANE_SELECTED_DEF ${PARUS_LLVM_VERSION})
endif()

target_compile_definitions(lei_eval PRIVATE
    LEI_PARUS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LEI_PARUS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LEI_PARUS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    LEI_PARUS_TOOL_PARUSC=\"parusc\"
    LEI_PARUS_TOOL_PARUSD=\"parusd\"
    LEI_PARUS_TOOL_PARUS_LLD=\"parus-lld\"
    LEI_PARUS_ENABLE_AOT_BACKEND=${LEI_PARUS_ENABLE_AOT_BACKEND_DEF}
    LEI_PARUS_ENABLE_JIT_BACKEND=${LEI_PARUS_ENABLE_JIT_BACKEND_DEF}
    LEI_PARUS_ENABLE_WASM_BACKEND=${LEI_PARUS_ENABLE_WASM_BACKEND_DEF}
    LEI_PARUS_AOT_ENABLE_LLVM=${LEI_PARUS_AOT_ENABLE_LLVM_DEF}
    LEI_PARUS_LLVM_LANE_SELECTED=${LEI_PARUS_LLVM_LANE_SELECTED_DEF}
    LEI_PARUS_DEFAULT_TARGET=\"\"
    LEI_PARUS_MACRO_BUDGET_AOT_DEPTH=64
    LEI_PARUS_MACRO_BUDGET_AOT_STEPS=20000
    LEI_PARUS_MACRO_BUDGET_AOT_OUTPUT_TOKENS=200000
    LEI_PARUS_MACRO_BUDGET_JIT_DEPTH=32
    LEI_PARUS_MACRO_BUDGET_JIT_STEPS=8000
    LEI_PARUS_MACRO_BUDGET_JIT_OUTPUT_TOKENS=80000
    LEI_PARUS_MACRO_BUDGET_HARD_MAX_DEPTH=256
    LEI_PARUS_MACRO_BUDGET_HARD_MAX_STEPS=200000
    LEI_PARUS_MACRO_BUDGET_HARD_MAX_OUTPUT_TOKENS=1000000
)
if (MSVC)
    target_compile_options(lei_eval PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_eval PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_library(lei_graph
    src/graph/ninja_lower.cpp
    src/graph/ninja_runner.cpp
    src/cache/graph_cache.cpp
)
target_link_libraries(lei_graph PUBLIC lei_eval)
target_include_directories(lei_graph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lei_graph PUBLIC cxx_std_23)
if (MSVC)
    target_compile_options(lei_graph PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_graph PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_executable(lei
    src/cli/main.cpp
)
target_link_libraries(lei PRIVATE lei_graph)
target_compile_features(lei PRIVATE cxx_std_23)
if (MSVC)
    target_compile_options(lei PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

if (LEI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
