cmake_minimum_required(VERSION 3.16...3.24)
project(sdrpp)

# Backends
option(OPT_BACKEND_GLFW "Use the GLFW backend" ON)
option(OPT_BACKEND_ANDROID "Use the Android backend" OFF)
option(OPT_BUILD_TESTS "Build SDRIAK tests" OFF)
if (OPT_BUILD_TESTS)
    enable_testing()
endif ()

# Dependency build management
# When OPT_BUILD_DEPS=ON, deps/ is configured and built as a sub-project before
# the main configure continues, and its install prefix is added to
# CMAKE_PREFIX_PATH. Default OFF — normal flow is to build deps once
# (cmake -S deps --preset vs2026-x64-release && cmake --build deps/build-vs2026-x64-Release --config Release)
# and point the app build at the install prefix via CMAKE_PREFIX_PATH.
option(OPT_BUILD_DEPS "Build third-party dependencies before configuring SDRIAK" OFF)
option(SDRPP_DEPS_OUTPUT_QUIET "Suppress output of the dependency build" OFF)
set(SDRPP_DEPS_PRESET "default" CACHE STRING "Preset name to use for the dependency build")
set(SDRPP_DEPS_BUILD_DIR "" CACHE PATH "Override the binary directory of the dependency build")
set(SDRPP_DEPS_BUILD_CONFIG "" CACHE STRING "Build configuration to use when bootstrapping deps with a multi-config generator")
set(SDRPP_DEP_ROOT_PACKAGES "" CACHE STRING "Override the top-level dependency recipes propagated to deps when OPT_BUILD_DEPS=ON")

# Compatibility Options
option(OPT_OVERRIDE_STD_FILESYSTEM "Use a local version of std::filesystem on systems that don't have it yet" OFF)
option(OPT_BUILD_APPIMAGE "Build AppImage-specific runtime code paths (Linux only - overrides resource paths via $APPDIR at launch)" OFF)

# Sources   
option(OPT_BUILD_AIRSPY_SOURCE "Build Airspy Source Module (Dependencies: libairspy)" ON)
option(OPT_BUILD_AIRSPYHF_SOURCE "Build Airspy HF+ Source Module (Dependencies: libairspyhf)" ON)
option(OPT_BUILD_AUDIO_SOURCE "Build Audio Source Module (Dependencies: rtaudio)" ON)
option(OPT_BUILD_BADGESDR_SOURCE "Build BadgeSDR Source Module (Dependencies: libusb)" OFF)
option(OPT_BUILD_BLADERF_SOURCE "Build BladeRF Source Module (Dependencies: libbladeRF)" OFF)
option(OPT_BUILD_DRAGONLABS_SOURCE "Build Dragon Labs Source Module (Dependencies: libdlcr)" OFF)
option(OPT_BUILD_FILE_SOURCE "Wav file source" ON)
option(OPT_BUILD_FOBOSSDR_SOURCE "Build FobosSDR Source Module (Dependencies: libfobos)" OFF)
option(OPT_BUILD_HACKRF_SOURCE "Build HackRF Source Module (Dependencies: libhackrf)" ON)
option(OPT_BUILD_HAROGIC_SOURCE "Build Harogic Source Module (Dependencies: htra_api)" OFF)
option(OPT_BUILD_HERMES_SOURCE "Build Hermes Source Module (no dependencies required)" ON)
option(OPT_BUILD_HYDRASDR_RFONE_SOURCE "Build HydraSDR RFOne Source Module (Dependencies: libhydrasdr)" ON)
option(OPT_BUILD_KCSDR_SOURCE "Build KCSDR Source Module (Dependencies: libkcsdr)" OFF)
option(OPT_BUILD_KIWISDR_SOURCE "Build KiwiSDR WebSDR Source Module (no dependencies required)" ON)
option(OPT_BUILD_LIMESDR_SOURCE "Build LimeSDR Source Module (Dependencies: liblimesuite)" OFF)
option(OPT_BUILD_NETWORK_SOURCE "Build Network Source Module (no dependencies required)" ON)
option(OPT_BUILD_PERSEUS_SOURCE "Build Perseus Source Module (Dependencies: libperseus-sdr)" OFF)
option(OPT_BUILD_PLUTOSDR_SOURCE "Build PlutoSDR Source Module (Dependencies: libiio, libad9361)" ON)
option(OPT_BUILD_QMX_SOURCE "Build direct QMX USB Source Module (Dependencies: rtaudio or aaudio)" ON)
option(OPT_BUILD_QMXSERVER_SOURCE "Build QMX server Source Module (Dependencies: enet)" ON)
option(OPT_BUILD_RFNM_SOURCE "Build RFNM Source Module (Dependencies: librfnm)" OFF)
option(OPT_BUILD_RFSPACE_SOURCE "Build RFspace Source Module (no dependencies required)" ON)
option(OPT_BUILD_RTL_SDR_SOURCE "Build RTL-SDR Source Module (Dependencies: librtlsdr)" ON)
option(OPT_BUILD_RTL_TCP_SOURCE "Build RTL-TCP Source Module (no dependencies required)" ON)
option(OPT_BUILD_SDDC_SOURCE "Build SDDC Source Module (Dependencies: libusb)" OFF)
option(OPT_BUILD_SDRIAK_SERVER_SOURCE "Build SDRIAK Server Source Module (no dependencies required)" ON)
option(OPT_BUILD_SDRPLAY_SOURCE "Build SDRplay Source Module (Dependencies: libsdrplay)" OFF)
option(OPT_BUILD_SOAPY_SOURCE "Build SoapySDR Source Module (Dependencies: soapysdr)" OFF)
option(OPT_BUILD_SPECTRAN_SOURCE "Build Spectran Source Module (Dependencies: Aaronia RTSA Suite)" OFF)
option(OPT_BUILD_SPECTRAN_HTTP_SOURCE "Build Spectran HTTP Source Module (no dependencies required)" ON)
option(OPT_BUILD_SPYSERVER_SOURCE "Build SpyServer Source Module (no dependencies required)" ON)
option(OPT_BUILD_USRP_SOURCE "Build USRP Source Module (libuhd)" OFF)

# Sinks
option(OPT_BUILD_ANDROID_AUDIO_SINK "Build Android Audio Sink Module (Dependencies: AAudio, only for android)" OFF)
option(OPT_BUILD_AUDIO_SINK "Build Audio Sink Module (Dependencies: rtaudio)" ON)
# Native CoreAudio output needs no third-party audio library: default it on
# where it can build, force it off everywhere else.
include(CMakeDependentOption)
cmake_dependent_option(OPT_BUILD_MACOS_COREAUDIO_SINK "Build the native CoreAudio Sink Module (no dependencies required, only for macOS)" ON "APPLE" OFF)
option(OPT_BUILD_NETWORK_SINK "Build Audio Network Sink Module (no dependencies required)" ON)
option(OPT_BUILD_NEW_PORTAUDIO_SINK "Build the new PortAudio Sink Module (Dependencies: portaudio)" OFF)
option(OPT_BUILD_PORTAUDIO_SINK "Build PortAudio Sink Module (Dependencies: portaudio)" OFF)

# Decoders
option(OPT_BUILD_ATV_DECODER "Build ATV decoder (no dependencies required)" ON)
option(OPT_BUILD_CW_ENGINE "Build the CW skimmer engine library the CW decoder modules link (no dependencies required)" ON)
cmake_dependent_option(OPT_BUILD_CW_DEEPCW "Build the CW engine's DeepCW neural decoders, v1 and v2 (ONNX Runtime, loaded at run time; the models are not bundled)" OFF "OPT_BUILD_CW_ENGINE" OFF)
option(OPT_BUILD_DAB_DECODER "Build the DAB/DAB+ decoder (no dependencies required)" OFF)
option(OPT_BUILD_FALCON9_DECODER "Build the falcon9 live decoder (Dependencies: ffplay)" OFF)
option(OPT_BUILD_KG_SSTV_DECODER "Build the KG SSTV (KG-STV) decoder module (no dependencies required)" OFF)
option(OPT_BUILD_M17_DECODER "Build the M17 decoder module (Dependencies: codec2)" OFF)
option(OPT_BUILD_METEOR_DEMODULATOR "Build the meteor demodulator module (no dependencies required)" ON)
option(OPT_BUILD_PAGER_DECODER "Build the pager decoder module (no dependencies required)" ON)
option(OPT_BUILD_RADIO "Main audio modulation decoder (AM, FM, SSB, etc...)" ON)
option(OPT_BUILD_RADIOSONDE_DECODER "Build the radiosonde decoder module (no dependencies required)" ON)
option(OPT_BUILD_RYFI_DECODER "RyFi data link decoder" OFF)
option(OPT_BUILD_VOR_RECEIVER "VOR beacon receiver" OFF)
option(OPT_BUILD_WEATHER_SAT_DECODER "Build the HRPT decoder module (no dependencies required)" OFF)

# Misc
option(OPT_BUILD_DISCORD_PRESENCE "Build the Discord Rich Presence module" ON)
option(OPT_BUILD_FREQUENCY_MANAGER "Build the Frequency Manager module" ON)
option(OPT_BUILD_IQ_EXPORTER "Build the IQ Exporter module" ON)
option(OPT_BUILD_NOISE_REDUCTION_LOGMMSE "Build the LogMMSE/OMLSA noise reduction module" ON)
option(OPT_BUILD_RECORDER "Audio and baseband recorder" ON)
option(OPT_BUILD_RIGCTL_CLIENT "Rigctl client to make SDRIAK act as a panadapter" ON)
option(OPT_BUILD_RIGCTL_SERVER "Rigctl backend for controlling SDRIAK with software like gpredict" ON)
option(OPT_BUILD_SCANNER "Frequency scanner" ON)
option(OPT_BUILD_SCHEDULER "Build the scheduler" OFF)
option(OPT_BUILD_SPOTS "Build Spots module" ON)
option(OPT_BUILD_WEBSDR_VIEW "Build WebSDR View module (no dependencies required)" ON)

# Other options
option(USE_INTERNAL_LIBCORRECT "Use an internal version of libcorrect" ON)
option(USE_BUNDLE_DEFAULTS "Set the default resource and module directories to the right ones for a MacOS .app" OFF)
option(COPY_MSVC_REDISTRIBUTABLES "Copy over the Visual C++ Redistributable" OFF)

# SDRplay distributes its API as a closed-source binary. Available variants:
#   - Linux x86_64 / Windows x86 / Windows x64: shipped per-arch and supported.
#   - macOS: shipped as a universal .pkg (x86_64 + arm64 fat binary) — both
#     Apple Silicon and Intel resolve through deps/+sdrplay/install_macos.cmake.
#   - Linux ARM / Windows ARM64 / Android: no usable binary, auto-disable.
# The recipe and plugin both gate on OPT_BUILD_SDRPLAY_SOURCE, so flipping it
# OFF here centralizes the policy for every entry point (CI scripts, dev
# builds, IDEs).
if (OPT_BUILD_SDRPLAY_SOURCE)
    set(_sdrplay_unsupported "")
    if (ANDROID)
        set(_sdrplay_unsupported "Android")
    elseif (WIN32 AND (CMAKE_GENERATOR_PLATFORM MATCHES "^ARM64"
                       OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)"))
        set(_sdrplay_unsupported "Windows on ARM64")
    elseif (UNIX AND NOT APPLE AND NOT ANDROID
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv6|armv7)")
        set(_sdrplay_unsupported "Linux ${CMAKE_SYSTEM_PROCESSOR}")
    endif ()
    if (_sdrplay_unsupported)
        message(STATUS "SDRplay: no upstream binary available for ${_sdrplay_unsupported}; disabling OPT_BUILD_SDRPLAY_SOURCE")
        set(OPT_BUILD_SDRPLAY_SOURCE OFF CACHE BOOL
            "Build SDRplay Source Module (Dependencies: libsdrplay)" FORCE)
    endif ()
endif ()

include(${CMAKE_SOURCE_DIR}/cmake/sdrpp_collect_required_deps.cmake)
sdrpp_collect_required_dep_roots(_sdrpp_dep_root_packages_auto)
if (SDRPP_DEP_ROOT_PACKAGES)
    sdrpp_normalize_dep_list(_sdrpp_dep_root_packages_effective "${SDRPP_DEP_ROOT_PACKAGES}")
    message(STATUS "Effective dependency roots (override): ${_sdrpp_dep_root_packages_effective}")
else ()
    set(_sdrpp_dep_root_packages_effective ${_sdrpp_dep_root_packages_auto})
    message(STATUS "Effective dependency roots (auto): ${_sdrpp_dep_root_packages_effective}")
endif ()

if (OPT_BUILD_DEPS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/deps/autobuild.cmake)
endif ()

# Module cmake path
set(SDRPP_MODULE_CMAKE "${CMAKE_SOURCE_DIR}/sdrpp_module.cmake")

# Build-tree runtime layout: the application and core runtime library live
# together, while SDRIAK plugins keep their existing per-module output layout.
if (MSVC)
    set(SDRPP_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
elseif (NOT ANDROID)
    if (CMAKE_CONFIGURATION_TYPES)
        set(SDRPP_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
    else ()
        set(SDRPP_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif ()
endif ()
if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
    set(SDRPP_MODULE_RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}/modules")
endif ()

# Root source folder
set(SDRPP_CORE_ROOT "${CMAKE_SOURCE_DIR}/core/src/")

# Language standards
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Set a default install prefix
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "..." FORCE)
    else()
        set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "..." FORCE)
    endif()
endif()

# Configure toolchain for android
if (ANDROID)
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate"
    )
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
endif ()

# Provide sdrpp_link_dep() helper to all module subdirectories. Try Config.cmake
# first, fall back to pkg-config — keeps Linux apt/dpkg flow working while
# letting Windows resolve via our deps install prefix.
include(${CMAKE_SOURCE_DIR}/cmake/sdrpp_find_dep.cmake)

# Debug menu: off by default. Compiled in for
#   1) CI nightly builds — any GitHub Actions build that is not a release-tag
#      build (GITHUB_REF_TYPE is "tag" only when a pushed vX.Y.Z tag is being
#      built; the docker-based Linux/AppImage workflows forward the two
#      GITHUB_* variables into the container), and
#   2) Debug configurations — per-config generator expression, so
#      multi-config generators (MSVC) get it in Debug builds only.
# All other builds compile the menu out along with the ImGui demo window it
# references (IMGUI_DISABLE_DEMO_WINDOWS turns imgui_demo.cpp into empty
# stubs; the symbols remain, keeping the generated exports.def valid).
# Applied before any add_subdirectory so every in-tree target agrees.
option(OPT_DEBUG_MENU "Compile the debug menu into all configurations" OFF)
set(_sdrpp_debug_menu ${OPT_DEBUG_MENU})
if (NOT _sdrpp_debug_menu AND DEFINED ENV{GITHUB_ACTIONS} AND NOT "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    set(_sdrpp_debug_menu ON)
endif ()
if (_sdrpp_debug_menu)
    message(STATUS "Debug menu: enabled in all configurations (OPT_DEBUG_MENU or CI nightly)")
    add_compile_definitions(SDRPP_ENABLE_DEBUG_MENU)
else ()
    message(STATUS "Debug menu: Debug configuration only")
    add_compile_definitions(
        $<$<CONFIG:Debug>:SDRPP_ENABLE_DEBUG_MENU>
        $<$<NOT:$<CONFIG:Debug>>:IMGUI_DISABLE_DEMO_WINDOWS>)
endif ()

# sdrpp_stage_runtime_dlls(): MSVC tests and tools stage their DLLs beside
# themselves, as sdriak.exe does below.
include(${CMAKE_SOURCE_DIR}/cmake/sdrpp_stage_runtime_dlls.cmake)

# Core of SDRIAK
add_subdirectory("core")

function(sdrpp_add_optional_modules)
    foreach (_entry IN LISTS ARGN)
        string(REPLACE ":" ";" _parts "${_entry}")
        list(LENGTH _parts _parts_len)
        if (NOT _parts_len EQUAL 2)
            message(FATAL_ERROR "Invalid module entry '${_entry}', expected NAME:relative/path")
        endif ()

        list(GET _parts 0 _opt)
        list(GET _parts 1 _dir)
        if (OPT_BUILD_${_opt})
            add_subdirectory("${_dir}")
        endif ()
    endforeach ()
endfunction()

# Source modules
sdrpp_add_optional_modules(
    AIRSPY_SOURCE:source_modules/airspy_source
    AIRSPYHF_SOURCE:source_modules/airspyhf_source
    AUDIO_SOURCE:source_modules/audio_source
    BADGESDR_SOURCE:source_modules/badgesdr_source
    BLADERF_SOURCE:source_modules/bladerf_source
    DRAGONLABS_SOURCE:source_modules/dragonlabs_source
    FILE_SOURCE:source_modules/file_source
    FOBOSSDR_SOURCE:source_modules/fobossdr_source
    HACKRF_SOURCE:source_modules/hackrf_source
    HAROGIC_SOURCE:source_modules/harogic_source
    HERMES_SOURCE:source_modules/hermes_source
    HYDRASDR_RFONE_SOURCE:source_modules/hydrasdr_source
    KCSDR_SOURCE:source_modules/kcsdr_source
    KIWISDR_SOURCE:source_modules/kiwisdr_source
    LIMESDR_SOURCE:source_modules/limesdr_source
    NETWORK_SOURCE:source_modules/network_source
    PERSEUS_SOURCE:source_modules/perseus_source
    PLUTOSDR_SOURCE:source_modules/plutosdr_source
    QMX_SOURCE:source_modules/qmx_source
    QMXSERVER_SOURCE:source_modules/qmxserver_source
    RFNM_SOURCE:source_modules/rfnm_source
    RFSPACE_SOURCE:source_modules/rfspace_source
    RTL_SDR_SOURCE:source_modules/rtl_sdr_source
    RTL_TCP_SOURCE:source_modules/rtl_tcp_source
    SDDC_SOURCE:source_modules/sddc_source
    SDRIAK_SERVER_SOURCE:source_modules/sdriak_server_source
    SDRPLAY_SOURCE:source_modules/sdrplay_source
    SOAPY_SOURCE:source_modules/soapy_source
    SPECTRAN_SOURCE:source_modules/spectran_source
    SPECTRAN_HTTP_SOURCE:source_modules/spectran_http_source
    SPYSERVER_SOURCE:source_modules/spyserver_source
    USRP_SOURCE:source_modules/usrp_source
)

# Sink modules
sdrpp_add_optional_modules(
    ANDROID_AUDIO_SINK:sink_modules/android_audio_sink
    AUDIO_SINK:sink_modules/audio_sink
    MACOS_COREAUDIO_SINK:sink_modules/macos_coreaudio_sink
    PORTAUDIO_SINK:sink_modules/portaudio_sink
    NETWORK_SINK:sink_modules/network_sink
    NEW_PORTAUDIO_SINK:sink_modules/new_portaudio_sink
)

# Decoders
sdrpp_add_optional_modules(
    ATV_DECODER:decoder_modules/atv_decoder
    CW_ENGINE:decoder_modules/cw_engine
    DAB_DECODER:decoder_modules/dab_decoder
    FALCON9_DECODER:decoder_modules/falcon9_decoder
    KG_SSTV_DECODER:decoder_modules/kg_sstv_decoder
    M17_DECODER:decoder_modules/m17_decoder
    METEOR_DEMODULATOR:decoder_modules/meteor_demodulator
    PAGER_DECODER:decoder_modules/pager_decoder
    RADIO:decoder_modules/radio
    RADIOSONDE_DECODER:decoder_modules/radiosonde_decoder
    RYFI_DECODER:decoder_modules/ryfi_decoder
    VOR_RECEIVER:decoder_modules/vor_receiver
    WEATHER_SAT_DECODER:decoder_modules/weather_sat_decoder
)

# Misc
sdrpp_add_optional_modules(
    DISCORD_PRESENCE:misc_modules/discord_integration
    FREQUENCY_MANAGER:misc_modules/frequency_manager
    IQ_EXPORTER:misc_modules/iq_exporter
    NOISE_REDUCTION_LOGMMSE:misc_modules/noise_reduction_logmmse
    RECORDER:misc_modules/recorder
    RIGCTL_CLIENT:misc_modules/rigctl_client
    RIGCTL_SERVER:misc_modules/rigctl_server
    SCANNER:misc_modules/scanner
    SCHEDULER:misc_modules/scheduler
    SPOTS:misc_modules/spots
    WEBSDR_VIEW:misc_modules/websdr_view
)

if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
endif ()

set(_sdrpp_executable_sources "src/main.cpp")
if (WIN32)
    list(APPEND _sdrpp_executable_sources "win32/resources.rc")
    set_property(
        SOURCE "win32/resources.rc"
        APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_SOURCE_DIR}/root/res/icons/windows/sdriak.ico"
    )
endif ()
if (MSVC)
    # utf8.manifest opts the process into the UTF-8 ANSI codepage (Win10 1903+);
    # CMake merges it into the linker-generated manifest via mt.exe.
    list(APPEND _sdrpp_executable_sources "win32/utf8.manifest")
endif ()
add_executable(sdrpp ${_sdrpp_executable_sources})

set_target_properties(sdrpp PROPERTIES OUTPUT_NAME "sdriak")

target_link_libraries(sdrpp PRIVATE sdrpp_core Threads::Threads)

# libcurl is resolved by deps/+libcurl (bundled profiles) or via the distro
# package (Linux). Static deps builds absorb it into sdrpp_core; system mode
# picks up the distro libcurl through the dynamic linker's default search.

# Copy dynamic libs over
if (MSVC)
    if (CMAKE_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "MSVC builds require CMake 3.21 or newer for $<TARGET_RUNTIME_DLLS> runtime DLL staging.")
    endif ()

    get_property(_sdrpp_plugin_targets GLOBAL PROPERTY SDRPP_PLUGIN_TARGETS)
    set(_sdrpp_runtime_targets sdrpp ${_sdrpp_plugin_targets})
    list(REMOVE_DUPLICATES _sdrpp_runtime_targets)

    sdrpp_runtime_source_dirs(_sdrpp_runtime_source_dirs)
    sdrpp_asan_runtime_dll(_sdrpp_asan_dll)

    set(_sdrpp_runtime_manifest "${CMAKE_CURRENT_BINARY_DIR}/sdrpp_runtime_dlls_$<CONFIG>.cmake")
    set(_sdrpp_runtime_manifest_content "set(SDRPP_RUNTIME_DLLS\n")
    foreach (_target IN LISTS _sdrpp_runtime_targets)
        string(APPEND _sdrpp_runtime_manifest_content "    [=[$<TARGET_RUNTIME_DLLS:${_target}>]=]\n")
    endforeach ()
    if (_sdrpp_asan_dll)
        string(APPEND _sdrpp_runtime_manifest_content "    [=[${_sdrpp_asan_dll}]=]\n")
    endif ()
    string(APPEND _sdrpp_runtime_manifest_content ")\nset(SDRPP_RUNTIME_SOURCE_DIRS\n")
    foreach (_dir IN LISTS _sdrpp_runtime_source_dirs)
        string(APPEND _sdrpp_runtime_manifest_content "    [=[${_dir}]=]\n")
    endforeach ()
    string(APPEND _sdrpp_runtime_manifest_content ")\n")
    file(GENERATE OUTPUT "${_sdrpp_runtime_manifest}" CONTENT "${_sdrpp_runtime_manifest_content}")

    get_filename_component(_sdrpp_msvc_bin_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(_sdrpp_dumpbin dumpbin HINTS "${_sdrpp_msvc_bin_dir}" NO_DEFAULT_PATH)

    add_custom_command(TARGET sdrpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/root" "$<TARGET_FILE_DIR:sdrpp>"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:sdrpp>/res/icons/windows"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:sdrpp>/res/icons/macos"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:sdrpp>/res/icons/linux"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sdrpp>/recordings"
        COMMENT "Copying Windows runtime root skeleton")

    add_custom_command(TARGET sdrpp POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DRUNTIME_MANIFEST="${_sdrpp_runtime_manifest}"
            -DDST="$<TARGET_FILE_DIR:sdrpp>"
            -DRUNTIME_DEPENDENCIES_COMMAND="${_sdrpp_dumpbin}"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        COMMENT "Copying linked runtime DLLs")

    if (COPY_MSVC_REDISTRIBUTABLES)
        # Get the list of Visual C++ runtime DLLs
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP True)
        include(InstallRequiredSystemLibraries)

        add_custom_target(do_always_msvc ALL
            COMMAND ${CMAKE_COMMAND}
                -DCONFIG="$<CONFIG>"
                -DDST="$<TARGET_FILE_DIR:sdrpp>"
                "-DREDIST_DLLS=${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}"
                -P "${CMAKE_SOURCE_DIR}/cmake/copy_msvc_redist.cmake"
            COMMENT "Copying MSVC redistributables"
            VERBATIM)
        add_dependencies(do_always_msvc sdrpp)
    endif ()
endif ()

# Create module cmake file
configure_file(${CMAKE_SOURCE_DIR}/sdrpp_module.cmake ${CMAKE_CURRENT_BINARY_DIR}/sdrpp_module.cmake @ONLY)

# Install directives
install(TARGETS sdrpp DESTINATION bin)
install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/
    DESTINATION share/sdriak
    PATTERN "windows" EXCLUDE
    PATTERN "macos" EXCLUDE
    PATTERN "linux" EXCLUDE
)
configure_file(${CMAKE_SOURCE_DIR}/sdriak.desktop ${CMAKE_CURRENT_BINARY_DIR}/sdriak.desktop @ONLY)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdriak.desktop DESTINATION share/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/license DESTINATION share/doc/sdriak RENAME copyright)
    foreach (_sdriak_icon_size 16 24 32 48 64 128 256)
        install(
            FILES ${CMAKE_SOURCE_DIR}/root/res/icons/linux/sdriak-${_sdriak_icon_size}.png
            DESTINATION share/icons/hicolor/${_sdriak_icon_size}x${_sdriak_icon_size}/apps
            RENAME sdriak.png
        )
    endforeach ()
    install(
        FILES ${CMAKE_SOURCE_DIR}/root/res/icons/sdriak.png
        DESTINATION share/icons/hicolor/512x512/apps
        RENAME sdriak.png
    )
    install(
        FILES ${CMAKE_SOURCE_DIR}/root/res/icons/linux/sdriak.svg
        DESTINATION share/icons/hicolor/scalable/apps
        RENAME sdriak.svg
    )

    # Bundled-shared deps (sdrplay's closed-source blob, librfnm, any other dep
    # the policy resolved to distro:bundled distro:shared) live under
    # SDRPP_DEPS_AUTOBUILD_PREFIX (deps/build-<preset>-<config>/destdir/usr/local)
    # rather than the main install tree. Ship them under lib/ alongside the
    # rest of our libraries so both dpkg-shlibdeps (packaging) and dlopen
    # (runtime) resolve them through a standard search path. AppImage builds
    # don't need this — linuxdeploy bundles them into the AppDir automatically.
    if (SDRPP_DEPS_AUTOBUILD_PREFIX AND NOT OPT_BUILD_APPIMAGE)
        file(GLOB _sdrpp_bundled_dep_libs "${SDRPP_DEPS_AUTOBUILD_PREFIX}/lib/*.so*")
        if (_sdrpp_bundled_dep_libs)
            install(FILES ${_sdrpp_bundled_dep_libs} DESTINATION lib)
        endif ()
    endif ()
endif ()

if (MSVC)
    set(SDRPP_WINDOWS_BUNDLE_STAGE_ROOT "${CMAKE_BINARY_DIR}/stage/windows-bundle" CACHE INTERNAL "Root directory for Windows bundle staging" FORCE)
    set(_sdrpp_windows_bundle_stage_dir "${SDRPP_WINDOWS_BUNDLE_STAGE_ROOT}/$<CONFIG>")
    get_property(_sdrpp_plugin_targets GLOBAL PROPERTY SDRPP_PLUGIN_TARGETS)

    add_custom_target(stage_windows_bundle
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${_sdrpp_windows_bundle_stage_dir}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_sdrpp_windows_bundle_stage_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/root" "${_sdrpp_windows_bundle_stage_dir}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_sdrpp_windows_bundle_stage_dir}/modules"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:sdrpp>" "${_sdrpp_windows_bundle_stage_dir}"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${_sdrpp_windows_bundle_stage_dir}/res/icons/windows"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${_sdrpp_windows_bundle_stage_dir}/res/icons/macos"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${_sdrpp_windows_bundle_stage_dir}/res/icons/linux"
        COMMENT "Staging Windows bundle under ${SDRPP_WINDOWS_BUNDLE_STAGE_ROOT}"
        VERBATIM)
    add_dependencies(stage_windows_bundle sdrpp)
    if (TARGET do_always_msvc)
        add_dependencies(stage_windows_bundle do_always_msvc)
    endif ()

    foreach (_sdrpp_plugin_target IN LISTS _sdrpp_plugin_targets)
        add_dependencies(stage_windows_bundle ${_sdrpp_plugin_target})
        add_custom_command(TARGET stage_windows_bundle POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${_sdrpp_plugin_target}>" "${_sdrpp_windows_bundle_stage_dir}/modules/"
            VERBATIM)
    endforeach ()
endif ()

if (APPLE)
    set(SDRPP_MACOS_BUNDLE_STAGE_ROOT "${CMAKE_BINARY_DIR}/stage/macos-bundle" CACHE INTERNAL "Root directory for macOS bundle staging" FORCE)
    set(_sdrpp_macos_bundle_dir "${SDRPP_MACOS_BUNDLE_STAGE_ROOT}/$<CONFIG>/SDRIAK.app")
    get_property(_sdrpp_plugin_targets GLOBAL PROPERTY SDRPP_PLUGIN_TARGETS)

    set(_sdrpp_stage_macos_commands
        COMMAND ${CMAKE_COMMAND} -E env
            sh "${CMAKE_SOURCE_DIR}/macos/stage_macos_bundle.sh"
            "${_sdrpp_macos_bundle_dir}"
            "$<TARGET_FILE:sdrpp>"
            "$<TARGET_FILE:sdrpp_core>")

    foreach (_sdrpp_plugin_target IN LISTS _sdrpp_plugin_targets)
        list(APPEND _sdrpp_stage_macos_commands "$<TARGET_FILE:${_sdrpp_plugin_target}>")
    endforeach ()

    add_custom_target(stage_macos_bundle
        ${_sdrpp_stage_macos_commands}
        COMMENT "Staging macOS app bundle under ${SDRPP_MACOS_BUNDLE_STAGE_ROOT}"
        VERBATIM)
    add_dependencies(stage_macos_bundle sdrpp sdrpp_core)
    foreach (_sdrpp_plugin_target IN LISTS _sdrpp_plugin_targets)
        add_dependencies(stage_macos_bundle ${_sdrpp_plugin_target})
    endforeach ()
endif ()

# Create uninstall target
configure_file(${CMAKE_SOURCE_DIR}/cmake_uninstall.cmake ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

# .deb packaging (replaces the old make_debian_package.sh). VERSION_FULL, if
# set in the environment, comes from CI (build_all.yml's prepare job, passed
# through the build container); otherwise fall back to deriving it locally
# via the same scripts/get_version.sh CI uses, so there is exactly one place
# that computes the git-describe-based build info.
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    if (DEFINED ENV{VERSION_FULL} AND NOT "$ENV{VERSION_FULL}" STREQUAL "")
        set(_sdrpp_version_full "$ENV{VERSION_FULL}")
    else ()
        execute_process(
            COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/get_version.sh" "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _sdrpp_get_version_out
            RESULT_VARIABLE _sdrpp_get_version_res
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REGEX MATCH "VERSION_FULL=([^\n]+)" _ "${_sdrpp_get_version_out}")
        set(_sdrpp_version_full "${CMAKE_MATCH_1}")
        if (NOT _sdrpp_get_version_res EQUAL 0 OR _sdrpp_version_full STREQUAL "")
            message(FATAL_ERROR "scripts/get_version.sh failed (exit ${_sdrpp_get_version_res}); cannot derive the .deb package version")
        endif ()
    endif ()

    # Split "X.Y.Z[-pre]+<count>-g<hash>" into base version / build info.
    string(REPLACE "+" ";" _sdrpp_version_full_parts "${_sdrpp_version_full}")
    list(GET _sdrpp_version_full_parts 0 _sdrpp_app_version)
    set(_sdrpp_build_info "")
    list(LENGTH _sdrpp_version_full_parts _sdrpp_version_full_parts_len)
    if (_sdrpp_version_full_parts_len GREATER 1)
        list(GET _sdrpp_version_full_parts 1 _sdrpp_build_info)
    endif ()

    # Debian uses '~' as a "pre-release" separator that sorts BELOW the base
    # version, so "1.2.1~beta2" < "1.2.1" in apt's version comparison.
    string(REPLACE "-" "~" _sdrpp_deb_base "${_sdrpp_app_version}")
    if (_sdrpp_build_info)
        set(CPACK_PACKAGE_VERSION "${_sdrpp_deb_base}-${_sdrpp_build_info}")
    else ()
        set(CPACK_PACKAGE_VERSION "${_sdrpp_deb_base}")
    endif ()

    set(CPACK_PACKAGE_NAME "sdriak")
    set(CPACK_PACKAGE_FILE_NAME "sdriak_debian_pkg")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SDRIAK software radio application")
    set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Vojtech Bubnik OK1IAK")

    # Resolve runtime deps via dpkg-shlibdeps instead of a hand-maintained
    # -dev package list, so it adapts to the build host's distro conventions
    # (libcurl4 vs libcurl4t64, libvolk2.X vs libvolk3, the libssl ABI of the
    # day). The private dirs below are where our own bundled/plugin libraries
    # live so their NEEDED entries resolve without relying on generator
    # auto-detection of non-standard library locations.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # dpkg-shlibdeps runs with its working directory set to the staging root
    # (CPACK_TEMPORARY_DIRECTORY), so these must be staging-relative paths,
    # not the absolute CMAKE_INSTALL_PREFIX-based install paths.
    string(REGEX REPLACE "^/" "" _sdrpp_deb_install_prefix_rel "${CMAKE_INSTALL_PREFIX}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
        "${_sdrpp_deb_install_prefix_rel}/lib"
        "${_sdrpp_deb_install_prefix_rel}/lib/sdriak/plugins")

    include(CPack)
endif ()
