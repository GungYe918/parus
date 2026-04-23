if(NOT PARUS_LLVM_VERSION STREQUAL "20")
    message(FATAL_ERROR "PARUS_ENABLE_MLIR=ON currently requires PARUS_LLVM_VERSION=20.")
endif()

set(_parus_mlir_hints "")
if(MLIR_DIR)
    list(APPEND _parus_mlir_hints "${MLIR_DIR}")
elseif(APPLE AND EXISTS "/opt/homebrew/opt/llvm@20/lib/cmake/mlir")
    list(APPEND _parus_mlir_hints "/opt/homebrew/opt/llvm@20/lib/cmake/mlir")
endif()

find_package(MLIR REQUIRED CONFIG HINTS ${_parus_mlir_hints})

if(NOT LLVM_VERSION_MAJOR EQUAL 20)
    message(FATAL_ERROR "PARUS_ENABLE_MLIR=ON requires MLIR/LLVM major 20.")
endif()

add_library(parus_backend_mlir STATIC
    ${CMAKE_CURRENT_LIST_DIR}/GOIRMLIRLowering.cpp
)

target_link_libraries(parus_backend_mlir
    PUBLIC
    parus_backend
    PRIVATE
    MLIRIR
    MLIRParser
    MLIRPass
    MLIRSupport
    MLIRFuncDialect
    MLIRArithDialect
    MLIRControlFlowDialect
    MLIRIndexDialect
    MLIRLLVMDialect
    MLIRFuncToLLVM
    MLIRArithToLLVM
    MLIRControlFlowToLLVM
    MLIRIndexToLLVM
    MLIRReconcileUnrealizedCasts
    MLIRBuiltinToLLVMIRTranslation
    MLIRLLVMToLLVMIRTranslation
    MLIRTargetLLVMIRExport
)

target_include_directories(parus_backend_mlir SYSTEM PRIVATE
    ${MLIR_INCLUDE_DIRS}
    ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(parus_backend_mlir PRIVATE
    PARUS_ENABLE_MLIR=1
)

if (MSVC)
    target_compile_options(parus_backend_mlir PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_backend_mlir PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
