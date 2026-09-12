#
# Run with -DSRC=<source-dir> -P this-script.
#
# Android open-by-fd, kept as a proper git patch because it is intended for an
# upstream pull request (github.com/greatscottgadgets/hackrf).
#
# libhackrf can only open a device libusb enumerated itself, but an
# unprivileged Android app never gets to walk the USB bus; it is handed an
# already-open file descriptor by UsbManager. Upstream already declares the
# DISABLE_USB_DEVICE_DISCOVERY option for that environment, but no source
# reads the macro it defines and there is no fd-taking entry point, so the
# option is currently inert. The patch adds hackrf_open_by_fd() (what
# source_modules/hackrf_source calls under __ANDROID__), makes hackrf_init()
# honour the macro, and fixes the option's default, which is written
# `option(... ANDROID)` and therefore never actually turns on. Originally from
# AlexandreRouma/hackrf@b1275e9c, rebased onto v2026.01.3.
#
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_helpers.cmake)

patch_apply_git_or_fail("${SRC}" "${CMAKE_CURRENT_LIST_DIR}/0001-android-open-by-fd.patch")
