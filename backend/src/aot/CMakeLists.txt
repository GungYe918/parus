# backend/src/aot/CMakeLists.txt

set(_parus_aot_sources
    ${CMAKE_CURRENT_LIST_DIR}/AOTBackend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/LLVMIRLowering.cpp
    ${CMAKE_CURRENT_LIST_DIR}/LLVMObjectEmission.cpp
)

if(PARUS_AOT_ENABLE_LLVM)
    if(NOT TARGET parus_llvmconfig)
    message(FATAL_ERROR "PARUS_AOT_ENABLE_LLVM=ON but parus_llvmconfig target is missing.")
    endif()
    if(NOT DEFINED PARUS_AOT_LLVM_LANE_SOURCE)
    message(FATAL_ERROR "PARUS_AOT_ENABLE_LLVM=ON but PARUS_AOT_LLVM_LANE_SOURCE is not defined.")
    endif()

    list(APPEND _parus_aot_sources
    ${CMAKE_CURRENT_LIST_DIR}/AOTBackendLLVMDispatch.cpp
    ${PARUS_AOT_LLVM_LANE_SOURCE}
    )
endif()

add_library(parus_backend_aot STATIC
    ${_parus_aot_sources}
)

target_link_libraries(parus_backend_aot
    PUBLIC
    parus_backend
)

if(PARUS_AOT_ENABLE_LLVM)
    target_link_libraries(parus_backend_aot PRIVATE parus_llvmconfig)
    target_compile_definitions(parus_backend_aot PRIVATE PARUS_AOT_ENABLE_LLVM=1)
    if(PARUS_LLVM_TOOLCHAIN_FOUND)
    target_compile_definitions(parus_backend_aot PRIVATE PARUS_LLVM_TOOLCHAIN_FOUND=1)
    else()
    target_compile_definitions(parus_backend_aot PRIVATE PARUS_LLVM_TOOLCHAIN_FOUND=0)
    endif()
else()
    target_compile_definitions(parus_backend_aot PRIVATE PARUS_AOT_ENABLE_LLVM=0)
    target_compile_definitions(parus_backend_aot PRIVATE PARUS_LLVM_TOOLCHAIN_FOUND=0)
endif()

if (MSVC)
    target_compile_options(parus_backend_aot PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_backend_aot PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
