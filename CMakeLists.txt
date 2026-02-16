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
option(PARUS_BUILD_PARUSD "Build Parus standalone LSP server (parusd)" ON)

# Backend module switches
option(PARUS_ENABLE_BACKEND "Build Parus backend modules" ON)
option(PARUS_ENABLE_JIT_BACKEND "Build JIT backend module" ON)
option(PARUS_ENABLE_WASM_BACKEND "Build WASM backend module" ON)
option(PARUS_ENABLE_AOT_BACKEND "Build AOT backend module" ON)
option(PARUS_AOT_ENABLE_LLVM "Enable LLVM engine inside AOT backend" ON)
option(PARUS_LLVM_USE_TOOLCHAIN "Use linked LLVM C++ API toolchain" ON)
set(PARUS_LLVM_CONFIG_EXECUTABLE "" CACHE FILEPATH "Path to llvm-config executable for the selected LLVM lane")

# LLVM lane fence: only 20 / 21 allowed
set(PARUS_LLVM_VERSION "20" CACHE STRING "LLVM major version lane for AOT backend (20|21)")
set_property(CACHE PARUS_LLVM_VERSION PROPERTY STRINGS 20 21)
option(PARUS_LLVM_REQUIRE_TOOLCHAIN "Require a matching LLVM toolchain for the selected LLVM backend version" OFF)

if(NOT PARUS_LLVM_VERSION STREQUAL "20" AND NOT PARUS_LLVM_VERSION STREQUAL "21")
    message(FATAL_ERROR
    "Unsupported PARUS_LLVM_VERSION='${PARUS_LLVM_VERSION}'. Supported values are: 20, 21.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

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

if(PARUS_BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
    endif()
endif()
