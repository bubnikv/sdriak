#
# libiio — IIO (Industrial I/O) bindings, dependency of PlutoSDR support.
# Version matches the Android kit pin (v0.24).
#
# libiio v0.24's CMakeLists uses pkg_check_modules(libusb-1.0) with no
# CMake-native fallback. The libusb override variables (LIBUSB_LIBRARIES /
# LIBUSB_INCLUDE_DIRS / LIBUSB_FOUND) are injected globally for every recipe
# via DEP_CMAKE_OPTS in deps/CMakeLists.txt — see the libusb override block
# there.
set(_libiio_usb_backend ON)
if (ANDROID)
    set(_libiio_usb_backend OFF)
endif ()

add_cmake_project(libiio
    URL https://github.com/analogdevicesinc/libiio/archive/refs/tags/v0.24.tar.gz
    URL_HASH SHA256=a2b5d848531ea64fd9f95327dfd5a588bd227d9577281ec375e822702c6a52d5
    PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DSRC=<SOURCE_DIR>
                       -P ${CMAKE_CURRENT_LIST_DIR}/patch_libiio.cmake
    CMAKE_ARGS
        -DWITH_TESTS=OFF
        -DWITH_DOC=OFF
        -DWITH_USB_BACKEND=${_libiio_usb_backend}
        -DWITH_NETWORK_BACKEND=ON
        -DHAVE_DNS_SD=OFF
        # SDRIAK uses libiio purely as a client library. On Linux, libiio
        # defaults WITH_IIOD=ON, which pulls in the IIO daemon and its
        # flex/bison-generated parser — extra host build deps we don't want
        # to require for a portable AppImage. Disable the daemon explicitly.
        -DWITH_IIOD=OFF
        # libiio installs its udev rule to an absolute UDEV_RULES_INSTALL_DIR
        # (/lib/udev/rules.d), outside our destdir. That fails for non-root
        # builds and would never be shipped anyway; skip it.
        -DINSTALL_UDEV_RULE=OFF
        # On Darwin, libiio's CMakeLists defaults OSX_FRAMEWORK + OSX_PACKAGE
        # to ON, which sets SKIP_INSTALL_ALL=ON and routes install into a
        # /Library/Frameworks .pkg instead of CMAKE_INSTALL_PREFIX. Disable
        # both so the standard install(TARGETS iio ...) drops libiio.dylib +
        # iio.h under our destdir like every other dep.
        -DOSX_FRAMEWORK=OFF
        -DOSX_PACKAGE=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

set(DEP_libiio_DEPENDS libxml2 libusb)

set(_libiio_compile_definitions "")
set(_libiio_package_dependencies "")
set(_libiio_link_libraries "")
sdrpp_dep_builds_shared(libiio _libiio_builds_shared)
if (NOT _libiio_builds_shared)
    list(APPEND _libiio_compile_definitions LIBIIO_STATIC)
    list(APPEND _libiio_package_dependencies libusb libxml2)
    list(APPEND _libiio_link_libraries libusb::libusb LibXml2::LibXml2)
    if (WIN32)
        list(APPEND _libiio_link_libraries wsock32 iphlpapi ws2_32)
    endif ()
endif ()

sdrpp_emit_imported_config(libiio
    LIB_NAMES   iio libiio
    DLL_NAMES   iio.dll libiio.dll
    HEADER      iio.h
    COMPILE_DEFINITIONS ${_libiio_compile_definitions}
    PACKAGE_DEPENDENCIES ${_libiio_package_dependencies}
    LINK_LIBRARIES ${_libiio_link_libraries}
)
