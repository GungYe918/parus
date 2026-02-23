if(PARUS_BUILD_LEI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Lei/CMakeLists.txt")
    add_subdirectory(Lei)
endif()

if(PARUS_BUILD_PARUSD)
    add_subdirectory(parusd)
endif()

if(PARUS_BUILD_ORCHESTRATOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/parus/CMakeLists.txt")
    add_subdirectory(parus)
endif()
