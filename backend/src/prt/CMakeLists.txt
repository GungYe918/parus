add_library(parus_backend_prt_hosted STATIC
    prt_hosted.cpp
)

add_library(parus_backend_prt_freestanding STATIC
    prt_freestanding.cpp
)

target_compile_features(parus_backend_prt_hosted PRIVATE cxx_std_23)
target_compile_features(parus_backend_prt_freestanding PRIVATE cxx_std_23)

if (MSVC)
    target_compile_options(parus_backend_prt_hosted PRIVATE /W4 /permissive-)
    target_compile_options(parus_backend_prt_freestanding PRIVATE /W4 /permissive-)
else()
    target_compile_options(parus_backend_prt_hosted PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
    target_compile_options(parus_backend_prt_freestanding PRIVATE -Wall -Wextra -Wpedantic -Wno-trigraphs)
endif()

if(APPLE)
    set(PARUS_PRT_OSX_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT PARUS_PRT_OSX_MIN_VERSION)
        set(PARUS_PRT_OSX_MIN_VERSION "14.0")
    endif()
    target_compile_options(parus_backend_prt_hosted PRIVATE "-mmacosx-version-min=${PARUS_PRT_OSX_MIN_VERSION}")
    target_compile_options(parus_backend_prt_freestanding PRIVATE "-mmacosx-version-min=${PARUS_PRT_OSX_MIN_VERSION}")
endif()
