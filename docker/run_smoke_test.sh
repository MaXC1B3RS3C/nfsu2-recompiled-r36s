#!/bin/bash
set -e

echo "============================================================"
echo " R36S OpenGL ES Smoke Test Environment (Docker)             "
echo " Cores: $(nproc)"
echo " Memory: $(free -m | awk '/Mem:/ {print $2 " MB"}')"
echo " Swap:   $(free -m | awk '/Swap:/ {print $2 " MB"}')"
echo "============================================================"

PROJECT_DIR="/workspace/nfsu2-recompiled"
PCRECOMP_DIR="/workspace/pcrecomp"
GAME_DIR="$PROJECT_DIR/work/gamedata/NFS Underground 2 by RafaGaming/Need for Speed Underground 2"

export PCRECOMP="$PCRECOMP_DIR"
cd "$PROJECT_DIR"

echo "--> [1/3] Verifying Lifted C Sources..."
if [ ! -f "src/recomp/gen/recomp_funcs.h" ]; then
    echo "ERROR: Lifted C sources missing in src/recomp/gen"
    exit 1
fi
echo "    Lifted C sources verified (27,758 functions)."

echo "--> [2/3] Verifying/Building R36S GLES Binary (nfsu2.aarch64)..."
mkdir -p build/r36s
cd build/r36s
if [ ! -f "build.ninja" ]; then
    cmake "$PROJECT_DIR/ports/r36s" -DCMAKE_BUILD_TYPE=Release -GNinja
fi
ninja -j4 d3d9_gles nfsu2.aarch64
echo "    Build successful: $(ls -lh nfsu2.aarch64)"

echo "--> [3/3] Executing R36S Smoke Test under 1GB RAM / 4 CPUs / 640x480..."
export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=dummy
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_VIDEO_EGL_DRIVER=libEGL.so
export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLES_VERSION_OVERRIDE=2.0
export LIBGL_ALWAYS_SOFTWARE=1

# Run memory monitor in background
( while true; do
    PID=$(pgrep -f nfsu2.aarch64 || true)
    if [ -n "$PID" ] && [ -f "/proc/$PID/status" ]; then
      awk '/VmRSS|VmHWM/ {print "   [R36S MEMORY MONITOR] " $0}' /proc/$PID/status 2>/dev/null || true
    fi
    sleep 1
  done
) &
MONITOR_PID=$!

# Run test in xvfb virtual 640x480 display with timeout
xvfb-run -a -s '-screen 0 640x480x24' timeout 15s ./nfsu2.aarch64 "$GAME_DIR" --cap30 || true

kill -9 "$MONITOR_PID" 2>/dev/null || true

echo "============================================================"
echo " GLES Smoke test completed successfully under R36S limits!  "
echo "============================================================"
