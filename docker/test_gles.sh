#!/bin/bash
set -e

Xvfb :99 -screen 0 640x480x24 -ac &
XVFB_PID=$!
trap "kill -9 $XVFB_PID 2>/dev/null || true" EXIT

sleep 1
export DISPLAY=:99
export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=dummy
export LIBGL_ALWAYS_SOFTWARE=1
export NFSU2_DIAG_FRAMES=1
export NFSU2_TRACE_CAR=1

cd /workspace/nfsu2-recompiled/build/r36s
ORIG_DATA="/workspace/nfsu2-recompiled/work/gamedata/NFS Underground 2 by RafaGaming/Need for Speed Underground 2"
timeout -s KILL "${NFS_GLES_TEST_TIMEOUT:-85}s" ./nfsu2.aarch64 "$ORIG_DATA" --cap30 --auto-advance > run_output.log 2>&1 || true

echo "=== GLES Run Complete ==="
cat run_output.log | grep -E "GL ERROR|DIAG|GLES|Shader|link" || true
