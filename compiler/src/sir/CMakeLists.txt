set(GAUPEL_SIR_SOURCES)

include(${CMAKE_CURRENT_LIST_DIR}/analysis/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/lower/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/verify/CMakeLists.txt)

set(GAUPEL_SIR_SOURCES ${GAUPEL_SIR_SOURCES} PARENT_SCOPE)
