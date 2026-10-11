#!/usr/bin/env bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${1:?Expected patched FFmpeg source directory}
WORK="$ROOT/build/ffmpeg-vendor/helper-test"
mkdir -p "$WORK"
# Exercise the actual acquisition block, not a separately maintained condition.
sed -n '/The string configuration interface is optional/,/while ((param = av_dict_get(q->qsv_params/{ /while ((param = av_dict_get(q->qsv_params/!p; }' \
    "$SOURCE/libavcodec/qsvenc.c" > "$WORK/qsv-config-interface-under-test.h"
test -s "$WORK/qsv-config-interface-under-test.h"
gcc -std=c17 -Wall -Wextra -Werror -I"$WORK" \
    "$ROOT/tools/ffmpeg/test-qsv-config-interface.c" -o "$WORK/test-qsv-config-interface.exe"
"$WORK/test-qsv-config-interface.exe"
