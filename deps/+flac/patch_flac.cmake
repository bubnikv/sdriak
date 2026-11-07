#
# Run with -DSRC=<source-dir> -P this-script.
#
# flac-config.cmake.in (FLAC 1.5.0) calls find_dependency(Ogg)
# unconditionally — it ignores whether libFLAC was actually built with Ogg
# support. Our recipe always configures with WITH_OGG=OFF (native .flac
# encoding only), and no Ogg exists anywhere in the deps prefix, so an
# unpatched config would make every consumer find_package(FLAC CONFIG)
# quietly fail on the missing Ogg. Drop the find_dependency block.
#
# CMakeLists.txt disables fseeko on 32-bit Android below API 24 by testing
# CMAKE_SYSTEM_VERSION, but the NDK legacy toolchain, which is the default,
# pins that to 1, so the test always passes. At API 24 the NDK then declares
# fseeko with a 64-bit off_t, which conflicts with the fseek fallback in
# share/compat.h. Test the real API level instead.
#
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_helpers.cmake)

set(_config "${SRC}/flac-config.cmake.in")
file(READ "${_config}" _content)
patch_replace_or_fail(_content
    "include(CMakeFindDependencyMacro)\nif(NOT TARGET Ogg::ogg)\n    find_dependency(Ogg)\nendif()"
    "# find_dependency(Ogg) removed: this libFLAC is built with WITH_OGG=OFF (sdrpp deps).")
file(WRITE "${_config}" "${_content}")
message(STATUS "Patched ${_config}")

set(_cmakelists "${SRC}/CMakeLists.txt")
file(READ "${_cmakelists}" _content)
patch_replace_or_fail(_content
    "if(ANDROID AND CMAKE_SYSTEM_VERSION VERSION_LESS 24 AND"
    "if(ANDROID AND ANDROID_PLATFORM_LEVEL VERSION_LESS 24 AND")
file(WRITE "${_cmakelists}" "${_content}")
message(STATUS "Patched ${_cmakelists}")
