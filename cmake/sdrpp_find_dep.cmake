#
# sdrpp_link_dep(<target> <pkg>
#     [PUBLIC|INTERFACE]
#     [PACKAGE_NAME <name>]
#     [PC_NAME <name>]
#     [TARGETS <names...>]
#     [SHARED_TARGETS <names...>]
#     [STATIC_TARGETS <names...>])
#
# Find a third-party library and link it into <target>. Strategy:
#   1. If CMAKE_PREFIX_PATH prefixes are provided, require a Config.cmake
#      target from those prefixes and stop there.
#   2. On Windows and Android, require the bundled deps / SDR kit flow.
#   3. On Unix desktop builds without explicit prefixes, fall back to the
#      system's Config.cmake or pkg-config package.
#
# Args:
#   <target>           consumer target to link the dependency into
#   <pkg>              policy key and default package/pkg-config name; also the
#                      default basis for imported target names (<pkg>::<pkg>,
#                      <pkg>)
#   PUBLIC/INTERFACE   target_link_libraries scope; defaults to PRIVATE
#   PACKAGE_NAME       Config-mode package name if it differs from <pkg>
#   PC_NAME <name>     pkg-config module name if it differs from <pkg>
#                      (e.g. libusb's pkg-config name is "libusb-1.0")
#   TARGETS <names>    candidate IMPORTED target names exported by Config.cmake.
#                      Defaults to "<pkg>::<pkg>;<pkg>". Override when upstream
#                      uses a non-conventional naming (e.g. FFTW3 exports
#                      "FFTW3::fftw3f").
#   SHARED_TARGETS     preferred target names when the dependency policy resolves
#                      to shared linkage. Falls back to TARGETS if omitted.
#   STATIC_TARGETS     preferred target names when the dependency policy resolves
#                      to static linkage. Falls back to TARGETS if omitted.
#
# Examples:
#   sdrpp_link_dep(hackrf_source libhackrf)
#   sdrpp_link_dep(rtl_sdr_source librtlsdr PACKAGE_NAME rtlsdr STATIC_TARGETS rtlsdr::rtlsdr_static)
#   sdrpp_link_dep(plutosdr_source libusb PC_NAME libusb-1.0)
#

if (COMMAND sdrpp_link_dep)
    return()
endif ()

get_filename_component(_sdrpp_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if (EXISTS "${_sdrpp_repo_root}/deps/cmake/DepPolicy.cmake")
    include("${_sdrpp_repo_root}/deps/cmake/DepPolicy.cmake")
endif ()
if (EXISTS "${_sdrpp_repo_root}/deps/cmake/DepClassification.cmake")
    include("${_sdrpp_repo_root}/deps/cmake/DepClassification.cmake")
endif ()

function(_sdrpp_link_dep_resolve_targets pkg out_var)
    cmake_parse_arguments(P "" "" "TARGETS;SHARED_TARGETS;STATIC_TARGETS" ${ARGN})

    set(_targets ${P_TARGETS})
    if (NOT _targets)
        set(_targets ${pkg}::${pkg} ${pkg})
    endif ()

    set(_preferred_targets ${_targets})
    if (COMMAND sdrpp_dep_builds_shared)
        sdrpp_dep_builds_shared(${pkg} _dep_builds_shared)
        if (_dep_builds_shared AND P_SHARED_TARGETS)
            set(_preferred_targets ${P_SHARED_TARGETS} ${_targets} ${P_STATIC_TARGETS})
        elseif (NOT _dep_builds_shared AND P_STATIC_TARGETS)
            set(_preferred_targets ${P_STATIC_TARGETS} ${_targets} ${P_SHARED_TARGETS})
        endif ()
    endif ()

    list(REMOVE_DUPLICATES _preferred_targets)
    set(${out_var} ${_preferred_targets} PARENT_SCOPE)
endfunction()

function(sdrpp_link_dep target pkg)
    cmake_parse_arguments(P "PUBLIC;INTERFACE" "PACKAGE_NAME;PC_NAME" "TARGETS;SHARED_TARGETS;STATIC_TARGETS" ${ARGN})

    set(_scope PRIVATE)
    if (P_PUBLIC)
        set(_scope PUBLIC)
    elseif (P_INTERFACE)
        set(_scope INTERFACE)
    endif ()

    # pkg-config module names are conventionally lowercase (rtaudio.pc,
    # volk.pc, fftw3f.pc, libcurl.pc, ...), so default PC_NAME to the
    # lowercase form of the caller's pkg arg — that way callers passing the
    # upstream CamelCase package name (RtAudio, Volk, FFTW3f) don't also
    # have to write `PC_NAME rtaudio` / `PC_NAME volk` / etc. Callers
    # override PC_NAME explicitly when the .pc name diverges from a simple
    # lowercase (libusb -> libusb-1.0, codec2 -> codec2, ...).
    set(_pc ${P_PC_NAME})
    if (NOT _pc)
        string(TOLOWER "${pkg}" _pc)
    endif ()

    set(_package ${P_PACKAGE_NAME})
    if (NOT _package)
        set(_package ${pkg})
    endif ()

    _sdrpp_link_dep_resolve_targets(${pkg} _targets
        TARGETS ${P_TARGETS}
        SHARED_TARGETS ${P_SHARED_TARGETS}
        STATIC_TARGETS ${P_STATIC_TARGETS})

    set(_prefix_hints ${CMAKE_PREFIX_PATH})

    # System-sourced deps (e.g. libcurl on distro profiles) intentionally live
    # outside the deps install prefix — they come from the host packages. The
    # strict step-1 gate below must not fire for them; fall through to the
    # system Config.cmake / pkg-config branches instead.
    set(_is_system FALSE)
    if (COMMAND sdrpp_dep_is_system)
        sdrpp_dep_is_system(${pkg} _is_system)
    endif ()

    # The strict FATAL gate only makes sense for deps we own a recipe for.
    # Callers may pass an upstream package name (FFTW3f, Volk, RtAudio, UHD,
    # SoapySDR, ...) that doesn't match any deps/+<name>/ recipe — in that
    # case the prefix has no Config.cmake we promised to install, so a miss
    # here is not the silent-bundled-build-failure the gate is supposed to
    # catch. We still try the prefix first (some unregistered upstream packages
    # land in the prefix as a side effect of a registered recipe — e.g. fftw3
    # installing FFTW3fConfig.cmake), we just don't fatally error if missing.
    set(_dep_registered FALSE)
    get_property(_registered_packages GLOBAL PROPERTY SDRPP_DEP_REGISTERED_PACKAGES)
    if ("${pkg}" IN_LIST _registered_packages)
        set(_dep_registered TRUE)
    endif ()

    # Step 1 - require Config.cmake from the deps / SDR kit prefixes when
    # explicit prefixes were provided. Falling through would let host system
    # packages or pkg-config mask a broken bundled dependency install.
    #
    # NO_CMAKE_FIND_ROOT_PATH bypasses cross-toolchain root-path filtering
    # (Android NDK sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY, which would
    # otherwise reject our explicit PATHS for living outside the NDK sysroot).
    # Imported configs can perform their own transitive find_package/find_*
    # calls, however, and those do not inherit NO_CMAKE_FIND_ROOT_PATH.  Add
    # the explicit prefixes as Android roots in this function scope so such
    # calls (curl 8.22 -> Mbed TLS, for example) can resolve the same install.
    if (ANDROID AND _prefix_hints)
        list(REMOVE_ITEM CMAKE_FIND_ROOT_PATH ${_prefix_hints})
        list(PREPEND CMAKE_FIND_ROOT_PATH ${_prefix_hints})
    endif ()
    if (_prefix_hints AND NOT _is_system)
        find_package(${_package} CONFIG QUIET PATHS ${_prefix_hints} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    endif ()
    foreach (t ${_targets})
        if (TARGET ${t})
            target_link_libraries(${target} ${_scope} ${t})
            return()
        endif ()
    endforeach ()
    if (_prefix_hints AND NOT _is_system AND _dep_registered)
        # Diagnostic context for debugging cross-toolchain / path-mode failures:
        # what we asked for, where we looked, and what's actually on disk.
        string(REPLACE ";" "\n    " _hints_pretty "${_prefix_hints}")
        set(_diag "")
        foreach (_hint IN LISTS _prefix_hints)
            string(APPEND _diag "    ${_hint}:\n")
            file(GLOB _cfg_files
                "${_hint}/lib/cmake/${_package}/*Config.cmake"
                "${_hint}/lib/cmake/${_package}/*-config.cmake"
                "${_hint}/lib/cmake/${_package}/*config.cmake")
            string(TOLOWER "${_package}" _package_lower)
            file(GLOB _cfg_files_lower
                "${_hint}/lib/cmake/${_package_lower}/*Config.cmake"
                "${_hint}/lib/cmake/${_package_lower}/*-config.cmake"
                "${_hint}/lib/cmake/${_package_lower}/*config.cmake")
            list(APPEND _cfg_files ${_cfg_files_lower})
            list(REMOVE_DUPLICATES _cfg_files)
            if (_cfg_files)
                foreach (_f IN LISTS _cfg_files)
                    string(APPEND _diag "      found: ${_f}\n")
                endforeach ()
            else ()
                string(APPEND _diag "      no Config.cmake under lib/cmake/${_package} or lib/cmake/${_package_lower}\n")
            endif ()
        endforeach ()
        message(FATAL_ERROR
            "sdrpp_link_dep(${target} ${pkg}) could not resolve ${pkg}: "
            "find_package(${_package} CONFIG) found no target matching [${_targets}] in CMAKE_PREFIX_PATH. "
            "Explicit dependency prefixes must provide CMake config targets; pkg-config fallback is disabled for those prefixes.\n"
            "Searched these prefix hints:\n"
            "${_diag}"
            "CMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}\n"
            "CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=${CMAKE_FIND_ROOT_PATH_MODE_PACKAGE}")
    endif ()

    # Step 2 - system Config.cmake on Unix desktop builds.
    if (WIN32 OR ANDROID)
        message(FATAL_ERROR
            "sdrpp_link_dep(${target} ${pkg}) could not resolve ${pkg}: "
            "find_package(${_package} CONFIG) found no target matching [${_targets}] in CMAKE_PREFIX_PATH. "
            "Build the deps preset for this platform and point CMAKE_PREFIX_PATH at its install prefix.")
    endif ()

    find_package(${_package} CONFIG QUIET)
    foreach (t ${_targets})
        if (TARGET ${t})
            target_link_libraries(${target} ${_scope} ${t})
            return()
        endif ()
    endforeach ()

    # Step 3 - pkg-config on Unix desktop builds. Reached when no explicit
    # prefixes were provided, or the dep is system-sourced (the deps prefix
    # intentionally doesn't carry it).
    find_package(PkgConfig QUIET)
    if (NOT PkgConfig_FOUND)
        message(FATAL_ERROR "sdrpp_link_dep(${target} ${pkg}) could not resolve ${pkg}: find_package(${_package} CONFIG) found no target matching [${_targets}] and pkg-config is unavailable.")
    endif ()

    string(MAKE_C_IDENTIFIER "${pkg}" _pkg_id)
    set(_pkg_module "_${_pkg_id}_PC")
    pkg_check_modules(${_pkg_module} QUIET IMPORTED_TARGET ${_pc})

    if (NOT ${_pkg_module}_FOUND)
        message(FATAL_ERROR "sdrpp_link_dep(${target} ${pkg}) could not resolve ${pkg}: find_package(${_package} CONFIG) found no target matching [${_targets}] and pkg-config module '${_pc}' was not found.")
    endif ()

    target_link_libraries(${target} ${_scope} PkgConfig::${_pkg_module})
endfunction()
