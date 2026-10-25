#!/bin/bash
# Cross build of the R36S device binary on an x86-64 Ubuntu 22.04 box (docker/Dockerfile.r36s-build).
# The same compile and link as build_vm.sh with PACKAGE=1 (the build of the released binaries), but with the
# aarch64-linux-gnu cross toolchain and the headers bundled in ports/r36s/sdk/include instead of an aarch64
# box's own: the D3D9->GLES layer, MojoShader, main.cpp and the host runtime, linked with the guest (lifted
# game) objects that build_guest_vm.sh compiled (GUEST_CC=aarch64-linux-gnu-gcc).
#   GUEST_DIR  guest objects (required)
#   OUT        work and output folder (default build/cross-r36s); the binary is $OUT/package/nfsu2.aarch64
#   The runtime headers come from ports/r36s/sdk/include/recomp32 (the same headers docker/r36s_build.sh
#   compiles the guest objects with), so no pcrecomp checkout is needed: the release pipeline runs this with
#   guest objects exported by build.py --export-guest.
#   NFS_EXE_VARIANT_H  the guest objects' nfs_exe_variant.h (default src/recomp/gen/nfs_exe_variant.h)
# The binary links against stubs named libSDL2-2.0.so.0 and libGLESv2.so.2 (the device has the real ones)
# and pins glibc symbol versions to <= 2.28 (compat/), so it starts on ArkOS 2.0 and newer.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT:-$ROOT/build/cross-r36s}"
X="${CROSS_COMPILE:-aarch64-linux-gnu-}"
CC="${X}gcc"; CXX="${X}g++"
JOBS="${JOBS:-$(nproc)}"
[ -n "$GUEST_DIR" ] && ls "$GUEST_DIR"/*.o >/dev/null 2>&1 || { echo "ERROR: no guest objects in GUEST_DIR=$GUEST_DIR"; exit 1; }
mkdir -p "$OUT/obj" "$OUT/package"

SDK="$ROOT/ports/r36s/sdk/include"
MOJO="$ROOT/third_party/mojoshader"
SDLI="-I$SDK -I$SDK/SDL2"
DXVK="$SDK/dxvk/native"
MOJODEF="-DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_METAL=0 -DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DMOJOSHADER_DEPTH_CLIPPING -DMOJOSHADER_FLIP_RENDERTARGET"
GLESINC="-I$ROOT/ports/r36s/d3d9_gles -I$MOJO -I$SDK/dxvk -I$DXVK -I$DXVK/windows -I$DXVK/directx $SDLI"
APPINC="-I$SDK/dxvk -I$ROOT/ports/r36s -I$ROOT/ports/r36s/d3d9_gles -I$ROOT/android/app/src/main/cpp -I$ROOT/src/runtime -I$ROOT/src/platform -I$SDK/recomp32 -I$SDK/win32hle $SDLI -isystem $DXVK -isystem $DXVK/windows -isystem $DXVK/directx"
APPDEF="-DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -DNFS_PARALLEL_GUEST -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DNFS_R36S -DNFS_GLES -DNFS_D3D9_BACKEND -DNFS_SDL_AUDIO -DRECOMP_STRICT_ICALL -DRECOMP_X87_WINDOW $MOJODEF"
# The SPEED2.EXE the guest objects were lifted from (run_lift.py writes it next to the lifted code); the
# native helpers take that exe's addresses (src/runtime/nfs_exe_addr.h).
VH="${NFS_EXE_VARIANT_H:-$ROOT/src/recomp/gen/nfs_exe_variant.h}"
[ -f "$VH" ] || { echo "ERROR: $VH missing: lift first (run_lift.py writes it)"; exit 1; }
VARIANT="$(sed -n 's/^#define NFS_EXE_VARIANT \([0-9]*\).*/\1/p' "$VH" | tr -d '\r')"
VID="$(sed -n 's/^#define NFS_EXE_LIFTED_ID "\([a-z0-9]*\)".*/\1/p' "$VH" | tr -d '\r')"
[ -n "$VARIANT" ] && [ -n "$VID" ] || { echo "ERROR: $VH names no exe variant"; exit 1; }
APPDEF="$APPDEF -DNFS_EXE_VARIANT=$VARIANT"
echo "host runtime for SPEED2.EXE variant $VARIANT ($VID)"
OPT="-O2 -g -DNDEBUG -fno-strict-aliasing -ffp-contract=off -mcpu=cortex-a35 -mtune=cortex-a35"

compile() { # src obj cmd...
  local src="$1" obj="$2"; shift 2
  echo "  CC $(basename "$src")"; "$@" -c "$src" -o "$obj"
}
pids=()
run() { compile "$@" & pids+=($!); while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done; }
run "$ROOT/ports/r36s/d3d9_gles/d3d9_gles.cpp" "$OUT/obj/d3d9_gles.o" $CXX -std=c++17 $OPT $MOJODEF -DNFS_R36S -DNFS_GLES $GLESINC
for c in mojoshader.c mojoshader_common.c profiles/mojoshader_profile_common.c profiles/mojoshader_profile_glsl.c; do
  run "$MOJO/$c" "$OUT/obj/$(basename "$c" .c).o" $CC $OPT $MOJODEF -DNFS_R36S -DNFS_GLES -I"$MOJO"
done
run "$ROOT/ports/r36s/main.cpp" "$OUT/obj/main.o" $CXX -std=c++17 $OPT $APPDEF $APPINC
run "$ROOT/android/app/src/main/cpp/guest_runtime.cpp" "$OUT/obj/guest_runtime.o" $CXX -std=c++17 $OPT $APPDEF $APPINC
run "$ROOT/android/app/src/main/cpp/d3d9_bridge.cpp" "$OUT/obj/d3d9_bridge.o" $CXX -std=c++17 $OPT $APPDEF $APPINC
for f in overrides fx_cache hud_half world_cull native_math native_scene widescreen; do
  run "$ROOT/src/runtime/$f.c" "$OUT/obj/$f.o" $CC $OPT $APPDEF $APPINC
done
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "ERROR: compile failed"; exit 1; }

# glibc compatibility, as build_vm.sh: old symbol versions pinned, crt1.o replaced, libstdc++/libgcc static.
CDIR="$OUT/compat"; mkdir -p "$CDIR"
$CC -O2 -fno-builtin -fno-stack-protector -c "$ROOT/ports/r36s/compat/glibc_compat.c" -o "$CDIR/glibc_compat.o"
$CC -c "$ROOT/ports/r36s/compat/compat_start.S" -o "$CDIR/compat_start.o"
$CC -dumpspecs | awk '/^\*startfile:/{f=1;print;next} /^\*/{f=0} f&&NF' \
  | sed -E "s#([^A-Za-z/])crt1\.o%s#\1$CDIR/compat_start.o#g" > "$CDIR/compat.specs"
grep -q "compat_start.o" "$CDIR/compat.specs" || { echo "ERROR: could not build compat.specs"; cat "$CDIR/compat.specs"; exit 1; }
LINKCOMPAT="-specs=$CDIR/compat.specs -static-libstdc++ -static-libgcc $CDIR/glibc_compat.o -Wl,--push-state,--no-as-needed -l:libpthread.so.0 -l:libdl.so.2 -l:librt.so.1 -Wl,--pop-state"

# Stubs with the device libraries' sonames (the real ones are on the device; none exist for this toolchain)
STUB="$OUT/stubs"; mkdir -p "$STUB"
${X}nm -u "$OUT"/obj/*.o | awk '{print $2}' | grep -E '^gl[A-Z]' | sort -u | sed 's/.*/void &(void){}/' > "$STUB/stub_gles.c"
$CC -shared -fPIC -o "$STUB/libGLESv2.so" -Wl,-soname,libGLESv2.so.2 "$STUB/stub_gles.c"
${X}nm -u "$OUT"/obj/*.o | awk '{print $2}' | grep -E '^SDL_' | sort -u | sed 's/.*/void &(void){}/' > "$STUB/stub_sdl.c"
$CC -shared -fPIC -o "$STUB/libSDL2.so" -Wl,-soname,libSDL2-2.0.so.0 "$STUB/stub_sdl.c"

echo "  LD package/nfsu2.aarch64 ($(ls "$GUEST_DIR"/*.o | wc -l) guest objects)"
$CXX -o "$OUT/package/nfsu2.aarch64" "$OUT"/obj/*.o "$GUEST_DIR"/*.o -L"$STUB" -lSDL2 -lGLESv2 -lpthread -ldl -lm -rdynamic -no-pie $LINKCOMPAT
cp "$OUT/package/nfsu2.aarch64" "$OUT/package/nfsu2.aarch64.debug"
${X}strip --strip-debug "$OUT/package/nfsu2.aarch64"
chmod 755 "$OUT/package/nfsu2.aarch64"
${X}objdump -p "$OUT/package/nfsu2.aarch64" | grep NEEDED
BADV=$(${X}readelf --dyn-syms -W "$OUT/package/nfsu2.aarch64" | awk '$7=="UND"{print $8}' | grep -oE '@(GLIBC_2\.(29|[3-9][0-9])|GLIBCXX|CXXABI)[A-Za-z0-9_.]*' | sort -u | tr '\n' ' ')
[ -z "$BADV" ] || { echo "ERROR: binary imports too-new symbol versions: $BADV"; exit 1; }
echo "glibc compat OK: max imported version $(${X}readelf --dyn-syms -W "$OUT/package/nfsu2.aarch64" | awk '$7=="UND"{print $8}' | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tail -1)"
if ${X}objdump -p "$OUT/package/nfsu2.aarch64" | grep -q 'NEEDED.*libGL\.so'; then echo "ERROR: links libGL.so"; exit 1; fi
grep -a -o -m1 'NFSU2_EXE_BUILT_FOR=[a-z0-9]*:[0-9]*' "$OUT/package/nfsu2.aarch64" | head -n1
echo "built $OUT/package/nfsu2.aarch64 ($(stat -c %s "$OUT/package/nfsu2.aarch64") bytes)"
