#
# libcurl — HTTP/WebSocket transport linked privately into sdrpp_core.
#
# Reworked from cmake/find_or_fetch_curl.cmake. Static + PIC build pinned to
# curl-8_22_0. Protocols are trimmed to the surface SDRIAK uses; TLS backend is
# platform-native wherever one still exists:
#   - Windows: Schannel
#   - macOS:   MbedTLS (Apple's Secure Transport backend was removed in
#              curl 8.15.0; the alternative, OpenSSL + USE_APPLE_SECTRUST,
#              would mean adding an OpenSSL dep just for macOS)
#   - Android: MbedTLS (built by deps/+mbedtls)
#   - Linux:   system OpenSSL (only matters when libcurl is forced bundled;
#              the distro profile resolves to system curl on trixie/sid/noble/
#              resolute, but focal/jammy/bookworm build this recipe)
#
# Do not pin back to 8.11.1 or below 8.12.0: 8.11.1 is the one release carrying
# CVE-2025-0665. Its threaded resolver closes the same eventfd twice on 64-bit
# targets — lib/asyn-thread.c lacks the `#ifndef USE_EVENTFD` guard that
# lib/multi.c has, and Curl_eventfd() puts one fd in both sock_pair slots — so
# every DNS resolve frees an fd number that another thread may already have
# reopened. On Android that trips fdsan and aborts the process.
#
# No CA bundle is shipped. MbedTLS carries no trust store of its own, so each
# build is pointed at the platform's: Android sets CURLOPT_CAPATH
# /system/etc/security/cacerts at runtime (see core/utils/curl_init), macOS
# compiles in CURL_CA_BUNDLE=/etc/ssl/cert.pem below. Note that the macOS file
# is a static bundle, not the Keychain — CAs a user installs into the Keychain
# are not honoured since the Secure Transport backend went away.
#
sdrpp_dep_get_linkage_option_bools(libcurl _libcurl_build_shared _libcurl_build_static)

set(_libcurl_cmake_args
    -DBUILD_CURL_EXE=OFF
    -DBUILD_TESTING=OFF
    -DBUILD_LIBCURL_DOCS=OFF
    -DBUILD_MISC_DOCS=OFF
    -DENABLE_CURL_MANUAL=OFF
    -DCURL_DISABLE_INSTALL=OFF
    -DCURL_ENABLE_EXPORT_TARGET=ON
    -DCURL_USE_LIBPSL=OFF
    -DUSE_APPLE_IDN=OFF
    -DUSE_LIBIDN2=OFF
    -DUSE_WIN32_IDN=OFF
    -DUSE_NGHTTP2=OFF
    -DCURL_USE_LIBSSH2=OFF
    # WebSockets are mandatory (core/CMakeLists.txt probes curl_ws_send).
    # They are on by default since the ENABLE_WEBSOCKETS option was replaced
    # by CURL_DISABLE_WEBSOCKETS; keep the intent explicit.
    -DCURL_DISABLE_WEBSOCKETS=OFF
    -DCURL_DISABLE_LDAP=ON
    -DCURL_DISABLE_LDAPS=ON
    -DCURL_DISABLE_FTP=ON
    -DCURL_DISABLE_DICT=ON
    -DCURL_DISABLE_TELNET=ON
    -DCURL_DISABLE_TFTP=ON
    -DCURL_DISABLE_RTSP=ON
    -DCURL_DISABLE_SMTP=ON
    -DCURL_DISABLE_POP3=ON
    -DCURL_DISABLE_IMAP=ON
    -DCURL_DISABLE_GOPHER=ON
    -DCURL_DISABLE_MQTT=ON
    # No CURL_DISABLE_SCP/SFTP here: upstream has never had those options
    # (they were dead flags in the 8.11.1 pin too). SCP/SFTP ride on libssh2,
    # which CURL_USE_LIBSSH2=OFF above already keeps out.
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

if (WIN32)
    list(APPEND _libcurl_cmake_args -DCURL_USE_SCHANNEL=ON)
elseif (ANDROID OR APPLE)
    # Pre-feed curl's bundled FindMbedTLS.cmake with the deps install layout
    # so it doesn't have to call find_package(MbedTLS) from a stale module
    # path. MBEDTLS_INCLUDE_DIR is singular; the plural spelling still works
    # through a deprecation shim in 8.22, but warns.
    set(_mbedtls_inc "${SDRPP_DEPS_INSTALL_PREFIX}/include")
    set(_mbedtls_lib "${SDRPP_DEPS_INSTALL_PREFIX}/lib/libmbedtls.a")
    set(_mbedx509_lib "${SDRPP_DEPS_INSTALL_PREFIX}/lib/libmbedx509.a")
    set(_mbedcrypto_lib "${SDRPP_DEPS_INSTALL_PREFIX}/lib/libmbedcrypto.a")
    list(APPEND _libcurl_cmake_args
        -DCURL_USE_MBEDTLS=ON
        -DMBEDTLS_INCLUDE_DIR=${_mbedtls_inc}
        -DMBEDTLS_LIBRARY=${_mbedtls_lib}
        -DMBEDX509_LIBRARY=${_mbedx509_lib}
        -DMBEDCRYPTO_LIBRARY=${_mbedcrypto_lib})
    if (APPLE)
        # Compile the trust store into *this* libcurl rather than overriding
        # CAINFO at runtime for every Apple build: a macOS build that resolves
        # libcurl to the system copy (SDRPP_DEP_FORCE_SYSTEM=libcurl) must keep
        # whatever store that curl was built against, and forcing a bundle on a
        # SecTrust-capable curl would silently cut off Keychain roots. Android
        # has no system libcurl to fall back to, so it keeps setting
        # CURLOPT_CAPATH from curl_init.cpp. Still overridable at runtime.
        list(APPEND _libcurl_cmake_args -DCURL_CA_BUNDLE=/etc/ssl/cert.pem)
    endif ()
else ()
    list(APPEND _libcurl_cmake_args -DCURL_USE_OPENSSL=ON)
endif ()

add_cmake_project(libcurl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG        curl-8_22_0
    GIT_SHALLOW    ON
    CMAKE_ARGS     ${_libcurl_cmake_args})

if (ANDROID OR APPLE)
    set(DEP_libcurl_DEPENDS mbedtls)
endif ()

sdrpp_validate_dep(libcurl
    PACKAGE_NAME   CURL
    TARGET         CURL::libcurl
    LIB_NAMES      curl libcurl
    DLL_NAMES      libcurl.dll curl.dll
    HEADER         curl.h
    INCLUDE_SUBDIR curl
    REQUIRES_CONFIG)
