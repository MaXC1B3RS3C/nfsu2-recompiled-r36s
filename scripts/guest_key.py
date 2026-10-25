#!/usr/bin/env python3
"""Fingerprint of everything the compiled game code (the guest objects) of one SPEED2.EXE build depends on.

    guest_key.py <variant>        e.g. v10 or v12 (config/exe_variants.txt); prints the key

The release links the guest objects that build.py --export-guest produced on a PC with the game (the exe
cannot be in the repository or the CI). They are only valid for the source they were compiled from, so
docker/r36s_build.sh stores this key in guest-objs-<variant>.tar.gz and the release recomputes it from its
own checkout: a different key means the objects are stale and must be exported again.

Inputs: the exe (its SHA-1 from config/exe_variants.txt), the lifter (pcrecomp commit + patches/pcrecomp),
the lift driver and its per-exe lists, the local x87 window analysis, the guest compile (script, flags,
arm64_guest.h, the recomp32 runtime headers) and the wrap list (config/exe_functions.txt). Line endings are
ignored, so a Windows checkout gives the same key as a Linux one.
"""
import glob, hashlib, os, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PCRECOMP_COMMIT = "35548b8"
GUEST_COMPILER = "aarch64-linux-gnu-gcc 11 (Ubuntu 22.04, docker/Dockerfile.r36s-build)"
INPUTS = [
    "patches/pcrecomp/*.patch",
    "scripts/run_lift.py", "scripts/exe_variant.py", "scripts/normalize_exe.py",
    "scripts/split_android_sources.py",
    "config/exe_variants.txt", "config/hooks.txt", "config/overrides.txt", "config/exclude.txt",
    "config/exe_functions.txt",
    "ports/r36s/lw/*.py", "ports/r36s/lw/*.sh",
    "ports/r36s/build_guest_vm.sh",
    "android/app/src/main/cpp/arm64_guest.h",
    "ports/r36s/sdk/include/recomp32/*.h",
]


def files():
    out = []
    for pat in INPUTS:
        hits = sorted(glob.glob(os.path.join(ROOT, *pat.split("/"))))
        if not hits:
            raise SystemExit(f"guest_key.py: nothing matches {pat}")
        out += hits
    return out


def key(variant_id):
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import exe_variant
    v = exe_variant.variant(variant_id)
    h = hashlib.sha1()
    h.update(f"{v['id']} {v['sha1']} {PCRECOMP_COMMIT} {GUEST_COMPILER}\n".encode())
    for p in files():
        rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
        with open(p, "rb") as f:
            data = f.read().replace(b"\r", b"")
        h.update(f"{rel} {len(data)}\n".encode())
        h.update(data)
    return h.hexdigest()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    print(key(sys.argv[1]))
