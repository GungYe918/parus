if(NOT TARGET parus_mlirconfig)
    message(FATAL_ERROR "PARUS_ENABLE_MLIR=ON but parus_mlirconfig target is missing.")
endif()

add_library(parus_backend_mlir STATIC
    ${CMAKE_CURRENT_LIST_DIR}/GOIRMLIRLowering.cpp
)

target_link_libraries(parus_backend_mlir
    PUBLIC
    parus_backend
    parus_backend_aot
    PRIVATE
    parus_mlirconfig
    MLIRIR
    MLIRParser
    MLIRPass
    MLIRSupport
    MLIRFuncDialect
    MLIRArithDialect
    MLIRControlFlowDialect
    MLIRIndexDialect
    MLIRMemRefDialect
    MLIRMemRefTransforms
    MLIRLLVMDialect
    MLIRFuncToLLVM
    MLIRArithToLLVM
    MLIRControlFlowToLLVM
    MLIRIndexToLLVM
    MLIRMemRefToLLVM
    MLIRReconcileUnrealizedCasts
    MLIRBuiltinToLLVMIRTranslation
    MLIRLLVMToLLVMIRTranslation
    MLIRTargetLLVMIRExport
)

target_compile_definitions(parus_backend_mlir PRIVATE
    PARUS_ENABLE_MLIR=1
    PARUS_MLIR_SELECTED_MAJOR=${PARUS_MLIR_VERSION}
)

if (MSVC)
    target_compile_options(parus_backend_mlir PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_backend_mlir PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
