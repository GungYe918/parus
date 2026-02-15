# backend/llvmconfig/CMakeLists.txt

set(_parus_llvm_selected "${PARUS_LLVM_VERSION}")
set(_parus_llvm_toolchain_ok FALSE)
set(_parus_llvm_config "")
set(_parus_llvm_version "")
set(_parus_llvm_include_dir "")
set(_parus_llvm_lib_dir "")
set(_parus_llvm_prefix "")
set(_parus_llvm_libfiles "")
set(_parus_llvm_system_libs "")
set(_parus_llvm_ldflags "")
set(_parus_llvm_link_dirs "")

if(NOT _parus_llvm_selected STREQUAL "20" AND NOT _parus_llvm_selected STREQUAL "21")
    message(FATAL_ERROR
        "Unsupported PARUS_LLVM_VERSION='${_parus_llvm_selected}'. Supported values are: 20, 21.")
endif()

if(PARUS_LLVM_USE_TOOLCHAIN)
    if(PARUS_LLVM_CONFIG_EXECUTABLE)
        set(_parus_llvm_config "${PARUS_LLVM_CONFIG_EXECUTABLE}")
    else()
        find_program(_parus_llvm_config NAMES llvm-config)
    endif()

    if(_parus_llvm_config)
        execute_process(
            COMMAND "${_parus_llvm_config}" --version
            RESULT_VARIABLE _parus_llvm_ver_rc
            OUTPUT_VARIABLE _parus_llvm_ver_raw
            ERROR_VARIABLE _parus_llvm_ver_err
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(_parus_llvm_ver_rc EQUAL 0)
            string(STRIP "${_parus_llvm_ver_raw}" _parus_llvm_version)
            string(REGEX MATCH "^[0-9]+" _parus_llvm_major "${_parus_llvm_version}")

            if(_parus_llvm_major STREQUAL _parus_llvm_selected)
                set(_parus_llvm_toolchain_ok TRUE)

                execute_process(
                    COMMAND "${_parus_llvm_config}" --includedir
                    OUTPUT_VARIABLE _parus_llvm_include_dir
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                execute_process(
                    COMMAND "${_parus_llvm_config}" --libdir
                    OUTPUT_VARIABLE _parus_llvm_lib_dir
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                execute_process(
                    COMMAND "${_parus_llvm_config}" --prefix
                    OUTPUT_VARIABLE _parus_llvm_prefix
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                execute_process(
                    COMMAND "${_parus_llvm_config}" --link-static --libfiles all
                    OUTPUT_VARIABLE _parus_llvm_libfiles_raw
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                execute_process(
                    COMMAND "${_parus_llvm_config}" --link-static --system-libs
                    OUTPUT_VARIABLE _parus_llvm_system_libs_raw
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                execute_process(
                    COMMAND "${_parus_llvm_config}" --ldflags
                    OUTPUT_VARIABLE _parus_llvm_ldflags_raw
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )

                string(REPLACE "\n" " " _parus_llvm_libfiles_raw "${_parus_llvm_libfiles_raw}")
                string(REPLACE "\n" " " _parus_llvm_system_libs_raw "${_parus_llvm_system_libs_raw}")
                string(REPLACE "\n" " " _parus_llvm_ldflags_raw "${_parus_llvm_ldflags_raw}")

                separate_arguments(_parus_llvm_libfiles UNIX_COMMAND "${_parus_llvm_libfiles_raw}")
                separate_arguments(_parus_llvm_system_libs UNIX_COMMAND "${_parus_llvm_system_libs_raw}")
                separate_arguments(_parus_llvm_ldflags UNIX_COMMAND "${_parus_llvm_ldflags_raw}")

                if(_parus_llvm_lib_dir)
                    list(APPEND _parus_llvm_link_dirs "${_parus_llvm_lib_dir}")
                endif()

                # Homebrew LLVM는 system-libs에 -lzstd/-lxml2를 내보내지만,
                # 링크 경로가 /opt/homebrew/lib 로 자동 추가되지 않는 경우가 있다.
                # lane별 llvm-config prefix와 brew --prefix를 기준으로 보강한다.
                if(APPLE)
                    find_program(_parus_brew_exe brew)
                    if(_parus_brew_exe)
                        execute_process(
                            COMMAND "${_parus_brew_exe}" --prefix
                            OUTPUT_VARIABLE _parus_brew_prefix
                            OUTPUT_STRIP_TRAILING_WHITESPACE
                        )
                        if(EXISTS "${_parus_brew_prefix}/lib")
                            list(APPEND _parus_llvm_link_dirs "${_parus_brew_prefix}/lib")
                        endif()
                        if(EXISTS "${_parus_brew_prefix}/opt/zstd/lib")
                            list(APPEND _parus_llvm_link_dirs "${_parus_brew_prefix}/opt/zstd/lib")
                        endif()
                        if(EXISTS "${_parus_brew_prefix}/opt/libxml2/lib")
                            list(APPEND _parus_llvm_link_dirs "${_parus_brew_prefix}/opt/libxml2/lib")
                        endif()
                        if(EXISTS "${_parus_brew_prefix}/opt/zlib/lib")
                            list(APPEND _parus_llvm_link_dirs "${_parus_brew_prefix}/opt/zlib/lib")
                        endif()
                    endif()

                    if(_parus_llvm_prefix MATCHES ".*/opt/llvm(@[0-9]+)?$")
                        get_filename_component(_parus_llvm_opt_dir "${_parus_llvm_prefix}" DIRECTORY)
                        get_filename_component(_parus_llvm_brew_prefix "${_parus_llvm_opt_dir}" DIRECTORY)
                        if(EXISTS "${_parus_llvm_brew_prefix}/lib")
                            list(APPEND _parus_llvm_link_dirs "${_parus_llvm_brew_prefix}/lib")
                        endif()
                    endif()
                endif()
                list(REMOVE_DUPLICATES _parus_llvm_link_dirs)
                foreach(_parus_link_dir IN LISTS _parus_llvm_link_dirs)
                    list(APPEND _parus_llvm_ldflags "-L${_parus_link_dir}")
                endforeach()
                list(REMOVE_DUPLICATES _parus_llvm_ldflags)
            else()
                message(STATUS
                    "parus_llvmconfig: llvm-config major mismatch. selected lane=${_parus_llvm_selected}, "
                    "found version=${_parus_llvm_version} (${_parus_llvm_config})")
            endif()
        else()
            message(STATUS
                "parus_llvmconfig: failed to query llvm-config version from '${_parus_llvm_config}': "
                "${_parus_llvm_ver_err}")
        endif()
    else()
        message(STATUS "parus_llvmconfig: llvm-config not found.")
    endif()
endif()

if(PARUS_LLVM_REQUIRE_TOOLCHAIN AND NOT _parus_llvm_toolchain_ok)
    message(FATAL_ERROR
        "PARUS_LLVM_REQUIRE_TOOLCHAIN=ON but LLVM ${_parus_llvm_selected} toolchain was not found.")
endif()

set(PARUS_AOT_LLVM_LANE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/lanes/v${_parus_llvm_selected}/AOTLlvmV${_parus_llvm_selected}.cpp"
    PARENT_SCOPE
)
set(PARUS_LLVM_TOOLCHAIN_FOUND "${_parus_llvm_toolchain_ok}" PARENT_SCOPE)
set(PARUS_LLVM_CONFIG_USED "${_parus_llvm_config}" PARENT_SCOPE)

add_library(parus_llvmconfig INTERFACE)

target_compile_definitions(parus_llvmconfig
    INTERFACE
    PARUS_WITH_LLVM=1
    PARUS_LLVM_SELECTED_MAJOR=${_parus_llvm_selected}
    PARUS_LLVM_TOOLCHAIN_FOUND=$<IF:$<BOOL:${_parus_llvm_toolchain_ok}>,1,0>
)

if(_parus_llvm_toolchain_ok)
    target_include_directories(parus_llvmconfig INTERFACE "${_parus_llvm_include_dir}")
    target_link_directories(parus_llvmconfig INTERFACE ${_parus_llvm_link_dirs})
    target_link_libraries(parus_llvmconfig INTERFACE ${_parus_llvm_libfiles} ${_parus_llvm_system_libs})
    target_link_options(parus_llvmconfig INTERFACE ${_parus_llvm_ldflags})
endif()

message(STATUS
    "parus_llvmconfig: lane=${_parus_llvm_selected}, use_toolchain=${PARUS_LLVM_USE_TOOLCHAIN}, "
    "toolchain_found=${_parus_llvm_toolchain_ok}, llvm_config=${_parus_llvm_config}, "
    "llvm_version=${_parus_llvm_version}, require_toolchain=${PARUS_LLVM_REQUIRE_TOOLCHAIN}")
