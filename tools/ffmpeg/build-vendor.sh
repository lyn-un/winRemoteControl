#!/usr/bin/env bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
export HOSTNAME=wrc-build
export revision=8.1.1-239f2c733d
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KEY=${1:?Expected input fingerprint}
MODE=${2:?Expected patched or original}
[[ "$KEY" =~ ^[0-9a-f]{16}$ ]] || exit 2
case "$MODE" in patched|original) ;; *) exit 2;; esac
WORK="$ROOT/build/ffmpeg-vendor/$KEY"
PREFIX="$ROOT/third_party/ffmpeg-wrc-vendor/$KEY/$MODE"
export PATH="$WORK/sources/nasm/nasm-2.16.03:$PATH"
for tool in gcc g++ make nasm pkg-config patch tar dlltool; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool (not installed automatically)" >&2; exit 2; }
done
SOURCE="$WORK/ffmpeg-source-$MODE"
if ! test -f "$SOURCE/.prepared"; then
    mkdir -p "$SOURCE"
    tar xf "$ROOT/build/ffmpeg-vendor/downloads/ffmpeg-239f2c733d.tar.gz" --strip-components=1 -C "$SOURCE"
    if test "$MODE" = patched; then
        (cd "$SOURCE" && patch --batch --fuzz=0 -p1 < "$ROOT/tools/ffmpeg/amf-dynamic-bitrate.patch")
        (cd "$SOURCE" && patch --batch --fuzz=0 -p1 < "$ROOT/tools/ffmpeg/qsv-optional-config-interface.patch")
        (cd "$SOURCE" && patch --batch --fuzz=0 -p1 < "$ROOT/tools/ffmpeg/nvenc-bitrate-reconfiguration.patch")
    fi
    # MF must be byte-identical to the clean upstream source.
    cmp "$SOURCE/libavcodec/mfenc.c" "$WORK/sources/ffmpeg/libavcodec/mfenc.c"
    touch "$SOURCE/.prepared"
fi
if test "$MODE" = patched; then
    bash "$ROOT/tools/ffmpeg/test-amf-rate-update.sh" "$SOURCE"
    bash "$ROOT/tools/ffmpeg/test-qsv-config-interface.sh" "$SOURCE"
    bash "$ROOT/tools/ffmpeg/test-nvenc-reconfiguration.sh" "$SOURCE"
fi
mkdir -p "$WORK/x264-build" "$WORK/ffmpeg-build-$MODE" "$WORK/deps/include/AMF"
cp -R "$WORK/sources/amf/amf/public/include/"* "$WORK/deps/include/AMF/"
make -C "$WORK/sources/nv-codec-headers" PREFIX="$WORK/deps" install
cd "$WORK/x264-build"
if ! test -f "$WORK/deps/x264/lib/libx264.a"; then
    "$WORK/sources/x264/configure" --prefix="$WORK/deps/x264" \
        --enable-static --disable-cli --disable-opencl --bit-depth=8
    make -j8
    make install
fi
export PKG_CONFIG_PATH="$WORK/deps/vpl/lib/pkgconfig:$WORK/deps/lib/pkgconfig:$WORK/deps/x264/lib/pkgconfig"
cd "$WORK/ffmpeg-build-$MODE"
"$SOURCE/configure" --prefix="$PREFIX" --target-os=mingw32 --arch=x86_64 \
    --enable-shared --disable-static --disable-autodetect --disable-programs --disable-doc \
    --disable-network --disable-avdevice --disable-avformat --disable-avfilter --disable-everything \
    --enable-gpl --enable-libx264 --enable-mediafoundation --enable-d3d11va \
    --enable-libvpl --enable-amf --enable-nvenc --enable-ffnvcodec \
    --enable-encoder=libx264,h264_mf,h264_qsv,h264_amf,h264_nvenc \
    --enable-decoder=h264 --enable-parser=h264 --enable-swscale --enable-swresample \
    --extra-cflags="-I$WORK/deps/include" --extra-ldflags=-static-libgcc \
    --extra-version="wrc-vendor-$MODE-$KEY"
sh "$SOURCE/ffbuild/version.sh" "$SOURCE" libavutil/ffversion.h "wrc-vendor-$MODE-$KEY"
make -j8 REVISION="$revision"
make install REVISION="$revision"
for item in avcodec:62 avutil:60 swscale:9 swresample:6; do
    name=${item%:*}
    version=${item#*:}
    dlltool -D "$name-$version.dll" -d "lib$name/$name-$version.def" -l "$PREFIX/lib/$name.lib"
done
cp "$WORK/deps/vpl/bin/libvpl.dll" /ucrt64/bin/libwinpthread-1.dll "$PREFIX/bin/"
for runtime in msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
    cp "$WORK/deps/vpl/bin/$runtime" "$PREFIX/bin/"
done
mkdir -p "$PREFIX/licenses"
cp "$SOURCE/COPYING.GPLv2" "$PREFIX/licenses/FFmpeg.GPLv2"
cp "$WORK/sources/x264/COPYING" "$PREFIX/licenses/x264.GPLv2"
cp "$WORK/sources/libvpl/LICENSE" "$PREFIX/licenses/libvpl.LICENSE"
cp "$WORK/sources/amf/LICENSE.txt" "$PREFIX/licenses/AMF.LICENSE"
mkdir -p "$PREFIX/licenses/nv-codec-headers"
cp "$WORK/sources/nv-codec-headers/include/ffnvcodec/"*.h "$PREFIX/licenses/nv-codec-headers/"
cp -R /ucrt64/share/licenses/libwinpthread "$PREFIX/licenses/"
cp "$ROOT/tools/ffmpeg/vendor-dependencies.json" config.h config_components.h ffbuild/config.mak "$PREFIX/"
if test "$MODE" = patched; then
    cp "$ROOT/tools/ffmpeg/amf-dynamic-bitrate.patch" "$PREFIX/"
    cp "$ROOT/tools/ffmpeg/qsv-optional-config-interface.patch" "$PREFIX/"
    cp "$ROOT/tools/ffmpeg/nvenc-bitrate-reconfiguration.patch" "$PREFIX/"
fi
{
    echo "fingerprint=$KEY mode=$MODE mf_patch=none"
    gcc --version | head -1
    nasm --version
    sha256sum "$ROOT/tools/ffmpeg/vendor-dependencies.json"
    if test "$MODE" = patched; then sha256sum "$PREFIX/amf-dynamic-bitrate.patch"; fi
    if test "$MODE" = patched; then sha256sum "$PREFIX/qsv-optional-config-interface.patch"; fi
    if test "$MODE" = patched; then sha256sum "$PREFIX/nvenc-bitrate-reconfiguration.patch"; fi
    sha256sum "$PREFIX"/bin/*.dll
} > "$PREFIX/wrc-build-manifest.txt"
gcc "$ROOT/tools/ffmpeg/vendor-probe.c" -I"$PREFIX/include" -L"$PREFIX/lib" \
    -lavcodec -lavutil -o "$WORK/vendor-probe-$MODE.exe"
PATH="$PREFIX/bin:/c/Windows/System32:/c/Windows" "$WORK/vendor-probe-$MODE.exe" \
    > "$PREFIX/wrc-encoder-enumeration.txt"
echo "Vendor FFmpeg installed: $PREFIX"
