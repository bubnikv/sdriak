#
# Mbed-TLS — TLS backend for the Android and macOS libcurl builds.
#
# Android and macOS pull this in (via DEP_libcurl_DEPENDS=mbedtls in
# deps/+libcurl/libcurl.cmake); macOS joined them when curl 8.15.0 removed the
# Secure Transport backend. Windows uses Schannel and Linux system OpenSSL. The
# recipe stays platform-agnostic so SDRPP_DEP_FORCE_* overrides on other
# platforms still work.
#
# Pinned to v3.6.7 — the current release of the 3.6 LTS line, which carries
# X.509 and certificate-chain fixes over the v3.6.2 this used to sit on.
# Upstream does install a CMake package (lib/cmake/MbedTLS), but nothing here
# consumes it: curl is the only user and takes the three static libs by
# absolute path through the MBEDTLS_* variables fed in libcurl.cmake, so no
# imported target is synthesized for this dep.
#
add_cmake_project(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v3.6.7
    GIT_SHALLOW    ON
    CMAKE_ARGS
        -DENABLE_PROGRAMS=OFF
        -DENABLE_TESTING=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

sdrpp_validate_dep(mbedtls
    LIB_NAMES   mbedtls
    HEADER      version.h
    INCLUDE_SUBDIR mbedtls)
