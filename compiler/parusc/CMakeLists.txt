# compiler/parusc/CMakeLists.txt
include(${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt)

add_executable(parusc
    src/main.cpp
    ${PARUSC_SOURCES}
)

if(TARGET parus_backend)
    target_link_libraries(parusc PRIVATE parus_backend)
else()
    target_link_libraries(parusc PRIVATE parus_frontend)
endif()

if(TARGET parus_backend_aot)
    target_link_libraries(parusc PRIVATE parus_backend_aot)
    target_compile_definitions(parusc PRIVATE PARUSC_HAS_AOT_BACKEND=1)
else()
    target_compile_definitions(parusc PRIVATE PARUSC_HAS_AOT_BACKEND=0)
endif()

target_include_directories(parusc
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(parusc PRIVATE cxx_std_23)

if (MSVC)
    target_compile_options(parusc PRIVATE /W4 /permissive-)
else()
    target_compile_options(parusc PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
