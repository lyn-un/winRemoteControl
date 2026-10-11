#!/usr/bin/env bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${1:?Expected patched FFmpeg source directory}
WORK="$ROOT/build/ffmpeg-vendor/helper-test"
mkdir -p "$WORK"
# Compile the patched function itself; never maintain a second implementation.
sed -n '/^static int amf_set_rate_property(/,/^int ff_amf_receive_packet(/{ /^int ff_amf_receive_packet(/!p; }' \
    "$SOURCE/libavcodec/amfenc.c" > "$WORK/amf-rate-update-under-test.h"
test -s "$WORK/amf-rate-update-under-test.h"
gcc -std=c17 -Wall -Wextra -Werror -I"$WORK" \
    "$ROOT/tools/ffmpeg/test-amf-rate-update.c" -o "$WORK/test-amf-rate-update.exe"
"$WORK/test-amf-rate-update.exe"
