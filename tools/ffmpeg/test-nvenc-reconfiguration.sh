#!/usr/bin/env bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${1:?Expected patched FFmpeg source directory}
WORK="$ROOT/build/ffmpeg-vendor/helper-test"
mkdir -p "$WORK"
# Compile the complete upstream reconfiguration function against a recording SDK stub.
sed -n '/^static void reconfig_encoder(/,/^}/p' "$SOURCE/libavcodec/nvenc.c" > "$WORK/nvenc-reconfiguration-under-test.h"
test -s "$WORK/nvenc-reconfiguration-under-test.h"
gcc -std=c17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -I"$WORK" \
    "$ROOT/tools/ffmpeg/test-nvenc-reconfiguration.c" -o "$WORK/test-nvenc-reconfiguration.exe"
"$WORK/test-nvenc-reconfiguration.exe"
