# backend/llvmconfig/CMakeLists.txt

set(_parus_llvm_selected "${PARUS_LLVM_VERSION}")
set(_parus_llvm_toolchain_ok FALSE)

if(NOT _parus_llvm_selected STREQUAL "20" AND NOT _parus_llvm_selected STREQUAL "21")
  message(FATAL_ERROR
    "Unsupported PARUS_LLVM_VERSION='${_parus_llvm_selected}'. Supported values are: 20, 21.")
endif()

# Optional toolchain probe. Source lane 분리는 toolchain 유무와 독립적으로 유지한다.
find_package(LLVM ${_parus_llvm_selected} QUIET CONFIG)
if(LLVM_FOUND)
  set(_parus_llvm_toolchain_ok TRUE)
endif()

if(PARUS_LLVM_REQUIRE_TOOLCHAIN AND NOT _parus_llvm_toolchain_ok)
  message(FATAL_ERROR
    "PARUS_LLVM_REQUIRE_TOOLCHAIN=ON but LLVM ${_parus_llvm_selected} toolchain was not found.")
endif()

set(PARUS_AOT_LLVM_LANE_SOURCE
  "${CMAKE_CURRENT_LIST_DIR}/lanes/v${_parus_llvm_selected}/AOTLlvmV${_parus_llvm_selected}.cpp"
  PARENT_SCOPE
)

add_library(parus_llvmconfig INTERFACE)

target_compile_definitions(parus_llvmconfig
  INTERFACE
  PARUS_WITH_LLVM=1
  PARUS_LLVM_SELECTED_MAJOR=${_parus_llvm_selected}
)

if(_parus_llvm_toolchain_ok)
  target_include_directories(parus_llvmconfig INTERFACE ${LLVM_INCLUDE_DIRS})
endif()

message(STATUS
  "parus_llvmconfig: lane=${_parus_llvm_selected}, toolchain_found=${_parus_llvm_toolchain_ok}, require_toolchain=${PARUS_LLVM_REQUIRE_TOOLCHAIN}")
