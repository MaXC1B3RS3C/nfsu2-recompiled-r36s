#!/bin/bash
# Temporary widescreen smoke run (opencode investigation). Not part of the port.
# Usage: NFSU2_SCREEN=854x480|1280x720 [NFSU2_WS_TRACE=1] widescreen_smoke.sh
set -e

SCREEN=${NFSU2_SCREEN:-854x480}
OUT="/workspace/nfsu2-recompiled/build/screenshots/wide_${SCREEN}"

Xvfb :98 -screen 0 1920x1080x24 -ac &
XVFB_PID=$!
trap "kill -9 $XVFB_PID 2>/dev/null || true" EXIT
sleep 1

export DISPLAY=:98
export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=dummy
export LIBGL_ALWAYS_SOFTWARE=1
export NFSU2_DIAG_FRAMES=1
export NFSU2_WS_TRACE=${NFSU2_WS_TRACE:-1}
export NFSU2_SCREEN="$SCREEN"
export NFSU2_WIDESCREEN_DAT=/workspace/nfsu2-recompiled/ports/r36s/package/widescreen/NFSUnderground2.WidescreenFix.dat

cd /workspace/nfsu2-recompiled/build/r36s
ORIG_DATA="/workspace/nfsu2-recompiled/work/gamedata/NFS Underground 2 by RafaGaming/Need for Speed Underground 2"
rm -f diag_frame_*.ppm run_wide.log
timeout -s KILL "${NFS_GLES_TEST_TIMEOUT:-85}s" ./nfsu2.aarch64 "$ORIG_DATA" --cap30 --auto-advance > run_wide.log 2>&1 || true

mkdir -p "$OUT"
cp -f diag_frame_*.ppm "$OUT"/ 2>/dev/null || true
cp -f run_wide.log "$OUT"/ 2>/dev/null || true

echo "=== GLES widescreen run complete: $SCREEN ==="
grep -E "screen |game renders|Widescreen|\[WS\]|render resolution|Reset to" run_wide.log | head -80
