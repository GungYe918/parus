# Official LLVM/MLIR release toolchain discovery for the gOIR MLIR backend.

function(parus_reject_homebrew_mlir_path var_name var_value)
    if(var_value MATCHES "^/opt/homebrew/" OR var_value MATCHES "^/usr/local/opt/llvm")
        message(FATAL_ERROR
            "${var_name}='${var_value}' points at a Homebrew LLVM/MLIR installation. "
            "PARUS_ENABLE_MLIR=ON must use an official llvm-project GitHub release toolchain "
            "via PARUS_MLIR_TOOLCHAIN_ROOT or explicit MLIR_DIR/LLVM_DIR.")
    endif()
endfunction()

if(NOT PARUS_ENABLE_MLIR)
    return()
endif()

if(NOT PARUS_MLIR_VERSION STREQUAL "22")
    message(FATAL_ERROR "PARUS_ENABLE_MLIR=ON currently supports only PARUS_MLIR_VERSION=22.")
endif()

if(NOT PARUS_LLVM_VERSION STREQUAL PARUS_MLIR_VERSION)
    message(FATAL_ERROR
        "PARUS_ENABLE_MLIR=ON requires PARUS_LLVM_VERSION == PARUS_MLIR_VERSION. "
        "Got PARUS_LLVM_VERSION='${PARUS_LLVM_VERSION}', PARUS_MLIR_VERSION='${PARUS_MLIR_VERSION}'.")
endif()

set(_parus_mlir_toolchain_root "${PARUS_MLIR_TOOLCHAIN_ROOT}")
if(_parus_mlir_toolchain_root)
    get_filename_component(_parus_mlir_toolchain_root "${_parus_mlir_toolchain_root}" ABSOLUTE)
    parus_reject_homebrew_mlir_path("PARUS_MLIR_TOOLCHAIN_ROOT" "${_parus_mlir_toolchain_root}")
endif()
if(MLIR_DIR)
    parus_reject_homebrew_mlir_path("MLIR_DIR" "${MLIR_DIR}")
endif()
if(LLVM_DIR)
    parus_reject_homebrew_mlir_path("LLVM_DIR" "${LLVM_DIR}")
endif()

set(_parus_mlir_hints "")
set(_parus_llvm_hints "")

if(_parus_mlir_toolchain_root)
    if(NOT EXISTS "${_parus_mlir_toolchain_root}")
        message(FATAL_ERROR
            "PARUS_MLIR_TOOLCHAIN_ROOT='${_parus_mlir_toolchain_root}' does not exist. "
            "Run scripts/fetch-llvm-mlir-toolchain.sh first.")
    endif()
    list(APPEND _parus_mlir_hints "${_parus_mlir_toolchain_root}/lib/cmake/mlir")
    list(APPEND _parus_llvm_hints "${_parus_mlir_toolchain_root}/lib/cmake/llvm")
elseif(MLIR_DIR AND LLVM_DIR)
    list(APPEND _parus_mlir_hints "${MLIR_DIR}")
    list(APPEND _parus_llvm_hints "${LLVM_DIR}")
else()
    message(FATAL_ERROR
        "PARUS_ENABLE_MLIR=ON requires PARUS_MLIR_TOOLCHAIN_ROOT or both explicit MLIR_DIR and LLVM_DIR. "
        "Homebrew discovery is intentionally disabled for the MLIR lane.")
endif()

find_package(LLVM REQUIRED CONFIG HINTS ${_parus_llvm_hints} NO_DEFAULT_PATH)
find_package(MLIR REQUIRED CONFIG HINTS ${_parus_mlir_hints} NO_DEFAULT_PATH)

if(NOT LLVM_VERSION_MAJOR EQUAL 22)
    message(FATAL_ERROR "PARUS_ENABLE_MLIR=ON requires LLVM major 22, found ${LLVM_VERSION}.")
endif()

if(NOT PARUS_MLIR_VERSION STREQUAL "${LLVM_VERSION_MAJOR}")
    message(FATAL_ERROR
        "PARUS_MLIR_VERSION='${PARUS_MLIR_VERSION}' does not match discovered LLVM major '${LLVM_VERSION_MAJOR}'.")
endif()

set(_parus_mlir_llvm_config "")
if(_parus_mlir_toolchain_root AND EXISTS "${_parus_mlir_toolchain_root}/bin/llvm-config")
    set(_parus_mlir_llvm_config "${_parus_mlir_toolchain_root}/bin/llvm-config")
elseif(LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/llvm-config")
    set(_parus_mlir_llvm_config "${LLVM_TOOLS_BINARY_DIR}/llvm-config")
endif()

if(NOT _parus_mlir_llvm_config)
    message(FATAL_ERROR
        "Official MLIR toolchain preflight failed: llvm-config was not found under the selected toolchain.")
endif()

if(APPLE AND _parus_mlir_toolchain_root)
    get_filename_component(_parus_mlir_cxx_compiler "${CMAKE_CXX_COMPILER}" ABSOLUTE)
    if(NOT _parus_mlir_cxx_compiler MATCHES "^${_parus_mlir_toolchain_root}/")
        message(FATAL_ERROR
            "Official macOS LLVM/MLIR ${PARUS_MLIR_RELEASE_VERSION} static libraries require the matching LLVM "
            "compiler/linker stack. Configure with "
            "-DCMAKE_C_COMPILER=${_parus_mlir_toolchain_root}/bin/clang "
            "-DCMAKE_CXX_COMPILER=${_parus_mlir_toolchain_root}/bin/clang++ "
            "-DCMAKE_AR=${_parus_mlir_toolchain_root}/bin/llvm-ar "
            "-DCMAKE_RANLIB=${_parus_mlir_toolchain_root}/bin/llvm-ranlib.")
    endif()
endif()

set(_parus_mlir_lib_dirs ${MLIR_LIBRARY_DIRS} ${LLVM_LIBRARY_DIRS})
if(_parus_mlir_toolchain_root)
    list(APPEND _parus_mlir_lib_dirs "${_parus_mlir_toolchain_root}/lib")
endif()
list(REMOVE_DUPLICATES _parus_mlir_lib_dirs)

set(_parus_mlir_static_found FALSE)
set(_parus_llvm_static_found FALSE)
foreach(_parus_lib_dir IN LISTS _parus_mlir_lib_dirs)
    if(EXISTS "${_parus_lib_dir}/libMLIRIR.a" OR EXISTS "${_parus_lib_dir}/MLIRIR.lib")
        set(_parus_mlir_static_found TRUE)
    endif()
    if(EXISTS "${_parus_lib_dir}/libLLVMCore.a" OR EXISTS "${_parus_lib_dir}/LLVMCore.lib")
        set(_parus_llvm_static_found TRUE)
    endif()
endforeach()

if(NOT _parus_mlir_static_found OR NOT _parus_llvm_static_found)
    message(FATAL_ERROR
        "Official MLIR toolchain preflight failed: static MLIR/LLVM libraries were not found. "
        "Expected libMLIRIR.a and libLLVMCore.a (or .lib equivalents). "
        "Use './install.sh --with-mlir --mlir-release-mode source' to build a static release toolchain.")
endif()

add_library(parus_mlirconfig INTERFACE)
target_include_directories(parus_mlirconfig SYSTEM INTERFACE
    ${MLIR_INCLUDE_DIRS}
    ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(parus_mlirconfig INTERFACE
    PARUS_ENABLE_MLIR=1
    PARUS_MLIR_SELECTED_MAJOR=${PARUS_MLIR_VERSION}
)

set(PARUS_MLIR_TOOLCHAIN_ROOT_RESOLVED "${_parus_mlir_toolchain_root}" CACHE INTERNAL "Resolved Parus MLIR toolchain root")
set(PARUS_MLIR_LLVM_CONFIG_USED "${_parus_mlir_llvm_config}" CACHE INTERNAL "llvm-config used for Parus MLIR toolchain")

message(STATUS
    "parus_mlirconfig: lane=${PARUS_MLIR_VERSION}, release=${PARUS_MLIR_RELEASE_VERSION}, "
    "toolchain_root=${_parus_mlir_toolchain_root}, llvm_config=${_parus_mlir_llvm_config}, "
    "llvm_version=${LLVM_VERSION}")
