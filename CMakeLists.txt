cmake_minimum_required(VERSION 3.25)

project(parus
    VERSION 0.1.0
    LANGUAGES C CXX
)

# ---- global C++ settings ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT MSVC)
    add_compile_options(-Wno-trigraphs)
endif()

option(PARUS_BUILD_TESTS "Build Parus tests" ON)
option(PARUS_BUILD_FUZZERS "Build parser fuzz targets (clang/libFuzzer only)" OFF)
option(PARUS_BUILD_PARUSD "Build Parus standalone LSP server (parusd)" ON)
option(PARUS_BUILD_LEI "Build LEI standalone DSL project under tools/Lei" ON)
option(PARUS_BUILD_ORCHESTRATOR "Build Parus orchestrator CLI (parus)" ON)

# C header import preparation (libclang)
option(PARUS_ENABLE_CIMPORT "Enable libclang-backed C import preparation paths" ON)
option(PARUS_CIMPORT_REQUIRE_LIBCLANG "Fail configure when libclang is not found" OFF)
set(PARUS_LIBCLANG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libclang" CACHE PATH "Root directory for vendored libclang headers/libraries")
set(PARUS_LIBCLANG_INCLUDE_DIR "" CACHE PATH "Override include directory that contains clang-c/Index.h")
set(PARUS_LIBCLANG_LIBRARY "" CACHE FILEPATH "Override libclang library path")

# Backend module switches
option(PARUS_ENABLE_BACKEND "Build Parus backend modules" ON)
option(PARUS_ENABLE_JIT_BACKEND "Build JIT backend module" ON)
option(PARUS_ENABLE_WASM_BACKEND "Build WASM backend module" ON)
option(PARUS_ENABLE_AOT_BACKEND "Build AOT backend module" ON)
option(PARUS_ENABLE_MLIR "Build MLIR-backed gOIR lowering module" OFF)
option(PARUS_AOT_ENABLE_LLVM "Enable LLVM engine inside AOT backend" ON)
option(PARUS_LLVM_USE_TOOLCHAIN "Use linked LLVM C++ API toolchain" ON)
set(PARUS_LLVM_CONFIG_EXECUTABLE "" CACHE FILEPATH "Path to llvm-config executable for the selected LLVM lane")

# LLVM lane fence: only 20 / 21 / 22 allowed.
set(PARUS_LLVM_VERSION "20" CACHE STRING "LLVM major version lane for AOT backend (20|21|22)")
set_property(CACHE PARUS_LLVM_VERSION PROPERTY STRINGS 20 21 22)
option(PARUS_LLVM_REQUIRE_TOOLCHAIN "Require a matching LLVM toolchain for the selected LLVM backend version" OFF)
set(PARUS_MLIR_VERSION "22" CACHE STRING "MLIR/LLVM major version lane for gOIR MLIR backend")
set_property(CACHE PARUS_MLIR_VERSION PROPERTY STRINGS 22)
set(PARUS_MLIR_RELEASE_VERSION "22.1.4" CACHE STRING "Official llvm-project release version for the MLIR lane")
set(PARUS_MLIR_TOOLCHAIN_ROOT "" CACHE PATH "Root of the official LLVM/MLIR release toolchain for PARUS_ENABLE_MLIR")

if(NOT PARUS_LLVM_VERSION STREQUAL "20" AND NOT PARUS_LLVM_VERSION STREQUAL "21" AND NOT PARUS_LLVM_VERSION STREQUAL "22")
    message(FATAL_ERROR
    "Unsupported PARUS_LLVM_VERSION='${PARUS_LLVM_VERSION}'. Supported values are: 20, 21, 22.")
endif()

if(NOT PARUS_MLIR_VERSION STREQUAL "22")
    message(FATAL_ERROR
    "Unsupported PARUS_MLIR_VERSION='${PARUS_MLIR_VERSION}'. Supported values are: 22.")
endif()

if(PARUS_ENABLE_MLIR AND NOT PARUS_LLVM_VERSION STREQUAL PARUS_MLIR_VERSION)
    message(FATAL_ERROR
    "PARUS_ENABLE_MLIR=ON requires PARUS_LLVM_VERSION == PARUS_MLIR_VERSION. "
    "Got PARUS_LLVM_VERSION='${PARUS_LLVM_VERSION}', PARUS_MLIR_VERSION='${PARUS_MLIR_VERSION}'.")
endif()

if(APPLE AND PARUS_ENABLE_MLIR AND PARUS_LLVM_VERSION STREQUAL "22" AND PARUS_MLIR_TOOLCHAIN_ROOT)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _parus_apple_sdk_root
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(_parus_apple_libcxx_include "${_parus_apple_sdk_root}/usr/include/c++/v1")
    set(_parus_apple_libcxx_tbd "${_parus_apple_sdk_root}/usr/lib/libc++.tbd")
    set(_parus_apple_libcxxabi_tbd "${_parus_apple_sdk_root}/usr/lib/libc++abi.tbd")
    if(NOT EXISTS "${_parus_apple_libcxx_include}")
        message(FATAL_ERROR
            "PARUS_ENABLE_MLIR=ON with LLVM lane 22 on macOS requires Apple SDK libc++ headers. "
            "Could not find '${_parus_apple_libcxx_include}'.")
    endif()
    if(NOT EXISTS "${_parus_apple_libcxx_tbd}" OR NOT EXISTS "${_parus_apple_libcxxabi_tbd}")
        message(FATAL_ERROR
            "PARUS_ENABLE_MLIR=ON with LLVM lane 22 on macOS requires Apple SDK libc++ link stubs under "
            "'${_parus_apple_sdk_root}/usr/lib'.")
    endif()
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>"
        "$<$<COMPILE_LANGUAGE:CXX>:-isystem>"
        "$<$<COMPILE_LANGUAGE:CXX>:${_parus_apple_libcxx_include}>"
    )
    add_library(parus_llvm22_darwin_compat OBJECT cmake/ParusLlvm22DarwinCompat.cpp)
    set_target_properties(parus_llvm22_darwin_compat PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    link_libraries(
        $<TARGET_OBJECTS:parus_llvm22_darwin_compat>
        "${_parus_apple_libcxx_tbd}"
        "${_parus_apple_libcxxabi_tbd}"
    )
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ParusLibClang.cmake)
parus_configure_libclang()

# Frontend only: AST -> SIR -> OIR
add_subdirectory(frontend)

# Backend only: LLVM/JIT/WASM bridge layers
if(PARUS_ENABLE_BACKEND)
    add_subdirectory(backend)
endif()

add_subdirectory(compiler/parusc)
add_subdirectory(tools)

# ---------------------------
# Two-stage build chain
# stage1: frontend/backend libs
# stage2: parusc tool (depends on stage1)
# ---------------------------
add_custom_target(parus_stage1)
add_dependencies(parus_stage1 parus_frontend)
if(TARGET parus_backend_aot)
    add_dependencies(parus_stage1 parus_backend_aot)
endif()
if(TARGET parus_backend_jit)
    add_dependencies(parus_stage1 parus_backend_jit)
endif()
if(TARGET parus_backend_wasm)
    add_dependencies(parus_stage1 parus_backend_wasm)
endif()

add_custom_target(parus_stage2)
if(TARGET parusc)
    add_dependencies(parus_stage2 parus_stage1 parusc)
endif()
if(TARGET parus-lld)
    add_dependencies(parus_stage2 parus-lld)
endif()
if(TARGET parusd)
    add_dependencies(parus_stage2 parusd)
endif()
if(TARGET parus)
    add_dependencies(parus_stage2 parus)
endif()

if(PARUS_BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
    endif()
endif()
