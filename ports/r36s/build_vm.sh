#!/bin/bash
# Fast host-side rebuild of nfsu2.aarch64 WITHOUT cmake/ninja/Docker.
# For an aarch64 Ubuntu 22.04 box (e.g. the Cowork Linux VM) that only has gcc/g++.
# Recompiles the D3D9->GLES layer, MojoShader, main.cpp and the host runtime, then links
# them with the guest (lifted game) objects already compiled by the Docker/cmake build
# in build/r36s/CMakeFiles/nfs_guest.dir (the guest code itself is not rebuilt here).
#
# Needs: build/r36s-vm/vm-sdk-headers.tar.gz (SDL 2.0.20 headers incl. a Linux SDL_config.h).
# Output: $OUT/nfsu2.aarch64 (default $HOME/nfsvm). Link against GLESv2 via a shim dir
# (libGLESv2.so -> Mesa libGL.so.1) when the box has no real libGLESv2.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PCRECOMP="${PCRECOMP:-$ROOT/../pcrecomp}"
OUT="${OUT:-$HOME/nfsvm}"
GLSHIM="${GLSHIM:-$HOME/glshim}"
JOBS="${JOBS:-4}"
mkdir -p "$OUT/obj" "$OUT/sdk" "$GLSHIM"

if [ ! -f "$OUT/sdk/include/SDL2/SDL.h" ]; then
  tar xzf "$ROOT/build/r36s-vm/vm-sdk-headers.tar.gz" -C "$OUT/sdk"
  # SDL_config.h was generated on x86_64: drop x86-only intrinsics headers
  sed -i -E '/#define HAVE_(IMMINTRIN|MMINTRIN|XMMINTRIN|EMMINTRIN|PMMINTRIN)_H/d' "$OUT/sdk/include/SDL2/SDL_config.h"
fi
if [ ! -e /usr/lib/aarch64-linux-gnu/libGLESv2.so.2 ] && [ ! -e "$GLSHIM/libGLESv2.so.2" ]; then
  ln -sf /usr/lib/aarch64-linux-gnu/libGL.so.1 "$GLSHIM/libGLESv2.so.2"
  ln -sf /usr/lib/aarch64-linux-gnu/libGL.so.1 "$GLSHIM/libGLESv2.so"
fi
[ -e /usr/lib/aarch64-linux-gnu/libSDL2.so ] || ln -sf /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 "$GLSHIM/libSDL2.so"

# Guest objects: copy once to local disk (linking over the shared mount is slow)
if [ ! -f "$OUT/guest/.stamp" ] || [ -n "$REFRESH_GUEST" ]; then
  rm -rf "$OUT/guest"; mkdir -p "$OUT/guest"
  find "$ROOT/build/r36s/CMakeFiles/nfs_guest.dir" -name '*.o' | while read -r o; do
    cp "$o" "$OUT/guest/$(echo "${o#*nfs_guest.dir/}" | tr '/' '_')"; done
  touch "$OUT/guest/.stamp"
fi

DXVK="$ROOT/build/dxvk-source/include/native"
MOJO="$ROOT/third_party/mojoshader"
SDLI="-I$OUT/sdk/include -I$OUT/sdk/include/SDL2"
MOJODEF="-DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_METAL=0 -DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DMOJOSHADER_DEPTH_CLIPPING -DMOJOSHADER_FLIP_RENDERTARGET"
GLESINC="-I$ROOT/ports/r36s/d3d9_gles -I$MOJO -I$DXVK -I$DXVK/windows -I$DXVK/directx $SDLI"
APPINC="-I$ROOT/build/dxvk-source/include -I$ROOT/ports/r36s -I$ROOT/ports/r36s/d3d9_gles -I$ROOT/android/app/src/main/cpp -I$ROOT/src/runtime -I$ROOT/src/platform -I$PCRECOMP/runtime/recomp32 -I$PCRECOMP/runtime/win32hle $SDLI -isystem $DXVK -isystem $DXVK/windows -isystem $DXVK/directx"
APPDEF="-DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -DNFS_PARALLEL_GUEST -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DNFS_R36S -DNFS_GLES -DNFS_D3D9_BACKEND -DNFS_SDL_AUDIO -DRECOMP_STRICT_ICALL -DRECOMP_X87_WINDOW $MOJODEF"
OPT="-O2 -g -DNDEBUG -fno-strict-aliasing -ffp-contract=off"

compile() { # src obj cmd...
  local src="$1" obj="$2"; shift 2
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$FORCE" ] || \
     { [ "${src##*.}" = cpp ] && [ -n "$(find "$ROOT/ports/r36s" "$ROOT/android/app/src/main/cpp" "$ROOT/src/runtime" \( -name '*.h' -o -name '*.inc' \) -newer "$obj" | head -1)" ]; }; then
    echo "  CC $(basename "$src")"; "$@" -c "$src" -o "$obj"
  fi
}
pids=()
compile "$ROOT/ports/r36s/d3d9_gles/d3d9_gles.cpp" "$OUT/obj/d3d9_gles.o" g++ -std=c++17 $OPT $MOJODEF -DNFS_R36S -DNFS_GLES $GLESINC & pids+=($!)
for c in mojoshader.c mojoshader_common.c profiles/mojoshader_profile_common.c profiles/mojoshader_profile_glsl.c; do
  compile "$MOJO/$c" "$OUT/obj/$(basename "$c" .c).o" gcc $OPT $MOJODEF -DNFS_R36S -DNFS_GLES -I"$MOJO" & pids+=($!)
done
compile "$ROOT/ports/r36s/main.cpp" "$OUT/obj/main.o" g++ -std=c++17 $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/android/app/src/main/cpp/guest_runtime.cpp" "$OUT/obj/guest_runtime.o" g++ -std=c++17 $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/android/app/src/main/cpp/d3d9_bridge.cpp" "$OUT/obj/d3d9_bridge.o" g++ -std=c++17 $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/overrides.c" "$OUT/obj/overrides.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/fx_cache.c" "$OUT/obj/fx_cache.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/hud_half.c" "$OUT/obj/hud_half.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/world_cull.c" "$OUT/obj/world_cull.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/native_math.c" "$OUT/obj/native_math.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/native_scene.c" "$OUT/obj/native_scene.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
compile "$ROOT/src/runtime/widescreen.c" "$OUT/obj/widescreen.o" gcc $OPT $APPDEF $APPINC & pids+=($!)
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compile failed"; exit 1; }

# glibc compatibility (default on): the binary must start on ArkOS 2.0 (glibc ~2.30) as well as on newer
# systems, but this box has glibc 2.35 and would otherwise import symbol versions up to GLIBC_2.34 and
# GLIBCXX_3.4.30. compat/ pins the old symbol versions, picks faster newer libm functions at startup, and
# replaces crt1.o; libstdc++/libgcc are linked statically. libpthread/libdl/librt stay in NEEDED because on
# glibc < 2.34 the pinned GLIBC_2.17 functions live there. GLIBC_COMPAT=0 gives the plain link.
LINKCOMPAT=""
if [ "${GLIBC_COMPAT:-1}" = 1 ]; then
  CDIR="$OUT/compat"; mkdir -p "$CDIR"
  gcc -O2 -fno-builtin -fno-stack-protector -c "$ROOT/ports/r36s/compat/glibc_compat.c" -o "$CDIR/glibc_compat.o"
  gcc -c "$ROOT/ports/r36s/compat/compat_start.S" -o "$CDIR/compat_start.o"
  gcc -dumpspecs | awk '/^\*startfile:/{f=1;print;next} /^\*/{f=0} f&&NF' \
    | sed -E "s#([[:space:]]):crt1.o%s#\1:$CDIR/compat_start.o#" > "$CDIR/compat.specs"
  grep -q "compat_start.o" "$CDIR/compat.specs" || { echo "ERROR: could not build compat.specs"; exit 1; }
  LINKCOMPAT="-specs=$CDIR/compat.specs -static-libstdc++ -static-libgcc $CDIR/glibc_compat.o -Wl,--push-state,--no-as-needed -l:libpthread.so.0 -l:libdl.so.2 -l:librt.so.1 -Wl,--pop-state"
fi

echo "  LD nfsu2.aarch64"
GUEST_DIR="${GUEST_DIR:-$OUT/guest}"
g++ -o "$OUT/nfsu2.aarch64" "$OUT"/obj/*.o "$GUEST_DIR"/*.o -L"$GLSHIM" -lSDL2 -lGLESv2 -lpthread -ldl -lm -rdynamic -no-pie $LINKCOMPAT $LDEXTRA
echo "built $OUT/nfsu2.aarch64"

# PACKAGE=1: also link a device build for the PortMaster package. The VM has no real libGLESv2, and the
# shim above makes the test binary depend on Mesa's libGL.so.1 (absent on the R36S). Link instead against
# a stub exporting the GLES2 entry points with soname libGLESv2.so.2, so the binary's NEEDED list matches
# the cmake/Docker build (libSDL2-2.0.so.0, libGLESv2.so.2, libstdc++, libm, libgcc_s, libc). The VM is
# Ubuntu 22.04 aarch64 like docker/Dockerfile.r36s-test, so glibc/libstdc++ requirements are identical.
if [ -n "$PACKAGE" ]; then
  STUB="$OUT/gles2stub"; mkdir -p "$STUB" "$OUT/package"
  nm -u "$OUT"/obj/*.o | awk '{print $2}' | grep -E '^gl[A-Z]' | sort -u | sed 's/.*/void &(void){}/' > "$STUB/stub.c"
  gcc -shared -fPIC -o "$STUB/libGLESv2.so" -Wl,-soname,libGLESv2.so.2 "$STUB/stub.c"
  echo "  LD package/nfsu2.aarch64 (GLESv2 stub)"
  g++ -o "$OUT/package/nfsu2.aarch64" "$OUT"/obj/*.o "$GUEST_DIR"/*.o -L"$STUB" -L"$GLSHIM" -lSDL2 -lGLESv2 -lpthread -ldl -lm -rdynamic -no-pie $LINKCOMPAT
  cp "$OUT/package/nfsu2.aarch64" "$OUT/package/nfsu2.aarch64.debug"  # symbols for device profiles (same layout)
  strip --strip-debug "$OUT/package/nfsu2.aarch64"
  objdump -p "$OUT/package/nfsu2.aarch64" | grep NEEDED
  if [ "${GLIBC_COMPAT:-1}" = 1 ]; then
    # newest glibc version the binary imports must stay <= 2.28 (ArkOS 2.0 is ~2.30), nothing from GLIBCXX/CXXABI
    BADV=$(readelf --dyn-syms -W "$OUT/package/nfsu2.aarch64" | awk '$7=="UND"{print $8}' | grep -oE '@(GLIBC_2\.(29|[3-9][0-9])|GLIBCXX|CXXABI)[A-Za-z0-9_.]*' | sort -u | tr '\n' ' ')
    [ -z "$BADV" ] || { echo "ERROR: package imports too-new symbol versions: $BADV"; exit 1; }
    echo "glibc compat OK: max imported version $(readelf --dyn-syms -W "$OUT/package/nfsu2.aarch64" | awk '$7=="UND"{print $8}' | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tail -1)"
  fi
  if objdump -p "$OUT/package/nfsu2.aarch64" | grep -q 'NEEDED.*libGL\.so'; then echo "ERROR: links libGL.so"; exit 1; fi
  PKG="$ROOT/ports/r36s/package/nfsu2.aarch64"
  [ -f "$PKG" ] && [ ! -f "$ROOT/build/r36s-vm/nfsu2.aarch64.package-prev" ] && cp -p "$PKG" "$ROOT/build/r36s-vm/nfsu2.aarch64.package-prev"
  cp "$OUT/package/nfsu2.aarch64" "$PKG" && chmod 755 "$PKG"
  echo "staged $PKG"
fi
