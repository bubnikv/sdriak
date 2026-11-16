/*
 * SDRIAK additions to the Mbed TLS configuration.
 *
 * Mbed TLS includes this file from build_info.h after its stock configuration.
 *
 * On Android, Mbed TLS 3.6.7 does not compile the Linux getrandom() path
 * because Bionic does not define __GLIBC__. It consequently reads the platform
 * entropy source from /dev/random. On Linux kernels before 5.6, that device can
 * block whenever the legacy blocking pool's entropy accounting is low, even
 * after the kernel CSPRNG is ready.
 *
 * SDRIAK is a user-launched application, not an early-boot service. Use
 * /dev/urandom so Mbed TLS reads the kernel CSPRNG without the legacy blocking
 * pool semantics. Do not copy this setting to software that can run before the
 * system CSPRNG has been initialized.
 */
#pragma once

#if defined(__ANDROID__) && !defined(MBEDTLS_PLATFORM_DEV_RANDOM)
#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/urandom"
#endif
