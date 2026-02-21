if(PARUS_BUILD_PARUSD)
    add_subdirectory(parusd)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Lei/CMakeLists.txt")
    add_subdirectory(Lei)
endif()
