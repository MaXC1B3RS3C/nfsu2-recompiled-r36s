#!/usr/bin/env python3
"""Build nfsu2.aarch64 (the R36S / PortMaster binary) from your own SPEED2.EXE.

Works on Windows, macOS and Linux. It lifts the game's code from SPEED2.EXE to C and compiles it for the
handheld; the binary is tied to the exact exe it was built from, so every player builds their own.

    python build.py "C:\\Games\\Need for Speed Underground 2"          (Windows)
    python3 build.py ~/Games/NFSU2/SPEED2.EXE                         (macOS / Linux)

Options:
    --out DIR        where to put the result (default: build/r36s next to this script)
    --install DIR    also copy the binary into a port folder, e.g. the SD card's ports/nfsu2
    --native         Linux only: build without Docker, with the tools installed on this machine
    --lift-only      only lift SPEED2.EXE to C (src/recomp/gen), natively on Windows, macOS or Linux:
                     needs Python with capstone and pefile, and git, but no Docker and no compiler
    --no-x87-window  with --lift-only: skip the local x87 window annotations (they only compile for the
                     R36S and Android builds; use this for the Windows x86 host build)
    --gen DIR        with --lift-only: where the lifted C goes (default src/recomp/gen)
    --setup-lifter   only fetch and patch the pcrecomp lifter (needs git; no game needed)
    --export-guest   also write guest-objs-<build>.tar.gz (the compiled game code) for the release pipeline,
                     which links the published binaries from it (maintainers; see BUILD.md, section 8)
    --publish        upload the exported guest objects to the GitHub release "r36s-guest-objs" (needs the
                     GitHub CLI, gh, signed in with write access to the repository)

Several exes can be given at once (e.g. a v1.0 and a v1.2 SPEED2.EXE); each is built in turn.
    --jobs N         parallel compiles (default: automatic)
    --check          only check the exe and the tools, build nothing
    --clean          delete the cached work (Docker volume or native work folder) and stop

Needs: Python 3.8+ and Docker (Docker Desktop on Windows and macOS, Docker Engine on Linux), or for --native
an Ubuntu/Debian Linux with the packages listed in BUILD.md, or for --lift-only just git and the Python
modules capstone and pefile (python -m pip install "capstone>=5.0,<6" pefile). The first build takes 30-90 minutes and about
8 GB of disk; later builds reuse the cached work. See BUILD.md for details and troubleshooting.
"""
import argparse, hashlib, os, platform, shutil, subprocess, sys, time

if sys.version_info < (3, 8):
    sys.exit("build.py needs Python 3.8 or newer")
for _s in (sys.stdout, sys.stderr):          # compiler output can hold characters a Windows console lacks
    try:
        _s.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import exe_variant  # noqa: E402  (config/exe_variants.txt)

IMAGE = "nfsu2-r36s-build"
VOLUME = "nfsu2-r36s-work"
SYSTEM = platform.system()          # Windows, Darwin, Linux
if SYSTEM == "Windows" and os.environ.get("LOCALAPPDATA"):
    NATIVE_WORK = os.path.join(os.environ["LOCALAPPDATA"], "nfsu2-r36s")
else:
    NATIVE_WORK = os.path.join(os.path.expanduser("~"), ".cache", "nfsu2-r36s")
PCRECOMP_URL = "https://github.com/sp00nznet/pcrecomp.git"
PCRECOMP_COMMIT = "35548b8"         # + patches/pcrecomp/*.patch (same as docker/r36s_build.sh)
RETAIL_SECUROM_SIZE = 5987978
DEFAULT_REPO = "manuelpllull/nfsu2-recompiled-r36s"
GUEST_RELEASE = "r36s-guest-objs"


def say(msg=""):
    print(msg, flush=True)


def fail(msg):
    say()
    say("ERROR: " + msg)
    sys.exit(1)


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def find_exe(path):
    path = os.path.abspath(os.path.expanduser(path))
    if os.path.isdir(path):
        for name in os.listdir(path):
            if name.lower() == "speed2.exe" and os.path.isfile(os.path.join(path, name)):
                return os.path.join(path, name)
        fail(f"no SPEED2.EXE in {path}")
    if not os.path.isfile(path):
        fail(f"{path} does not exist")
    return path


def check_exe(exe):
    v = exe_variant.detect(exe)
    if v:
        say(f"SPEED2.EXE: {exe}")
        say(f"  {os.path.getsize(exe):,} bytes, sha1 {sha1(exe)} -> supported build {v['id']}")
        return v
    size = os.path.getsize(exe)
    say(f"SPEED2.EXE: {exe}\n  {size:,} bytes, sha1 {sha1(exe)}")
    msg = "this SPEED2.EXE is not one the port supports."
    if size == RETAIL_SECUROM_SIZE:
        msg += ("\nIt is the retail exe with its SecuROM copy protection: its code is encrypted in the file, so it "
                "cannot be lifted.")
    msg += "\nSupported exes (config/exe_variants.txt):"
    for s in exe_variant.variants():
        msg += f"\n  {s['id']}: {s['size']:,} bytes, sha1 {s['sha1']}"
    fail(msg)


def run(cmd, log=None, env=None, check=True):
    """Run a command, echoing its output (and appending it to log)."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, bufsize=1,
                         universal_newlines=True, encoding="utf-8", errors="replace")
    with (open(log, "a", encoding="utf-8") if log else open(os.devnull, "w")) as lf:
        for line in p.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            lf.write(line)
    p.wait()
    if check and p.returncode:
        fail(f"{cmd[0]} failed (exit {p.returncode})" + (f"; full log: {log}" if log else ""))
    return p.returncode


def quiet(cmd):
    try:
        return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
                              timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return subprocess.CompletedProcess(cmd, 1, "", str(e))


# ---------------------------------------------------------------------------------------------- Docker
def docker_ready(start):
    if not shutil.which("docker"):
        hint = {"Windows": "install Docker Desktop (https://www.docker.com/products/docker-desktop/)",
                "Darwin": "install Docker Desktop (https://www.docker.com/products/docker-desktop/)",
                "Linux": "install Docker Engine (https://docs.docker.com/engine/install/) or use --native"}
        fail("docker not found: " + hint.get(SYSTEM, hint["Linux"]))
    r = quiet(["docker", "info"])
    if r.returncode == 0:
        return
    if "permission denied" in (r.stderr or "").lower():
        fail("Docker is installed but this user may not use it: add yourself to the docker group "
             "(sudo usermod -aG docker $USER, then log in again) or run with sudo")
    if start:
        if SYSTEM == "Windows":
            exe = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Docker", "Docker",
                               "Docker Desktop.exe")
            if os.path.exists(exe):
                say("Starting Docker Desktop...")
                subprocess.Popen([exe])
        elif SYSTEM == "Darwin":
            say("Starting Docker Desktop...")
            quiet(["open", "-a", "Docker"])
        for _ in range(60):
            time.sleep(5)
            if quiet(["docker", "info"]).returncode == 0:
                return
    fail("Docker is not running: start Docker Desktop (Windows/macOS) or the docker service "
         "(Linux: sudo systemctl start docker) and try again")


GUEST_RELEASE = "r36s-guest-objs"
DEFAULT_REPO = "manuelpllull/nfsu2-recompiled-r36s"


def build_docker(exe, out, jobs, log, export=False, image_done=[False]):
    if not image_done[0]:
        say("\n== Docker image")
        run(["docker", "build", "-t", IMAGE, "-f", os.path.join(ROOT, "docker", "Dockerfile.r36s-build"),
             os.path.join(ROOT, "docker")], log)
        image_done[0] = True
    say("\n== Lift and compile (first time: 30-90 minutes)")
    cmd = ["docker", "run", "--rm",
           "--mount", f"type=volume,source={VOLUME},target=/work",
           "--mount", f"type=bind,source={ROOT},target=/src,readonly",
           "--mount", f"type=bind,source={os.path.dirname(exe)},target=/exe,readonly",
           "--mount", f"type=bind,source={out},target=/out",
           "-e", f"EXE=/exe/{os.path.basename(exe)}"]
    if jobs:
        cmd += ["-e", f"NFSU2_JOBS={jobs}"]
    if export:
        cmd += ["-e", "EXPORT_GUEST=1"]
    if SYSTEM == "Linux" and hasattr(os, "getuid"):
        cmd += ["-e", f"HOST_UID={os.getuid()}", "-e", f"HOST_GID={os.getgid()}"]
    cmd += [IMAGE, "bash", "/src/docker/r36s_build.sh"]
    run(cmd, log)


def build_native(exe, out, jobs, log, work=NATIVE_WORK, export=False):
    if SYSTEM != "Linux":
        fail("--native works on Linux only (on Windows it works inside WSL2 Ubuntu); use Docker here, "
             "or --lift-only to lift without compiling")
    env = dict(os.environ, EXE=exe, NFSU2_SRC=ROOT, NFSU2_WORK=work, NFSU2_OUT=out)
    if jobs:
        env["NFSU2_JOBS"] = str(jobs)
    if export:
        env["EXPORT_GUEST"] = "1"
    say(f"\n== Lift and compile natively (work folder {work}; first time: 30-90 minutes)")
    run(["bash", os.path.join(ROOT, "docker", "r36s_build.sh")], log, env=env)


def check_native_tools():
    missing = [t for t in ("aarch64-linux-gnu-gcc", "aarch64-linux-gnu-g++", "aarch64-linux-gnu-nm",
                           "aarch64-linux-gnu-strip", "git", "bash", "tar") if not shutil.which(t)]
    r = quiet([sys.executable, "-c", "import capstone, pefile"])
    if r.returncode:
        missing.append("python modules capstone, pefile")
    if missing:
        fail("missing for --native: " + ", ".join(missing) + " (see BUILD.md, Linux without Docker)")


# ---------------------------------------------------------------------------------------------- lift only
def file_key(*parts):
    """sha1 over strings and file contents (line endings ignored), for the caches."""
    h = hashlib.sha1()
    for p in parts:
        if os.path.isfile(p):
            with open(p, "rb") as f:
                h.update(f.read().replace(b"\r", b""))
        else:
            h.update(str(p).encode())
    return h.hexdigest()


def lifter_tools():
    missing = [] if shutil.which("git") else ["git"]
    r = quiet([sys.executable, "-c", "import capstone, pefile"])
    if r.returncode:
        missing.append("the Python modules capstone and pefile "
                       f"({os.path.basename(sys.executable)} -m pip install \"capstone>=5.0,<6\" pefile)")
    if missing:
        fail("missing for the lift: " + ", ".join(missing) + " (see BUILD.md, section 7)")


def setup_lifter(work, log):
    """pcrecomp at PCRECOMP_COMMIT with the project's patches and runtime headers, in work/pcrecomp."""
    import glob
    patches = sorted(glob.glob(os.path.join(ROOT, "patches", "pcrecomp", "*.patch")))
    key = file_key(PCRECOMP_COMMIT, *patches)
    pc = os.path.join(work, "pcrecomp")
    marker = os.path.join(pc, ".nfsu2-patched")
    if not (os.path.isfile(marker) and open(marker).read().strip() == key):
        say(f"\n== pcrecomp {PCRECOMP_COMMIT} + {len(patches)} patches -> {pc}")
        shutil.rmtree(pc, ignore_errors=True)
        os.makedirs(work, exist_ok=True)
        # LF line endings (the patches are LF), whatever core.autocrlf says on this machine
        run(["git", "-c", "core.autocrlf=false", "clone", "-q", PCRECOMP_URL, pc], log)
        run(["git", "-C", pc, "config", "core.autocrlf", "false"], log)
        run(["git", "-C", pc, "checkout", "-q", PCRECOMP_COMMIT], log)
        run(["git", "-C", pc, "-c", "user.name=build", "-c", "user.email=build@localhost",
             "am", "-q", "--3way"] + patches, log)
        with open(marker, "w") as f:
            f.write(key)
    # the runtime headers the port compiles with (patched headers + the thread-local CPU state)
    sdk = os.path.join(ROOT, "ports", "r36s", "sdk", "include", "recomp32")
    for name in os.listdir(sdk):
        if name.endswith((".h", ".c")):
            shutil.copyfile(os.path.join(sdk, name), os.path.join(pc, "runtime", "recomp32", name))
    say(f"lifter ready: {pc}")
    return pc


def lift_native(exe, variant, gen, work, x87_window, log):
    """docker/r36s_build.sh steps 1-3 in Python, on any OS: analysis copy, seeds, catalog, lift, x87 data."""
    import json
    pc = setup_lifter(work, log)
    py = sys.executable
    env = dict(os.environ, PCRECOMP=pc)
    lw = os.path.join(ROOT, "ports", "r36s", "lw")
    L = os.path.join(work, "lift-" + variant["id"])
    os.makedirs(L, exist_ok=True)
    exesha = sha1(exe)
    patchkey = open(os.path.join(pc, ".nfsu2-patched")).read().strip()
    liftkey = file_key(exesha, patchkey, str(x87_window),
                       *[os.path.join(ROOT, *p.split("/")) for p in (
                           "scripts/run_lift.py", "scripts/exe_variant.py", "scripts/normalize_exe.py",
                           "config/exe_variants.txt", "config/hooks.txt", "config/overrides.txt",
                           "config/exclude.txt", "ports/r36s/lw/analyze.py", "ports/r36s/lw/annotate.py")])
    done = os.path.join(gen, ".lift-done")
    if os.path.isfile(done) and open(done).read().strip() == liftkey:
        say(f"\nlifted code for this exe and lifter already in {gen}")
        return
    ana = os.path.join(L, "SPEED2.analysis.exe")
    say("\n== analysis copy and seeds")
    run([py, os.path.join(ROOT, "scripts", "normalize_exe.py"), exe, ana], log, env=env)
    run([py, os.path.join(pc, "tools", "cpp", "vtable_scan.py"), ana, "--seeds", os.path.join(L, "vtable_seeds.json")],
        log, env=env)
    run([py, os.path.join(pc, "tools", "cpp", "rtti.py"), ana, "--seeds", os.path.join(L, "rtti_seeds.json")],
        log, env=env)
    seen, seeds = set(), []
    for n in ("vtable_seeds.json", "rtti_seeds.json"):
        for e in json.load(open(os.path.join(L, n))):
            if e["address"] not in seen:
                seen.add(e["address"])
                seeds.append(e)
    with open(os.path.join(L, "seeds_all.json"), "w") as f:
        json.dump(seeds, f)
    say(f"{len(seeds)} seeds")
    cat, catkey_file = os.path.join(L, "catalog.json"), os.path.join(L, "catalog.key")
    catkey = file_key(exesha, patchkey)
    if not (os.path.isfile(cat) and os.path.isfile(catkey_file) and open(catkey_file).read().strip() == catkey):
        say("\n== function catalog (5-15 minutes)")
        run([py, os.path.join(ROOT, "scripts", "run_lift.py"), "catalog", ana, os.path.join(L, "seeds_all.json"), cat],
            log, env=env)
        with open(catkey_file, "w") as f:
            f.write(catkey)
    say(f"\n== lift to C -> {gen}")
    shutil.rmtree(gen, ignore_errors=True)
    os.makedirs(gen)
    run([py, os.path.join(ROOT, "scripts", "run_lift.py"), "lift", ana, cat, gen], log, env=env)
    if x87_window:
        say("\n== local x87 window data (R36S / Android)")
        info = os.path.join(L, "nfs_lw_info.json")
        run([py, os.path.join(lw, "annotate.py"), "strip", gen], log)
        run([py, os.path.join(lw, "analyze.py"), gen, info], log)
        run([py, os.path.join(lw, "annotate.py"), "apply", gen, info], log)
        run([py, os.path.join(lw, "annotate.py"), "header", gen, info, os.path.join(gen, "nfs_lw.h")], log)
    with open(done, "w") as f:
        f.write(liftkey)


# ---------------------------------------------------------------------------------------------- install
def install(binary, variant, port_dir, versioned=False):
    port_dir = os.path.abspath(os.path.expanduser(port_dir))
    if not os.path.isdir(port_dir):
        fail(f"--install: {port_dir} is not a folder (it should be the port folder, e.g. <SD card>/ports/nfsu2)")
    game_exe = None
    gd = os.path.join(port_dir, "gamedata")
    if os.path.isdir(gd):
        for name in os.listdir(gd):
            if name.lower() == "speed2.exe":
                game_exe = os.path.join(gd, name)
    if versioned:
        pass   # one binary per build: the launcher picks the one for the exe in gamedata/
    elif game_exe and exe_variant.detect(game_exe) != variant:
        say(f"WARNING: {game_exe} is not the {variant['id']} exe this binary was built from; copy that exe "
            "into gamedata/ too, or the launcher will refuse to start")
    elif not game_exe:
        say(f"NOTE: no gamedata/SPEED2.EXE in {port_dir} yet: copy your game files there (see BUILD.md)")
    dst = os.path.join(port_dir, "nfsu2.aarch64" + ("." + variant["id"] if versioned else ""))
    if os.path.exists(dst):
        shutil.copy2(dst, dst + ".prev")
        say(f"previous binary kept as {dst}.prev")
    shutil.copyfile(binary, dst)
    if sha1(dst) != sha1(binary):
        fail(f"copy to {dst} is damaged; check the SD card")
    try:
        os.chmod(dst, 0o755)
    except OSError:
        pass
    say(f"installed {dst}")


# ---------------------------------------------------------------------------------------------- publish
def publish(tarballs, repo):
    """Upload guest-objs-*.tar.gz (+ .sha1) to the GitHub release the release pipeline reads them from."""
    if not shutil.which("gh"):
        fail("--publish needs the GitHub CLI: https://cli.github.com/ (winget install GitHub.cli, "
             "brew install gh, or your distribution's gh package), then gh auth login. Or upload these files "
             f"by hand to the release '{GUEST_RELEASE}' of {repo}:\n  " + "\n  ".join(tarballs))
    if quiet(["gh", "auth", "status"]).returncode:
        fail("gh is not signed in: run gh auth login")
    if quiet(["gh", "release", "view", GUEST_RELEASE, "-R", repo]).returncode:
        say(f"creating the release {GUEST_RELEASE} on {repo}")
        run(["gh", "release", "create", GUEST_RELEASE, "-R", repo, "--prerelease",
             "--title", "Guest objects for the release builds (not a game release)",
             "--notes", "Compiled game code exported by build.py --export-guest. The release workflow links "
                        "the published nfsu2.aarch64 binaries from these files. Not for players."])
    files = []
    for tb in tarballs:
        files += [tb, tb + ".sha1"]
    say(f"\n== uploading {len(tarballs)} guest object archive(s) to {repo} release {GUEST_RELEASE}")
    run(["gh", "release", "upload", GUEST_RELEASE, "-R", repo, "--clobber"] + files)
    say("uploaded; the next release of main links its binaries from them")


# ---------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="See BUILD.md for the full guide.")
    ap.add_argument("exe", nargs="*", help="SPEED2.EXE, or the game folder that contains it (several allowed)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "r36s"), help="output folder")
    ap.add_argument("--install", metavar="PORT_DIR", help="copy the binary into this port folder")
    ap.add_argument("--native", action="store_true", help="Linux: build without Docker")
    ap.add_argument("--jobs", type=int, help="parallel compiles")
    ap.add_argument("--check", action="store_true", help="check the exe and tools only")
    ap.add_argument("--clean", action="store_true", help="delete the cached work and stop")
    ap.add_argument("--lift-only", action="store_true",
                    help="only lift to C, natively on any OS (needs git, capstone, pefile; no Docker)")
    ap.add_argument("--no-x87-window", action="store_true",
                    help="with --lift-only: no R36S/Android x87 annotations (for the Windows host build)")
    ap.add_argument("--gen", default=os.path.join(ROOT, "src", "recomp", "gen"),
                    help="with --lift-only: output folder of the lifted C")
    ap.add_argument("--work", default=NATIVE_WORK, help="work folder for --lift-only, --native, --setup-lifter")
    ap.add_argument("--setup-lifter", action="store_true", help="only fetch and patch the pcrecomp lifter")
    ap.add_argument("--export-guest", action="store_true",
                    help="also write guest-objs-<build>.tar.gz for the release pipeline")
    ap.add_argument("--publish", action="store_true",
                    help="upload the exported guest objects to the GitHub release r36s-guest-objs (needs gh)")
    ap.add_argument("--repo", default=DEFAULT_REPO, help="GitHub repository for --publish")
    a = ap.parse_args()
    work = os.path.abspath(os.path.expanduser(a.work))

    say(f"NFSU2 R36S build ({SYSTEM} {platform.machine()}, Python {platform.python_version()})")
    if a.setup_lifter:
        if not shutil.which("git"):
            fail("git not found (see BUILD.md, section 7)")
        setup_lifter(work, None)
        return 0
    if a.clean:
        if a.native or a.lift_only:
            shutil.rmtree(work, ignore_errors=True)
            say(f"deleted {work}")
        else:
            docker_ready(start=False)
            quiet(["docker", "volume", "rm", VOLUME])
            say(f"deleted the Docker volume {VOLUME}")
        return 0
    if not a.exe:
        ap.error("give the path of SPEED2.EXE or of the game folder")

    exes = [find_exe(e) for e in a.exe]
    variants = [check_exe(e) for e in exes]
    ids = [v["id"] for v in variants]
    if len(set(ids)) != len(ids):
        fail("two of the exes are the same build (" + ", ".join(ids) + "); give each build once")
    if a.publish:
        a.export_guest = True
    if a.lift_only:
        lifter_tools()
    elif a.native:
        check_native_tools()
    else:
        docker_ready(start=not a.check)
    if a.check:
        say("\nchecks passed: the exe is supported and the tools are ready")
        return 0
    if a.lift_only:
        if len(exes) > 1:
            fail("--lift-only lifts one exe at a time (into --gen)")
        exe, variant = exes[0], variants[0]
        gen = os.path.abspath(os.path.expanduser(a.gen))
        os.makedirs(work, exist_ok=True)
        log = os.path.join(work, "lift.log")
        with open(log, "w", encoding="utf-8") as lf:
            lf.write(f"build.py --lift-only on {SYSTEM} {platform.machine()}, exe {exe} ({variant['id']}), {time.ctime()}\n")
        t0 = time.time()
        lift_native(exe, variant, gen, work, not a.no_x87_window, log)
        say(f"\nDone in {int(time.time() - t0) // 60} min: lifted C for SPEED2.EXE {variant['id']} in {gen}")
        say(f"  log: {log}")
        return 0

    out = os.path.abspath(os.path.expanduser(a.out))
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "build.log")
    with open(log, "w", encoding="utf-8") as lf:
        lf.write(f"build.py on {SYSTEM} {platform.machine()}, exes {', '.join(exes)} ({', '.join(ids)}), {time.ctime()}\n")
    results, tarballs = [], []
    for exe, variant in zip(exes, variants):
        t0 = time.time()
        say(f"\n######## SPEED2.EXE {variant['id']}: {exe}")
        if a.native:
            build_native(exe, out, a.jobs, log, work, a.export_guest)
        else:
            build_docker(exe, out, a.jobs, log, a.export_guest)
        binary = os.path.join(out, f"nfsu2-{variant['id']}.aarch64")
        if not os.path.isfile(binary):
            fail(f"the build ended without {binary}; see {log}")
        say(f"\n{variant['id']} done in {int(time.time() - t0) // 60} min: {binary}")
        say(f"  sha1 {sha1(binary)}, built for SPEED2.EXE {variant['id']}")
        results.append((binary, variant))
        if a.export_guest:
            tb = os.path.join(out, f"guest-objs-{variant['id']}.tar.gz")
            if not os.path.isfile(tb):
                fail(f"the build ended without {tb}; see {log}")
            say(f"  guest objects for the release: {tb}")
            tarballs.append(tb)

    if a.install:
        for binary, variant in results:
            install(binary, variant, a.install, versioned=len(results) > 1)
    else:
        say("\nCopy the binary to the SD card as ports/nfsu2/nfsu2.aarch64 (or nfsu2.aarch64.<build>, e.g.\n"
            "nfsu2.aarch64.v12; the launcher picks the one for your SPEED2.EXE), together with this same\n"
            "SPEED2.EXE in ports/nfsu2/gamedata/ (or run again with --install <SD card>/ports/nfsu2).")
    if a.publish:
        publish(tarballs, a.repo)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit("\ninterrupted")
