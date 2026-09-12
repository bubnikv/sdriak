cmake_minimum_required(VERSION 3.16...3.24)
project(sdrpp_core)

if (USE_INTERNAL_LIBCORRECT)
    add_subdirectory("libcorrect/")
endif ()

# Main code
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

if (MSVC)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# Configure backend sources
if (OPT_BACKEND_GLFW)
    file(GLOB_RECURSE BACKEND_SRC "backends/glfw/*.cpp" "backends/glfw/*.c")
endif ()
if (OPT_BACKEND_ANDROID)
    file(GLOB_RECURSE BACKEND_SRC "backends/android/*.cpp" "backends/android/*.c")
    set(BACKEND_SRC ${BACKEND_SRC} ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
endif ()

# Add code to dyn lib
if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
endif ()

add_library(sdrpp_core SHARED ${SRC} ${BACKEND_SRC})
if (ANDROID OR OPT_BACKEND_ANDROID)
    set(SDRPP_CONFIG_MULTIPLE_INSTANCES 0)
    message(STATUS "Multiple-instance config synchronization: disabled")
else ()
    set(SDRPP_CONFIG_MULTIPLE_INSTANCES 1)
    message(STATUS "Multiple-instance config synchronization: enabled")
endif ()
target_compile_definitions(sdrpp_core PRIVATE
    OPT_CONFIG_MULTIPLE_INSTANCES=${SDRPP_CONFIG_MULTIPLE_INSTANCES})
if (MSVC AND SDRPP_RUNTIME_OUTPUT_DIRECTORY)
    add_custom_command(TARGET sdrpp_core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove -f
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/$<TARGET_FILE_NAME:sdrpp_core>"
        VERBATIM)
endif ()

if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(config_manager_tests tests/config_manager_tests.cpp)
    target_link_libraries(config_manager_tests PRIVATE sdrpp_core)
    target_compile_definitions(config_manager_tests PRIVATE
        OPT_CONFIG_MULTIPLE_INSTANCES=${SDRPP_CONFIG_MULTIPLE_INSTANCES})
    add_test(NAME config_manager_tests COMMAND config_manager_tests)

    add_executable(band_register_set_tests tests/band_register_set_tests.cpp)
    target_link_libraries(band_register_set_tests PRIVATE sdrpp_core)
    add_test(NAME band_register_set_tests COMMAND band_register_set_tests)

    add_executable(band_picker_model_tests tests/band_picker_model_tests.cpp)
    target_link_libraries(band_picker_model_tests PRIVATE sdrpp_core)
    add_test(NAME band_picker_model_tests COMMAND band_picker_model_tests)

    add_executable(bandplan_load_tests
        tests/bandplan_load_tests.cpp
        src/gui/widgets/bandplan.cpp)
    target_link_libraries(bandplan_load_tests PRIVATE sdrpp_core)
    add_test(NAME bandplan_load_tests COMMAND bandplan_load_tests)

    # colormaps and the theme manager reach their state through exported
    # symbols, so these link against the library instead of recompiling it.
    add_executable(colormap_load_tests tests/colormap_load_tests.cpp)
    target_link_libraries(colormap_load_tests PRIVATE sdrpp_core)
    add_test(NAME colormap_load_tests COMMAND colormap_load_tests)

    add_executable(theme_load_tests tests/theme_load_tests.cpp)
    target_link_libraries(theme_load_tests PRIVATE sdrpp_core)
    add_test(NAME theme_load_tests COMMAND theme_load_tests)
endif ()

target_compile_definitions(sdrpp_core PRIVATE
    SDRPP_IS_CORE
    "FLOG_ANDROID_TAG=\"SDRIAK\"")
if (USE_BUNDLE_DEFAULTS)
    target_compile_definitions(sdrpp_core PRIVATE IS_MACOS_BUNDLE)
endif ()
if (MSVC)
    target_compile_definitions(sdrpp_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif ()

# Set the install prefix
target_compile_definitions(sdrpp_core PUBLIC INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}")

# AppImage build: enable runtime code paths that resolve resources via $APPDIR.
if (OPT_BUILD_APPIMAGE)
    target_compile_definitions(sdrpp_core PUBLIC BUILD_APPIMAGE)
endif ()

# Include core headers
target_include_directories(sdrpp_core PUBLIC "src/")
target_include_directories(sdrpp_core PUBLIC "src/imgui")

# Configure backend includes and libraries
if (OPT_BACKEND_GLFW)
    target_include_directories(sdrpp_core PUBLIC "backends/glfw")
    target_include_directories(sdrpp_core PUBLIC "backends/glfw/imgui")
endif ()
if (OPT_BACKEND_ANDROID)
    target_include_directories(sdrpp_core PUBLIC "backends/android")
    target_include_directories(sdrpp_core PUBLIC "backends/android/imgui")
endif ()

# Link to libcorrect
if (USE_INTERNAL_LIBCORRECT)
    target_include_directories(sdrpp_core PUBLIC "libcorrect/include")
    target_link_libraries(sdrpp_core PUBLIC correct_static)
endif (USE_INTERNAL_LIBCORRECT)

# libcurl is a core implementation detail. Modules use core networking
# wrappers (utils/curl_http, etc.) instead of including curl headers or
# linking libcurl directly. Resolved from the deps install prefix (bundled
# profile) or the distro libcurl on Linux.
sdrpp_link_dep(sdrpp_core libcurl PRIVATE
    PACKAGE_NAME CURL
    PC_NAME      libcurl
    TARGETS      CURL::libcurl)

# WebSocket support is mandatory. The bundled recipe always forces
# CURL_DISABLE_WEBSOCKETS=OFF; distro libcurl may have been packaged without
# it (Ubuntu 20.04 / 22.04 jammy ship 7.68 / 7.81, no WS API in the
# header at all), so probe explicitly when the imported target is in
# module form.
#
# This is a compile-only probe (CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY)
# rather than a link probe, because:
#   1. check_symbol_exists's generated try_compile sub-project can't see
#      IMPORTED targets from the parent (CURL::libcurl + its transitive
#      OpenSSL::SSL / ZLIB::ZLIB targets), so a link-based probe would
#      have to recursively resolve every IMPORTED target to a literal
#      library file — fragile and version-sensitive.
#   2. The signal we actually want is whether curl_ws_send is *declared*
#      in the headers. Upstream only adds the declaration when WS is
#      configured in; a host whose curl headers declare it but whose lib
#      strips the implementation is theoretical (would break at app link
#      with a clear undefined-reference error, not silently at runtime).
if (TARGET CURL::libcurl)
    include(CheckSymbolExists)
    get_target_property(_sdrpp_curl_inc CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
    if (_sdrpp_curl_inc)
        set(CMAKE_REQUIRED_INCLUDES "${_sdrpp_curl_inc}")
    endif ()
    set(_sdrpp_saved_try_compile_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_symbol_exists(curl_ws_send "curl/curl.h" SDRPP_CURL_HAS_WS)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_sdrpp_saved_try_compile_type}")
    unset(CMAKE_REQUIRED_INCLUDES)
    if (NOT SDRPP_CURL_HAS_WS)
        message(FATAL_ERROR
            "libcurl was built without WebSocket support. Install a libcurl "
            "with --enable-websockets, or rebuild deps with "
            "SDRPP_DEP_FORCE_BUNDLED=libcurl.")
    endif ()
endif ()

if (OPT_OVERRIDE_STD_FILESYSTEM)
    target_include_directories(sdrpp_core PUBLIC "std_replacement")
endif ()

if (OPT_BACKEND_GLFW)
    sdrpp_link_dep(sdrpp_core glfw3 PUBLIC PC_NAME glfw3 TARGETS glfw3::glfw glfw)
endif ()

sdrpp_link_dep(sdrpp_core FFTW3f PUBLIC PC_NAME fftw3f TARGETS FFTW3::fftw3f)
sdrpp_link_dep(sdrpp_core zstd PUBLIC PC_NAME libzstd
    TARGETS zstd::libzstd_shared zstd::libzstd zstd zstd::libzstd_static
    SHARED_TARGETS zstd::libzstd_shared zstd::libzstd zstd
    STATIC_TARGETS zstd::libzstd_static)
sdrpp_link_dep(sdrpp_core Volk PUBLIC PC_NAME volk TARGETS Volk::volk volk::volk volk)

# libFLAC is a core implementation detail of the utils/wav writer (recorder
# FLAC container). Modules consume wav::Writer, not FLAC itself — keep the
# encoder headers out of wav.h so PRIVATE linkage holds.
sdrpp_link_dep(sdrpp_core flac PRIVATE
    PACKAGE_NAME FLAC
    PC_NAME      flac
    TARGETS      FLAC::FLAC)

# libopus + libogg back the utils/wav writer's Opus audio recorder (Ogg-Opus
# container). Same as FLAC: implementation detail of wav::Writer, headers kept
# out of wav.h, linked PRIVATE.
sdrpp_link_dep(sdrpp_core opus PRIVATE
    PACKAGE_NAME Opus
    PC_NAME      opus
    TARGETS      Opus::opus)
sdrpp_link_dep(sdrpp_core libogg PRIVATE
    PACKAGE_NAME Ogg
    PC_NAME      ogg
    TARGETS      Ogg::ogg)

if (NOT ANDROID)
    find_package(OpenGL REQUIRED)
    target_link_libraries(sdrpp_core PUBLIC OpenGL::GL)
endif ()

if (MSVC)
    # WinSock2
    target_link_libraries(sdrpp_core PUBLIC wsock32 ws2_32 iphlpapi advapi32)
elseif (ANDROID)
    target_include_directories(sdrpp_core PUBLIC
        ${ANDROID_NDK}/sources/android/native_app_glue
    )

    target_link_libraries(sdrpp_core PUBLIC
        android
        EGL
        GLESv3
        log
    )
else()
    if (NOT USE_INTERNAL_LIBCORRECT)
        find_package(PkgConfig REQUIRED)
        # GLOBAL: the imported target sits in sdrpp_core's PUBLIC link
        # interface, so plugin targets in sibling directories must see it too.
        pkg_check_modules(CORRECT REQUIRED IMPORTED_TARGET GLOBAL libcorrect)
        target_link_libraries(sdrpp_core PUBLIC PkgConfig::CORRECT)
    endif (NOT USE_INTERNAL_LIBCORRECT)

    if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        target_link_libraries(sdrpp_core PUBLIC stdc++fs)
    endif ()

endif ()

# Install directives
set_target_properties(sdrpp_core PROPERTIES OUTPUT_NAME "sdriak_core")
install(TARGETS sdrpp_core DESTINATION lib)
