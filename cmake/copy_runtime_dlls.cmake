if (NOT DEFINED RUNTIME_MANIFEST OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_runtime_dlls.cmake requires -DRUNTIME_MANIFEST=<file> and -DDST=<dir>")
endif ()
# Optional:
#   -DPRUNE=OFF  keep DLLs from the source dirs that this manifest does not
#                list (a directory shared with other targets' DLLs).
#   -DSEED=<exe> resolve transitive dependencies of this binary and its
#                listed DLLs only, instead of every .exe/.dll in DST.
#   -DTAG=<name> distinguishes temporary files of parallel stagings.

# file(GET_RUNTIME_DEPENDENCIES) below normalizes paths before matching under
# CMP0207 (CMake 3.31+). NEW is behavior-safe here and silences the per-DLL
# author warnings emitted when the policy is left unset.
if (POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif ()

if (NOT DEFINED PRUNE)
    set(PRUNE ON)
endif ()
if (NOT DEFINED TAG)
    set(TAG "stage")
endif ()

file(MAKE_DIRECTORY "${DST}")

include("${RUNTIME_MANIFEST}")

if (SDRPP_RUNTIME_DLLS)
    list(REMOVE_ITEM SDRPP_RUNTIME_DLLS "")
    list(REMOVE_DUPLICATES SDRPP_RUNTIME_DLLS)
endif ()

# Copies src into DST unless an up-to-date copy is there. Several stagings can
# run at once into one directory (parallel test links), so the copy goes to a
# private temporary name and is renamed into place; losing that race to an
# equal copy is fine.
function(_sdrpp_stage_dll src)
    get_filename_component(_name "${src}" NAME)
    get_filename_component(_src_abs "${src}" ABSOLUTE)
    get_filename_component(_dst_abs "${DST}/${_name}" ABSOLUTE)
    if (_src_abs STREQUAL _dst_abs)
        return()
    endif ()
    if (EXISTS "${_dst_abs}")
        file(SIZE "${_src_abs}" _src_size)
        file(SIZE "${_dst_abs}" _dst_size)
        file(TIMESTAMP "${_src_abs}" _src_time "%Y%m%d%H%M%S" UTC)
        file(TIMESTAMP "${_dst_abs}" _dst_time "%Y%m%d%H%M%S" UTC)
        if (_src_size EQUAL _dst_size AND NOT _src_time STRGREATER _dst_time)
            return()
        endif ()
    endif ()
    set(_tmp "${_dst_abs}.${TAG}.tmp")
    file(COPY_FILE "${_src_abs}" "${_tmp}" RESULT _copy_result)
    if (NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "copy_runtime_dlls: cannot copy ${_src_abs}: ${_copy_result}")
    endif ()
    file(RENAME "${_tmp}" "${_dst_abs}" RESULT _rename_result)
    if (NOT _rename_result EQUAL 0)
        file(REMOVE "${_tmp}")
        if (NOT EXISTS "${_dst_abs}")
            message(FATAL_ERROR "copy_runtime_dlls: cannot place ${_dst_abs}: ${_rename_result}")
        endif ()
    endif ()
endfunction ()

set(_runtime_names "")
foreach (_dll IN LISTS SDRPP_RUNTIME_DLLS)
    get_filename_component(_name "${_dll}" NAME)
    list(APPEND _runtime_names "${_name}")
endforeach ()
list(REMOVE_DUPLICATES _runtime_names)

if (PRUNE)
    foreach (_src_dir IN LISTS SDRPP_RUNTIME_SOURCE_DIRS)
        if (NOT EXISTS "${_src_dir}")
            continue()
        endif ()

        file(GLOB _source_dlls "${_src_dir}/*.dll")
        foreach (_source_dll IN LISTS _source_dlls)
            get_filename_component(_source_name "${_source_dll}" NAME)
            if (NOT "${_source_name}" IN_LIST _runtime_names AND EXISTS "${DST}/${_source_name}")
                file(REMOVE "${DST}/${_source_name}")
            endif ()
        endforeach ()
    endforeach ()
endif ()

foreach (_dll IN LISTS SDRPP_RUNTIME_DLLS)
    if (EXISTS "${_dll}")
        _sdrpp_stage_dll("${_dll}")
    endif ()
endforeach ()

if (DEFINED RUNTIME_DEPENDENCIES_COMMAND AND EXISTS "${RUNTIME_DEPENDENCIES_COMMAND}")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${RUNTIME_DEPENDENCIES_COMMAND}")
endif ()

if (DEFINED SEED)
    set(_seed_executables "${SEED}")
    set(_seed_libraries "")
    foreach (_name IN LISTS _runtime_names)
        if (EXISTS "${DST}/${_name}")
            list(APPEND _seed_libraries "${DST}/${_name}")
        endif ()
    endforeach ()
else ()
    file(GLOB _seed_executables "${DST}/*.exe")
    file(GLOB _seed_libraries "${DST}/*.dll")
endif ()
if (_seed_executables OR _seed_libraries)
    file(GET_RUNTIME_DEPENDENCIES
        RESOLVED_DEPENDENCIES_VAR _resolved_dependencies
        UNRESOLVED_DEPENDENCIES_VAR _unresolved_dependencies
        CONFLICTING_DEPENDENCIES_PREFIX _conflicting_dependencies
        EXECUTABLES ${_seed_executables}
        LIBRARIES ${_seed_libraries}
        DIRECTORIES ${SDRPP_RUNTIME_SOURCE_DIRS})

    foreach (_resolved_dependency IN LISTS _resolved_dependencies)
        file(TO_CMAKE_PATH "${_resolved_dependency}" _resolved_dependency_norm)
        string(TOLOWER "${_resolved_dependency_norm}" _resolved_dependency_lower)

        set(_copy_dependency FALSE)
        foreach (_src_dir IN LISTS SDRPP_RUNTIME_SOURCE_DIRS)
            file(TO_CMAKE_PATH "${_src_dir}" _src_dir_norm)
            string(TOLOWER "${_src_dir_norm}" _src_dir_lower)
            string(FIND "${_resolved_dependency_lower}" "${_src_dir_lower}/" _src_dir_match)
            if (_src_dir_match EQUAL 0)
                set(_copy_dependency TRUE)
                break()
            endif ()
        endforeach ()

        if (_copy_dependency)
            _sdrpp_stage_dll("${_resolved_dependency}")
        endif ()
    endforeach ()
endif ()
