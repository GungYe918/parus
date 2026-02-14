set(GAUPELC_SOURCES)

include(${CMAKE_CURRENT_LIST_DIR}/cli/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/dump/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/driver/CMakeLists.txt)

set(GAUPELC_SOURCES ${GAUPELC_SOURCES} PARENT_SCOPE)
