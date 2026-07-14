#!/usr/bin/env bash
# Build a Linux scrcpy-to-RFB server. FFmpeg is static; libjpeg/zlib/glibc are
# intentionally linked from the Ubuntu 22.04 ABI baseline.
set -euo pipefail

case "$(dpkg --print-architecture)" in
  amd64) artifact=scrcpy-rfb-linux-amd64 ;;
  arm64) artifact=scrcpy-rfb-linux-arm64 ;;
  *) echo "unsupported architecture: $(dpkg --print-architecture)" >&2; exit 1 ;;
esac

rm -rf /tmp/ffmpeg-build /tmp/ffmpeg-install \
       /tmp/libvncserver-build /tmp/libvncserver-install
mkdir -p /tmp/ffmpeg-build /src/dist

cd /tmp/ffmpeg-build
/opt/ffmpeg/configure \
  --prefix=/tmp/ffmpeg-install \
  --disable-everything \
  --disable-autodetect \
  --disable-doc \
  --disable-network \
  --disable-programs \
  --disable-shared \
  --disable-avdevice \
  --disable-avfilter \
  --disable-avformat \
  --disable-swresample \
  --enable-static \
  --enable-pic \
  --enable-avcodec \
  --enable-avutil \
  --enable-swscale \
  --enable-decoder=h264 \
  --enable-parser=h264
make -j"$(nproc)"
make install

cmake \
  -S /src \
  -B /tmp/libvncserver-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DLIBVNCSERVER_INSTALL=ON \
  -DWITH_ZLIB=ON \
  -DWITH_LZO=OFF \
  -DWITH_JPEG=ON \
  -DWITH_PNG=OFF \
  -DWITH_SDL=OFF \
  -DWITH_GTK=OFF \
  -DWITH_LIBSSH2=OFF \
  -DWITH_GNUTLS=OFF \
  -DWITH_OPENSSL=OFF \
  -DWITH_SYSTEMD=OFF \
  -DWITH_GCRYPT=OFF \
  -DWITH_FFMPEG=OFF \
  -DWITH_WEBSOCKETS=OFF \
  -DWITH_SASL=OFF \
  -DWITH_EXAMPLES=OFF \
  -DWITH_TESTS=OFF
cmake --build /tmp/libvncserver-build --parallel
cmake --install /tmp/libvncserver-build --prefix /tmp/libvncserver-install

cc \
  -D_GNU_SOURCE \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -I/tmp/libvncserver-install/include \
  -I/tmp/ffmpeg-install/include \
  /src/tools/scrcpy-rfb/scrcpy-rfb.c \
  /tmp/libvncserver-install/lib/libvncserver.a \
  /tmp/ffmpeg-install/lib/libavcodec.a \
  /tmp/ffmpeg-install/lib/libswscale.a \
  /tmp/ffmpeg-install/lib/libavutil.a \
  -pthread \
  -ljpeg \
  -lz \
  -lm \
  -o "/src/dist/${artifact}"

strip "/src/dist/${artifact}"
file "/src/dist/${artifact}"
(
  cd /src/dist
  sha256sum "${artifact}" > "${artifact}.sha256"
)
