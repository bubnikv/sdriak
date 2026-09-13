#!/bin/bash
set -e
cd /root

# VOLK_PACKAGE: volk library package name (libvolk2-dev or libvolk-dev)
# EXTRA_APT: space-separated extra apt packages (e.g. ca-certificates)
# CURL_SOURCE: 'system' uses the distro libcurl; 'bundled' forces deps to
# build libcurl from source with the bundled MbedTLS backend.
VOLK_PACKAGE=${VOLK_PACKAGE:-libvolk2-dev}
CURL_SOURCE=${CURL_SOURCE:-system}

CURL_DEP_ARG=""
# System builds link the distro libcurl; bundled builds use MbedTLS, so both
# libcurl and OpenSSL development packages are dead weight there.
CURL_APT="libcurl4-openssl-dev"
if [ "$CURL_SOURCE" = "bundled" ]; then
    CURL_DEP_ARG="-DSDRPP_DEP_FORCE_BUNDLED=libcurl"
    CURL_APT=""
fi

# apt-get update immediately before install is not enough: when the archive is
# mid-publish, the index can still reference a package revision whose .deb was
# already pruned from the pool, and the install 404s (Acquire::Retries only
# retries transient errors, not 404). Wipe the lists and retry the whole
# update+install cycle so the second attempt sees the post-publish index.
apt_install_with_refresh() {
    local attempt
    for attempt in 1 2 3; do
        rm -rf /var/lib/apt/lists/*
        if apt-get -o Acquire::Retries=3 update && \
           apt-get -o Acquire::Retries=3 install -y "$@"; then
            return 0
        fi
        echo "APT install failed (attempt ${attempt}/3), likely an archive sync race; retrying with a fresh index..." >&2
        sleep $((attempt * 20))
    done
    return 1
}

# Install dependencies and tools. ca-certificates must exist before bundled
# curl configures: MbedTLS supplies no trust store and curl bakes in the path.
apt_install_with_refresh build-essential cmake git pkg-config ca-certificates libfftw3-dev libglfw3-dev ${VOLK_PACKAGE} liborc-0.4-dev libzstd-dev ${CURL_APT} libairspy-dev libairspyhf-dev \
    libiio-dev libad9361-dev librtaudio-dev libhackrf-dev librtlsdr-dev libbladerf-dev liblimesuite-dev p7zip-full wget portaudio19-dev \
    libcodec2-dev libflac-dev libopus-dev libogg-dev autoconf libtool xxd libspdlog-dev ${EXTRA_APT}

# Older base images (Debian bullseye, Ubuntu focal) ship a cmake too old for
# the --preset CLI (3.19+) and CMakePresets v3 (3.21+) that deps/ relies on.
# Pull a current cmake from PyPI when apt's is too old. PEP 668 isn't enforced
# on these distros so a plain `pip install` is fine.
CMAKE_VER=$(cmake --version 2>/dev/null | head -n1 | awk '{print $3}')
if [ -n "$CMAKE_VER" ]; then
    CMAKE_MAJOR=$(echo "$CMAKE_VER" | cut -d. -f1)
    CMAKE_MINOR=$(echo "$CMAKE_VER" | cut -d. -f2)
    if [ "$CMAKE_MAJOR" -lt 3 ] || { [ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -lt 21 ]; }; then
        echo "apt cmake $CMAKE_VER is too old for deps/ presets; upgrading via pip"
        apt_install_with_refresh --no-install-recommends python3-pip
        python3 -m pip install --no-cache-dir cmake
        hash -r
    fi
fi

# Allow git commands on the volume-mounted repo (git ≥ 2.35.2 safe.directory check)
git config --global --add safe.directory /root/sdriak

cd sdriak
cmake --preset ci-linux-deb ${CURL_DEP_ARG}
cd build
make VERBOSE=1 -j"$(nproc)"
cpack

cd ..
mv build/sdriak_debian_pkg.deb .
