# Runtime DLL staging for MSVC executables other than sdriak.exe: tests,
# benches and tools. sdriak.exe's own POST_BUILD step (root CMakeLists.txt)
# stages the DLLs of the app and every plugin, but a test is built and run by
# ctest without sdrpp, and module tests sit in their module's binary
# directory, so each executable stages its own DLLs next to itself:
#   - its $<TARGET_RUNTIME_DLLS> (sdriak_core.dll and imported shared deps),
#   - their transitive dependencies found in the deps prefix's bin/ (fftw3f,
#     volk, spdlog, pthreadVC3, zlib, ...), resolved with dumpbin,
#   - the AddressSanitizer runtime when the build uses /fsanitize=address.
# Nothing is pruned: a test often shares sdriak.exe's directory, which holds
# DLLs other targets need. No-op on non-MSVC toolchains (rpath).

# The deps prefix bin/ directories that transitive DLLs may come from.
function(sdrpp_runtime_source_dirs out_var)
    set(_dirs "")
    foreach (_pp ${CMAKE_PREFIX_PATH})
        if (EXISTS "${_pp}/bin")
            list(APPEND _dirs "${_pp}/bin")
        endif ()
    endforeach ()
    list(REMOVE_DUPLICATES _dirs)
    set(${out_var} "${_dirs}" PARENT_SCOPE)
endfunction ()

# The MSVC AddressSanitizer runtime DLL when any configured flag set enables
# /fsanitize=address, else empty. It sits beside cl.exe.
function(sdrpp_asan_runtime_dll out_var)
    set(_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")
    foreach (_cfg IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_cfg}" _cfg_upper)
        string(APPEND _flags " ${CMAKE_C_FLAGS_${_cfg_upper}} ${CMAKE_CXX_FLAGS_${_cfg_upper}}")
    endforeach ()
    set(_dll "")
    if (MSVC AND _flags MATCHES "[/-]fsanitize=address")
        get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if (CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_name "clang_rt.asan_dynamic-x86_64.dll")
        else ()
            set(_name "clang_rt.asan_dynamic-i386.dll")
        endif ()
        if (EXISTS "${_cl_dir}/${_name}")
            set(_dll "${_cl_dir}/${_name}")
        else ()
            message(WARNING "AddressSanitizer is on but ${_name} is not beside ${CMAKE_CXX_COMPILER}")
        endif ()
    endif ()
    set(${out_var} "${_dll}" PARENT_SCOPE)
endfunction ()

# Call in the directory that defines <target> (add_custom_command(TARGET)
# cannot reach targets of other directories).
function(sdrpp_stage_runtime_dlls target)
    if (NOT MSVC)
        return()
    endif ()
    sdrpp_runtime_source_dirs(_source_dirs)
    sdrpp_asan_runtime_dll(_asan_dll)

    set(_manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}_runtime_dlls_$<CONFIG>.cmake")
    set(_content "set(SDRPP_RUNTIME_DLLS\n    [=[$<TARGET_RUNTIME_DLLS:${target}>]=]\n")
    if (_asan_dll)
        string(APPEND _content "    [=[${_asan_dll}]=]\n")
    endif ()
    string(APPEND _content ")\nset(SDRPP_RUNTIME_SOURCE_DIRS\n")
    foreach (_dir IN LISTS _source_dirs)
        string(APPEND _content "    [=[${_dir}]=]\n")
    endforeach ()
    string(APPEND _content ")\n")
    file(GENERATE OUTPUT "${_manifest}" CONTENT "${_content}")

    get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(SDRPP_DUMPBIN dumpbin HINTS "${_linker_dir}" NO_DEFAULT_PATH)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DRUNTIME_MANIFEST=${_manifest}"
            "-DDST=$<TARGET_FILE_DIR:${target}>"
            "-DSEED=$<TARGET_FILE:${target}>"
            "-DTAG=${target}"
            -DPRUNE=OFF
            "-DRUNTIME_DEPENDENCIES_COMMAND=${SDRPP_DUMPBIN}"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        COMMENT "Staging runtime DLLs for ${target}"
        VERBATIM)
endfunction ()

# sdrpp_stage_runtime_dlls() for every executable the calling directory has
# defined so far (call it after the directory's add_executable()s).
function(sdrpp_stage_directory_runtime_dlls)
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if (_type STREQUAL "EXECUTABLE" AND NOT _target STREQUAL "sdrpp")
            sdrpp_stage_runtime_dlls(${_target})
        endif ()
    endforeach ()
endfunction ()
