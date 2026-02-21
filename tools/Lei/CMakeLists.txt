add_library(lei_core
    src/lex/lexer.cpp
)

target_include_directories(lei_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(lei_core PUBLIC cxx_std_23)

if (MSVC)
    target_compile_options(lei_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_core PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_library(lei_parser
    src/parse/parser.cpp
)
target_link_libraries(lei_parser PUBLIC lei_core)
target_include_directories(lei_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lei_parser PUBLIC cxx_std_23)
if (MSVC)
    target_compile_options(lei_parser PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_parser PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_library(lei_eval
    src/eval/evaluator.cpp
)
target_link_libraries(lei_eval PUBLIC lei_parser)
target_include_directories(lei_eval PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lei_eval PUBLIC cxx_std_23)
if (MSVC)
    target_compile_options(lei_eval PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_eval PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_library(lei_graph
    src/graph/ninja_lower.cpp
)
target_link_libraries(lei_graph PUBLIC lei_eval)
target_include_directories(lei_graph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lei_graph PUBLIC cxx_std_23)
if (MSVC)
    target_compile_options(lei_graph PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_graph PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_executable(lei-build
    src/cli/main.cpp
)
target_link_libraries(lei-build PRIVATE lei_graph)
target_compile_features(lei-build PRIVATE cxx_std_23)
if (MSVC)
    target_compile_options(lei-build PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei-build PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

if (PARUS_BUILD_TESTS)
    add_subdirectory(tests)
endif()

