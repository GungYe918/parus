set(PARUS_TYCK_SOURCES)

include(${CMAKE_CURRENT_LIST_DIR}/core/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/stmt/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/expr/CMakeLists.txt)

set(PARUS_TYCK_SOURCES ${PARUS_TYCK_SOURCES} PARENT_SCOPE)
