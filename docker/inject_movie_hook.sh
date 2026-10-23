#!/bin/bash
# ==============================================================================
# Rebuild ONLY src/recomp/gen/recomp_0021.o with the extra FMV hooks, using the
# exact same guest flags as ports/r36s/build_guest_vm.sh.
#
# The four "centred full-screen quad" siblings (sub_005364E0 = EA Games / THX,
# sub_005366E0 = NFSU2 title card + loading screens, sub_00536910,
# sub_00536BF0) carry 4:3 art.  This injects a hook after each one pushes its
# model half-extents (0x005366C2 / 0x005368E6 / 0x00536BD1) and a hook right
# before each builds its matrix with sub_0051AD40 (0x0053656C / 0x00536743 /
# 0x00536973 / 0x00536CB5), so widescreen.c can pillarbox and centre them.
#
# 0x005366C2 already comes from config/hooks.txt; the rest are added here until
# a full re-lift (they are listed in config/hooks.txt too).
#
# Run inside the r36s-test-runner container (native aarch64 gcc).
# ==============================================================================
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PCRECOMP="${PCRECOMP:-$ROOT/../pcrecomp}"
GUEST_DIR="${1:-$ROOT/build/ci-r36s/guest-objs}"
SRC="$ROOT/src/recomp/gen/recomp_0021.c"
TMP="$ROOT/build/ci-r36s/recomp_0021.wrap.c"
OBJ_TMP="$ROOT/build/ci-r36s/recomp_0021.tmp.o"
CC="${CC:-gcc}"

python3 - "$SRC" "$TMP" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()

# (anchor, hook VA).  The anchor is the instruction the hook must precede:
# after the four extent pushes (before `mov ecx, ebx`) or right before the
# sub_0051AD40 matrix call.
hooks = [
    ("    ecx = ebx; /* 0x005368E6: mov ecx, ebx */", "0x005368E6u"),
    ("    ecx = ebx; /* 0x00536BD1: mov ecx, ebx */", "0x00536BD1u"),
    ("    RECOMP_CALL(sub_0051AD40); /* 0x00536743: call 0x51ad40 */", "0x00536743u"),
    ("    RECOMP_CALL(sub_0051AD40); /* 0x0053656C: call 0x51ad40 */", "0x0053656Cu"),
    ("    RECOMP_CALL(sub_0051AD40); /* 0x00536973: call 0x51ad40 */", "0x00536973u"),
    ("    RECOMP_CALL(sub_0051AD40); /* 0x00536CB5: call 0x51ad40 */", "0x00536CB5u"),
]
for needle, hook in hooks:
    n = text.count(needle)
    assert n == 1, "%s anchor count = %d" % (hook, n)
    text = text.replace(needle, "    RECOMP_HOOK_INT(%s);\n" % hook + needle)

# Reproduce build_guest_vm.sh's wrap rename for this file.
w = "void sub_005361B0(void) {"
assert text.count(w) == 1, "sub_005361B0 definition count = %d" % text.count(w)
text = text.replace(w, "void sub_005361B0_orig(void) {")

open(dst, "w").write(text)
print("[inject] patched %s (%d hooks)" % (dst, len(hooks)))
PY

FLAGS="-O2 -fno-pie -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DRECOMP_TLS=__thread \
-DRECOMP_TLS_STRUCT -ftls-model=local-exec -fno-math-errno -fno-stack-clash-protection \
-fno-stack-protector -DNFS_X87_LOCAL -DNFS_X87_LW -w -fwrapv -fno-strict-aliasing \
-fexceptions -ffp-contract=off -g0 -fvisibility=hidden \
-include $ROOT/android/app/src/main/cpp/arm64_guest.h -DRECOMP_STRICT_ICALL \
-DRECOMP_LOCAL_REGS -DRECOMP_X87_WINDOW -DRECOMP_MEM_QUAL= \
-I$PCRECOMP/runtime/recomp32 -I$ROOT/src/recomp/gen"
if "$CC" -mcpu=cortex-a35 -E - </dev/null >/dev/null 2>&1; then
    FLAGS="$FLAGS -mcpu=cortex-a35"
fi

echo "[inject] compiling recomp_0021.o"
$CC -c "$TMP" -o "$OBJ_TMP" $FLAGS
[ -f "$GUEST_DIR/recomp_0021.o" ] && [ ! -f "$GUEST_DIR/recomp_0021.o.orig" ] && \
    cp -f "$GUEST_DIR/recomp_0021.o" "$GUEST_DIR/recomp_0021.o.orig"
cp -f "$OBJ_TMP" "$GUEST_DIR/recomp_0021.o"
nm "$GUEST_DIR/recomp_0021.o" | grep -q " T sub_005366E0$" && echo "[inject] sub_005366E0 present"
echo "[inject] installed $GUEST_DIR/recomp_0021.o"
