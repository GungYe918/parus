# parus/cmake
cmake_minimum_required(VERSION 3.25)

project(parus
  VERSION 0.1.0
  LANGUAGES CXX
)

# ---- global C++ settings ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT MSVC)
  add_compile_options(-Wno-trigraphs)
endif()

option(PARUS_BUILD_TESTS "Build Parus tests" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(compiler)
add_subdirectory(tools/parusc)


if(PARUS_BUILD_TESTS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  endif()
endif()
