# backend/CMakeLists.txt

add_library(parus_backend INTERFACE)

target_include_directories(parus_backend
    INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(parus_backend INTERFACE cxx_std_23)

target_link_libraries(parus_backend
    INTERFACE
    parus_frontend
)

if(PARUS_ENABLE_AOT_BACKEND AND PARUS_AOT_ENABLE_LLVM)
    add_subdirectory(llvmconfig)
endif()

if(PARUS_ENABLE_AOT_BACKEND)
    add_subdirectory(src/aot)
endif()

add_subdirectory(src/link)
add_subdirectory(src/parlib)
add_subdirectory(tools/parus-lld)

if(PARUS_ENABLE_JIT_BACKEND)
    add_subdirectory(src/jit)
endif()

if(PARUS_ENABLE_WASM_BACKEND)
    add_subdirectory(src/wasm)
endif()
