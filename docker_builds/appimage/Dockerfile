FROM ubuntu:focal
ENV DEBIAN_FRONTEND=noninteractive

# Build toolchain and host-side prerequisites.
# Focal is chosen for its old glibc baseline (2.31) — the AppImage will run on
# any Linux distro shipped after early 2020. Third-party SDR libraries are built
# through deps/ with the dedicated appimage preset; keep only host ABI/runtime
# packages here (GLFW/OpenGL stack, audio backend headers, Python for Volk, and
# generic build tools).
RUN apt-get -o Acquire::Retries=3 update && \
    apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        build-essential git pkg-config ca-certificates \
        python3 python3-pip \
        wget unzip p7zip-full file patchelf xxd \
        autoconf automake libtool \
        libglfw3-dev libpulse-dev libasound2-dev \
        liborc-0.4-dev && \
    rm -rf /var/lib/apt/lists/*

# Focal's apt cmake (3.16) predates `cmake --preset` (needs 3.19+) and
# CMakePresets v3 (needs 3.21+), both of which deps/ relies on. Pull a current
# cmake from PyPI — it installs to /usr/local/bin and shadows the apt one.
RUN python3 -m pip install --no-cache-dir mako cmake

# Install linuxdeploy and appimagetool for the host architecture.
# Both ship as AppImages that need FUSE — inside Docker we extract them once.
RUN ARCH=$(uname -m) && \
    wget -q -O /tmp/linuxdeploy.AppImage \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage" && \
    chmod +x /tmp/linuxdeploy.AppImage && \
    cd /tmp && /tmp/linuxdeploy.AppImage --appimage-extract >/dev/null && \
    mv squashfs-root /opt/linuxdeploy && \
    ln -sf /opt/linuxdeploy/AppRun /usr/local/bin/linuxdeploy && \
    rm /tmp/linuxdeploy.AppImage && \
    wget -q -O /tmp/appimagetool.AppImage \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-${ARCH}.AppImage" && \
    chmod +x /tmp/appimagetool.AppImage && \
    cd /tmp && /tmp/appimagetool.AppImage --appimage-extract >/dev/null && \
    mv squashfs-root /opt/appimagetool && \
    ln -sf /opt/appimagetool/AppRun /usr/local/bin/appimagetool && \
    rm /tmp/appimagetool.AppImage

COPY make_appimage.sh /root/make_appimage.sh
RUN chmod +x /root/make_appimage.sh
