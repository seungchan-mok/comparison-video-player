# Build environment of the portable Linux bundle (scripts/package-linux.sh).
#
# glibc 2.28 (AlmaLinux 8, the manylinux_2_28 image) so the bundle runs on Ubuntu 20.04+,
# Debian 12+, RHEL 8+ and Fedora; gcc-toolset links the C++ runtime parts newer than the system's
# statically, so binaries need no libstdc++ newer than GCC 8's. Qt is the official 6.8 LTS build
# (made for glibc 2.28, ships its own ICU). FFmpeg is built here, LGPL, with every native decoder,
# encoder and (de)muxer plus:
#   dav1d       AV1 software decoding (BSD, linked statically)
#   OpenH264    H.264 export without an NVIDIA GPU (BSD, linked statically)
#   NVDEC/NVENC through nv-codec-headers 11.1 (loaded at run time; NVIDIA driver 470.57+)
#   VA-API      decoding, against the image's libva 2.13; the system libva is used when there is
#               one. The VA-API encoders (unused by scvp) are left out: they need vaSyncBuffer,
#               which Ubuntu 20.04's libva 2.7 lacks.
# appimagetool and the AppImage runtime make the single-file AppImage of the same bundle.
# Every download is pinned by checksum.
FROM quay.io/pypa/manylinux_2_28_x86_64@sha256:39df0042d5cc900b085aa25a0659368b42a0006c54c474299b785b44c1b4ff82

RUN dnf -y --enablerepo=powertools,epel install \
        nasm libva-devel zlib-devel mesa-libGL-devel mesa-libEGL-devel libX11-devel libxcb-devel \
        fontconfig-devel libxkbcommon-devel libxkbcommon-x11 dbus-libs glib2 \
        xcb-util xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm \
    && dnf clean all \
    && /opt/python/cp312-cp312/bin/pip install --no-cache-dir --root-user-action=ignore \
        aqtinstall==3.3.0 meson==1.9.1 ninja==1.13.0 \
    && ln -s /opt/python/cp312-cp312/bin/aqt /opt/python/cp312-cp312/bin/meson \
        /opt/python/cp312-cp312/bin/ninja /usr/local/bin/

RUN aqt install-qt linux desktop 6.8.3 linux_gcc_64 --archives qtbase icu -O /opt/qt \
    && rm -rf /opt/qt/6.8.3/gcc_64/doc /opt/qt/6.8.3/gcc_64/examples
ENV SCVP_QT_DIR=/opt/qt/6.8.3/gcc_64

WORKDIR /tmp/build
ENV PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig:/opt/ffmpeg/lib64/pkgconfig:/opt/ffmpeg/share/pkgconfig

# fetch URL SHA256: download into the current directory, then check it.
RUN printf '#!/bin/sh\nset -e\nf="$(basename "$1")"\ncurl -fsSL --retry 3 -o "$f" "$1"\necho "$2  $f" | sha256sum -c -\n' \
        > /usr/local/bin/fetch && chmod +x /usr/local/bin/fetch

RUN fetch https://github.com/FFmpeg/nv-codec-headers/releases/download/n11.1.5.4/nv-codec-headers-11.1.5.4.tar.gz \
        3651b887e2ada1d71b579b3dea22a7fda74e44919cf70dcff79b5b17fadc2078 \
    && tar xf nv-codec-headers-11.1.5.4.tar.gz \
    && make -C nv-codec-headers-11.1.5.4 install PREFIX=/opt/ffmpeg \
    && rm -rf /tmp/build/*

RUN fetch https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz \
        686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5 \
    && tar xf dav1d-1.5.4.tar.xz \
    && meson setup dav1d-1.5.4/build dav1d-1.5.4 --prefix=/opt/ffmpeg --libdir=lib --buildtype=release \
        --default-library=static -Db_staticpic=true -Denable_tools=false -Denable_tests=false \
    && ninja -C dav1d-1.5.4/build install \
    && rm -rf /tmp/build/*

RUN fetch https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz \
        558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69 \
    && tar xf v2.6.0.tar.gz \
    && meson setup openh264-2.6.0/build openh264-2.6.0 --prefix=/opt/ffmpeg --libdir=lib --buildtype=release \
        --default-library=static -Db_staticpic=true -Dtests=disabled \
    && ninja -C openh264-2.6.0/build install \
    && sed -i 's/^Libs: .*-lopenh264/& -lstdc++/' /opt/ffmpeg/lib/pkgconfig/openh264.pc \
    && rm -rf /tmp/build/*

RUN fetch https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz \
        7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3 \
    && tar xf ffmpeg-8.1.3.tar.xz \
    && cd ffmpeg-8.1.3 \
    && ./configure --prefix=/opt/ffmpeg --enable-shared --disable-static --enable-pic \
        --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swresample \
        --disable-network --disable-autodetect --pkg-config-flags=--static \
        --enable-zlib --enable-libdav1d --enable-libopenh264 --enable-vaapi \
        --enable-cuda --enable-ffnvcodec --enable-nvdec --enable-nvenc --disable-encoder='*_vaapi' \
    && make -j"$(nproc)" install \
    && rm -rf /tmp/build/*

# AppImage: appimagetool, unpacked so it runs without FUSE, and the static type2 runtime that
# goes into the AppImage (needs no libfuse2 on the user's system).
RUN fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
        ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 \
    && chmod +x appimagetool-x86_64.AppImage \
    && ./appimagetool-x86_64.AppImage --appimage-extract >/dev/null \
    && mv squashfs-root /opt/appimagetool \
    && ln -s /opt/appimagetool/AppRun /usr/local/bin/appimagetool \
    && fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
        2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d \
    && mv runtime-x86_64 /opt/appimagetool/runtime-x86_64 \
    && rm -rf /tmp/build/*

WORKDIR /src
