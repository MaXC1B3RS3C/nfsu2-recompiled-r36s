#!/bin/bash
# Headless smoke test of nfsu2.aarch64 on an aarch64 Linux box without Docker (e.g. the Cowork VM).
# Runs the game under Xvfb + Mesa llvmpipe with --cap30 --auto-advance, captures the diagnostic frames
# (menus ~1000-1250, showroom ~1800-1900, race grid ~2100, countdown/launch ~2250-2400) and converts
# them to PNG in $RUN (default build/r36s-vm/run).
#   SECS=110 bash ports/r36s/smoke_vm.sh            # full run to the race
#   NFSU2_TRACE_FRAME=2300 bash ports/r36s/smoke_vm.sh   # per-draw state trace of one frame
#   SCREEN=854x480 bash ports/r36s/smoke_vm.sh           # another panel size (the game adapts to it)
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${BIN:-$HOME/nfsvm/nfsu2.aarch64}"
RUN="${RUN:-$ROOT/build/r36s-vm/run}"
SECS="${SECS:-110}"
GAME="${GAME:-$ROOT/work/gamedata/NFS Underground 2 by RafaGaming/Need for Speed Underground 2}"
GLSHIM="${GLSHIM:-$HOME/glshim}"
WORK="${WORK:-$HOME/nfsrun}"
mkdir -p "$RUN" "$WORK"; RUN="$(cd "$RUN" && pwd)"
rm -f "$WORK"/diag_frame_*.ppm "$WORK"/diag_screen_*.ppm "$WORK"/run_output.log
cd "$WORK"
# Same graphics options as the device launcher (NFSU2_GRAPHICS=performance|balanced|quality, default
# performance like the card's nfsu2.cfg), unless NFSU2_REGISTRY is given explicitly.
if [ -z "$NFSU2_REGISTRY" ]; then
  NFSU2_GRAPHICS="${NFSU2_GRAPHICS:-performance}"
  eval "$(sed -n '/^COMMON=/,/^esac/p' "$ROOT/ports/r36s/package/NFSU2.sh")"
  export NFSU2_REGISTRY="$COMMON;$PROFILE"
fi
export NFSU2_R36S_EMULATE="${NFSU2_R36S_EMULATE:-1}" LP_NUM_THREADS="${LP_NUM_THREADS:-4}" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 NFSU2_DIAG_FRAMES=1 NFSU2_GL_CHECK=1
export LD_LIBRARY_PATH="$GLSHIM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
xvfb-run -a -s "-screen 0 ${SCREEN:-640x480}x24 -nolisten tcp" timeout -s KILL "$SECS" stdbuf -oL -eL "$BIN" "$GAME" ${FPS_ARG:---cap30} --auto-advance > run_output.log 2>&1 || true
python3 - "$WORK" "$RUN" <<'PY'
import sys, glob, os
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(os.path.join(src, 'diag_frame_*.ppm')) + glob.glob(os.path.join(src, 'diag_screen_*.ppm'))):
    Image.open(f).save(os.path.join(dst, os.path.basename(f)[:-4] + '.png'))
PY
cp run_output.log "$RUN/run_output.log"
grep "R36S model" run_output.log | tail -4
echo "frames: $(ls "$RUN"/diag_frame_*.png 2>/dev/null | wc -l)  GL errors: $(grep -c 'GL ERROR' run_output.log || true)"
grep -E "NOTE:|compile failed|link failed|Framebuffer incomplete|MojoShader error|CreateCubeTexture|\[D3D9 RT\]|caps:" run_output.log | sort | uniq -c | sort -rn | head -40
