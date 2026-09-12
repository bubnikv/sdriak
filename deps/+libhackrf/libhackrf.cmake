#
# libhackrf — built from greatscottgadgets upstream. Configure the standalone
# library project so the dependency prefix does not acquire the hackrf_* tools
# or their FFTW discovery/build requirements.
#
# We used to build the AlexandreRouma fork, because upstream's Android build
# takes libhackrf from the android-sdr-kit Docker image, which vendors that
# fork. Our Android job builds the deps in this tree instead (see
# .github/workflows/build_android.yml), so nothing ties us to it — and the
# fork stopped at Jan 2023, ~870 commits behind upstream.
#
# The one thing the fork gave us that upstream lacks — hackrf_open_by_fd() for
# Android — is carried as a patch now; see patch_libhackrf.cmake.
#
# DISABLE_USB_DEVICE_DISCOVERY=ON is needed on Android to bypass libusb device
# enumeration, which requires access to the USB bus that an unprivileged app
# does not have. On desktop we leave it OFF. (Until 2026-09 this recipe passed
# -DDISABLE_USB_ENUMERATION=ON, the option's pre-2023 name, which CMake
# accepted and ignored — so the Android build never actually got the setting.)
#
set(_libhackrf_android_args "")
if (ANDROID)
    list(APPEND _libhackrf_android_args -DDISABLE_USB_DEVICE_DISCOVERY=ON)
endif ()

add_cmake_project(libhackrf
    GIT_REPOSITORY https://github.com/greatscottgadgets/hackrf
    # v2026.01.3 (2026-01-27); bump when intentional.
    GIT_TAG        1cfe7dfe98d333450217d50e3f3a1ad0702e000f
    GIT_SHALLOW    OFF
    SOURCE_SUBDIR  host/libhackrf
    PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DSRC=<SOURCE_DIR>
                       -P ${CMAKE_CURRENT_LIST_DIR}/patch_libhackrf.cmake
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DINSTALL_UDEV_RULES=OFF
        ${_libhackrf_android_args}
)

set(DEP_libhackrf_DEPENDS libusb)
if (WIN32)
    list(APPEND DEP_libhackrf_DEPENDS pthreads)
endif ()

sdrpp_emit_imported_config(libhackrf
    LIB_NAMES   hackrf
    DLL_NAMES   hackrf.dll
    HEADER      hackrf.h
    INCLUDE_SUBDIR libhackrf
)
