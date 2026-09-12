#
# librtlsdr — built from osmocom upstream.
#
# We used to build the AlexandreRouma fork, because upstream's Android build
# takes librtlsdr from the android-sdr-kit Docker image, which vendors that
# fork. Our Android job builds the deps in this tree instead (see
# .github/workflows/build_android.yml), so nothing ties us to it — and the
# fork has been stalled since Jan 2024, missing among other things RTL-SDR
# Blog V4L (R828S) support and the V4/V4L tracking-filter fix in HF
# upconversion mode, both of which landed upstream in v2.0.3.
#
# The two things the fork gave us that upstream lacks — rtlsdr_open_sys_dev()
# for Android and the BUILD_UTILITIES option used just below — are carried as
# patches now; see patch_librtlsdr.cmake.
#
add_cmake_project(librtlsdr
    GIT_REPOSITORY https://github.com/osmocom/rtl-sdr
    # v2.0.3 (2026-08-11); bump when intentional.
    GIT_TAG        797f8143266d983c56d8f35d2d442527529dd8a5
    GIT_SHALLOW    OFF
    PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DSRC=<SOURCE_DIR>
                       -P ${CMAKE_CURRENT_LIST_DIR}/patch_librtlsdr.cmake
    INSTALL_COMMAND
        ${CMAKE_COMMAND} --build . --target install --config ${CMAKE_BUILD_TYPE}
        COMMAND ${CMAKE_COMMAND}
            -DROOT=${SDRPP_DEPS_INSTALL_PREFIX}
            -P ${CMAKE_CURRENT_LIST_DIR}/fix_librtlsdr_config.cmake
    CMAKE_ARGS
        -DBUILD_UTILITIES=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

set(DEP_librtlsdr_DEPENDS libusb)

sdrpp_validate_dep(librtlsdr
    PACKAGE_NAME     rtlsdr
    TARGET           rtlsdr::rtlsdr
    STATIC_TARGET    rtlsdr::rtlsdr_static
    SHARED_TARGET    rtlsdr::rtlsdr
    LIB_NAMES        rtlsdr
    # Upstream's static target keeps the `_static` suffix on Windows but
    # overrides OUTPUT_NAME back to `rtlsdr` on UNIX, producing `librtlsdr.a`
    # alongside `librtlsdr.so*`. List both basenames so find_library matches
    # either install layout.
    STATIC_LIB_NAMES rtlsdr_static rtlsdr
    SHARED_LIB_NAMES rtlsdr
    DLL_NAMES        rtlsdr.dll
    HEADER           rtl-sdr.h
    CONFIG_SUBDIR    rtlsdr
    REQUIRES_CONFIG)
