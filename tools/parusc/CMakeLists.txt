# tools/parusc/cmake
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

target_compile_features(parusc PRIVATE cxx_std_23)

if (MSVC)
  target_compile_options(parusc PRIVATE /W4 /permissive-)
else()
  target_compile_options(parusc PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()
