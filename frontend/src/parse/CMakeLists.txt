set(PARUS_PARSE_SOURCES)

include(${CMAKE_CURRENT_LIST_DIR}/common/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/decl/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/stmt/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/expr/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/type/CMakeLists.txt)

set(PARUS_PARSE_SOURCES ${PARUS_PARSE_SOURCES} PARENT_SCOPE)
