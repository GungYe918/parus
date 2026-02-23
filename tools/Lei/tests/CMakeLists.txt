add_executable(lei_tests
    main.cpp
)

target_link_libraries(lei_tests PRIVATE lei_graph)
target_compile_features(lei_tests PRIVATE cxx_std_23)
target_compile_definitions(lei_tests PRIVATE
    LEI_TEST_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases"
    LEI_PROJECT_DIR="${CMAKE_CURRENT_LIST_DIR}/.."
    LEI_BUILD_BIN="$<TARGET_FILE:lei-build>"
)

if (MSVC)
    target_compile_options(lei_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(lei_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

add_test(NAME lei_tests COMMAND lei_tests)
