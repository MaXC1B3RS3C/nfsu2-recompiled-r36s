#!/bin/bash
# Rebuild the lifted game code (guest objects) in the Cowork VM, resumably, with chosen flags.
# Each device_bash call is limited to ~3 minutes, so this starts compiles until BUDGET seconds have
# passed and can be re-run until it prints "guest objects complete". Objects go to $GUEST_OUT (default
# ~/nfsvm/guest-opt); build_vm.sh links them with GUEST_DIR=$GUEST_OUT.
#   GUEST_CFLAGS   default "-O2 -mcpu=cortex-a35 -fno-pie -DNFS_FIXED_GUEST_BASE=...": in-order scheduling,
#                  direct (non-GOT) globals and a constant guest base (see arm64_guest.h)
#   BUDGET         seconds to keep starting compiles (default 60)
#   GUEST_CC       compiler (default gcc; pgo_clang_vm.sh passes clang)
# Files over 8 MB are split at function boundaries (scripts/split_android_sources.py, as the CMake build
# does for the largest one) so no single compile outlasts a call.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PCRECOMP="${PCRECOMP:-$ROOT/../pcrecomp}"
GUEST_OUT="${GUEST_OUT:-$HOME/nfsvm/guest-v10}"
GUEST_CFLAGS="${GUEST_CFLAGS:--O2 -mcpu=cortex-a35 -fno-pie -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -ftls-model=local-exec -fno-math-errno -fno-stack-clash-protection -fno-stack-protector -DNFS_X87_LOCAL -DNFS_X87_LW}"
BUDGET="${BUDGET:-30}"
GUEST_CC="${GUEST_CC:-gcc}"
JOBS="${JOBS:-4}"
SPLIT="${GUEST_OUT}-split"
mkdir -p "$GUEST_OUT" "$SPLIT"
rm -f "$GUEST_OUT"/*.tmp
FLAGS="$GUEST_CFLAGS -w -fwrapv -fno-strict-aliasing -fexceptions -ffp-contract=off -g0 -fvisibility=hidden \
 -include $ROOT/android/app/src/main/cpp/arm64_guest.h -DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_X87_WINDOW \
 -DRECOMP_MEM_QUAL= -I$PCRECOMP/runtime/recomp32 -I$ROOT/src/recomp/gen"
STAMP="$GUEST_OUT/.flags"
# The stamp ignores where the checkout is mounted (the same folder is reachable as ~/mnt/Projects/... and
# ~/mnt/nfsu2-recompiled in the VM): a different path once wiped every object. Run from ~/mnt/Projects/....
STAMPED="${FLAGS//$ROOT/ROOT}"; [ "$GUEST_CC" = gcc ] || STAMPED="$GUEST_CC $STAMPED"; STAMPED="${STAMPED//$PCRECOMP/PCRECOMP}"
[ -d "$PCRECOMP/runtime/recomp32" ] || { echo "ERROR: $PCRECOMP not found (run from ~/mnt/Projects/nfsu2-recompiled)"; exit 1; }
if [ "$(cat "$STAMP" 2>/dev/null)" != "$STAMPED" ]; then rm -f "$GUEST_OUT"/*.o; echo "$STAMPED" > "$STAMP"; fi
SRCS=()
# recomp_0034.c is a handful of enormous functions: at -O2 each part takes minutes, longer than one call
# allows; its parts are built at -O1 (~40 s each). (BIG_OPT overrides.)
BIG_OPT="${BIG_OPT:--O1}"
for f in "$ROOT"/src/recomp/gen/recomp_*.c; do
  if [ "$(stat -c %s "$f")" -gt 8000000 ]; then
    stem=$(basename "$f" .c)
    if [ ! -f "$SPLIT/$stem.done" ] || [ "$f" -nt "$SPLIT/$stem.done" ]; then
      python3 "$ROOT/scripts/split_android_sources.py" "$f" "$SPLIT" --max-bytes 6000000 > /dev/null
      touch "$SPLIT/$stem.done"
    fi
    for p in "$SPLIT/${stem}"_part_*.c; do SRCS+=("$p"); done
  else
    SRCS+=("$f")
  fi
done
HDR="$ROOT/android/app/src/main/cpp/arm64_guest.h"
fresh() { [ -f "$1" ] && [ "$1" -nt "$2" ] && [ "$1" -nt "$HDR" ]; }
start=$(date +%s)
for src in "${SRCS[@]}"; do
  obj="$GUEST_OUT/$(basename "$src" .c).o"
  fresh "$obj" "$src" && continue
  [ $(( $(date +%s) - start )) -ge "$BUDGET" ] && break
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.5; done
  F="$FLAGS"; case "$(basename "$src")" in recomp_0034_part_*) F="$FLAGS $BIG_OPT";; esac
  # Lifted functions replaced by a native wrapper (src/runtime/fx_cache.c): the definition is renamed
  # *_orig; every caller lives in another file, so their calls reach the wrapper.
  case "$(basename "$src")" in recomp_0039.c) F="$F -Dsub_00664743=sub_00664743_orig";; esac
  # Wrapped functions whose callers are in their own file: only the definition is renamed *_orig, in a
  # copy of the source (the callers then reach the native wrapper). fx_cache.c / hud_half.c / world_cull.c /
  # native_math.c / native_scene.c.
  cc_src="$src"; wrap=""
  case "$(basename "$src")" in
    recomp_0040.c) wrap="sub_00668FAA sub_006689E4 sub_00668EC5";;
    recomp_0021.c) wrap="sub_005361B0";;
    recomp_0029.c) wrap="sub_005CF150 sub_005D3080 sub_005BF2E0 sub_005C5600 sub_005CE860";;  # sub_005BF2E0: GetRacingResolution (guest_runtime.cpp); the last two: native_scene.c
    recomp_0057.c) wrap="sub_0075BC00";;
    recomp_0000.c) wrap="sub_00401310 sub_00401430";;
    recomp_0042_part_001.c) wrap="sub_00689A17";;
    recomp_0004.c) wrap="sub_00440FF0 sub_0043CA20 sub_0043D250 sub_0043CA30 sub_00440EA0";;  # sub_00440EA0: native_scene.c
    recomp_0003.c) wrap="sub_0043C310 sub_0043C3B0 sub_0043BE50";;
    recomp_0024.c) wrap="sub_00576170";;
    recomp_0030.c) wrap="sub_005D6700 sub_005D62C0";;  # native_scene.c
    recomp_0028.c) wrap="sub_005BBF20 sub_005BC010";;
  esac
  if [ -n "$wrap" ]; then
    cc_src="$GUEST_OUT/$(basename "$src" .c).wrap.c"
    cp "$src" "$cc_src"
    for w in $wrap; do
      sed -i "s/^void $w(void) {\$/void ${w}_orig(void) {/" "$cc_src"
      grep -q "^void ${w}_orig(void) {\$" "$cc_src" || { echo "ERROR: $w definition not found"; exit 1; }
    done
  fi
  ( $GUEST_CC -c "$cc_src" -o "$obj.tmp" $F && mv "$obj.tmp" "$obj" && echo "  CC $(basename "$src")" ) &
done
wait
left=0
for src in "${SRCS[@]}"; do fresh "$GUEST_OUT/$(basename "$src" .c).o" "$src" || left=$((left+1)); done
# Objects from sources that no longer exist (e.g. a file now built from parts) must not be linked.
for o in "$GUEST_OUT"/*.o; do
  n=$(basename "$o" .o); found=0
  for src in "${SRCS[@]}"; do [ "$(basename "$src" .c)" = "$n" ] && { found=1; break; }; done
  [ $found = 1 ] || rm -f "$o"
done
if [ "$left" = 0 ]; then echo "guest objects complete ($GUEST_OUT)"; else echo "guest objects: $left left, run again"; fi
