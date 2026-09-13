#
# Worker invoked via `cmake -P` from the validate step of every dep.
# Inputs (via -D):
#   PREFIX, NAME, PACKAGE_NAME, TARGET_NAME, LIB_NAMES, DLL_NAMES, HEADER,
#   INCLUDE_SUBDIR, CONFIG_SUBDIR, REQUIRES_CONFIG, SHARED, WIN,
#   VALIDATION_DIR, PROBES_DIR, SDRPP_TRACE_DEP_ARTIFACTS,
#   CMAKE_TOOLCHAIN_FILE, and Android toolchain settings
#
# Re-splits the |-packed list args, runs file-existence probes against
# the install prefix, and writes a manifest on success. Aggregates all
# misses into a single FATAL_ERROR pointing at the recipe.
#
# Script mode cannot load most Config.cmake files because they call
# add_library(IMPORTED). When REQUIRES_CONFIG is set, this worker therefore
# also configures a tiny throwaway CMake project and checks the imported
# target's TYPE, IMPORTED_LOCATION, and IMPORTED_IMPLIB there.
#

string(REPLACE "|" ";" LIB_NAMES "${LIB_NAMES}")
string(REPLACE "|" ";" DLL_NAMES "${DLL_NAMES}")

if (NOT DEFINED PACKAGE_NAME OR "${PACKAGE_NAME}" STREQUAL "")
    set(PACKAGE_NAME "${NAME}")
endif ()

if (NOT DEFINED SDRPP_TRACE_DEP_ARTIFACTS)
    set(SDRPP_TRACE_DEP_ARTIFACTS "OFF")
endif ()

if (NOT DEFINED VALIDATION_DIR OR "${VALIDATION_DIR}" STREQUAL "")
    set(VALIDATION_DIR "${PREFIX}/share/sdrpp-deps")
endif ()
if (NOT DEFINED PROBES_DIR OR "${PROBES_DIR}" STREQUAL "")
    set(PROBES_DIR "${VALIDATION_DIR}/probes")
endif ()
get_filename_component(VALIDATION_DIR "${VALIDATION_DIR}" ABSOLUTE)
get_filename_component(PROBES_DIR "${PROBES_DIR}" ABSOLUTE)

function(_sdrpp_validate_dep_should_trace out_var)
    set(_raw "${SDRPP_TRACE_DEP_ARTIFACTS}")
    string(STRIP "${_raw}" _raw)
    string(TOUPPER "${_raw}" _upper)

    if ("${_upper}" MATCHES "^(ON|TRUE|YES|1|ALL)$")
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif ()
    if ("${_upper}" STREQUAL "" OR "${_upper}" MATCHES "^(OFF|FALSE|NO|0|NONE)$")
        set(${out_var} FALSE PARENT_SCOPE)
        return()
    endif ()

    string(REPLACE "|" ";" _names "${_raw}")
    string(REPLACE "," ";" _names "${_names}")
    string(REGEX REPLACE "[ \t\r\n]+" ";" _names "${_names}")
    list(FILTER _names EXCLUDE REGEX "^$")

    string(TOLOWER "${NAME}" _name_lc)
    set(_enabled FALSE)
    foreach (_candidate IN LISTS _names)
        string(STRIP "${_candidate}" _candidate)
        string(TOLOWER "${_candidate}" _candidate_lc)
        if ("${_candidate_lc}" STREQUAL "${_name_lc}")
            set(_enabled TRUE)
        endif ()
    endforeach ()

    set(${out_var} ${_enabled} PARENT_SCOPE)
endfunction()

function(_sdrpp_trace_artifact_path label value)
    set(_value "${value}")
    if ("${_value}" STREQUAL "" OR "${_value}" MATCHES "-NOTFOUND$")
        message(STATUS "  ${label}: <not found>")
    elseif (EXISTS "${_value}")
        message(STATUS "  ${label}: ${_value}")
    else ()
        message(STATUS "  ${label}: ${_value} (missing)")
    endif ()
endfunction()

set(_errors "")

# 1. Library file (.lib/.a/.so/.dylib).
# On UNIX, some upstreams (zlib) install both shared and static variants under
# the same basename (libz.dylib + libz.a). The default suffix order on macOS
# prefers .dylib, so a static-policy validation would silently latch onto the
# shared lib. Restrict to .a when SHARED=0 to keep the linkage choice honest.
if (NOT WIN AND NOT SHARED)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif ()
unset(_lib CACHE)
find_library(_lib NAMES ${LIB_NAMES}
    HINTS "${PREFIX}/lib" "${PREFIX}/bin"
    NO_DEFAULT_PATH)
if (NOT _lib)
    list(APPEND _errors
        "library not found — searched for [${LIB_NAMES}] under ${PREFIX}/lib and ${PREFIX}/bin")
endif ()

# 2. Runtime DLL — only meaningful on Windows shared builds.
set(_dll "")
if (WIN AND SHARED AND DLL_NAMES)
    unset(_dll_found CACHE)
    find_file(_dll_found NAMES ${DLL_NAMES}
        HINTS "${PREFIX}/bin" "${PREFIX}/lib"
        NO_DEFAULT_PATH)
    if (NOT _dll_found)
        list(APPEND _errors
            "DLL not found — searched for [${DLL_NAMES}] under ${PREFIX}/bin and ${PREFIX}/lib")
    else ()
        set(_dll "${_dll_found}")
    endif ()
endif ()
if (WIN AND NOT SHARED AND DLL_NAMES AND _lib)
    unset(_static_runtime_found CACHE)
    find_file(_static_runtime_found NAMES ${DLL_NAMES}
        HINTS "${PREFIX}/bin" "${PREFIX}/lib"
        NO_DEFAULT_PATH)
    if (_static_runtime_found)
        get_filename_component(_lib_stem "${_lib}" NAME_WE)
        get_filename_component(_runtime_stem "${_static_runtime_found}" NAME_WE)
        string(REGEX REPLACE "^lib" "" _lib_stem_cmp "${_lib_stem}")
        string(REGEX REPLACE "^lib" "" _runtime_stem_cmp "${_runtime_stem}")
        if (_lib_stem_cmp STREQUAL _runtime_stem_cmp)
            list(APPEND _errors
                "static policy selected '${_lib}', but runtime '${_static_runtime_found}' also exists with the same basename, so the selected .lib is likely a Windows import library, not a static archive")
        endif ()
    endif ()
endif ()

# 3. Header.
set(_inc "")
if (NOT "${HEADER}" STREQUAL "")
    unset(_inc_found CACHE)
    set(_hints "${PREFIX}/include")
    if (NOT "${INCLUDE_SUBDIR}" STREQUAL "")
        list(INSERT _hints 0 "${PREFIX}/include/${INCLUDE_SUBDIR}")
    endif ()
    find_path(_inc_found NAMES "${HEADER}"
        HINTS ${_hints}
        NO_DEFAULT_PATH)
    if (NOT _inc_found)
        string(REPLACE ";" ", " _hints_pretty "${_hints}")
        list(APPEND _errors
            "header '${HEADER}' not found — searched ${_hints_pretty}")
    else ()
        set(_inc "${_inc_found}")
    endif ()
endif ()

# 4. Package config presence.
set(_cfg "")
if (REQUIRES_CONFIG)
    string(TOLOWER "${PACKAGE_NAME}" _package_name_lower)
    set(_config_hints "")
    if (DEFINED CONFIG_SUBDIR AND NOT "${CONFIG_SUBDIR}" STREQUAL "")
        list(APPEND _config_hints "${PREFIX}/lib/cmake/${CONFIG_SUBDIR}")
    endif ()
    list(APPEND _config_hints
        "${PREFIX}/lib/cmake/${PACKAGE_NAME}"
        "${PREFIX}/lib/cmake/${_package_name_lower}")

    unset(_cfg_found CACHE)
    find_file(_cfg_found
        NAMES
            "${PACKAGE_NAME}Config.cmake"
            "${PACKAGE_NAME}-config.cmake"
            "${_package_name_lower}Config.cmake"
            "${_package_name_lower}-config.cmake"
        HINTS ${_config_hints}
        NO_DEFAULT_PATH)
    if (NOT _cfg_found)
        string(REPLACE ";" ", " _config_hints_pretty "${_config_hints}")
        list(APPEND _errors
            "package config missing - searched ${_config_hints_pretty}")
    else ()
        set(_cfg "${_cfg_found}")
    endif ()
endif ()

function(_sdrpp_validate_dep_write_target_probe out_var)
    file(TO_CMAKE_PATH "${PREFIX}" _probe_prefix)
    file(TO_CMAKE_PATH "${_lib}" _probe_expected_lib)
    set(_probe_expected_location "${_probe_expected_lib}")
    set(_probe_expected_implib "")
    if (WIN AND SHARED)
        file(TO_CMAKE_PATH "${_dll}" _probe_expected_location)
        set(_probe_expected_implib "${_probe_expected_lib}")
    endif ()

    set(_probe_expected_type STATIC_LIBRARY)
    if (SHARED)
        set(_probe_expected_type SHARED_LIBRARY)
    endif ()

    set(_probe_src "${PROBES_DIR}/${NAME}/src")
    set(_probe_bin "${PROBES_DIR}/${NAME}/build")
    file(REMOVE_RECURSE "${_probe_src}" "${_probe_bin}")
    file(MAKE_DIRECTORY "${_probe_src}")

    string(CONFIGURE [=[
cmake_minimum_required(VERSION 3.16)
# Enable C/C++ so CMake runs compiler-ABI detection — that's what populates
# CMAKE_LIBRARY_ARCHITECTURE, which in turn adds Debian/Ubuntu multiarch
# paths (e.g. /usr/lib/x86_64-linux-gnu) to find_library's default search.
# With project(NONE) the probe can't resolve transitive system deps
# (CURLConfig.cmake's find_dependency(ZLIB) finds zlib.h but misses libz.so
# because the multiarch dir isn't searched).
project(sdrpp_validate_dep_target C CXX)

set(_prefix "@_probe_prefix@")
set(_package_name "@PACKAGE_NAME@")
set(_target_name "@TARGET_NAME@")
set(_expected_type "@_probe_expected_type@")
set(_expected_location "@_probe_expected_location@")
set(_expected_implib "@_probe_expected_implib@")

set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)
list(PREPEND CMAKE_PREFIX_PATH "${_prefix}")

set(_probe_module_dir "${CMAKE_CURRENT_BINARY_DIR}/cmake")
file(MAKE_DIRECTORY "${_probe_module_dir}")
file(WRITE "${_probe_module_dir}/FindThreads.cmake" [==[
if (NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED)
endif ()
set(Threads_FOUND TRUE)
set(THREADS_FOUND TRUE)
set(CMAKE_THREAD_LIBS_INIT "")
]==])
list(PREPEND CMAKE_MODULE_PATH "${_probe_module_dir}")

# Android's toolchain restricts package, library, and include searches to
# CMAKE_FIND_ROOT_PATH.  The package under test can load transitive dependency
# modules from its Config.cmake (curl 8.22 does this for Mbed TLS), and those
# nested searches do not inherit NO_CMAKE_FIND_ROOT_PATH from the outer
# find_package call below.  Make the bundled prefix an explicit Android root so
# the probe sees the same dependency closure that the main build consumes.
if (ANDROID)
    list(REMOVE_ITEM CMAKE_FIND_ROOT_PATH "${_prefix}")
    list(PREPEND CMAKE_FIND_ROOT_PATH "${_prefix}")
endif ()

find_package(${_package_name} CONFIG REQUIRED
    PATHS "${_prefix}"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)

if (NOT TARGET ${_target_name})
    message(FATAL_ERROR "package '${_package_name}' did not define target '${_target_name}'")
endif ()

get_target_property(_type ${_target_name} TYPE)
if (NOT _type OR _type MATCHES "-NOTFOUND$")
    message(FATAL_ERROR "target '${_target_name}' has no TYPE property")
endif ()
if (_expected_type STREQUAL "STATIC_LIBRARY" AND _type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "target '${_target_name}' is SHARED_LIBRARY, expected static-compatible target")
endif ()
if (_expected_type STREQUAL "SHARED_LIBRARY" AND _type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "target '${_target_name}' is STATIC_LIBRARY, expected shared-compatible target")
endif ()

function(_normalize_path out_var path)
    if ("${path}" STREQUAL "" OR "${path}" MATCHES "-NOTFOUND$")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif ()
    file(TO_CMAKE_PATH "${path}" _path)
    # REALPATH (not ABSOLUTE) so versioned ELF symlinks resolve to their target:
    # find_library returns libfoo.so, the imported target points at libfoo.so.X.Y.
    get_filename_component(_path "${_path}" REALPATH)
    set(${out_var} "${_path}" PARENT_SCOPE)
endfunction()

function(_collect_imported_paths out_var prop)
    set(_props "${prop}")
    get_target_property(_configs ${_target_name} IMPORTED_CONFIGURATIONS)
    if (_configs)
        foreach (_cfg IN LISTS _configs)
            string(TOUPPER "${_cfg}" _cfg_upper)
            list(APPEND _props "${prop}_${_cfg_upper}")
        endforeach ()
    endif ()
    foreach (_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        list(APPEND _props "${prop}_${_cfg}")
    endforeach ()
    list(REMOVE_DUPLICATES _props)

    set(_values "")
    foreach (_prop IN LISTS _props)
        get_target_property(_value ${_target_name} "${_prop}")
        if (_value AND NOT _value MATCHES "-NOTFOUND$")
            foreach (_one IN LISTS _value)
                _normalize_path(_norm "${_one}")
                if (_norm)
                    list(APPEND _values "${_norm}")
                endif ()
            endforeach ()
        endif ()
    endforeach ()
    list(REMOVE_DUPLICATES _values)
    set(${out_var} "${_values}" PARENT_SCOPE)
endfunction()

function(_assert_path_list_contains label expected list_var)
    if ("${expected}" STREQUAL "")
        return()
    endif ()
    _normalize_path(_expected "${expected}")
    set(_found FALSE)
    foreach (_actual IN LISTS ${list_var})
        if (_actual STREQUAL _expected)
            set(_found TRUE)
        endif ()
    endforeach ()
    if (NOT _found)
        string(REPLACE ";" "\n    " _actual_pretty "${${list_var}}")
        if ("${_actual_pretty}" STREQUAL "")
            set(_actual_pretty "<none>")
        endif ()
        message(FATAL_ERROR
            "${label} for target '${_target_name}' does not point at the selected artifact\n"
            "  expected: ${_expected}\n"
            "  actual:\n"
            "    ${_actual_pretty}")
    endif ()
endfunction()

_collect_imported_paths(_locations IMPORTED_LOCATION)
_collect_imported_paths(_implibs IMPORTED_IMPLIB)

_assert_path_list_contains("IMPORTED_LOCATION" "${_expected_location}" _locations)
if (_expected_implib)
    _assert_path_list_contains("IMPORTED_IMPLIB" "${_expected_implib}" _implibs)
elseif (_expected_type STREQUAL "STATIC_LIBRARY" AND _implibs)
    string(REPLACE ";" "\n    " _implibs_pretty "${_implibs}")
    message(FATAL_ERROR
        "static target '${_target_name}' unexpectedly has IMPORTED_IMPLIB values:\n"
        "    ${_implibs_pretty}")
endif ()
]=] _probe_content @ONLY)

    file(WRITE "${_probe_src}/CMakeLists.txt" "${_probe_content}")
    set(${out_var} "${_probe_src};${_probe_bin}" PARENT_SCOPE)
endfunction()

if (REQUIRES_CONFIG AND _cfg AND _lib AND NOT "${TARGET_NAME}" STREQUAL "")
    _sdrpp_validate_dep_write_target_probe(_probe_dirs)
    list(GET _probe_dirs 0 _probe_src)
    list(GET _probe_dirs 1 _probe_bin)
    set(_probe_cmake_args "")
    if (DEFINED CMAKE_TOOLCHAIN_FILE AND NOT "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
        list(APPEND _probe_cmake_args
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
    endif ()
    foreach (_var IN ITEMS
            ANDROID_ABI
            ANDROID_PLATFORM
            ANDROID_STL
            ANDROID_ARM_MODE
            ANDROID_ARM_NEON)
        if (DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
            list(APPEND _probe_cmake_args "-D${_var}:STRING=${${_var}}")
        endif ()
    endforeach ()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_probe_src}" -B "${_probe_bin}"
            ${_probe_cmake_args}
        RESULT_VARIABLE _probe_result
        OUTPUT_VARIABLE _probe_stdout
        ERROR_VARIABLE _probe_stderr)
    if (NOT _probe_result EQUAL 0)
        set(_probe_output "${_probe_stdout}${_probe_stderr}")
        string(REPLACE "\n" "\n    " _probe_output "${_probe_output}")
        string(REPLACE ";" "\\;" _probe_output "${_probe_output}")
        list(APPEND _errors
            "imported target verification failed for '${TARGET_NAME}' from find_package(${PACKAGE_NAME} CONFIG):\n    ${_probe_output}")
    endif ()
endif ()

# 5. Manifest path (written after validation succeeds).
set(_manifest_dir "${VALIDATION_DIR}")
set(_manifest "${_manifest_dir}/${NAME}.installed")

_sdrpp_validate_dep_should_trace(_trace_dep_artifacts)
if (_trace_dep_artifacts)
    string(REPLACE ";" ", " _lib_names_pretty "${LIB_NAMES}")
    string(REPLACE ";" ", " _dll_names_pretty "${DLL_NAMES}")
    set(_linkage static)
    if (SHARED)
        set(_linkage shared)
    endif ()

    set(_interface_include "${_inc}")
    if ("${_interface_include}" STREQUAL "" AND "${HEADER}" STREQUAL "")
        set(_interface_include "${PREFIX}/include")
    endif ()

    set(_imported_location "${_lib}")
    set(_imported_implib "")
    if (WIN AND SHARED)
        set(_imported_location "${_dll}")
        set(_imported_implib "${_lib}")
    endif ()

    message(STATUS "ValidateDep[${NAME}] artifacts consumed by SDRIAK:")
    message(STATUS "  package: find_package(${PACKAGE_NAME} CONFIG)")
    message(STATUS "  target: ${TARGET_NAME}")
    message(STATUS "  prefix hint (CMAKE_PREFIX_PATH): ${PREFIX}")
    message(STATUS "  linkage: ${_linkage}")
    message(STATUS "  library candidates: ${_lib_names_pretty}")
    _sdrpp_trace_artifact_path("library file" "${_lib}")
    if (WIN AND SHARED)
        message(STATUS "  runtime candidates: ${_dll_names_pretty}")
        _sdrpp_trace_artifact_path("runtime binary" "${_dll}")
    else ()
        message(STATUS "  runtime binary: <not applicable>")
    endif ()
    if (NOT "${HEADER}" STREQUAL "")
        message(STATUS "  header: ${HEADER}")
        _sdrpp_trace_artifact_path("include directory" "${_interface_include}")
    else ()
        message(STATUS "  header: <not checked>")
        _sdrpp_trace_artifact_path("include directory" "${_interface_include}")
    endif ()
    if (REQUIRES_CONFIG)
        _sdrpp_trace_artifact_path("Config.cmake" "${_cfg}")
    else ()
        message(STATUS "  Config.cmake: <not required by validation>")
    endif ()
    _sdrpp_trace_artifact_path("target IMPORTED_LOCATION" "${_imported_location}")
    if (WIN AND SHARED)
        _sdrpp_trace_artifact_path("target IMPORTED_IMPLIB" "${_imported_implib}")
    endif ()
    _sdrpp_trace_artifact_path("target INTERFACE_INCLUDE_DIRECTORIES" "${_interface_include}")
    message(STATUS "  manifest: ${_manifest} (written after validation)")
endif ()

if (_errors)
    set(_msg "ValidateDep[${NAME}]: install at ${PREFIX} is incomplete or misplaced")
    foreach (e ${_errors})
        set(_msg "${_msg}\n  - ${e}")
    endforeach ()
    set(_msg "${_msg}\n  → fix deps/+${NAME}/${NAME}.cmake; the build/install step likely failed silently or installed to an unexpected layout.")
    message(FATAL_ERROR "${_msg}")
endif ()

# 6. Manifest.
file(MAKE_DIRECTORY "${_manifest_dir}")
set(_content "name=${NAME}\npackage=${PACKAGE_NAME}\ntarget=${TARGET_NAME}\nlibrary=${_lib}\n")
if (_dll)
    set(_content "${_content}dll=${_dll}\n")
endif ()
if (_inc)
    set(_content "${_content}include=${_inc}\n")
endif ()
if (_cfg)
    set(_content "${_content}config=${_cfg}\n")
endif ()
file(WRITE "${_manifest}" "${_content}")

message(STATUS "ValidateDep[${NAME}]: OK")
