#
# libhackrf — built from greatscottgadgets upstream. The CMake project lives
# in host/.
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
    SOURCE_SUBDIR  host
    PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DSRC=<SOURCE_DIR>
                       -P ${CMAKE_CURRENT_LIST_DIR}/patch_libhackrf.cmake
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DENABLE_HACKRF_SWEEP=OFF
        ${_libhackrf_android_args}
)

# hackrf_sweep is the only part of the hackrf host tree that needs FFTW, and we
# build it with -DENABLE_HACKRF_SWEEP=OFF: we ship none of the hackrf_* command
# line tools, and leaving it on made the build differ per platform — upstream's
# FindFFTW3f.cmake locates our fftw3 through CMAKE_PREFIX_PATH on desktop but
# not under the Android NDK, whose CMAKE_FIND_ROOT_PATH_MODE_*=ONLY hides the
# deps prefix from a plain find_library(). Restore fftw3 below if the option is
# ever turned back on. (The FFTW_* pre-feed in deps/CMakeLists.txt does not help
# here: upstream's module reads FFTW3f_*.)
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
