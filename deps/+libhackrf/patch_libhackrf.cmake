#
# Run with -DSRC=<source-dir> -P this-script.
#
# Android open-by-fd and libusb lifecycle hardening, kept as proper git patches
# because they are intended for upstream pull requests
# (github.com/greatscottgadgets/hackrf).
#
# libhackrf can only open a device libusb enumerated itself, but an
# unprivileged Android app never gets to walk the USB bus; it is handed an
# already-open file descriptor by UsbManager. Upstream already declares the
# DISABLE_USB_DEVICE_DISCOVERY option for that environment, but no source
# reads the macro it defines and there is no fd-taking entry point, so the
# option is currently inert. The patch adds hackrf_open_by_fd() (what
# source_modules/hackrf_source calls under __ANDROID__), makes hackrf_init()
# disable enumeration when either the option or __ANDROID__ says it must, and
# fixes the option's default, which is written `option(... ANDROID)` and
# therefore never actually turns on. Originally from AlexandreRouma/hackrf at
# b1275e9c, rebased onto v2026.01.3.
#
# The follow-up patch makes teardown bounded, removes the obsolete completion
# condition variable, and preserves objects whose transfers libusb has not
# returned. It also validates enumeration, allocation, and open failures, uses
# libusb's no-discovery initialization on current Android libusb, and accepts
# the pthreads target name exported by this repository's dependency build.
#
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_helpers.cmake)

set(_android_patch "${CMAKE_CURRENT_LIST_DIR}/0001-android-open-by-fd.patch")
set(_lifecycle_patch "${CMAKE_CURRENT_LIST_DIR}/0002-harden-libusb-lifecycle.patch")

# Patch 2 intentionally changes some lines introduced by patch 1. Once both
# are present, that overlap prevents patch_apply_git_or_fail() from recognizing
# patch 1 by reverse-applying it. Recognize the final series state first so an
# incremental ExternalProject patch step remains idempotent.
find_program(_git git REQUIRED)
execute_process(
    COMMAND "${_git}" apply --reverse --check "${_lifecycle_patch}"
    WORKING_DIRECTORY "${SRC}"
    RESULT_VARIABLE _lifecycle_patch_applied
    ERROR_QUIET
)
if (_lifecycle_patch_applied EQUAL 0)
    message(STATUS "HackRF patch series is already applied")
    return()
endif ()

patch_apply_git_or_fail("${SRC}" "${_android_patch}")
patch_apply_git_or_fail("${SRC}" "${_lifecycle_patch}")
