#
# Initial dependency classification table for SDRIAK.
#
# Linkage defaults stay shared unless a dependency has been reviewed and marked
# as a static source-build candidate. Distro/system packages stay shared.
#

if (DEFINED SDRPP_DEP_CLASSIFICATION_LOADED)
    return()
endif ()
set(SDRPP_DEP_CLASSIFICATION_LOADED TRUE)

# Core/runtime dependencies.
sdrpp_register_dep(fftw3
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE core)

# libFLAC is consumed by core's utils/wav writer (recorder FLAC container).
# Static when bundled: encoder-only usage linked into sdrpp_core, keeps the
# Windows/macOS/Android packaging untouched. Distro builds use libflac-dev
# (CMake config on bookworm+, flac.pc pkg-config fallback on older bases).
sdrpp_register_dep(flac
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE core)

# libopus + libogg are consumed by core's utils/wav writer (recorder Opus
# audio container: Opus packets framed in Ogg, RFC 7845). Static when bundled,
# same as flac; distro builds use libopus-dev / libogg-dev (CMake config or
# opus.pc/ogg.pc pkg-config fallback).
sdrpp_register_dep(opus
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE core)

sdrpp_register_dep(libogg
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE core)

sdrpp_register_dep(glfw3
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE core)

sdrpp_register_dep(volk
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE core)

sdrpp_register_dep(zstd
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE core)

# Shared runtime dependencies used by multiple modules or by multiple other deps.
sdrpp_register_dep(libusb
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE shared-runtime)

sdrpp_register_dep(portaudio
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE shared-runtime)

sdrpp_register_dep(rtaudio
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE shared-runtime)

sdrpp_register_dep(zlib
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE shared-runtime)

sdrpp_register_dep(pthreads
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE shared-runtime)

# Single-module / feature-specific dependencies.
sdrpp_register_dep(codec2
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(libad9361
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(libairspy
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(libairspyhf
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(libbladeRF
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(libcurl
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    # Always static when we build it. Bundled+shared would ship libcurl.so.4
    # in the .deb at /usr/lib/, which on multiarch Debian/Ubuntu sits behind
    # /usr/lib/x86_64-linux-gnu/libcurl.so.4 in ld.so.cache order — the
    # system curl would shadow ours at runtime, defeating the WS-mandatory
    # bundled build. Static links curl's symbols (curl_ws_send included)
    # directly into libsdriak_core.so. Its bundled MbedTLS backend is static as
    # well, avoiding an OpenSSL ABI dependency in portable/focal builds.
    # The distro:static value only takes effect when policy resolves the
    # source to bundled (FORCE_BUNDLED or matrix curl_source=bundled);
    # distro:system builds link the host's shared libcurl as before.
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE core)

sdrpp_register_dep(mbedtls
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE module-private)

sdrpp_register_dep(libfobos
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE module-private)

sdrpp_register_dep(libhackrf
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(libhydrasdr
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE module-private)

sdrpp_register_dep(libiio
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(libmirisdr
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(libperseus-sdr
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE module-private)

sdrpp_register_dep(librfnm
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(librtlsdr
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(libxml2
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(LimeSuite
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(sdrplay
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)

sdrpp_register_dep(sondedump
    DEFAULT_SOURCE  portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE module-private)

sdrpp_register_dep(spdlog
    DEFAULT_SOURCE  portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE module-private)
