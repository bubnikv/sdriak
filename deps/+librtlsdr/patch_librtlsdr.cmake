#
# Run with -DSRC=<source-dir> -P this-script.
#
# Both patches are kept as proper git patches because they are intended for
# upstream pull requests (github.com/osmocom/rtl-sdr) and are applied in
# order.
#
# 0001 — Android open-by-fd. osmocom's librtlsdr can only open a device it
# enumerated itself, but an Android app never gets to enumerate the USB bus;
# it is handed an already-open file descriptor by UsbManager. The patch adds
# rtlsdr_open_sys_dev(), which wraps that fd via libusb_wrap_sys_device(),
# and is what source_modules/rtl_sdr_source calls under __ANDROID__. It also
# restores the BUILD_UTILITIES option (present in the AlexandreRouma fork we
# used to build against, absent upstream) that librtlsdr.cmake passes to skip
# building rtl_test/rtl_fm/etc. Originally written for the AlexandreRouma
# fork; this is Marcelo Gadotti's rebase onto osmocom v2.0.3, taken from
# mrgadotti/SDRPlusPlus@7b232112.
#
# 0002 — Streaming-teardown crash fix. rtlsdr_read_async()'s cancellation
# loop can exit with transfers still owned by the kernel (libusb_handle_events()
# error, or the dev_lost unplug path which only does one zero-timeout event
# pass) and then frees them, leaving freed nodes on libusb's flying-transfers
# list and crashing inside libusb_close(). See the patch header for the full
# analysis.
#
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_helpers.cmake)

patch_apply_git_or_fail("${SRC}" "${CMAKE_CURRENT_LIST_DIR}/0001-android-open-by-fd.patch")
patch_apply_git_or_fail("${SRC}" "${CMAKE_CURRENT_LIST_DIR}/0002-fix-use-after-free-in-async-teardown.patch")
